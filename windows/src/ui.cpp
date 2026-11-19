// Imtidad settings window (see ui.hpp). Drawn with GDI+ into an off-screen bitmap.
#include "ui.hpp"

#include <objidl.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <gdiplus.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace screenlink::ui {
namespace {

using namespace Gdiplus;

constexpr int kResIllustration = 201, kResIcons = 202, kResFontRegular = 203, kResFontBold = 204, kResAppPng = 205;
constexpr float kW = 900, kH = 430;   // client size in DIPs
constexpr int kIconCell = 64;

HINSTANCE g_instance = nullptr;
std::function<State()> g_state;
Actions g_actions;
HWND g_hwnd = nullptr;
ULONG_PTR g_gdiplus = 0;
std::unique_ptr<PrivateFontCollection> g_fonts;
std::unique_ptr<Bitmap> g_illustration, g_icons, g_app;
std::wstring g_family = L"Segoe UI";
RectF g_cards[5];
RectF g_audio_toggle;
RectF g_theme_button;

// ------------------------------------------------------------------ light / dark look (2.0.1)
// 0 follows Windows (Settings > Personalization > Colors), 1 light, 2 dark. Kept in HKCU\Software\ScreenLink\Theme.
int g_theme_mode = 0;

struct Theme {
    UINT32 ground, panel, border, soft, selected, hero, hero_muted, text, muted, accent, good;
};
const Theme kLight{0xEEF1F6, 0xFFFFFF, 0xE3E7EF, 0xF1F3F7, 0xF5F8FF, 0x0B1530, 0xC3CCE6, 0x0F1729, 0x5B6478, 0x1D64D8, 0x15803D};
const Theme kDark{0x071230, 0x0D1B45, 0x22357A, 0x13245A, 0x12286A, 0x1D64D8, 0xE3ECFF, 0xEAF0FF, 0xA9B4D6, 0x1D64D8, 0x4ADE80};

bool windows_uses_dark() {
    DWORD v = 1, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
        return v == 0;
    return false;
}

bool is_dark() { return g_theme_mode == 2 || (g_theme_mode == 0 && windows_uses_dark()); }
const Theme& theme() { return is_dark() ? kDark : kLight; }

void load_theme_mode() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ScreenLink", L"Theme", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
        g_theme_mode = static_cast<int>(v % 3);
}

void save_theme_mode() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ScreenLink", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        const DWORD v = static_cast<DWORD>(g_theme_mode);
        RegSetValueExW(key, L"Theme", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(key);
    }
}

// Title bar and window border in the colours of the current look.
void apply_window_theme(HWND hwnd) {
    if (!hwnd) return;
    const Theme& th = theme();
    BOOL dark = is_dark();
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));                 // DWMWA_USE_IMMERSIVE_DARK_MODE
    auto ref = [](UINT32 v) { return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255); };
    COLORREF caption = ref(th.ground), border = ref(th.border), title = ref(th.text);
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));           // DWMWA_CAPTION_COLOR (Windows 11)
    DwmSetWindowAttribute(hwnd, 34, &border, sizeof(border));             // DWMWA_BORDER_COLOR
    DwmSetWindowAttribute(hwnd, 36, &title, sizeof(title));               // DWMWA_TEXT_COLOR
}

