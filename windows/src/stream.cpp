#include "stream.hpp"

#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "display.hpp"
#include "input.hpp"
#include "nv12.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace screenlink {
namespace {

// Minimal COM smart pointer (avoids depending on WRL).
template <typename T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T** put() { reset(); return &p_; }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    explicit operator bool() const { return p_ != nullptr; }
    template <typename U> HRESULT as(Com<U>& out) const { return p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out.put())); }
private:
    T* p_ = nullptr;
};

std::string hr_text(const char* what, HRESULT hr) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

bool send_all(SOCKET s, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        const int n = send(s, p, static_cast<int>(std::min<size_t>(size, 1 << 20)), 0);
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

void put_u32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (i * 8))); }
void put_u64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 7; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (i * 8))); }

// ---------------------------------------------------------------- capture
class Capturer {
public:
    // gdi_name: e.g. L"\\\\.\\DISPLAY6"; empty = primary display.
    std::string init(const std::wstring& gdi_name = {}) {
        Com<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.put()));
        if (FAILED(hr)) return hr_text("CreateDXGIFactory1", hr);
        const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        Com<IDXGIAdapter1> chosen_adapter;
        Com<IDXGIOutput> chosen_output;
        for (UINT a = 0;; ++a) {
            Com<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(a, adapter.put()) == DXGI_ERROR_NOT_FOUND) break;
            for (UINT o = 0;; ++o) {
                Com<IDXGIOutput> output;
                if (adapter->EnumOutputs(o, output.put()) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC d{};
                output->GetDesc(&d);
                const bool wanted = gdi_name.empty() ? d.Monitor == primary : gdi_name == d.DeviceName;
                if ((!chosen_output && gdi_name.empty()) || wanted) {
                    const bool is_primary = wanted;
                    chosen_adapter.reset();
                    adapter->QueryInterface(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(chosen_adapter.put()));
                    chosen_output.reset();
                    output->QueryInterface(__uuidof(IDXGIOutput), reinterpret_cast<void**>(chosen_output.put()));
                    if (is_primary) goto found;
                }
            }
        }
    found:
        if (!chosen_output) return gdi_name.empty() ? "no display output found" : "virtual display output not found";
        D3D_FEATURE_LEVEL level;
        hr = D3D11CreateDevice(chosen_adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               nullptr, 0, D3D11_SDK_VERSION, device_.put(), &level, context_.put());
        if (FAILED(hr)) return hr_text("D3D11CreateDevice", hr);
        if (FAILED(hr = chosen_output.as(output_))) return hr_text("IDXGIOutput1", hr);
        return duplicate();
    }

    std::string duplicate() {
        dupl_.reset();
        staging_.reset();
        HRESULT hr = output_->DuplicateOutput(device_.get(), dupl_.put());
        if (FAILED(hr)) return hr_text("DuplicateOutput", hr);
        DXGI_OUTPUT_DESC od{};
        output_->GetDesc(&od);
        monitor_.left = od.DesktopCoordinates.left;
        monitor_.top = od.DesktopCoordinates.top;
        monitor_.width = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
        monitor_.height = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
        DXGI_OUTDUPL_DESC dd{};
        dupl_->GetDesc(&dd);
        width_ = static_cast<int>(dd.ModeDesc.Width);
        height_ = static_cast<int>(dd.ModeDesc.Height);
        rotation_ = dd.Rotation;
        // For a turned screen the duplicated image keeps the panel's own (unturned) shape.
        if (rotation_ == DXGI_MODE_ROTATION_ROTATE90 || rotation_ == DXGI_MODE_ROTATION_ROTATE270) std::swap(width_, height_);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(width_);
        td.Height = static_cast<UINT>(height_);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device_->CreateTexture2D(&td, nullptr, staging_.put());
        if (FAILED(hr)) return hr_text("CreateTexture2D", hr);
        return {};
    }

    // got=true when a new desktop image was copied into the staging texture.
    std::string acquire(UINT timeout_ms, bool& got) {
        got = false;
        if (!dupl_) {
            std::string err = duplicate();
            if (!err.empty()) { Sleep(100); return {}; }  // e.g. secure desktop: retry quietly
        }
        DXGI_OUTDUPL_FRAME_INFO info{};
        Com<IDXGIResource> resource;
        HRESULT hr = dupl_->AcquireNextFrame(timeout_ms, &info, resource.put());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return {};
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) { dupl_.reset(); return {}; }
        if (FAILED(hr)) return hr_text("AcquireNextFrame", hr);
        if (info.LastPresentTime.QuadPart != 0 || first_) {
            Com<ID3D11Texture2D> tex;
            if (SUCCEEDED(resource.as(tex))) {
                context_->CopyResource(staging_.get(), tex.get());
                got = true;
                first_ = false;
            }
        }
        if (info.LastMouseUpdateTime.QuadPart != 0) pointer_moved_ = true;
        dupl_->ReleaseFrame();
        return {};
    }

    bool map(const uint8_t*& data, size_t& pitch) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        data = static_cast<const uint8_t*>(m.pData);
        pitch = m.RowPitch;
        return true;
    }
    void unmap() { context_->Unmap(staging_.get(), 0); }

    bool take_pointer_moved() { const bool v = pointer_moved_; pointer_moved_ = false; return v; }
    int width() const { return width_; }
    int height() const { return height_; }
    const MonitorRect& monitor() const { return monitor_; }
    // The duplicated image is not turned: for an upright (rotated) screen it must be turned by us.
    int quarter_turns() const {
        return rotation_ == DXGI_MODE_ROTATION_ROTATE90 ? 1 : rotation_ == DXGI_MODE_ROTATION_ROTATE270 ? 3
             : rotation_ == DXGI_MODE_ROTATION_ROTATE180 ? 2 : 0;
    }

