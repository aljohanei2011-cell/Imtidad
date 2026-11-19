#include "mixer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>

#include <map>
#include <nlohmann/json.hpp>

namespace screenlink {
namespace {

template <class T> struct Ptr {
    T* p = nullptr;
    ~Ptr() { if (p) p->Release(); }
    T** put() { if (p) { p->Release(); p = nullptr; } return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring process_name(DWORD pid) {
    std::wstring name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &n)) {
            name = path;
            name = name.substr(name.find_last_of(L'\\') + 1);
            const size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos) name = name.substr(0, dot);
        }
        CloseHandle(h);
    }
    return name;
}

bool default_device(Ptr<IMMDevice>& dev) {
    Ptr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(en.put())))) return false;
    return SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.put()));
}

// Calls fn(pid, ISimpleAudioVolume*) for each program's sound session.
template <class Fn> void each_session(Fn fn) {
    Ptr<IMMDevice> dev;
    if (!default_device(dev)) return;
    Ptr<IAudioSessionManager2> mgr;
    if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(mgr.put())))) return;
    Ptr<IAudioSessionEnumerator> list;
    if (FAILED(mgr->GetSessionEnumerator(list.put()))) return;
    int count = 0;
    list->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        Ptr<IAudioSessionControl> ctl;
        if (FAILED(list->GetSession(i, ctl.put()))) continue;
        Ptr<IAudioSessionControl2> ctl2;
        if (FAILED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(ctl2.put())))) continue;
        if (ctl2->IsSystemSoundsSession() == S_OK) continue;
        AudioSessionState state;
        if (SUCCEEDED(ctl->GetState(&state)) && state == AudioSessionStateExpired) continue;
        DWORD pid = 0;
        ctl2->GetProcessId(&pid);
        if (pid == 0) continue;
        Ptr<ISimpleAudioVolume> vol;
        if (FAILED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(vol.put())))) continue;
        fn(pid, vol.p);
    }
}

}  // namespace

std::string volumes_json() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    nlohmann::json out = {{"type", "VOLUMES"}};
    Ptr<IMMDevice> dev;
    if (default_device(dev)) {
        Ptr<IAudioEndpointVolume> ep;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(ep.put())))) {
            float v = 0;
            BOOL m = FALSE;
            ep->GetMasterVolumeLevelScalar(&v);
            ep->GetMute(&m);
            out["master"] = {{"v", v}, {"m", m != FALSE}};
        }
    }
    nlohmann::json apps = nlohmann::json::array();
    std::map<DWORD, bool> seen;
    each_session([&](DWORD pid, ISimpleAudioVolume* vol) {
        if (seen[pid]) return;
        seen[pid] = true;
        float v = 0;
        BOOL m = FALSE;
        vol->GetMasterVolume(&v);
        vol->GetMute(&m);
        const std::wstring name = process_name(pid);
        if (name.empty()) return;
        apps.push_back({{"pid", pid}, {"name", utf8(name)}, {"v", v}, {"m", m != FALSE}});
    });
    out["apps"] = apps;
    CoUninitialize();
    return out.dump();
}

bool set_volume(unsigned long pid, float v, int mute) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    if (pid == 0) {
        Ptr<IMMDevice> dev;
        Ptr<IAudioEndpointVolume> ep;
        if (default_device(dev) &&
            SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(ep.put())))) {
            if (v >= 0) ep->SetMasterVolumeLevelScalar(std::min(1.f, v), nullptr);
            if (mute >= 0) ep->SetMute(mute ? TRUE : FALSE, nullptr);
            ok = true;
        }
    } else {
        each_session([&](DWORD p, ISimpleAudioVolume* vol) {
            if (p != pid) return;
            if (v >= 0) vol->SetMasterVolume(std::min(1.f, v), nullptr);
            if (mute >= 0) vol->SetMute(mute ? TRUE : FALSE, nullptr);
            ok = true;
        });
    }
    CoUninitialize();
    return ok;
}

}  // namespace screenlink