Bitmap* load_png(int id) {
    HRSRC r = FindResourceW(g_instance, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    if (!r) return nullptr;
    HGLOBAL h = LoadResource(g_instance, r);
    const void* data = LockResource(h);
    const DWORD size = SizeofResource(g_instance, r);
    IStream* stream = SHCreateMemStream(static_cast<const BYTE*>(data), size);
    if (!stream) return nullptr;
    Bitmap* b = Bitmap::FromStream(stream);
    stream->Release();
    if (b && b->GetLastStatus() != Ok) { delete b; return nullptr; }
    return b;
}

void add_font(int id) {
    HRSRC r = FindResourceW(g_instance, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!r) return;
    const void* data = LockResource(LoadResource(g_instance, r));
    g_fonts->AddMemoryFont(data, static_cast<INT>(SizeofResource(g_instance, r)));
}

void load_assets() {
    if (g_gdiplus) return;
    GdiplusStartupInput in;
    GdiplusStartup(&g_gdiplus, &in, nullptr);
    g_fonts = std::make_unique<PrivateFontCollection>();
    add_font(kResFontRegular);
    add_font(kResFontBold);
    if (g_fonts->GetFamilyCount() > 0) {
        FontFamily fam;
        INT found = 0;
        g_fonts->GetFamilies(1, &fam, &found);
        WCHAR name[LF_FACESIZE] = {};
        if (found && fam.GetFamilyName(name) == Ok) g_family = name;
    }
    g_illustration.reset(load_png(kResIllustration));
    g_icons.reset(load_png(kResIcons));
    g_app.reset(load_png(kResAppPng));
}

std::unique_ptr<Font> font(float size, bool bold) {
    if (g_fonts && g_fonts->GetFamilyCount() > 0)
        return std::make_unique<Font>(g_family.c_str(), size, bold ? FontStyleBold : FontStyleRegular, UnitPixel, g_fonts.get());
    return std::make_unique<Font>(L"Segoe UI", size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
}

Color rgb(UINT32 v, BYTE a = 255) { return Color(a, (v >> 16) & 255, (v >> 8) & 255, v & 255); }

void rounded(GraphicsPath& p, RectF r, float rad) {
    const float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

void text(Graphics& g, const std::wstring& s, RectF r, float size, bool bold, Color c,
          StringAlignment align = StringAlignmentNear, StringAlignment valign = StringAlignmentNear) {
    StringFormat f;
    f.SetFormatFlags(StringFormatFlagsDirectionRightToLeft | StringFormatFlagsNoWrap);
    f.SetAlignment(align);  // Near = right edge in right-to-left text
    f.SetLineAlignment(valign);
    f.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush b(c);
    auto fnt = font(size, bold);
    g.DrawString(s.c_str(), -1, fnt.get(), r, &f, &b);
}

// The icon sheet is white line art: the matrix turns white into `tint`.
void icon(Graphics& g, IconId id, RectF r, Color tint = Color(255, 255, 255, 255), float alpha = 1.f) {
    if (!g_icons) return;
    ImageAttributes attrs;
    const REAL cr = tint.GetR() / 255.f, cg = tint.GetG() / 255.f, cb = tint.GetB() / 255.f;
    ColorMatrix m = {{{cr, 0, 0, 0, 0}, {0, cg, 0, 0, 0}, {0, 0, cb, 0, 0}, {0, 0, 0, alpha, 0}, {0, 0, 0, 0, 1}}};
    attrs.SetColorMatrix(&m);
    g.DrawImage(g_icons.get(), r, static_cast<REAL>(id * kIconCell), 0.f, static_cast<REAL>(kIconCell),
                static_cast<REAL>(kIconCell), UnitPixel, &attrs);
}

// Flat card with a thin border (new design). `hero`: the navy/blue card with white writing.
void card_back(Graphics& g, RectF r, bool hot, bool hero = false, float radius = 14) {
    const Theme& th = theme();
    GraphicsPath p;
    rounded(p, r, radius);
    if (hero) {
        SolidBrush fill(rgb(th.hero));
        g.FillPath(&fill, &p);
        if (hot) { SolidBrush lift(Color(22, 255, 255, 255)); g.FillPath(&lift, &p); }
        return;
    }
    SolidBrush fill(rgb(hot ? th.selected : th.panel));
    g.FillPath(&fill, &p);
    Pen border(rgb(hot ? th.accent : th.border), 1.f);
    g.DrawPath(&border, &p);
}

void toggle(Graphics& g, RectF r, bool on) {
    GraphicsPath p;
    rounded(p, r, r.Height / 2);
    const Theme& th = theme();
    if (on) {
        SolidBrush b(rgb(th.accent));
        g.FillPath(&b, &p);
    } else {
        SolidBrush b(rgb(th.soft));
        g.FillPath(&b, &p);
        Pen pen(rgb(th.border), 1.f);
        g.DrawPath(&pen, &p);
    }
    const float k = r.Height - 6;
    // Right-to-left switch: "on" puts the knob on the left.
    const float x = on ? r.X + 3 : r.GetRight() - 3 - k;
    SolidBrush knob(on ? Color(255, 255, 255, 255) : rgb(th.muted));
    g.FillEllipse(&knob, x, r.Y + 3, k, k);
}

POINT g_hover{-1, -1};

void paint(Graphics& g) {
    const State st = g_state ? g_state() : State{};
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    const Theme& th = theme();
    SolidBrush bg(rgb(th.ground));
    g.FillRectangle(&bg, 0.f, 0.f, kW, kH);

    // Left: a card with the picture, name and tagline, and the light/dark switch.
    const RectF brand(20, 20, 388, 390);
    card_back(g, brand, false, false, 18);
    {
        static const wchar_t* kModes[] = {L"المظهر: تلقائي", L"المظهر: فاتح", L"المظهر: داكن"};
        g_theme_button = RectF(34, 32, 132, 30);
        const bool hot = g_theme_button.Contains(static_cast<REAL>(g_hover.x), static_cast<REAL>(g_hover.y));
        GraphicsPath p;
        rounded(p, g_theme_button, 15);
        SolidBrush fill(rgb(hot ? th.selected : th.soft));
        g.FillPath(&fill, &p);
        Pen border(rgb(hot ? th.accent : th.border), 1.f);
        g.DrawPath(&border, &p);
        text(g, kModes[g_theme_mode % 3], g_theme_button, 12.5f, false, rgb(th.text), StringAlignmentCenter, StringAlignmentCenter);
    }
    if (g_illustration) g.DrawImage(g_illustration.get(), RectF(70, 62, 288, 214));
    text(g, L"امتداد", RectF(24, 280, 380, 46), 32, true, rgb(th.text), StringAlignmentCenter);
    text(g, L"شاشتك الثانية، في جيبك", RectF(24, 324, 380, 26), 16, false, rgb(th.muted), StringAlignmentCenter);
    {
        const std::wstring os = st.os.empty() ? L"Windows" : st.os;
        auto f = font(13, false);
        RectF box;
        g.MeasureString(os.c_str(), -1, f.get(), PointF(0, 0), &box);
        const float total = box.Width + 18, left = 24 + (380 - total) / 2;
        SolidBrush muted(rgb(th.muted));
        g.DrawString(os.c_str(), -1, f.get(), PointF(left, 354), &muted);
        const float x = left + box.Width + 4, y = 358;
        SolidBrush blue(rgb(th.accent));
        g.FillRectangle(&blue, x, y, 6.f, 6.f); g.FillRectangle(&blue, x + 7, y, 6.f, 6.f);
        g.FillRectangle(&blue, x, y + 7, 6.f, 6.f); g.FillRectangle(&blue, x + 7, y + 7, 6.f, 6.f);
    }
    text(g, L"تطوير عاصم الجهني  •  © 2026 جميع الحقوق محفوظة", RectF(24, 384, 380, 20), 12, false, rgb(th.muted), StringAlignmentCenter);

    // Right: status cards. The first (connected devices) is the navy/blue main card of the design.
    const float x0 = 426, x1 = 880, h = 70, gap = 9.f;
    std::wstring dev = st.devices.empty() ? L"لا يوجد جهاز متصل — صِل التابلت بالكيبل" : st.devices.front();
    if (!st.devices.empty()) {
        if (st.devices.size() > 1) dev += L" +" + std::to_wstring(st.devices.size() - 1);
        dev += L" • " + st.device_link;
        if (st.streaming && st.fps > 0) dev += L" • " + std::to_wstring(static_cast<int>(st.fps + 0.5)) + L" FPS";
    }
    struct Row { IconId icon; std::wstring title, line1, line2; int right; };  // right: 0 chevron, 1 switch
    static const wchar_t* kSides[] = {L"على يمين الشاشة الرئيسية", L"على يسار الشاشة الرئيسية", L"فوق الشاشة الرئيسية", L"تحت الشاشة الرئيسية"};
    std::wstring vdd2 = std::wstring(kSides[st.side % 4]) + L"  •  " +
                        (st.vdd_width > 0 ? std::to_wstring(st.vdd_width) + L" × " + std::to_wstring(st.vdd_height) : L"بدقة التابلت");
    const Row rows[5] = {
        {IconDevices, L"الأجهزة المتصلة", dev, L"", 0},
        {IconChip, L"ترميز الفيديو (Video Encoder)‏", st.encoder.empty() ? L"—" : L"‎" + st.encoder + L"‎", L"", 0},
        {st.audio_enabled ? IconSound : IconMute, L"الصوت", L"نقل صوت الكمبيوتر إلى التابلت", L"", 1},
        {IconMonitor, L"الشاشة الثانية", st.vdd_found ? L"جاهزة — اضغط لتغيير مكانها" : L"غير مثبتة: أعد تشغيل المثبّت واختر تعريف الشاشة",
         st.vdd_found ? vdd2 : L"", 0},
        {IconWifi, L"Wi-Fi", st.wifi_allowed ? L"مسموح بالاتصال" : L"غير مسموح — اضغط للسماح",
         (st.ip.empty() ? L"" : L"عنوان IP: " + st.ip + L"   •   ") + L"رمز الاتصال: " + st.pin, 0},
    };
    const Color white(255, 255, 255, 255);
    for (int i = 0; i < 5; ++i) {
        RectF r(x0, 20 + i * (h + gap), x1 - x0, h);
        g_cards[i] = r;
        const bool hot = r.Contains(static_cast<REAL>(g_hover.x), static_cast<REAL>(g_hover.y));
        const bool hero = i == 0;
        card_back(g, r, hot, hero);
        const Row& row = rows[i];
        const Color title = hero ? white : rgb(th.text), muted = hero ? rgb(th.hero_muted) : rgb(th.muted);
        {   // icon on a rounded square
            const RectF chip(r.GetRight() - 58, r.Y + (h - 42) / 2, 42, 42);
            GraphicsPath p;
            rounded(p, chip, 12);
            SolidBrush fill(hero ? Color(38, 255, 255, 255) : rgb(th.soft));
            g.FillPath(&fill, &p);
            icon(g, row.icon, RectF(chip.X + 9, chip.Y + 9, 24, 24), hero ? white : rgb(th.text));
        }
        if (i == 0 && !st.devices.empty()) {
            SolidBrush green(rgb(0x4ADE80));
            g.FillEllipse(&green, r.GetRight() - 24, r.Y + h / 2 + 10, 9.f, 9.f);
        }
        const float tx = r.X + 64, tw = r.Width - 64 - 72;
        const bool two = !row.line2.empty();
        const float top = r.Y + (two ? 8 : 15);
        text(g, row.title, RectF(tx, top, tw, 22), 15, true, title);
        text(g, row.line1, RectF(tx, top + 22, tw, 20), 12.5f, false, muted);
        if (two) text(g, row.line2, RectF(tx, top + 40, tw, 20), 12.5f, false, muted);
        if (row.right == 1) {
            g_audio_toggle = RectF(r.X + 18, r.Y + (h - 26) / 2, 48, 26);
            toggle(g, g_audio_toggle, st.audio_enabled);
        } else {
            icon(g, IconChevron, RectF(r.X + 20, r.Y + (h - 20) / 2, 20, 20), muted, 0.9f);
        }
    }
}

float scale_of(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    return (fn ? fn(hwnd) : 96) / 96.f;
}

// Mouse position in drawing coordinates (the window is mirrored for right-to-left).
POINT to_logical(HWND hwnd, LPARAM lp) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const float s = scale_of(hwnd);
    const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp));
    return POINT{static_cast<LONG>((rc.right - x) / s), static_cast<LONG>(y / s)};
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int w = rc.right, h = rc.bottom;
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
            HGDIOBJ old = SelectObject(mem, bmp);
            {
                Graphics g(mem);
                const float s = scale_of(hwnd);
                g.ScaleTransform(s, s);
                paint(g);
            }
            BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY | NOMIRRORBITMAP);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            g_hover = to_logical(hwnd, lp);
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&t);
            bool any = false;
            for (const auto& r : g_cards) any = any || r.Contains(static_cast<REAL>(g_hover.x), static_cast<REAL>(g_hover.y));
            any = any || g_theme_button.Contains(static_cast<REAL>(g_hover.x), static_cast<REAL>(g_hover.y));
            SetCursor(LoadCursorW(nullptr, any ? IDC_HAND : IDC_ARROW));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE:
            g_hover = POINT{-1, -1};
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE;  // set in WM_MOUSEMOVE
            break;
        case WM_LBUTTONUP: {
            const POINT p = to_logical(hwnd, lp);
            const PointF pf(static_cast<REAL>(p.x), static_cast<REAL>(p.y));
            if (g_cards[0].Contains(pf) && g_actions.open_app) g_actions.open_app();
            if (g_cards[2].Contains(pf) && g_actions.toggle_audio) g_actions.toggle_audio();
            if (g_cards[3].Contains(pf) && g_actions.display_settings) g_actions.display_settings();
            if (g_cards[4].Contains(pf) && g_actions.allow_wifi) g_actions.allow_wifi();
            if (g_theme_button.Contains(pf)) set_theme_mode((g_theme_mode + 1) % 3);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETTINGCHANGE:
            // Windows switched between light and dark: follow it when the look is "automatic".
            if (lp && lstrcmpiW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0 && g_theme_mode == 0) {
                apply_window_theme(hwnd);
                use_dark_menus();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            g_hwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void init(HINSTANCE instance, std::function<State()> state, Actions actions) {
    g_instance = instance;
    g_state = std::move(state);
    g_actions = std::move(actions);
    load_theme_mode();
    load_assets();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0));
    wc.lpszClassName = L"ImtidadSettings";
    RegisterClassExW(&wc);
}