private:
    DXGI_MODE_ROTATION rotation_ = DXGI_MODE_ROTATION_IDENTITY;
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<IDXGIOutput1> output_;
    Com<IDXGIOutputDuplication> dupl_;
    Com<ID3D11Texture2D> staging_;
    MonitorRect monitor_;
    int width_ = 0, height_ = 0;
    bool first_ = true;
    bool pointer_moved_ = false;
};

// ---------------------------------------------------------------- encoder
class Encoder {
public:
    ~Encoder() { shutdown(); }

    // Tries a hardware encoder first (AMD/NVIDIA/Intel), then the Microsoft software encoder (H.264 only).
    // hevc: H.265 (hardware only); on failure the caller asks again for H.264.
    std::string init(int width, int height, int fps, int bitrate, bool allow_hardware, bool hevc = false) {
        width_ = width; height_ = height; fps_ = fps; bitrate_ = bitrate; hevc_ = hevc;
        std::string err;
        if (hevc) {
            if (!allow_hardware) return "H.265 needs a GPU encoder";
            err = init_hardware();
            if (!err.empty()) { shutdown(); hevc_ = false; }
            return err;
        }
        if (allow_hardware) {
            err = init_hardware();
            if (err.empty()) return {};
            shutdown();
        }
        std::string sw = init_software();
        if (!sw.empty() && !err.empty()) return err + "; " + sw;
        return sw;
    }

    // Switch to the software encoder after a hardware failure.
    std::string fall_back_to_software() {
        shutdown();
        return init_software();
    }

    const std::string& name() const { return name_; }
    bool hardware() const { return hardware_; }
    bool hevc() const { return hevc_; }

    // Live changes from the tablet (adaptive quality). Not every encoder accepts them; failures are ignored.
    bool set_bitrate(int bitrate) {
        if (!api_) return false;
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = static_cast<ULONG>(bitrate);
        const bool ok = SUCCEEDED(api_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v));
        if (ok) bitrate_ = bitrate;
        return ok;
    }
    void force_keyframe() {
        if (!api_) return;
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = 1;
        api_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
    }

    std::string encode(const std::vector<uint8_t>& nv12, int64_t time_100ns, std::vector<std::vector<uint8_t>>& outputs) {
        Com<IMFMediaBuffer> buf;
        HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()), buf.put());
        if (FAILED(hr)) return hr_text("MFCreateMemoryBuffer", hr);
        BYTE* p = nullptr;
        buf->Lock(&p, nullptr, nullptr);
        std::memcpy(p, nv12.data(), nv12.size());
        buf->Unlock();
        buf->SetCurrentLength(static_cast<DWORD>(nv12.size()));
        Com<IMFSample> sample;
        MFCreateSample(sample.put());
        sample->AddBuffer(buf.get());
        sample->SetSampleTime(time_100ns);
        sample->SetSampleDuration(10'000'000LL / fps_);
        return async_ ? encode_async(sample.get(), outputs) : encode_sync(sample.get(), outputs);
    }

