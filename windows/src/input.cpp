#include "input.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

#ifdef _WIN32
// Pen injection needs the Windows 10 1809 (RS5) declarations; the functions are loaded at runtime.
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#undef WDK_NTDDI_VERSION
#define WDK_NTDDI_VERSION 0x0A000006
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace screenlink {

using nlohmann::json;

std::wstring utf8_to_wide(const std::string& s) {
    std::wstring out;
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned int c = static_cast<unsigned char>(s[i]);
        unsigned int cp = 0;
        int extra = 0;
        if (c < 0x80) { cp = c; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; extra = 1; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; extra = 2; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; extra = 3; }
        else { ++i; continue; }
        for (int k = 1; k <= extra; ++k) {
            if (i + k >= s.size()) return out;
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        i += 1 + extra;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
    }
    return out;
}

static double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

std::optional<InputEvent> parse_input(const std::string& line) {
    const json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    const std::string t = j.value("t", "");
    InputEvent e;
    if (t == "p") {
        const std::string a = j.value("a", "");
        if (a == "d") e.kind = InputEvent::Kind::PointerDown;
        else if (a == "m") e.kind = InputEvent::Kind::PointerMove;
        else if (a == "u") e.kind = InputEvent::Kind::PointerUp;
        else if (a == "r") e.kind = InputEvent::Kind::RightClick;
        else return std::nullopt;
        e.x = clamp01(j.value("x", 0.0));
        e.y = clamp01(j.value("y", 0.0));
        return e;
    }
    if (t == "pen") {
        const std::string a = j.value("a", "");
        if (a == "h") e.kind = InputEvent::Kind::PenHover;
        else if (a == "d") e.kind = InputEvent::Kind::PenDown;
        else if (a == "m") e.kind = InputEvent::Kind::PenMove;
        else if (a == "u") e.kind = InputEvent::Kind::PenUp;
        else if (a == "x") e.kind = InputEvent::Kind::PenLeave;
        else return std::nullopt;
        e.x = clamp01(j.value("x", 0.0));
        e.y = clamp01(j.value("y", 0.0));
        e.pressure = std::clamp(j.value("p", 0.0), 0.0, 1.0);
        e.tilt_x = std::clamp(j.value("tx", 0), -90, 90);
        e.tilt_y = std::clamp(j.value("ty", 0), -90, 90);
        e.eraser = j.value("e", 0) != 0;
        e.barrel = j.value("b", 0) != 0;
        return e;
    }
    if (t == "s") {
        e.kind = InputEvent::Kind::Scroll;
        e.x = clamp01(j.value("x", 0.5));
        e.y = clamp01(j.value("y", 0.5));
        e.wheel = std::clamp(j.value("dy", 0), -1200, 1200);
        return e.wheel == 0 ? std::nullopt : std::optional<InputEvent>(e);
    }
    if (t == "k") {
        e.kind = InputEvent::Kind::Text;
        e.text = utf8_to_wide(j.value("text", ""));
        if (e.text.empty() || e.text.size() > 1000) return std::nullopt;
        return e;
    }
    if (t == "v") {
        e.kind = InputEvent::Kind::Key;
        const int vk = j.value("vk", 0);
        if (vk <= 0 || vk > 0xFE) return std::nullopt;
        e.keys = {vk};
        return e;
    }
    if (t == "rel") {
        e.kind = InputEvent::Kind::RelMove;
        e.dx = std::clamp(j.value("dx", 0), -4000, 4000);
        e.dy = std::clamp(j.value("dy", 0), -4000, 4000);
        return (e.dx == 0 && e.dy == 0) ? std::nullopt : std::optional<InputEvent>(e);
    }
    if (t == "btn") {
        e.kind = InputEvent::Kind::Button;
        const std::string b = j.value("b", "l"), a = j.value("a", "c");
        if (b != "l" && b != "r" && b != "m") return std::nullopt;
        if (a != "d" && a != "u" && a != "c") return std::nullopt;
        e.button = b[0];
        e.button_action = a[0];
        return e;
    }
    if (t == "kd" || t == "ku") {
        e.kind = t == "kd" ? InputEvent::Kind::KeyDown : InputEvent::Kind::KeyUp;
        const int vk = j.value("vk", 0);
        if (vk <= 0 || vk > 0xFE) return std::nullopt;
        e.keys = {vk};
        return e;
    }
    if (t == "zoom") {
        e.kind = InputEvent::Kind::Zoom;
        e.x = clamp01(j.value("x", 0.5));
        e.y = clamp01(j.value("y", 0.5));
        e.wheel = std::clamp(j.value("dy", 0), -1200, 1200);
        return e.wheel == 0 ? std::nullopt : std::optional<InputEvent>(e);
    }
    if (t == "c") {
        e.kind = InputEvent::Kind::Combo;
        if (!j.contains("keys") || !j["keys"].is_array()) return std::nullopt;
        for (const auto& k : j["keys"]) {
            if (!k.is_number_integer()) return std::nullopt;
            const int vk = k.get<int>();
            if (vk <= 0 || vk > 0xFE) return std::nullopt;
            e.keys.push_back(vk);
        }
        if (e.keys.empty() || e.keys.size() > 4) return std::nullopt;
        return e;
    }
    return std::nullopt;
}

#ifdef _WIN32

static INPUT mouse_at(const InputEvent& e, const MonitorRect& m, DWORD flags) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    const int vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    const double px = m.left + e.x * (m.width - 1);
    const double py = m.top + e.y * (m.height - 1);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = static_cast<LONG>((px - vx) * 65535.0 / vw + 0.5);
    in.mi.dy = static_cast<LONG>((py - vy) * 65535.0 / vh + 0.5);
    in.mi.dwFlags = flags | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return in;
}