void show_settings() {
    if (g_hwnd) {
        ShowWindow(g_hwnd, SW_RESTORE);
        SetForegroundWindow(g_hwnd);
        return;
    }
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD ex = WS_EX_LAYOUTRTL | WS_EX_APPWINDOW;
    g_hwnd = CreateWindowExW(ex, L"ImtidadSettings", L"امتداد", style, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                             nullptr, nullptr, g_instance, nullptr);
    if (!g_hwnd) return;
    apply_window_theme(g_hwnd);
    const float s = scale_of(g_hwnd);
    RECT r{0, 0, static_cast<LONG>(kW * s), static_cast<LONG>(kH * s)};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    // Centre on the main screen.
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = r.right - r.left, h = r.bottom - r.top;
    SetWindowPos(g_hwnd, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h,
                 SWP_NOZORDER);
    SetTimer(g_hwnd, 1, 1000, nullptr);
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
}

void refresh() {
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

bool dark_theme() { return is_dark(); }
int theme_mode() { return g_theme_mode; }

void set_theme_mode(int mode) {
    g_theme_mode = ((mode % 3) + 3) % 3;
    save_theme_mode();
    apply_window_theme(g_hwnd);
    use_dark_menus();
    refresh();
}

namespace {
HBITMAP to_hbitmap(Bitmap* src, Rect from, int size, const Color* tint = nullptr) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb || !src) return hb;
    Bitmap tmp(size, size, size * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
    Graphics g(&tmp);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.Clear(Color(0, 0, 0, 0));
    if (tint) {
        ImageAttributes attrs;
        const REAL cr = tint->GetR() / 255.f, cg = tint->GetG() / 255.f, cb = tint->GetB() / 255.f;
        ColorMatrix m = {{{cr, 0, 0, 0, 0}, {0, cg, 0, 0, 0}, {0, 0, cb, 0, 0}, {0, 0, 0, 1, 0}, {0, 0, 0, 0, 1}}};
        attrs.SetColorMatrix(&m);
        g.DrawImage(src, Rect(0, 0, size, size), from.X, from.Y, from.Width, from.Height, UnitPixel, &attrs);
    } else {
        g.DrawImage(src, Rect(0, 0, size, size), from.X, from.Y, from.Width, from.Height, UnitPixel);
    }
    return hb;
}
int menu_icon_px() { return GetSystemMetrics(SM_CXSMICON); }
}  // namespace