private:
    void shutdown() {
        if (mft_) {
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, in_id_);
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        api_.reset();
        events_.reset();
        mft_.reset();
        async_ = false;
        hardware_ = false;
        need_input_ = 0;
    }

    std::string init_hardware() {
        MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video, hevc_ ? MFVideoFormat_HEVC : MFVideoFormat_H264};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                               &in_info, &out_info, &acts, &count);
        if (FAILED(hr) || count == 0) {
            if (acts) CoTaskMemFree(acts);
            return hevc_ ? "no hardware H.265 encoder" : "no hardware H.264 encoder";
        }
        std::string err = "hardware encoder rejected the settings";
        for (UINT32 i = 0; i < count; ++i) {
            if (mft_) continue;
            wchar_t* fname = nullptr;
            UINT32 len = 0;
            std::string friendly = "hardware encoder";
            if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fname, &len)) && fname) {
                char buf8[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, fname, -1, buf8, sizeof(buf8), nullptr, nullptr);
                friendly = buf8;
                CoTaskMemFree(fname);
            }
            if (FAILED(acts[i]->ActivateObject(__uuidof(IMFTransform), reinterpret_cast<void**>(mft_.put())))) {
                mft_.reset();
                continue;
            }
            Com<IMFAttributes> attrs;
            if (SUCCEEDED(mft_->GetAttributes(attrs.put())) && attrs) {
                UINT32 is_async = 0;
                attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
                if (is_async) {
                    attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                    async_ = true;
                }
                attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
            }
            if (async_ && FAILED(mft_->QueryInterface(__uuidof(IMFMediaEventGenerator), reinterpret_cast<void**>(events_.put())))) {
                mft_.reset();
                async_ = false;
                continue;
            }
            const std::string e = configure();
            if (!e.empty()) {
                err = friendly + ": " + e;
                events_.reset();
                mft_.reset();
                async_ = false;
                continue;
            }
            name_ = friendly;
            hardware_ = true;
        }
        for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
        return mft_ ? std::string() : err;
    }

    std::string init_software() {
        HRESULT hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFTransform),
                                      reinterpret_cast<void**>(mft_.put()));
        if (FAILED(hr)) return hr_text("H.264 encoder", hr);
        name_ = "Microsoft H.264 (software)";
        hardware_ = false;
        async_ = false;
        return configure();
    }

    std::string configure() {
        in_id_ = 0;
        out_id_ = 0;
        DWORD in_ids[1] = {0}, out_ids[1] = {0};
        if (SUCCEEDED(mft_->GetStreamIDs(1, in_ids, 1, out_ids))) { in_id_ = in_ids[0]; out_id_ = out_ids[0]; }

        // ICodecAPI IID spelled out: some toolchains (MinGW) have no __uuidof for it.
        static const GUID kIID_ICodecAPI = {0x901db4c7, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};
        api_.reset();
        Com<ICodecAPI>& api = api_;
        if (SUCCEEDED(mft_->QueryInterface(kIID_ICodecAPI, reinterpret_cast<void**>(api.put())))) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
            api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
            v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_CBR;
            api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
            v.vt = VT_UI4; v.ulVal = static_cast<ULONG>(bitrate_);
            api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
            v.vt = VT_UI4; v.ulVal = static_cast<ULONG>(fps_ * 2);
            api->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
            v.vt = VT_UI4; v.ulVal = 0;
            api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
        }

        HRESULT hr;
        Com<IMFMediaType> out;
        MFCreateMediaType(out.put());
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, hevc_ ? MFVideoFormat_HEVC : MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate_));
        MFSetAttributeSize(out.get(), MF_MT_FRAME_SIZE, width_, height_);
        MFSetAttributeRatio(out.get(), MF_MT_FRAME_RATE, fps_, 1);
        MFSetAttributeRatio(out.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, hevc_ ? static_cast<UINT32>(eAVEncH265VProfile_Main_420_8) : static_cast<UINT32>(eAVEncH264VProfile_Base));
        if (FAILED(hr = mft_->SetOutputType(out_id_, out.get(), 0))) {
            if (hevc_) return hr_text("SetOutputType (H.265)", hr);
            // Some hardware encoders refuse Baseline: try Main (still no B-frames).
            out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
            if (FAILED(hr = mft_->SetOutputType(out_id_, out.get(), 0))) return hr_text("SetOutputType", hr);
        }

        Com<IMFMediaType> in;
        MFCreateMediaType(in.put());
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(in.get(), MF_MT_FRAME_SIZE, width_, height_);
        MFSetAttributeRatio(in.get(), MF_MT_FRAME_RATE, fps_, 1);
        MFSetAttributeRatio(in.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(hr = mft_->SetInputType(in_id_, in.get(), 0))) return hr_text("SetInputType", hr);

        MFT_OUTPUT_STREAM_INFO info{};
        mft_->GetOutputStreamInfo(out_id_, &info);
        provides_samples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        out_size_ = std::max<DWORD>(info.cbSize, static_cast<DWORD>(width_ * height_));

        // Some encoders (AMD H.265) only honour rate control once the media types are set: apply it again.
        if (api_) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_CBR;
            api_->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
            v.vt = VT_UI4; v.ulVal = static_cast<ULONG>(bitrate_);
            api_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
        }
        if (FAILED(hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) return hr_text("BEGIN_STREAMING", hr);
        if (FAILED(hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, in_id_))) return hr_text("START_OF_STREAM", hr);
        return {};
    }

    std::string encode_sync(IMFSample* sample, std::vector<std::vector<uint8_t>>& outputs) {
        HRESULT hr = mft_->ProcessInput(in_id_, sample, 0);
        if (hr == MF_E_NOTACCEPTING) {
            std::string err = drain(outputs, false);
            if (!err.empty()) return err;
            hr = mft_->ProcessInput(in_id_, sample, 0);
        }
        if (FAILED(hr)) return hr_text("ProcessInput", hr);
        return drain(outputs, false);
    }

    // Handles queued events without blocking. Returns false on a fatal error (in err).
    bool pump_events(std::vector<std::vector<uint8_t>>& outputs, std::string& err) {
        for (;;) {
            Com<IMFMediaEvent> ev;
            const HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, ev.put());
            if (hr == MF_E_NO_EVENTS_AVAILABLE) return true;
            if (FAILED(hr)) { err = hr_text("GetEvent", hr); return false; }
            MediaEventType type = MEUnknown;
            ev->GetType(&type);
            if (type == METransformNeedInput) {
                ++need_input_;
            } else if (type == METransformHaveOutput) {
                err = drain(outputs, true);
                if (!err.empty()) return false;
            }
        }
    }

    std::string encode_async(IMFSample* sample, std::vector<std::vector<uint8_t>>& outputs) {
        std::string err;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (need_input_ == 0) {
            if (!pump_events(outputs, err)) return err;
            if (need_input_ > 0) break;
            if (std::chrono::steady_clock::now() > deadline) return "hardware encoder stalled";
            Sleep(1);
        }
        const HRESULT hr = mft_->ProcessInput(in_id_, sample, 0);
        if (FAILED(hr)) return hr_text("ProcessInput", hr);
        --need_input_;
        // Wait briefly for this frame's output so latency stays at about one frame.
        const size_t before = outputs.size();
        const auto out_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
        while (outputs.size() == before && std::chrono::steady_clock::now() < out_deadline) {
            if (!pump_events(outputs, err)) return err;
            if (outputs.size() == before) Sleep(1);
        }
        return {};
    }

    // single=true: collect exactly one output (async HaveOutput event).
    std::string drain(std::vector<std::vector<uint8_t>>& outputs, bool single) {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER db{};
            db.dwStreamID = out_id_;
            Com<IMFSample> own;
            if (!provides_samples_) {
                Com<IMFMediaBuffer> b;
                MFCreateMemoryBuffer(out_size_, b.put());
                MFCreateSample(own.put());
                own->AddBuffer(b.get());
                db.pSample = own.get();
            }
            DWORD status = 0;
            HRESULT hr = mft_->ProcessOutput(0, 1, &db, &status);
            if (db.pEvents) db.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return {};
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                Com<IMFMediaType> t;
                if (SUCCEEDED(mft_->GetOutputAvailableType(out_id_, 0, t.put()))) mft_->SetOutputType(out_id_, t.get(), 0);
                if (single) return {};
                continue;
            }
            if (FAILED(hr)) return hr_text("ProcessOutput", hr);
            IMFSample* produced = db.pSample;
            Com<IMFMediaBuffer> contiguous;
            if (produced && SUCCEEDED(produced->ConvertToContiguousBuffer(contiguous.put()))) {
                BYTE* data = nullptr;
                DWORD len = 0;
                contiguous->Lock(&data, nullptr, &len);
                outputs.emplace_back(data, data + len);
                contiguous->Unlock();
            }
            if (provides_samples_ && produced) produced->Release();
            if (single) return {};
        }
    }

    Com<IMFTransform> mft_;
    Com<IMFMediaEventGenerator> events_;
    Com<ICodecAPI> api_;
    bool hevc_ = false;
    int width_ = 0, height_ = 0, fps_ = 30, bitrate_ = 8'000'000;
    DWORD in_id_ = 0, out_id_ = 0;
    bool provides_samples_ = false;
    bool async_ = false;
    bool hardware_ = false;
    int need_input_ = 0;
    DWORD out_size_ = 0;
    std::string name_;
};

