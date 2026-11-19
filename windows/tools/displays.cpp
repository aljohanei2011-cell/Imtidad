// Lists display paths (active and inactive) with monitor names: helps identify the virtual display.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <vector>

int main() {
    SetProcessDPIAware();
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &np, &nm) != ERROR_SUCCESS) return 1;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return 1;
    for (UINT32 i = 0; i < np; ++i) {
        const auto& p = paths[i];
        if (!p.targetInfo.targetAvailable) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof(tn);
        tn.header.adapterId = p.targetInfo.adapterId;
        tn.header.id = p.targetInfo.id;
        DisplayConfigGetDeviceInfo(&tn.header);
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = p.sourceInfo.adapterId;
        sn.header.id = p.sourceInfo.id;
        DisplayConfigGetDeviceInfo(&sn.header);
        wprintf(L"path %u active=%d source=%ls target=%u name='%ls' device='%ls' tech=%d\n", i,
                (p.flags & DISPLAYCONFIG_PATH_ACTIVE) ? 1 : 0, sn.viewGdiDeviceName, p.targetInfo.id,
                tn.monitorFriendlyDeviceName, tn.monitorDevicePath, static_cast<int>(tn.outputTechnology));
    }
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        wprintf(L"adapter %ls '%ls' flags=0x%lx\n", dd.DeviceName, dd.DeviceString, dd.StateFlags);
        dd.cb = sizeof(dd);
    }
    return 0;
}
