// Imtidad settings window (see ui.hpp). Drawn with GDI+ into an off-screen bitmap.
#include "ui.hpp"

#include <objidl.h>
#include <shellapi.h>
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
constexpr float kW = 1120, kH = 680;  // client size in DIPs (2.0.1: the new dashboard layout)
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
std::vector<float> g_fps_history;   // last minute of frames per second, for the small line

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
    const Color white(255, 255, 255, 255);
    SolidBrush bg(rgb(th.ground));
    g.FillRectangle(&bg, 0.f, 0.f, kW, kH);
    auto hot_at = [](const RectF& r) { return r.Contains(static_cast<REAL>(g_hover.x), static_cast<REAL>(g_hover.y)); };
    auto fill_round = [&](const RectF& r, float rad, Color c) { GraphicsPath p; rounded(p, r, rad); SolidBrush b(c); g.FillPath(&b, &p); };
    auto stroke_round = [&](const RectF& r, float rad, Color c) { GraphicsPath p; rounded(p, r, rad); Pen pen(c, 1.f); g.DrawPath(&pen, &p); };
    for (auto& c : g_cards) c = RectF();

    // ── Side bar (right, as Arabic reads): name, connected devices, Wi-Fi code, look, rights.
    const float sx = kW - 268, sw = 252;
    {
        const RectF brand(sx, 16, sw, 64);
        card_back(g, brand, false);
        if (g_app) g.DrawImage(g_app.get(), RectF(brand.GetRight() - 52, brand.Y + 14, 36, 36));
        text(g, L"امتداد", RectF(brand.X + 30, brand.Y + 12, sw - 96, 22), 16, true, rgb(th.text));
        text(g, L"الإصدار 2.0.1 · يعمل", RectF(brand.X + 30, brand.Y + 34, sw - 96, 18), 12, false, rgb(th.muted));
        SolidBrush ok(rgb(th.good));
        g.FillEllipse(&ok, brand.X + 16, brand.Y + 28, 8.f, 8.f);
    }
    text(g, L"الأجهزة المتصلة", RectF(sx + 12, 98, sw - 50, 20), 13, false, rgb(th.muted));
    text(g, std::to_wstring(st.devices.size()), RectF(sx + 12, 98, 30, 20), 13, false, rgb(th.muted), StringAlignmentFar);
    float dy = 124;
    if (st.devices.empty()) {
        text(g, L"لا يوجد جهاز — صِل التابلت بالكيبل", RectF(sx + 8, dy, sw - 16, 20), 12.5f, false, rgb(th.muted));
        dy += 30;
    }
    for (size_t i = 0; i < st.devices.size() && i < 4; ++i) {
        const RectF row(sx, dy, sw, 50);
        const RectF chip(row.GetRight() - 44, row.Y + 9, 32, 32);
        fill_round(chip, 9, rgb(i == 0 ? 0xE8F0FE : th.soft));
        if (is_dark()) fill_round(chip, 9, rgb(i == 0 ? 0x16306E : th.soft));
        icon(g, IconDevices, RectF(chip.X + 7, chip.Y + 7, 18, 18), i == 0 ? rgb(th.accent) : rgb(th.text));
        text(g, st.devices[i], RectF(row.X + 28, row.Y + 7, sw - 84, 20), 14, false, rgb(th.text));
        text(g, st.device_link + (i == 0 && st.streaming ? L" · يعرض الآن" : L""), RectF(row.X + 28, row.Y + 27, sw - 84, 18), 12, false, rgb(th.muted));
        SolidBrush ok(rgb(th.good));
        g.FillEllipse(&ok, row.X + 10, row.Y + 21, 8.f, 8.f);
        dy += 54;
    }
    {   // Wi-Fi: allowed or not, and the connection code
        const RectF box(sx, kH - 214, sw, 118);
        card_back(g, box, false);
        const RectF allow(box.X + 12, box.Y + 10, sw - 24, 34);
        g_cards[4] = allow;
        if (hot_at(allow)) fill_round(allow, 8, rgb(th.selected));
        text(g, L"السماح بالاتصال عبر Wi-Fi", RectF(allow.X + 34, allow.Y + 7, allow.Width - 40, 20), 13.5f, false, rgb(th.text));
        const RectF tick(allow.X + 6, allow.Y + 8, 18, 18);
        fill_round(tick, 4, st.wifi_allowed ? rgb(th.accent) : rgb(th.soft));
        if (!st.wifi_allowed) stroke_round(tick, 4, rgb(th.border));
        else { Pen pen(white, 2.f); g.DrawLine(&pen, tick.X + 4, tick.Y + 9, tick.X + 8, tick.Y + 13); g.DrawLine(&pen, tick.X + 8, tick.Y + 13, tick.X + 14, tick.Y + 5); }
        const RectF code(box.X + 12, box.Y + 52, sw - 24, 54);
        fill_round(code, 10, rgb(th.soft));
        text(g, L"رمز الاتصال", RectF(code.X + 10, code.Y + 8, code.Width - 20, 18), 12.5f, false, rgb(th.muted));
        text(g, st.ip.empty() ? L"" : L"‎" + st.ip, RectF(code.X + 10, code.Y + 28, code.Width - 20, 18), 11.5f, false, rgb(th.muted));
        text(g, L"‎" + st.pin, RectF(code.X + 10, code.Y + 14, 120, 30), 22, true, rgb(th.text), StringAlignmentFar);
    }
    {
        static const wchar_t* kModes[] = {L"المظهر: تلقائي", L"المظهر: فاتح", L"المظهر: داكن"};
        g_theme_button = RectF(sx, kH - 86, sw, 32);
        fill_round(g_theme_button, 16, rgb(hot_at(g_theme_button) ? th.selected : th.panel));
        stroke_round(g_theme_button, 16, rgb(hot_at(g_theme_button) ? th.accent : th.border));
        text(g, kModes[g_theme_mode % 3], g_theme_button, 13, false, rgb(th.text), StringAlignmentCenter, StringAlignmentCenter);
    }
    text(g, L"تطوير عاصم الجهني · © 2026 جميع الحقوق محفوظة", RectF(sx, kH - 44, sw, 30), 11.5f, false, rgb(th.muted), StringAlignmentCenter);

    // ── Main panel
    const RectF panel(12, 12, sx - 28, kH - 24);
    fill_round(panel, 18, rgb(th.panel));
    stroke_round(panel, 18, rgb(th.border));
    const float px = panel.X + 26, pw = panel.Width - 52;
    {   // header: path and the main button
        text(g, L"امتداد  /  الرئيسية", RectF(px + 430, panel.Y + 20, pw - 430, 24), 15, false, rgb(th.muted));
        const RectF send(px, panel.Y + 14, 200, 38);
        g_cards[1] = send;
        fill_round(send, 10, hot_at(send) ? rgb(th.accent) : (is_dark() ? rgb(th.accent) : rgb(0x0B1530)));
        text(g, L"إرسال ملف إلى الجهاز", send, 14, false, white, StringAlignmentCenter, StringAlignmentCenter);
        const RectF open(px + 212, panel.Y + 14, 200, 38);
        g_cards[0] = open;
        fill_round(open, 10, rgb(hot_at(open) ? th.selected : th.panel));
        stroke_round(open, 10, rgb(hot_at(open) ? th.accent : th.border));
        text(g, L"فتح التطبيق على التابلت", open, 14, false, rgb(th.text), StringAlignmentCenter, StringAlignmentCenter);
        Pen line(rgb(th.border), 1.f);
        g.DrawLine(&line, panel.X, panel.Y + 66, panel.GetRight(), panel.Y + 66);
    }
    float y = panel.Y + 86;
    {
        std::wstring title = st.streaming ? L"يعرض الآن" : (st.devices.empty() ? L"جاهز، بانتظار التابلت" : L"الجهاز متصل وجاهز");
        text(g, title, RectF(px, y, pw, 34), 24, true, rgb(th.text));
        std::wstring sub = st.devices.empty() ? L"صِل التابلت أو الجوال بالكيبل، أو افتح التطبيق عليه عبر Wi-Fi."
                                              : st.devices.front() + L" متصل عبر " + st.device_link + (st.streaming ? L"، والعرض يعمل." : L".");
        text(g, sub, RectF(px, y + 36, pw, 22), 14, false, rgb(th.muted));
        y += 76;
    }
    {   // four figures in one strip, with a note row under them
        const RectF strip(px, y, pw, 148);
        fill_round(strip, 16, rgb(th.panel));
        const float cw = pw / 4;
        static const wchar_t* kSides[] = {L"يمين الشاشة الرئيسية", L"يسار الشاشة الرئيسية", L"فوق الشاشة الرئيسية", L"تحت الشاشة الرئيسية"};
        struct Tile { std::wstring label, value, unit, note; bool spark; };
        const Tile tiles[4] = {
            {L"معدل الإطارات", st.streaming && st.fps > 0 ? std::to_wstring(static_cast<int>(st.fps + 0.5)) : L"—", L"إطار/ث",
             st.streaming ? L"أثناء العرض" : L"لا يوجد عرض الآن", true},
            {L"الترميز", st.encoder.empty() ? L"—" : (st.encoder.find(L"265") != std::wstring::npos ? L"H.265" : L"H.264"), L"",
             st.encoder.empty() ? L"يظهر عند بدء العرض" : L"‎" + st.encoder, false},
            {L"الشاشة الثانية", st.vdd_found ? L"جاهزة" : L"غير مثبتة", L"",
             st.vdd_found ? std::wstring(kSides[st.side % 4]) : L"أعد تشغيل المثبّت واختر تعريف الشاشة", false},
            {L"Wi-Fi", st.wifi_allowed ? L"مسموح" : L"مغلق", L"", st.ip.empty() ? L"" : L"‎" + st.ip, false},
        };
        for (int i = 0; i < 4; ++i) {
            const float tx = strip.GetRight() - (i + 1) * cw;   // right to left
            const RectF cell(tx, strip.Y, cw, 100);
            if (i < 3) { Pen sep(rgb(th.border), 1.f); g.DrawLine(&sep, tx, strip.Y + 16, tx, strip.Y + 88); }
            text(g, tiles[i].label, RectF(cell.X + 16, cell.Y + 16, cw - 32, 20), 13.5f, false, rgb(th.muted));
            text(g, tiles[i].value + (tiles[i].unit.empty() ? L"" : L" " + tiles[i].unit), RectF(cell.X + 16, cell.Y + 38, cw - 32, 34), 24, true, rgb(th.text));
            text(g, tiles[i].note, RectF(cell.X + 16, cell.Y + 72, cw - 32, 18), 12, false, rgb(th.muted));
            if (tiles[i].spark && g_fps_history.size() > 1) {
                const RectF sp(cell.X + 16, cell.Y + 44, 80, 26);
                float mx = 1;
                for (float v : g_fps_history) mx = std::max(mx, v);
                std::vector<PointF> pts;
                const size_t n = g_fps_history.size();
                for (size_t k = 0; k < n; ++k)
                    pts.emplace_back(sp.X + sp.Width * k / (n - 1), sp.GetBottom() - sp.Height * g_fps_history[k] / mx);
                Pen pen(rgb(th.accent), 2.f);
                pen.SetLineJoin(LineJoinRound);
                g.DrawLines(&pen, pts.data(), static_cast<INT>(pts.size()));
            }
        }
        const RectF note(strip.X, strip.Y + 100, strip.Width, 48);
        {
            GraphicsPath p;
            p.AddLine(note.X, note.Y, note.GetRight(), note.Y);
            p.AddArc(note.GetRight() - 32, note.GetBottom() - 32, 32.f, 32.f, 0, 90);
            p.AddArc(note.X, note.GetBottom() - 32, 32.f, 32.f, 90, 90);
            p.CloseFigure();
            SolidBrush b(rgb(th.soft));
            g.FillPath(&b, &p);
        }
        stroke_round(strip, 16, rgb(th.border));
        icon(g, IconCheck, RectF(note.GetRight() - 38, note.Y + 14, 20, 20), rgb(th.muted));
        text(g, st.wifi_allowed ? L"أجهزة Wi-Fi الجديدة تُدخل رمز الاتصال مرة واحدة، ثم تتصل بمفتاحها الخاص."
                                : L"الاتصال عبر Wi-Fi مغلق. اضغط «السماح بالاتصال عبر Wi-Fi» في الشريط الجانبي لتفعيله.",
             RectF(note.X + 16, note.Y + 14, note.Width - 64, 20), 13, false, rgb(th.text));
        y += 168;
    }
    {   // bottom: devices (wide) and quick settings
        const float qw = 300, gap = 18;
        const RectF dev(px + qw + gap, y, pw - qw - gap, panel.GetBottom() - y - 22);
        const RectF quick(px, y, qw, dev.Height);
        card_back(g, dev, false, false, 16);
        text(g, L"الأجهزة", RectF(dev.X + 20, dev.Y + 16, dev.Width - 40, 22), 16, true, rgb(th.text));
        text(g, std::to_wstring(st.devices.size()) + L" متصل  ·  اسحب ملفًا إلى هذه النافذة لإرساله إلى الجهاز",
             RectF(dev.X + 20, dev.Y + 40, dev.Width - 40, 18), 12.5f, false, rgb(th.muted));
        const RectF head(dev.X + 1, dev.Y + 68, dev.Width - 2, 30);
        SolidBrush hb(rgb(th.soft));
        g.FillRectangle(&hb, head);
        const float c1 = dev.GetRight() - 20, colw = (dev.Width - 40) / 3;
        text(g, L"الجهاز", RectF(c1 - colw * 1.4f, head.Y + 6, colw * 1.4f, 18), 12, false, rgb(th.muted));
        text(g, L"الاتصال", RectF(c1 - colw * 2.2f, head.Y + 6, colw * 0.8f, 18), 12, false, rgb(th.muted));
        text(g, L"الحالة", RectF(dev.X + 20, head.Y + 6, colw * 0.8f, 18), 12, false, rgb(th.muted));
        float ry = head.GetBottom();
        if (st.devices.empty())
            text(g, L"لا يوجد جهاز متصل الآن.", RectF(dev.X + 20, ry + 16, dev.Width - 40, 20), 13.5f, false, rgb(th.muted));
        for (size_t i = 0; i < st.devices.size() && ry + 52 < dev.GetBottom(); ++i) {
            text(g, st.devices[i], RectF(c1 - colw * 1.4f, ry + 15, colw * 1.4f, 20), 14, false, rgb(th.text));
            text(g, st.device_link, RectF(c1 - colw * 2.2f, ry + 15, colw * 0.8f, 20), 14, false, rgb(th.text));
            const RectF pill(dev.X + 20, ry + 12, 110, 26);
            fill_round(pill, 13, rgb(th.soft));
            SolidBrush ok(rgb(th.good));
            g.FillEllipse(&ok, pill.GetRight() - 18, pill.Y + 9, 8.f, 8.f);
            text(g, i == 0 && st.streaming ? L"يعرض الآن" : L"متصل", RectF(pill.X + 6, pill.Y + 4, pill.Width - 30, 18), 12.5f, false, rgb(th.text));
            Pen sep(rgb(th.border), 1.f);
            g.DrawLine(&sep, dev.X, ry + 50, dev.GetRight(), ry + 50);
            ry += 50;
        }

        card_back(g, quick, false, false, 16);
        text(g, L"إعدادات سريعة", RectF(quick.X + 18, quick.Y + 16, quick.Width - 36, 22), 16, true, rgb(th.text));
        struct Q { IconId icon; std::wstring title, sub; int kind; };   // kind: 0 switch (sound), 1 chevron
        const Q qs[2] = {
            {st.audio_enabled ? IconSound : IconMute, L"صوت الكمبيوتر", L"نقل الصوت إلى التابلت", 0},
            {IconMonitor, L"مكان الشاشة الثانية",
             st.vdd_found ? std::wstring(L"اضغط للتغيير") + (st.vdd_width > 0 ? L" · ‎" + std::to_wstring(st.vdd_width) + L"×" + std::to_wstring(st.vdd_height) : L"") : L"غير مثبتة", 1},
        };
        for (int i = 0; i < 2; ++i) {
            const RectF r(quick.X + 10, quick.Y + 52 + i * 60, quick.Width - 20, 54);
            g_cards[2 + i] = r;
            if (hot_at(r)) fill_round(r, 12, rgb(th.selected));
            const RectF chip(r.GetRight() - 46, r.Y + 9, 36, 36);
            fill_round(chip, 10, rgb(th.soft));
            icon(g, qs[i].icon, RectF(chip.X + 8, chip.Y + 8, 20, 20), rgb(th.text));
            text(g, qs[i].title, RectF(r.X + 60, r.Y + 8, r.Width - 116, 20), 14, false, rgb(th.text));
            text(g, qs[i].sub, RectF(r.X + 60, r.Y + 28, r.Width - 116, 18), 12, false, rgb(th.muted));
            if (qs[i].kind == 0) { g_audio_toggle = RectF(r.X + 8, r.Y + 14, 46, 26); toggle(g, g_audio_toggle, st.audio_enabled); }
            else icon(g, IconChevron, RectF(r.X + 14, r.Y + 17, 20, 20), rgb(th.muted), 0.9f);
        }
        if (g_illustration && quick.Height > 230) {
            const float iw = std::min(quick.Width - 60, (quick.Height - 180) * 330.f / 245.f);
            g.DrawImage(g_illustration.get(), RectF(quick.X + (quick.Width - iw) / 2, quick.GetBottom() - iw * 245.f / 330.f - 14, iw, iw * 245.f / 330.f));
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
            if (g_cards[1].Contains(pf) && g_actions.send_files) g_actions.send_files();
            if (g_cards[2].Contains(pf) && g_actions.toggle_audio) g_actions.toggle_audio();
            if (g_cards[3].Contains(pf) && g_actions.display_settings) g_actions.display_settings();
            if (g_cards[4].Contains(pf) && g_actions.allow_wifi) g_actions.allow_wifi();
            if (g_theme_button.Contains(pf)) set_theme_mode((g_theme_mode + 1) % 3);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_DROPFILES: {
            // Files dragged from Explorer onto the window go to the device.
            HDROP drop = reinterpret_cast<HDROP>(wp);
            std::vector<std::wstring> files;
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t path[MAX_PATH * 4];
                if (DragQueryFileW(drop, i, path, ARRAYSIZE(path))) files.emplace_back(path);
            }
            DragFinish(drop);
            if (!files.empty() && g_actions.drop_files) g_actions.drop_files(files);
            return 0;
        }
        case WM_TIMER: {
            const State st = g_state ? g_state() : State{};
            g_fps_history.push_back(st.streaming ? static_cast<float>(st.fps) : 0.f);
            if (g_fps_history.size() > 60) g_fps_history.erase(g_fps_history.begin());
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
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
    const DWORD ex = WS_EX_LAYOUTRTL | WS_EX_APPWINDOW | WS_EX_ACCEPTFILES;
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