static INPUT key(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    switch (vk) {  // keys that need the extended flag
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_INSERT: case VK_DELETE: case VK_LWIN: case VK_RWIN:
        case VK_VOLUME_MUTE: case VK_VOLUME_DOWN: case VK_VOLUME_UP: case VK_MEDIA_NEXT_TRACK:
        case VK_MEDIA_PREV_TRACK: case VK_MEDIA_STOP: case VK_MEDIA_PLAY_PAUSE:
            in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            break;
        default: break;
    }
    return in;
}

// Windows pen injection (Windows 10 1809+), loaded at runtime so older systems still start.
static bool inject_pen(const InputEvent& e, const MonitorRect& m) {
    using CreateFn = HSYNTHETICPOINTERDEVICE(WINAPI*)(POINTER_INPUT_TYPE, ULONG, POINTER_FEEDBACK_MODE);
    using InjectFn = BOOL(WINAPI*)(HSYNTHETICPOINTERDEVICE, const POINTER_TYPE_INFO*, UINT32);
    static HMODULE user32 = GetModuleHandleW(L"user32.dll");
    static auto create = reinterpret_cast<CreateFn>(GetProcAddress(user32, "CreateSyntheticPointerDevice"));
    static auto injectfn = reinterpret_cast<InjectFn>(GetProcAddress(user32, "InjectSyntheticPointerInput"));
    static HSYNTHETICPOINTERDEVICE device = create ? create(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT) : nullptr;
    if (!device || !injectfn) return false;

    POINTER_TYPE_INFO info{};
    info.type = PT_PEN;
    POINTER_PEN_INFO& pen = info.penInfo;
    pen.pointerInfo.pointerType = PT_PEN;
    pen.pointerInfo.pointerId = 1;
    pen.pointerInfo.ptPixelLocation.x = static_cast<LONG>(m.left + e.x * (m.width - 1) + 0.5);
    pen.pointerInfo.ptPixelLocation.y = static_cast<LONG>(m.top + e.y * (m.height - 1) + 0.5);
    pen.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
    pen.tiltX = e.tilt_x;
    pen.tiltY = e.tilt_y;
    pen.penFlags = (e.barrel ? PEN_FLAG_BARREL : 0) | (e.eraser ? (PEN_FLAG_ERASER | PEN_FLAG_INVERTED) : 0);
    POINTER_FLAGS flags = 0;
    switch (e.kind) {
        case InputEvent::Kind::PenHover:
            flags = POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE;
            break;
        case InputEvent::Kind::PenDown:
            flags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN | POINTER_FLAG_FIRSTBUTTON;
            pen.pointerInfo.ButtonChangeType = POINTER_CHANGE_FIRSTBUTTON_DOWN;
            break;
        case InputEvent::Kind::PenMove:
            flags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE | POINTER_FLAG_FIRSTBUTTON;
            break;
        case InputEvent::Kind::PenUp:
            flags = POINTER_FLAG_INRANGE | POINTER_FLAG_UP;
            pen.pointerInfo.ButtonChangeType = POINTER_CHANGE_FIRSTBUTTON_UP;
            break;
        default:  // PenLeave
            flags = POINTER_FLAG_UPDATE;
            break;
    }
    pen.pointerInfo.pointerFlags = flags;
    const bool contact = e.kind == InputEvent::Kind::PenDown || e.kind == InputEvent::Kind::PenMove;
    pen.pressure = contact ? static_cast<UINT32>(std::max(1.0, e.pressure * 1024.0)) : 0;
    return injectfn(device, &info, 1) != FALSE;
}

