// Remote input from the tablet: parsing is portable, injection is Windows-only.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace screenlink {

struct InputEvent {
    enum class Kind { PointerDown, PointerMove, PointerUp, RightClick, Scroll, Text, Key, Combo,
                      PenHover, PenDown, PenMove, PenUp, PenLeave,
                      RelMove, Button, KeyDown, KeyUp, Zoom };
    Kind kind{};
    double x = 0, y = 0;       // 0..1 relative to the streamed monitor
    int wheel = 0;             // scroll: positive = up, in WHEEL_DELTA units (120 per notch)
    std::wstring text;         // Text
    std::vector<int> keys;     // Key (one VK) or Combo (VKs pressed in order, released in reverse)
    double pressure = 0;       // pen: 0..1
    int tilt_x = 0, tilt_y = 0;// pen: degrees, -90..90
    bool eraser = false;       // pen: eraser end
    bool barrel = false;       // pen: side button held
    int dx = 0, dy = 0;        // RelMove: pixels
    char button = 'l';         // Button: l / r / m
    char button_action = 'c';  // Button: d(own) / u(p) / c(lick)
};

// Parses one JSON line sent by the tablet on the video connection.
//   {"t":"p","a":"d|m|u|r","x":0.5,"y":0.5}   pointer down/move/up or right click
//   {"t":"s","x":..,"y":..,"dy":-120}         scroll
//   {"t":"k","text":"..."}                    Unicode text
//   {"t":"v","vk":13}                          one virtual key (press + release)
//   {"t":"c","keys":[17,67]}                   key combination, e.g. Ctrl+C
//   {"t":"pen","a":"h|d|m|u|x","x":..,"y":..,"p":0.5,"tx":0,"ty":0,"e":0,"b":0}
//                                              pen hover/down/move/up/leave with pressure and tilt
//   {"t":"rel","dx":5,"dy":-3}                 touchpad: move the pointer by pixels
//   {"t":"btn","b":"l|r|m","a":"d|u|c"}        touchpad: button down/up/click where the pointer is
//   {"t":"kd","vk":17} / {"t":"ku","vk":17}    hold / release a key (Ctrl, Shift, Alt, Space for drawing)
//   {"t":"zoom","x":..,"y":..,"dy":120}        pinch: Ctrl + mouse wheel at a point
std::optional<InputEvent> parse_input(const std::string& line);

// Monitor rectangle in virtual-desktop pixels, used to map 0..1 coordinates.
struct MonitorRect { int left = 0, top = 0, width = 1, height = 1; };

#ifdef _WIN32
// Returns false if Windows refused the input (GetLastError has the reason).
bool inject(const InputEvent& event, const MonitorRect& monitor);
#endif

std::wstring utf8_to_wide(const std::string& s);

}  // namespace screenlink
