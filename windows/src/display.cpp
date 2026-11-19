#include "display.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwchar>
#include <map>
#include <vector>

namespace screenlink {
namespace {

struct Config {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

bool query(UINT32 flags, Config& c) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(flags, &np, &nm) != ERROR_SUCCESS) return false;
        c.paths.resize(np);
        c.modes.resize(nm);
        const LONG r = QueryDisplayConfig(flags, &np, c.paths.data(), &nm, c.modes.data(), nullptr);
        if (r == ERROR_INSUFFICIENT_BUFFER) continue;
        if (r != ERROR_SUCCESS) return false;
        c.paths.resize(np);
        c.modes.resize(nm);
        return true;
    }
    return false;
}

bool is_virtual_target(const DISPLAYCONFIG_PATH_INFO& p) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
    tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tn.header.size = sizeof(tn);
    tn.header.adapterId = p.targetInfo.adapterId;
    tn.header.id = p.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) return false;
    return std::wcsstr(tn.monitorFriendlyDeviceName, L"VDD") != nullptr ||
           std::wcsstr(tn.monitorDevicePath, L"MTT1337") != nullptr;
}

std::wstring source_name(const DISPLAYCONFIG_PATH_INFO& p) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
    sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    sn.header.size = sizeof(sn);
    sn.header.adapterId = p.sourceInfo.adapterId;
    sn.header.id = p.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) return {};
    return sn.viewGdiDeviceName;
}

std::string err_text(const char* what, LONG code) {
    return std::string(what) + " failed (" + std::to_string(code) + ")";
}

}  // namespace

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

VirtualDisplayInfo find_virtual_display() {
    VirtualDisplayInfo info;
    Config c;
    if (!query(QDC_ALL_PATHS, c)) return info;
    for (const auto& p : c.paths) {
        if (!p.targetInfo.targetAvailable || !is_virtual_target(p)) continue;
        info.found = true;
        if (p.flags & DISPLAYCONFIG_PATH_ACTIVE) {
            info.active = true;
            info.gdi_name = source_name(p);
            break;
        }
    }
    return info;
}

struct Remembered { HWND hwnd; RECT rel; bool maximized; };
static std::vector<Remembered> g_remembered;

static bool monitor_rect(const std::wstring& gdi, RECT& out) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(gdi.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0)) return false;
    out = RECT{dm.dmPosition.x, dm.dmPosition.y, dm.dmPosition.x + static_cast<LONG>(dm.dmPelsWidth),
               dm.dmPosition.y + static_cast<LONG>(dm.dmPelsHeight)};
    return true;
}

void remember_windows(const std::wstring& gdi) {
    RECT mon{};
    if (!monitor_rect(gdi, mon)) return;
    g_remembered.clear();
    struct Ctx { RECT mon; std::vector<Remembered>* out; } ctx{mon, &g_remembered};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(w) || IsIconic(w) || GetWindow(w, GW_OWNER)) return TRUE;
        WINDOWPLACEMENT wp{sizeof(wp)};
        GetWindowPlacement(w, &wp);
        RECT r{};
        GetWindowRect(w, &r);
        const LONG cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        if (cx < c->mon.left || cx >= c->mon.right || cy < c->mon.top || cy >= c->mon.bottom) return TRUE;
        const RECT nr = wp.rcNormalPosition;
        c->out->push_back({w, RECT{nr.left - c->mon.left, nr.top - c->mon.top, nr.right - c->mon.left, nr.bottom - c->mon.top},
                           wp.showCmd == SW_SHOWMAXIMIZED});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

void restore_windows(const std::wstring& gdi) {
    RECT mon{};
    if (g_remembered.empty() || !monitor_rect(gdi, mon)) return;
    for (const auto& r : g_remembered) {
        if (!IsWindow(r.hwnd)) continue;
        WINDOWPLACEMENT wp{sizeof(wp)};
        GetWindowPlacement(r.hwnd, &wp);
        wp.rcNormalPosition = RECT{r.rel.left + mon.left, r.rel.top + mon.top, r.rel.right + mon.left, r.rel.bottom + mon.top};
        wp.showCmd = r.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        wp.flags = 0;
        SetWindowPlacement(r.hwnd, &wp);
    }
    g_remembered.clear();
}

static Side g_side = Side::Right;
static std::string g_last_place_error;
std::string last_place_error() { return g_last_place_error; }
void set_virtual_side(Side side) { g_side = side; }
Side virtual_side() { return g_side; }