// Simple arrow cursor drawn into the NV12 frame (Desktop Duplication does not include the pointer).
void draw_cursor(std::vector<uint8_t>& nv12, int w, int h, int cx, int cy, int scale_div) {
    static const char* kArrow[] = {
        "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X", "X........X",
        "X.....XXXX", "X..X..X", "X.X X..X", "XX  X..X", "X    X..X", "     X..X", "      XX",
    };
    const int rows = static_cast<int>(sizeof(kArrow) / sizeof(kArrow[0]));
    uint8_t* yp = nv12.data();
    uint8_t* uvp = nv12.data() + static_cast<size_t>(w) * h;
    const int px = cx / scale_div, py = cy / scale_div;
    for (int r = 0; r < rows; ++r) {
        const int y = py + r;
        if (y < 0 || y >= h) continue;
        for (int c = 0; kArrow[r][c]; ++c) {
            const char ch = kArrow[r][c];
            if (ch == ' ') continue;
            const int x = px + c;
            if (x < 0 || x >= w) continue;
            yp[static_cast<size_t>(y) * w + x] = ch == 'X' ? 16 : 235;
            uint8_t* uv = uvp + static_cast<size_t>(y / 2) * w + (x & ~1);
            uv[0] = 128;
            uv[1] = 128;
        }
    }
}

void dump_bmp(const std::wstring& path, const uint8_t* src, size_t pitch, int w, int h) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = -h; ih.biPlanes = 1; ih.biBitCount = 32;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(w) * h * 4;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n;
    WriteFile(f, &fh, sizeof(fh), &n, nullptr);
    WriteFile(f, &ih, sizeof(ih), &n, nullptr);
    for (int y = 0; y < h; ++y) WriteFile(f, src + static_cast<size_t>(y) * pitch, static_cast<DWORD>(w) * 4, &n, nullptr);
    CloseHandle(f);
}

// Turns a w x h BGRA image by quarter turns (1 = 90 degrees clockwise) into out.
void rotate_bgra(const uint8_t* src, size_t pitch, int w, int h, int turns, std::vector<uint8_t>& out) {
    const int ow = (turns % 2) ? h : w, oh = (turns % 2) ? w : h;
    out.resize(static_cast<size_t>(ow) * oh * 4);
    auto* o = reinterpret_cast<uint32_t*>(out.data());
    for (int y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(src + static_cast<size_t>(y) * pitch);
        for (int x = 0; x < w; ++x) {
            int nx, ny;
            switch (turns) {
                case 1: nx = h - 1 - y; ny = x; break;
                case 2: nx = w - 1 - x; ny = h - 1 - y; break;
                default: nx = y; ny = w - 1 - x; break;  // 3
            }
            o[static_cast<size_t>(ny) * ow + nx] = row[x];
        }
    }
}

void halve_bgra(const uint8_t* src, size_t pitch, int w, int h, std::vector<uint8_t>& out) {
    const int ow = w / 2, oh = h / 2;
    out.resize(static_cast<size_t>(ow) * oh * 4);
    for (int y = 0; y < oh; ++y) {
        const uint8_t* r0 = src + static_cast<size_t>(2 * y) * pitch;
        const uint8_t* r1 = r0 + pitch;
        uint8_t* o = out.data() + static_cast<size_t>(y) * ow * 4;
        for (int x = 0; x < ow; ++x) {
            for (int c = 0; c < 4; ++c) {
                o[x * 4 + c] = static_cast<uint8_t>((r0[x * 8 + c] + r0[x * 8 + 4 + c] + r1[x * 8 + c] + r1[x * 8 + 4 + c] + 2) / 4);
            }
        }
    }
}