// Menu icons in the colour of the current look; made once per look and kept.
HBITMAP menu_bitmap(IconId id) {
    load_assets();
    static std::mutex lock;
    static HBITMAP cache[2][IconCircle + 1] = {};
    std::lock_guard<std::mutex> guard(lock);
    const int d = is_dark() ? 1 : 0;
    if (id < 0 || id > IconCircle) return nullptr;
    if (!cache[d][id]) {
        const Color tint = rgb(theme().text);
        cache[d][id] = to_hbitmap(g_icons.get(), Rect(id * kIconCell, 0, kIconCell, kIconCell), menu_icon_px(), &tint);
    }
    return cache[d][id];
}

HBITMAP app_bitmap() {
    load_assets();
    if (!g_app) return nullptr;
    return to_hbitmap(g_app.get(), Rect(0, 0, static_cast<INT>(g_app->GetWidth()), static_cast<INT>(g_app->GetHeight())),
                      menu_icon_px() * 3 / 2);
}

void use_dark_menus() {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int);
    using Flush = void(WINAPI*)();
    auto set_mode = reinterpret_cast<SetMode>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))));
    auto flush = reinterpret_cast<Flush>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))));
    if (set_mode) set_mode(is_dark() ? 2 : 3);  // ForceDark / ForceLight, following the app's look
    if (flush) flush();
}