// Puts the virtual display next to the main one and turns it upright if asked.
static std::string place(const VirtualDisplayInfo& info, bool portrait) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(info.gdi_name.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0)) return "EnumDisplaySettings failed";
    const bool upright_now = dm.dmDisplayOrientation == DMDO_90 || dm.dmDisplayOrientation == DMDO_270;
    if (portrait != upright_now) {
        std::swap(dm.dmPelsWidth, dm.dmPelsHeight);
        dm.dmDisplayOrientation = portrait ? DMDO_90 : DMDO_DEFAULT;
    }
    const int w = static_cast<int>(dm.dmPelsWidth), h = static_cast<int>(dm.dmPelsHeight);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT p = mi.rcMonitor;
    LONG x = p.right, y = p.top;
    switch (g_side) {
        case Side::Right: x = p.right; y = p.top; break;
        case Side::Left: x = p.left - w; y = p.top; break;
        case Side::Top: x = p.left + ((p.right - p.left) - w) / 2; y = p.top - h; break;
        case Side::Bottom: x = p.left + ((p.right - p.left) - w) / 2; y = p.bottom; break;
    }
    dm.dmPosition.x = x;
    dm.dmPosition.y = y;
    dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYORIENTATION;
    LONG r = ChangeDisplaySettingsExW(info.gdi_name.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) return err_text("ChangeDisplaySettingsEx(place)", r);
    r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) return err_text("ChangeDisplaySettingsEx(apply)", r);
    Sleep(300);
    return {};
}

std::string attach_virtual_display(int width, int height, int refresh_hz, VirtualDisplayInfo& info) {
    const bool portrait = height > width && width > 0;
    if (portrait) std::swap(width, height);  // the driver lists landscape modes
    info = find_virtual_display();
    if (!info.found) return "Virtual Display Driver is not installed";
    if (!info.active) {
        const LONG r = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
        if (r != ERROR_SUCCESS) return err_text("SetDisplayConfig(extend)", r);
        for (int i = 0; i < 30 && !info.active; ++i) {
            Sleep(100);
            info = find_virtual_display();
        }
        if (!info.active) return "the virtual display did not attach";
    }
    if (width > 0 && height > 0) {
        DEVMODEW current{};
        current.dmSize = sizeof(current);
        EnumDisplaySettingsW(info.gdi_name.c_str(), ENUM_CURRENT_SETTINGS, &current);
        if (static_cast<int>(current.dmPelsWidth) != width || static_cast<int>(current.dmPelsHeight) != height) {
            // Pick the supported mode closest to the request (same size, highest refresh <= requested).
            DEVMODEW best{};
            bool have = false;
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            for (DWORD i = 0; EnumDisplaySettingsW(info.gdi_name.c_str(), i, &dm); ++i) {
                if (static_cast<int>(dm.dmPelsWidth) == width && static_cast<int>(dm.dmPelsHeight) == height &&
                    (refresh_hz <= 0 || static_cast<int>(dm.dmDisplayFrequency) <= refresh_hz) &&
                    (!have || dm.dmDisplayFrequency > best.dmDisplayFrequency)) {
                    best = dm;
                    have = true;
                }
                dm.dmSize = sizeof(dm);
            }
            if (have) {
                best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
                const LONG r = ChangeDisplaySettingsExW(info.gdi_name.c_str(), &best, nullptr, CDS_UPDATEREGISTRY, nullptr);
                if (r != DISP_CHANGE_SUCCESSFUL) return err_text("ChangeDisplaySettingsEx", r);
                Sleep(300);
            }
            // No matching mode: keep the current one (the tablet letterboxes it).
        }
    }
    // Position / rotation problems are not fatal: the screen still works where Windows put it.
    g_last_place_error = place(info, portrait);
    info = find_virtual_display();
    return {};
}

std::string detach_virtual_display() {
    Config c;
    if (!query(QDC_ONLY_ACTIVE_PATHS, c)) return "QueryDisplayConfig failed";
    Config keep;
    std::map<UINT32, UINT32> remap;
    auto copy_mode = [&](UINT32 idx) -> UINT32 {
        if (idx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || idx >= c.modes.size()) return idx;
        auto it = remap.find(idx);
        if (it != remap.end()) return it->second;
        keep.modes.push_back(c.modes[idx]);
        const UINT32 n = static_cast<UINT32>(keep.modes.size() - 1);
        remap[idx] = n;
        return n;
    };
    bool removed = false;
    for (auto p : c.paths) {
        if (is_virtual_target(p)) { removed = true; continue; }
        p.sourceInfo.modeInfoIdx = copy_mode(p.sourceInfo.modeInfoIdx);
        p.targetInfo.modeInfoIdx = copy_mode(p.targetInfo.modeInfoIdx);
        keep.paths.push_back(p);
    }
    if (!removed) return {};
    if (keep.paths.empty()) return "refusing to detach the only display";
    const LONG r = SetDisplayConfig(static_cast<UINT32>(keep.paths.size()), keep.paths.data(),
                                    static_cast<UINT32>(keep.modes.size()), keep.modes.data(),
                                    SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    return r == ERROR_SUCCESS ? std::string() : err_text("SetDisplayConfig(detach)", r);
}

}  // namespace screenlink