// Reads the optional first line {"t":"cfg","fps":60,"bitrate":12000000,"max_width":1920} (waits up to 1 s).
// Any bytes after it stay in `buffer` for the input reader.
StreamSettings read_settings(SOCKET s, StreamSettings settings, LineBuffer& buffer, std::string& first_line) {
    DWORD timeout_ms = 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    char buf[1024];
    std::string line;
    while (!buffer.next_line(line)) {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0 || !buffer.append(buf, static_cast<size_t>(n))) { line.clear(); break; }
    }
    timeout_ms = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    const auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_object() && j.value("t", "") == "cfg") {
        settings.fps = std::clamp(j.value("fps", settings.fps), 10, 120);
        settings.hevc = j.value("codec", "h264") == "hevc";
        settings.window = j.value("window", 0LL);
        settings.bitrate = std::clamp(j.value("bitrate", settings.bitrate), 1'000'000, 50'000'000);
        settings.max_width = std::clamp(j.value("max_width", settings.max_width), 640, 8192);
        settings.extend = j.value("display", "") == "virtual";
        settings.pad = j.value("display", "") == "pad";
        settings.hardware = j.value("encoder", "auto") != "software";
        settings.width = std::clamp(j.value("width", 0), 0, 7680);
        settings.height = std::clamp(j.value("height", 0), 0, 4320);
    } else {
        first_line = line;  // not a settings line: treat it as input later
    }
    return settings;
}

}  // namespace

std::string detect_hardware_encoder() {
    MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    std::string name;
    if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                            &in_info, &out_info, &acts, &count)) && count > 0) {
        wchar_t* fname = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(acts[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fname, &len)) && fname) {
            char buf8[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, fname, -1, buf8, sizeof(buf8), nullptr, nullptr);
            name = buf8;
            CoTaskMemFree(fname);
        }
        for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    }
    if (acts) CoTaskMemFree(acts);
    return name;
}

namespace {
std::function<void(const StreamStatus&)> g_status_hook;
std::mutex g_private_mutex;
bool g_private_on = false;
std::vector<std::wstring> g_private_exes;

std::wstring exe_of(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    std::wstring name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &n)) {
            name = path;
            name = name.substr(name.find_last_of(L'\\') + 1);
            for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
        }
        CloseHandle(h);
    }
    return name;
}

// Screen rectangles of visible windows that belong to private programs.
std::vector<RECT> private_rects() {
    std::vector<RECT> out;
    std::vector<std::wstring> exes;
    {
        std::lock_guard<std::mutex> lock(g_private_mutex);
        if (!g_private_on) return out;
        exes = g_private_exes;
    }
    struct Ctx { std::vector<std::wstring>* exes; std::vector<RECT>* out; } ctx{&exes, &out};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(w) || IsIconic(w)) return TRUE;
        const std::wstring e = exe_of(w);
        for (const auto& x : *c->exes) {
            if (e == x) {
                RECT r{};
                if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(w, &r);
                c->out->push_back(r);
                break;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

// Paints a dark block over a rectangle of the NV12 frame.
void blank_nv12(std::vector<uint8_t>& nv12, int w, int h, int x0, int y0, int x1, int y1) {
    x0 = std::clamp(x0, 0, w) & ~1; x1 = std::clamp(x1, 0, w);
    y0 = std::clamp(y0, 0, h) & ~1; y1 = std::clamp(y1, 0, h);
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; ++y) std::memset(nv12.data() + static_cast<size_t>(y) * w + x0, 32, static_cast<size_t>(x1 - x0));
    uint8_t* uv = nv12.data() + static_cast<size_t>(w) * h;
    for (int y = y0 / 2; y < (y1 + 1) / 2; ++y) std::memset(uv + static_cast<size_t>(y) * w + x0, 128, static_cast<size_t>(x1 - x0));
}
}  // namespace

void set_private_programs(bool enabled, const std::vector<std::wstring>& exes) {
    std::lock_guard<std::mutex> lock(g_private_mutex);
    g_private_on = enabled;
    g_private_exes = exes;
}
void set_status_hook(std::function<void(const StreamStatus&)> hook) { g_status_hook = std::move(hook); }

// Drawing pad: no picture is sent; the tablet is a pen tablet for the main screen (or one window).
static void run_pad_session(SOCKET s, const StreamSettings& settings, LineBuffer& buffer, const std::string& first,
                            const std::function<void(const std::string&)>& log) {
    MonitorRect area;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    area.left = mi.rcMonitor.left; area.top = mi.rcMonitor.top;
    area.width = mi.rcMonitor.right - mi.rcMonitor.left; area.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
    HWND w = settings.window ? reinterpret_cast<HWND>(static_cast<intptr_t>(settings.window)) : nullptr;
    if (w && IsWindow(w)) {
        RECT r{};
        if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(w, &r);
        area.left = r.left; area.top = r.top; area.width = r.right - r.left; area.height = r.bottom - r.top;
    }
    std::vector<uint8_t> header = {'S', 'L', 'V', '1'};
    put_u32(header, static_cast<uint32_t>(area.width));
    put_u32(header, static_cast<uint32_t>(area.height));
    put_u32(header, 0);
    if (!send_all(s, header.data(), header.size())) { closesocket(s); return; }
    log("Drawing pad session: " + std::to_string(area.width) + "x" + std::to_string(area.height));
    std::set<int> held;
    uint64_t events = 0;
    auto handle = [&](const std::string& l) {
        auto ev = parse_input(l);
        if (!ev) return;
        if (ev->kind == InputEvent::Kind::KeyDown) held.insert(ev->keys[0]);
        if (ev->kind == InputEvent::Kind::KeyUp) held.erase(ev->keys[0]);
        inject(*ev, area);
        ++events;
    };
    std::string line;
    if (!first.empty()) handle(first);
    while (buffer.next_line(line)) handle(line);
    char buf[4096];
    for (;;) {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0 || !buffer.append(buf, static_cast<size_t>(n))) break;
        while (buffer.next_line(line)) handle(line);
    }
    for (int vk : held) { InputEvent up; up.kind = InputEvent::Kind::KeyUp; up.keys = {vk}; inject(up, area); }
    closesocket(s);
    log("Drawing pad ended; " + std::to_string(events) + " events");
}