// ------------------------------------------------------------------ annotation overlay
namespace {
HWND g_overlay = nullptr;
std::unique_ptr<Bitmap> g_ink;
POINT g_last{-1, -1};
bool g_down = false;
RECT g_area{};

void overlay_present() {
    if (!g_overlay || !g_ink) return;
    const int w = g_area.right - g_area.left, h = g_area.bottom - g_area.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP hb = nullptr;
    g_ink->GetHBITMAP(Color(0, 0, 0, 0), &hb);
    HGDIOBJ old = SelectObject(mem, hb);
    POINT src{0, 0}, dst{g_area.left, g_area.top};
    SIZE size{w, h};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_overlay, screen, &dst, &size, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(hb);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void overlay_clear() {
    if (!g_ink) return;
    Graphics g(g_ink.get());
    g.Clear(Color(1, 0, 0, 0));  // almost invisible, but still catches the pen and mouse
    // A thin red frame shows that drawing is on.
    Pen frame(Color(200, 229, 83, 75), 6);
    g.DrawRectangle(&frame, 3, 3, static_cast<INT>(g_ink->GetWidth()) - 6, static_cast<INT>(g_ink->GetHeight()) - 6);
    overlay_present();
}

LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN:
            g_down = true;
            SetCapture(hwnd);
            g_last = POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            return 0;
        case WM_MOUSEMOVE:
            if (g_down && g_ink) {
                const POINT p{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
                Graphics g(g_ink.get());
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                Pen pen(Color(255, 229, 83, 75), 5);
                pen.SetStartCap(LineCapRound);
                pen.SetEndCap(LineCapRound);
                g.DrawLine(&pen, static_cast<INT>(g_last.x), static_cast<INT>(g_last.y), static_cast<INT>(p.x), static_cast<INT>(p.y));
                g_last = p;
                overlay_present();
            }
            return 0;
        case WM_LBUTTONUP:
            g_down = false;
            ReleaseCapture();
            return 0;
        case WM_RBUTTONUP:
            overlay_clear();  // right click: wipe
            return 0;
        case WM_RBUTTONDBLCLK:
            annotate(0);      // double right click: stop drawing
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) annotate(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

bool annotating() { return g_overlay != nullptr; }

void annotate(int action) {
    if (action == 3) action = g_overlay ? 0 : 1;
    if (action == 0) {
        if (g_overlay) DestroyWindow(g_overlay);
        g_overlay = nullptr;
        g_ink.reset();
        return;
    }
    if (action == 2) { overlay_clear(); return; }
    if (g_overlay) return;
    load_assets();
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = overlay_proc;
        wc.style = CS_DBLCLKS;
        wc.hInstance = g_instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.lpszClassName = L"ImtidadInk";
        RegisterClassW(&wc);
        registered = true;
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    g_area = mi.rcMonitor;
    const int w = g_area.right - g_area.left, h = g_area.bottom - g_area.top;
    g_ink = std::make_unique<Bitmap>(w, h, PixelFormat32bppARGB);
    g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ImtidadInk", L"", WS_POPUP,
                                g_area.left, g_area.top, w, h, nullptr, nullptr, g_instance, nullptr);
    ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
    overlay_clear();
}

unsigned pixel_under_cursor() {
    POINT p;
    GetCursorPos(&p);
    HDC dc = GetDC(nullptr);
    const COLORREF c = GetPixel(dc, p.x, p.y);
    ReleaseDC(nullptr, dc);
    return (static_cast<unsigned>(GetRValue(c)) << 16) | (static_cast<unsigned>(GetGValue(c)) << 8) | GetBValue(c);
}

bool save_screenshot_png(const std::wstring& path) {
    load_assets();
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    bool ok = false;
    {
        Bitmap image(bmp, nullptr);
        UINT n = 0, size = 0;
        GetImageEncodersSize(&n, &size);
        std::vector<BYTE> buf(size);
        auto* codecs = reinterpret_cast<ImageCodecInfo*>(buf.data());
        if (size && GetImageEncoders(n, size, codecs) == Ok) {
            for (UINT i = 0; i < n; ++i) {
                if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
                    ok = image.Save(path.c_str(), &codecs[i].Clsid, nullptr) == Ok;
                    break;
                }
            }
        }
    }
    DeleteObject(bmp);
    return ok;
}

void shutdown() {
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_illustration.reset();
    g_icons.reset();
    g_app.reset();
    g_fonts.reset();
    if (g_gdiplus) GdiplusShutdown(g_gdiplus);
    g_gdiplus = 0;
}

}  // namespace screenlink::ui
