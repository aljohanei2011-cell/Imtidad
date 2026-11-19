// Imtidad settings window and tray-menu artwork (GDI+, dark, right-to-left), following the design board.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace screenlink::ui {

struct State {
    std::vector<std::wstring> devices;   // e.g. L"Xiaomi Tablet"
    std::wstring device_link;            // L"USB" / L"Wi-Fi"
    bool streaming = false;
    double fps = 0;
    std::wstring encoder;                // e.g. L"AMD (Hardware - H.264)"
    bool audio_enabled = true;
    bool vdd_found = false;
    int vdd_width = 0, vdd_height = 0;
    bool wifi_allowed = false;
    std::wstring ip;                     // LAN IPv4 of this PC
    std::wstring os;                     // L"Windows 11"
    std::wstring pin;                    // Wi-Fi connection code
    int side = 0;                        // second screen: 0 right, 1 left, 2 top, 3 bottom
};

struct Actions {
    std::function<void()> open_app;          // devices card
    std::function<void()> toggle_audio;      // sound switch
    std::function<void()> display_settings;  // second-screen card (changes its side)
    std::function<void()> allow_wifi;        // Wi-Fi card
};

void init(HINSTANCE instance, std::function<State()> state, Actions actions);
void show_settings();
void refresh();  // repaint if open

// Icon images for menu items (line icons in the current look's colour), sized for menus. Index constants below.
enum IconId { IconDevices, IconChip, IconSound, IconMonitor, IconWifi, IconChevron, IconOpen, IconDownload,
              IconStop, IconCheck, IconLog, IconPower, IconSettings, IconMute, IconCircle };
HBITMAP menu_bitmap(IconId id);
HBITMAP app_bitmap();  // the app icon for the menu header

// Context menus for this process in the app's look, dark or light (Windows 10 1903+ / 11).
void use_dark_menus();

// Light or dark look (2.0.1): 0 follows Windows, 1 light, 2 dark. Saved in the registry.
bool dark_theme();
int theme_mode();
void set_theme_mode(int mode);

void shutdown();

// Drawing over the main screen (for explaining). action: 1 on, 0 off, 2 clear, 3 toggle. Call on the UI thread.
void annotate(int action);
bool annotating();

// Colour of the pixel under the mouse pointer, as 0xRRGGBB.
unsigned pixel_under_cursor();

// Saves the whole desktop (all screens) as a PNG file. Returns false on failure.
bool save_screenshot_png(const std::wstring& path);

}  // namespace screenlink::ui