bool inject(const InputEvent& e, const MonitorRect& m) {
    std::vector<INPUT> ins;
    switch (e.kind) {
        case InputEvent::Kind::PointerDown: ins.push_back(mouse_at(e, m, MOUSEEVENTF_LEFTDOWN)); break;
        case InputEvent::Kind::PointerMove: ins.push_back(mouse_at(e, m, 0)); break;
        case InputEvent::Kind::PointerUp: ins.push_back(mouse_at(e, m, MOUSEEVENTF_LEFTUP)); break;
        case InputEvent::Kind::RightClick:
            ins.push_back(mouse_at(e, m, MOUSEEVENTF_RIGHTDOWN));
            ins.push_back(mouse_at(e, m, MOUSEEVENTF_RIGHTUP));
            break;
        case InputEvent::Kind::Scroll: {
            ins.push_back(mouse_at(e, m, 0));
            INPUT w{};
            w.type = INPUT_MOUSE;
            w.mi.dwFlags = MOUSEEVENTF_WHEEL;
            w.mi.mouseData = static_cast<DWORD>(e.wheel);
            ins.push_back(w);
            break;
        }
        case InputEvent::Kind::Text:
            for (wchar_t ch : e.text) {
                if (ch == L'\n') { ins.push_back(key(VK_RETURN, false)); ins.push_back(key(VK_RETURN, true)); continue; }
                INPUT down{};
                down.type = INPUT_KEYBOARD;
                down.ki.wScan = ch;
                down.ki.dwFlags = KEYEVENTF_UNICODE;
                INPUT up = down;
                up.ki.dwFlags |= KEYEVENTF_KEYUP;
                ins.push_back(down);
                ins.push_back(up);
            }
            break;
        case InputEvent::Kind::Key:
            ins.push_back(key(static_cast<WORD>(e.keys[0]), false));
            ins.push_back(key(static_cast<WORD>(e.keys[0]), true));
            break;
        case InputEvent::Kind::Combo:
            for (int vk : e.keys) ins.push_back(key(static_cast<WORD>(vk), false));
            for (auto it = e.keys.rbegin(); it != e.keys.rend(); ++it) ins.push_back(key(static_cast<WORD>(*it), true));
            break;
        case InputEvent::Kind::RelMove: {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dx = e.dx;
            in.mi.dy = e.dy;
            in.mi.dwFlags = MOUSEEVENTF_MOVE;
            ins.push_back(in);
            break;
        }
        case InputEvent::Kind::Button: {
            DWORD down = MOUSEEVENTF_LEFTDOWN, up = MOUSEEVENTF_LEFTUP;
            if (e.button == 'r') { down = MOUSEEVENTF_RIGHTDOWN; up = MOUSEEVENTF_RIGHTUP; }
            if (e.button == 'm') { down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; }
            INPUT in{};
            in.type = INPUT_MOUSE;
            if (e.button_action != 'u') { in.mi.dwFlags = down; ins.push_back(in); }
            if (e.button_action != 'd') { in.mi.dwFlags = up; ins.push_back(in); }
            break;
        }
        case InputEvent::Kind::KeyDown:
            ins.push_back(key(static_cast<WORD>(e.keys[0]), false));
            break;
        case InputEvent::Kind::KeyUp:
            ins.push_back(key(static_cast<WORD>(e.keys[0]), true));
            break;
        case InputEvent::Kind::Zoom: {
            ins.push_back(mouse_at(e, m, 0));
            ins.push_back(key(VK_CONTROL, false));
            INPUT w{};
            w.type = INPUT_MOUSE;
            w.mi.dwFlags = MOUSEEVENTF_WHEEL;
            w.mi.mouseData = static_cast<DWORD>(e.wheel);
            ins.push_back(w);
            ins.push_back(key(VK_CONTROL, true));
            break;
        }
        case InputEvent::Kind::PenHover:
        case InputEvent::Kind::PenDown:
        case InputEvent::Kind::PenMove:
        case InputEvent::Kind::PenUp:
        case InputEvent::Kind::PenLeave:
            if (inject_pen(e, m)) return true;
            {
                // No pen injection on this Windows: behave like a mouse.
                InputEvent mouse = e;
                if (e.kind == InputEvent::Kind::PenDown) ins.push_back(mouse_at(mouse, m, MOUSEEVENTF_LEFTDOWN));
                else if (e.kind == InputEvent::Kind::PenUp) ins.push_back(mouse_at(mouse, m, MOUSEEVENTF_LEFTUP));
                else if (e.kind != InputEvent::Kind::PenLeave) ins.push_back(mouse_at(mouse, m, 0));
            }
            break;
    }
    if (ins.empty()) return true;
    return SendInput(static_cast<UINT>(ins.size()), ins.data(), sizeof(INPUT)) == ins.size();
}

#endif

}  // namespace screenlink