void run_video_session(SOCKET s, const StreamSettings& defaults, const std::function<void(const std::string&)>& log,
                       const std::function<void(const StreamSettings&)>& on_start) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const BOOL no_delay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
    LineBuffer in_buffer;
    std::string pending_input;
    const StreamSettings settings = read_settings(s, defaults, in_buffer, pending_input);
    if (on_start) on_start(settings);
    if (settings.pad) {
        run_pad_session(s, settings, in_buffer, pending_input, log);
        CoUninitialize();
        return;
    }

    std::wstring gdi;
    std::string err;
    if (settings.extend) {
        VirtualDisplayInfo vd;
        err = attach_virtual_display(settings.width, settings.height, 60, vd);
        if (err.empty()) {
            gdi = vd.gdi_name;
            log("Virtual display attached: " + to_utf8(gdi));
            restore_windows(gdi);
        }
    }
    if (!settings.extend && settings.window) {
        // Capture the screen the chosen window is on.
        HWND w = reinterpret_cast<HWND>(static_cast<intptr_t>(settings.window));
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (IsWindow(w) && GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi)) gdi = mi.szDevice;
    }
    Capturer cap;
    if (err.empty()) {
        for (int attempt = 0; attempt < 20; ++attempt) {  // a just-attached display needs a moment
            err = cap.init(gdi);
            if (err.empty()) break;
            Sleep(150);
        }
    }
    if (err.empty()) {
        // Wait for the first desktop image so width/height are known and a frame exists.
        bool got = false;
        for (int i = 0; i < 20 && !got && err.empty(); ++i) err = cap.acquire(100, got);
    }
    // Logical (as seen on screen) size, after the screen's own rotation.
    const int turns = err.empty() ? cap.quarter_turns() : 0;
    const int lw = (turns % 2) ? cap.height() : cap.width();
    const int lh = (turns % 2) ? cap.width() : cap.height();
    // One window only: crop to it (its size is fixed for the session; it may move).
    HWND window = settings.window ? reinterpret_cast<HWND>(static_cast<intptr_t>(settings.window)) : nullptr;
    if (window && !IsWindow(window)) window = nullptr;
    auto window_rect = [&](RECT& r) {
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(window, &r);
    };
    int src_w = lw, src_h = lh;
    if (window && err.empty()) {
        RECT r{};
        window_rect(r);
        src_w = std::clamp(static_cast<int>(r.right - r.left), 64, lw);
        src_h = std::clamp(static_cast<int>(r.bottom - r.top), 64, lh);
    }
    const int div = (err.empty() && src_w > settings.max_width) ? 2 : 1;
    const int enc_w = (src_w / div) & ~1;
    const int enc_h = (src_h / div) & ~1;

    Encoder enc;
    if (err.empty() && settings.hevc && settings.hardware) {
        const std::string e = enc.init(enc_w, enc_h, settings.fps, settings.bitrate, true, true);
        if (!e.empty()) log("H.265 unavailable (" + e + "), using H.264");
        else err = "";
    }
    if (err.empty() && !enc.hevc()) err = enc.init(enc_w, enc_h, settings.fps, settings.bitrate, settings.hardware);
    if (!err.empty()) {
        log("Video session failed: " + err);
        std::vector<uint8_t> msg = {'S', 'L', 'E', 'R'};
        put_u32(msg, static_cast<uint32_t>(err.size()));
        msg.insert(msg.end(), err.begin(), err.end());
        send_all(s, msg.data(), msg.size());
        closesocket(s);
        if (settings.extend) detach_virtual_display();
        CoUninitialize();
        return;
    }
    log("Streaming " + std::to_string(cap.width()) + "x" + std::to_string(cap.height()) + " as " + std::to_string(enc_w) +
        "x" + std::to_string(enc_h) + " @" + std::to_string(settings.fps) + "fps, " +
        std::to_string(settings.bitrate / 1000) + " kbps, encoder: " + enc.name() + (enc.hevc() ? " (H.265)" : " (H.264)"));

    StreamStatus ui_status;
    ui_status.encoder = enc.name();
    ui_status.hardware = enc.hardware();
    ui_status.hevc = enc.hevc();
    ui_status.width = enc_w;
    ui_status.height = enc_h;
    ui_status.fps = settings.fps;
    if (g_status_hook) g_status_hook(ui_status);
    uint64_t ui_frames = 0, ui_bytes = 0;
    auto ui_start = std::chrono::steady_clock::now();

    // SLV2 adds the codec (0 = H.264, 1 = H.265); SLV1 stays for H.264 so older apps keep working.
    std::vector<uint8_t> header = {'S', 'L', 'V', static_cast<uint8_t>(settings.hevc ? '2' : '1')};
    put_u32(header, static_cast<uint32_t>(enc_w));
    put_u32(header, static_cast<uint32_t>(enc_h));
    put_u32(header, static_cast<uint32_t>(settings.fps));
    if (settings.hevc) put_u32(header, enc.hevc() ? 1u : 0u);
    std::atomic<int> want_bitrate{0};
    std::atomic<bool> want_keyframe{false};
    std::atomic<bool> running{send_all(s, header.data(), header.size())};

    // Input from the tablet arrives on the same socket. Its 0..1 coordinates cover the streamed area.
    const MonitorRect monitor = cap.monitor();
    std::mutex area_mutex;
    MonitorRect area = monitor;
    if (window) { area.width = src_w; area.height = src_h; }
    auto current_area = [&] { std::lock_guard<std::mutex> lock(area_mutex); return area; };
    std::atomic<uint64_t> n_pointer{0}, n_pen{0}, n_failed{0};
    std::atomic<unsigned long> last_error{0};
    std::mutex held_mutex;
    std::set<int> held_keys;  // keys held with "kd", released if the tablet goes away
    auto handle = [&](const std::string& l) {
        if (l.find("\"t\":\"br\"") != std::string::npos || l.find("\"t\":\"kf\"") != std::string::npos) {
            const auto j = nlohmann::json::parse(l, nullptr, false);
            if (j.is_object() && j.value("t", "") == "br") want_bitrate = std::clamp(j.value("bitrate", 0), 500'000, 80'000'000);
            if (j.is_object() && j.value("t", "") == "kf") want_keyframe = true;
            return;
        }
        auto ev = parse_input(l);
        if (!ev) return;
        if (ev->kind == InputEvent::Kind::KeyDown || ev->kind == InputEvent::Kind::KeyUp) {
            std::lock_guard<std::mutex> lock(held_mutex);
            if (ev->kind == InputEvent::Kind::KeyDown) held_keys.insert(ev->keys[0]); else held_keys.erase(ev->keys[0]);
        }
        const bool pen = ev->kind >= InputEvent::Kind::PenHover && ev->kind <= InputEvent::Kind::PenLeave;
        (pen ? n_pen : n_pointer)++;
        if (!inject(*ev, current_area())) {
            if (n_failed++ == 0) last_error = GetLastError();
        }
    };
    std::thread reader([&] {
        LineBuffer& buffer = in_buffer;
        std::string line;
        if (!pending_input.empty()) handle(pending_input);
        while (buffer.next_line(line)) handle(line);
        char buf[4096];
        while (running) {
            const int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (!buffer.append(buf, static_cast<size_t>(n))) break;
            while (buffer.next_line(line)) handle(line);
        }
        running = false;
    });

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    const auto frame_interval = std::chrono::microseconds(1'000'000 / settings.fps);
    auto next_frame = clock::now();
    auto last_sent = clock::time_point{};
    bool dirty = true;
    std::vector<uint8_t> nv12(static_cast<size_t>(enc_w) * enc_h * 3 / 2);
    std::vector<uint8_t> half;
    std::vector<uint8_t> rotated;
    std::vector<RECT> hidden;   // private windows, refreshed every 15 frames
    uint64_t frame_counter = 0;
    // Diagnostics: if %TEMP%\Imtidad\dump-frame exists, the first frame is saved as frame.bmp next to it.
    wchar_t tmp_dir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp_dir);
    const std::wstring dump_flag = std::wstring(tmp_dir) + L"Imtidad\\dump-frame";
    const std::wstring dump_path = std::wstring(tmp_dir) + L"Imtidad\\frame.bmp";
    const bool dump_wanted = GetFileAttributesW(dump_flag.c_str()) != INVALID_FILE_ATTRIBUTES;
    bool dump_next = false;
    int frame_no = 0;
    std::vector<std::vector<uint8_t>> outputs;
    POINT last_cursor{-1, -1};
    uint64_t frames = 0;
    uint64_t stat_frames = 0, stat_convert_us = 0, stat_encode_us = 0, stat_bytes = 0;
    auto stat_start = clock::now();

    while (running) {
        const auto now = clock::now();
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - now).count();
        bool got = false;
        err = cap.acquire(static_cast<UINT>(std::max<long long>(0, wait)), got);
        if (!err.empty()) { log("Capture error: " + err); break; }
        if (got) dirty = true;
        if (cap.take_pointer_moved()) dirty = true;
        if (clock::now() < next_frame) continue;
        next_frame += frame_interval;
        if (clock::now() > next_frame + frame_interval * 3) next_frame = clock::now() + frame_interval;  // fell behind

        POINT cursor{};
        GetCursorPos(&cursor);
        if (cursor.x != last_cursor.x || cursor.y != last_cursor.y) dirty = true;
        // Resend a frame at least every 500 ms so a static screen still reaches the decoder.
        if (!dirty && clock::now() - last_sent < std::chrono::milliseconds(500)) continue;

        if (const int br = want_bitrate.exchange(0)) {
            const bool ok = enc.set_bitrate(br);
            log("Bitrate -> " + std::to_string(br / 1000) + " kbps" + (ok ? "" : " (encoder refused)"));
        }
        if (want_keyframe.exchange(false)) enc.force_keyframe();

        const auto t0 = clock::now();
        const uint8_t* data = nullptr;
        size_t pitch = 0;
        if (!cap.map(data, pitch)) continue;
        const uint8_t* src = data;
        size_t src_pitch = pitch;
        bool mapped = true;
        if (turns) {
            rotate_bgra(data, pitch, cap.width(), cap.height(), turns, rotated);
            cap.unmap();
            mapped = false;
            src = rotated.data();
            src_pitch = static_cast<size_t>(lw) * 4;
        }
        // Single window: follow it if it moves (crop stays the session's size, kept on screen).
        int crop_x = 0, crop_y = 0;
        if (window) {
            RECT r{};
            window_rect(r);
            crop_x = std::clamp(static_cast<int>(r.left) - monitor.left, 0, lw - src_w);
            crop_y = std::clamp(static_cast<int>(r.top) - monitor.top, 0, lh - src_h);
            src += static_cast<size_t>(crop_y) * src_pitch + static_cast<size_t>(crop_x) * 4;
            std::lock_guard<std::mutex> lock(area_mutex);
            area.left = monitor.left + crop_x;
            area.top = monitor.top + crop_y;
        }
        if (dump_wanted && ++frame_no == 20) dump_next = true;  // a settled frame, not the very first
        if (dump_next) {
            dump_next = false;
            dump_bmp(dump_path, src, src_pitch, src_w, src_h);
            log("Debug frame written");
        }
        if (div == 2) {
            halve_bgra(src, src_pitch, src_w, src_h, half);
            if (mapped) { cap.unmap(); mapped = false; }
            bgra_to_nv12(half.data(), static_cast<size_t>(src_w / 2) * 4, enc_w, enc_h, nv12.data());
        } else {
            bgra_to_nv12(src, src_pitch, enc_w, enc_h, nv12.data());
            if (mapped) { cap.unmap(); mapped = false; }
        }
        if (frame_counter++ % 15 == 0) hidden = private_rects();
        for (const RECT& r : hidden) {
            blank_nv12(nv12, enc_w, enc_h, (r.left - monitor.left - crop_x) / div, (r.top - monitor.top - crop_y) / div,
                       (r.right - monitor.left - crop_x) / div, (r.bottom - monitor.top - crop_y) / div);
        }
        draw_cursor(nv12, enc_w, enc_h, cursor.x - monitor.left - crop_x, cursor.y - monitor.top - crop_y, div);
        last_cursor = cursor;
        dirty = false;

        const int64_t pts_us = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - start).count();
        const auto t1 = clock::now();
        outputs.clear();
        err = enc.encode(nv12, pts_us * 10, outputs);
        if (!err.empty() && enc.hardware() && !enc.hevc()) {
            log("Hardware encoder failed (" + err + "), switching to software");
            err = enc.fall_back_to_software();
            if (err.empty()) err = enc.encode(nv12, pts_us * 10, outputs);
        }
        if (!err.empty()) { log("Encode error: " + err); break; }
        const auto t2 = clock::now();
        stat_convert_us += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        stat_encode_us += std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
        ++stat_frames;
        ++ui_frames;
        for (const auto& o : outputs) ui_bytes += o.size();
        if (clock::now() - ui_start >= std::chrono::seconds(2)) {
            const double secs = std::chrono::duration<double>(clock::now() - ui_start).count();
            ui_status.fps = ui_frames / secs;
            ui_status.mbps = ui_bytes * 8 / 1e6 / secs;
            ui_status.encoder = enc.name();
            ui_status.hardware = enc.hardware();
            if (g_status_hook) g_status_hook(ui_status);
            ui_frames = 0; ui_bytes = 0; ui_start = clock::now();
        }
        if (clock::now() - stat_start >= std::chrono::seconds(10)) {
            const double secs = std::chrono::duration<double>(clock::now() - stat_start).count();
            char line[200];
            std::snprintf(line, sizeof(line), "Stats: %.1f fps, capture+convert %.1f ms, encode %.1f ms, %.2f Mbps (%s)",
                          stat_frames / secs, stat_convert_us / 1000.0 / std::max<uint64_t>(1, stat_frames),
                          stat_encode_us / 1000.0 / std::max<uint64_t>(1, stat_frames), stat_bytes * 8 / 1e6 / secs,
                          enc.name().c_str());
            log(line);
            stat_frames = 0; stat_convert_us = 0; stat_encode_us = 0; stat_bytes = 0;
            stat_start = clock::now();
        }
        for (const auto& o : outputs) {
            std::vector<uint8_t> packet;
            packet.reserve(o.size() + 12);
            put_u32(packet, static_cast<uint32_t>(o.size()));
            put_u64(packet, static_cast<uint64_t>(pts_us));
            packet.insert(packet.end(), o.begin(), o.end());
            if (!send_all(s, packet.data(), packet.size())) { running = false; break; }
            ++frames;
            stat_bytes += o.size();
        }
        last_sent = clock::now();
    }

    running = false;
    shutdown(s, SD_BOTH);
    closesocket(s);
    reader.join();
    {
        std::lock_guard<std::mutex> lock(held_mutex);
        for (int vk : held_keys) {
            InputEvent up;
            up.kind = InputEvent::Kind::KeyUp;
            up.keys = {vk};
            inject(up, current_area());
        }
    }
    log("Video session ended after " + std::to_string(frames) + " frames; input: " + std::to_string(n_pointer) +
        " touch/keys, " + std::to_string(n_pen) + " pen, " + std::to_string(n_failed) + " refused" +
        (n_failed ? " (error " + std::to_string(last_error) + ")" : std::string()));
    if (settings.extend) {
        remember_windows(gdi);
        const std::string derr = detach_virtual_display();
        log(derr.empty() ? "Virtual display detached" : "Detach failed: " + derr);
    }
    CoUninitialize();
}

}  // namespace screenlink
