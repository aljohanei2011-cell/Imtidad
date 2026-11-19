// Video streaming session: Desktop Duplication -> NV12 -> Media Foundation H.264 -> TCP.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

#include <functional>
#include <string>
#include <vector>

namespace screenlink {

constexpr unsigned short kVideoPort = 48766;

struct StreamSettings {
    int fps = 30;
    int bitrate = 8'000'000;   // bits per second
    int max_width = 2560;      // wider desktops are halved
    bool extend = false;       // stream the virtual display (second screen) instead of the main one
    int width = 0, height = 0; // requested virtual display resolution (extend mode)
    bool hardware = true;      // try the GPU encoder first
    bool pad = false;          // drawing pad: pen/touch input only, no picture
    long long window = 0;      // stream one window only (HWND), 0 = whole screen
    bool hevc = false;         // client asked for H.265 (header SLV2 then tells which codec is used)
};

// Live numbers for the settings window.
struct StreamStatus {
    std::string encoder;   // e.g. "AMDh264Encoder"
    bool hardware = false;
    bool hevc = false;
    double fps = 0, mbps = 0;
    int width = 0, height = 0;
};
// Friendly name of the first GPU H.264 encoder, or empty when there is none.
std::string detect_hardware_encoder();
// Privacy: windows of these programs (exe names, lower case, e.g. "keepass.exe") are blacked out on the tablet.
void set_private_programs(bool enabled, const std::vector<std::wstring>& exes);

void set_status_hook(std::function<void(const StreamStatus&)> hook);

// Runs one streaming session on an accepted socket until the client disconnects.
// Wire format (server -> client, big-endian):
//   header: "SLV1" u32 width u32 height u32 fps
//   or error: "SLER" u32 length <UTF-8 message>, then the connection closes
//   frame:  u32 length  u64 pts_us  <length bytes of H.264 Annex-B>
// Client -> server: JSON lines with input events (see input.hpp).
// on_start (optional) is called once the session's settings are known.
void run_video_session(SOCKET s, const StreamSettings& settings, const std::function<void(const std::string&)>& log,
                       const std::function<void(const StreamSettings&)>& on_start = {});

}  // namespace screenlink
