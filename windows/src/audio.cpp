#include "audio.hpp"

#include <ws2tcpip.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace screenlink {
namespace {

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** put() { return &p; }
    T* operator->() const { return p; }
};

bool send_all(SOCKET s, const void* data, size_t size) {
    const char* c = static_cast<const char*>(data);
    while (size > 0) {
        const int n = send(s, c, static_cast<int>(size), 0);
        if (n <= 0) return false;
        c += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (i * 8)));
}

int16_t to_i16(float f) {
    f = std::clamp(f, -1.0f, 1.0f);
    return static_cast<int16_t>(f * 32767.0f);
}

}  // namespace

void run_audio_session(SOCKET s, const std::function<void(const std::string&)>& log) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const BOOL no_delay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));

    std::string err;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* fmt = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(enumerator.put()));
    if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.put()));
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&fmt);
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1'000'000, 0, fmt, nullptr);
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture.put()));
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "audio capture failed (0x%08lX)", static_cast<unsigned long>(hr));
        log(buf);
        if (fmt) CoTaskMemFree(fmt);
        closesocket(s);
        CoUninitialize();
        return;
    }

    bool is_float = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        is_float = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    const int channels = fmt->nChannels;
    const int bits = fmt->wBitsPerSample;
    const uint32_t rate = fmt->nSamplesPerSec;
    log("Audio: streaming " + std::to_string(rate) + " Hz, " + std::to_string(channels) + " ch, " +
        (is_float ? "float" : std::to_string(bits) + "-bit") + " -> PCM16 stereo");

    std::vector<uint8_t> header = {'S', 'L', 'A', '1'};
    put_u32(header, rate);
    put_u32(header, 2);
    put_u32(header, 16);
    bool ok = send_all(s, header.data(), header.size());

    // Detect the client closing the connection while the PC is silent.
    u_long nonblocking = 1;
    std::vector<int16_t> pcm;
    std::vector<uint8_t> packet;
    const size_t chunk_frames = rate / 100;  // 10 ms
    uint64_t sent = 0;
    while (ok) {
        Sleep(5);
        char probe;
        ioctlsocket(s, FIONBIO, &nonblocking);
        const int r = recv(s, &probe, 1, 0);
        if (r == 0 || (r < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) break;

        UINT32 packet_frames = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet_frames)) && packet_frames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            for (UINT32 f = 0; f < frames; ++f) {
                for (int c = 0; c < 2; ++c) {
                    const int src_c = std::min(c, channels - 1);
                    int16_t v = 0;
                    if (!silent) {
                        const BYTE* sample = data + (static_cast<size_t>(f) * channels + src_c) * (bits / 8);
                        if (is_float && bits == 32) { float x; std::memcpy(&x, sample, 4); v = to_i16(x); }
                        else if (bits == 16) { std::memcpy(&v, sample, 2); }
                        else if (bits == 24) { v = static_cast<int16_t>(sample[1] | (sample[2] << 8)); }
                        else if (bits == 32) { int32_t x; std::memcpy(&x, sample, 4); v = static_cast<int16_t>(x >> 16); }
                    }
                    pcm.push_back(v);
                }
            }
            capture->ReleaseBuffer(frames);
        }
        while (pcm.size() >= chunk_frames * 2) {
            const size_t n = chunk_frames * 2;
            packet.clear();
            put_u32(packet, static_cast<uint32_t>(n * 2));
            const auto* bytes = reinterpret_cast<const uint8_t*>(pcm.data());
            packet.insert(packet.end(), bytes, bytes + n * 2);
            pcm.erase(pcm.begin(), pcm.begin() + static_cast<long>(n));
            u_long blocking = 0;
            ioctlsocket(s, FIONBIO, &blocking);
            if (!send_all(s, packet.data(), packet.size())) { ok = false; break; }
            sent += n / 2;
        }
    }
    client->Stop();
    CoTaskMemFree(fmt);
    closesocket(s);
    log("Audio session ended (" + std::to_string(sent / std::max<uint32_t>(rate, 1)) + " s sent)");
    CoUninitialize();
}

}  // namespace screenlink
