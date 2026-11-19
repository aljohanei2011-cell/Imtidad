// Virtual display (Virtual Display Driver by MTT) control: find, attach, set mode, detach.
// Uses the Windows display configuration API, which does not need administrator rights.
#pragma once

#include <string>

namespace screenlink {

struct VirtualDisplayInfo {
    bool found = false;        // driver present (a target named "VDD by MTT")
    bool active = false;       // attached to the desktop
    std::wstring gdi_name;     // e.g. \\.\DISPLAY6 when active
};

VirtualDisplayInfo find_virtual_display();

// Where the second screen sits next to the main one (chosen in the settings window).
enum class Side { Right = 0, Left = 1, Top = 2, Bottom = 3 };
void set_virtual_side(Side side);
Side virtual_side();
std::string last_place_error();  // empty if the last placement worked

// Attaches the virtual display in "extend" mode and sets its resolution (0 = keep).
// A taller-than-wide request (tablet held upright) uses the landscape mode turned 90 degrees.
// Returns an error message, or empty on success; `info` receives the active display.
std::string attach_virtual_display(int width, int height, int refresh_hz, VirtualDisplayInfo& info);

// Detaches the virtual display so windows cannot get lost on a screen nobody sees.
std::string detach_virtual_display();

// Windows that were on the second screen are put back there the next time it is attached.
void remember_windows(const std::wstring& gdi_name);
void restore_windows(const std::wstring& gdi_name);

std::string to_utf8(const std::wstring& w);

}  // namespace screenlink
