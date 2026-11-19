// ScreenLink host for Windows: runs in the notification area (no console window).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <mfapi.h>
#include <objbase.h>
#include <iphlpapi.h>
#include <dwmapi.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <random>
#include <cstdio>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "audio.hpp"
#include "nlohmann/json.hpp"
#include "display.hpp"
#include "protocol.hpp"
#include "stream.hpp"
#include "ui.hpp"
#include "mixer.hpp"
#include "input.hpp"

namespace {

constexpr unsigned short kPort = 48765;
constexpr unsigned short kDiscoveryPort = 48767;
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_REFRESH = WM_APP + 2;
constexpr UINT WM_NOTIFY_USER = WM_APP + 3;
constexpr UINT WM_SHOW_SETTINGS = WM_APP + 4;

enum MenuId : UINT {
    ID_STATUS = 100, ID_HEADER, ID_SETTINGS, ID_OPEN_APP, ID_INSTALL_APP, ID_STOP_STREAM, ID_WIFI, ID_AUTOSTART, ID_LOG, ID_EXIT, ID_FORGET, ID_PRIVATE, ID_SEND_APP, ID_SEND_FILE,
};

std::mutex g_log_mutex;
std::mutex g_state_mutex;
HWND g_window = nullptr;
NOTIFYICONDATAW g_tray{};
std::wstring g_exe_dir;
std::wstring g_data_dir;
std::atomic<bool> g_streaming{false};
std::atomic<SOCKET> g_video_socket{INVALID_SOCKET};
std::wstring g_stream_mode;            // shown in the menu while streaming
std::set<std::string> g_usb_devices;   // serials linked with adb reverse
std::wstring g_pending_balloon_title, g_pending_balloon_text;
std::map<std::string, std::wstring> g_usb_models;   // serial -> model name
std::atomic<bool> g_audio_enabled{true};
screenlink::StreamStatus g_live;                   // last numbers from the stream
std::wstring g_encoder_text;                        // shown in the settings window
int g_vdd_w = 0, g_vdd_h = 0;                       // last second-screen size
std::wstring g_os_name = L"Windows";

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void log(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    static FILE* file = [] { return _wfopen((g_data_dir + L"\\screenlink-host.log").c_str(), L"w"); }();
    if (!file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(file, "%02d:%02d:%02d %s\n", t.wHour, t.wMinute, t.wSecond, text.c_str());
    std::fflush(file);
}

void notify(const std::wstring& title, const std::wstring& text) {
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_pending_balloon_title = title;
        g_pending_balloon_text = text;
    }
    if (g_window) PostMessageW(g_window, WM_NOTIFY_USER, 0, 0);
}

void refresh_tray() {
    if (g_window) PostMessageW(g_window, WM_REFRESH, 0, 0);
}

// Runs a console program without a window and returns its output.
std::string run_hidden(const std::wstring& exe, const std::wstring& args, DWORD timeout_ms = 15000, DWORD* exit_code = nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    std::string out;
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr);
        wr = nullptr;
        char chunk[4096];
        DWORD n = 0;
        const ULONGLONG start = GetTickCount64();
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;
            if (avail > 0) {
                if (!ReadFile(rd, chunk, std::min<DWORD>(avail, sizeof(chunk)), &n, nullptr) || n == 0) break;
                out.append(chunk, n);
                continue;
            }
            if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
                while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0 &&
                       ReadFile(rd, chunk, std::min<DWORD>(avail, sizeof(chunk)), &n, nullptr) && n > 0) {
                    out.append(chunk, n);
                }
                break;
            }
            if (GetTickCount64() - start > timeout_ms) { TerminateProcess(pi.hProcess, 1); break; }
        }
        if (exit_code) GetExitCodeProcess(pi.hProcess, exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (wr) CloseHandle(wr);
    CloseHandle(rd);
    return out;
}

bool file_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring find_adb() {
    for (const std::wstring& p : {g_exe_dir + L"\\adb\\adb.exe", g_exe_dir + L"\\adb.exe",
                                  g_exe_dir + L"\\toolchain\\android-sdk\\platform-tools\\adb.exe"}) {
        if (file_exists(p)) return p;
    }
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return {};
}

std::wstring find_apk() {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_exe_dir + L"\\Imtidad-*.apk").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        return g_exe_dir + L"\\" + fd.cFileName;
    }
    for (const std::wstring& p : {g_exe_dir + L"\\Imtidad.apk", g_exe_dir + L"\\ScreenLink.apk", g_exe_dir + L"\\app-debug.apk"}) {
        if (file_exists(p)) return p;
    }
    return {};
}

// ------------------------------------------------------------------ sockets
bool send_line(SOCKET s, const std::string& value) {
    const std::string data = value + "\n";
    size_t offset = 0;
    while (offset < data.size()) {
        const int n = send(s, data.data() + offset, static_cast<int>(data.size() - offset), 0);
        if (n <= 0) return false;
        offset += static_cast<size_t>(n);
    }
    return true;
}

std::string peer_name(SOCKET s) {
    sockaddr_in addr{};
    int len = sizeof(addr);
    char ip[INET_ADDRSTRLEN] = "?";
    if (getpeername(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0) inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return ip;
}

// ------------------------------------------------------------------ control sessions (1.2)
// Each connected tablet. Wi-Fi tablets must enter the PIN (or be trusted) before anything else.
struct Session {
    SOCKET s = INVALID_SOCKET;
    std::string peer;
    bool authed = false;
    int failures = 0;
    std::string nonce;   // random per connection; a trusted device signs it with its key
    std::mutex send_mutex;
    bool send(const std::string& line) {
        std::lock_guard<std::mutex> lock(send_mutex);
        return send_line(s, line);
    }
};
std::mutex g_sessions_mutex;
std::vector<std::shared_ptr<Session>> g_sessions;
std::set<std::string> g_authorized_ips;   // may open video / audio
std::wstring g_pin;                        // shown in the settings window; read and change it with g_guard_mutex held
std::wstring g_last_clip;                  // avoids sending the clipboard back and forth
constexpr UINT WM_SET_CLIP = WM_APP + 5;
constexpr UINT WM_ANNOTATE = WM_APP + 6;
const wchar_t* kTrustedKey = L"Software\\ScreenLink\\Trusted";

std::wstring lan_ip();
std::string lan_mac();
void take_screenshot_for_tablet();
bool offer_file(const std::wstring& path, bool temporary);

bool ip_authorized(const std::string& ip) {
    if (ip == "127.0.0.1") return true;  // USB (adb reverse) or this PC
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    return g_authorized_ips.count(ip) > 0;
}

void broadcast(const std::string& line) {
    std::vector<std::shared_ptr<Session>> list;
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        list = g_sessions;
    }
    for (auto& sess : list) if (sess->authed) sess->send(line);
}

size_t session_count() {
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    return g_sessions.size();
}

// ------------------------------------------------------------------ pairing security (2.0.1)
// - The PIN has 6 digits. Wrong PINs lock that address for a while (longer each time), and after
//   many wrong PINs in total a new PIN is made, so guessing it over Wi-Fi is not practical.
// - Pairing with the PIN gives the device a secret key (kept here encrypted with DPAPI). Later
//   connections prove they hold the key by signing a random value (HMAC-SHA256); the key itself
//   and the device id are no longer enough on their own.
const wchar_t* kTrustedNamesKey = L"Software\\ScreenLink\\TrustedNames";
constexpr int kTriesBeforeLock = 5;          // wrong PINs from one address before it must wait
constexpr int kFailuresBeforeNewPin = 20;    // wrong PINs in total before a new PIN is made
constexpr int kMaxLockSeconds = 15 * 60;

struct PinGuard {
    int failures = 0;
    int lockouts = 0;
    ULONGLONG until = 0;   // GetTickCount64 time when this address may try again
};
std::mutex g_guard_mutex;
std::map<std::string, PinGuard> g_pin_guard;   // by IP address
int g_pin_failures_total = 0;                  // since the PIN was last changed
ULONGLONG g_last_pin_failure = 0;

bool random_bytes(void* data, size_t n) {
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, static_cast<PUCHAR>(data), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::string to_hex(const unsigned char* data, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += digits[data[i] >> 4]; s += digits[data[i] & 15]; }
    return s;
}

std::string random_hex(size_t bytes) {
    std::vector<unsigned char> b(bytes);
    if (!random_bytes(b.data(), b.size())) return {};
    return to_hex(b.data(), b.size());
}

// Compares in the same time whatever the content, so timing tells nothing about the secret.
bool same_secret(const std::string& a, const std::string& b) {
    if (a.size() != b.size() || a.empty()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

std::string hmac_sha256_hex(const std::vector<unsigned char>& key, const std::string& message) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char out[32] = {};
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0)) &&
              BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(message.data())), static_cast<ULONG>(message.size()), 0)) &&
              BCRYPT_SUCCESS(BCryptFinishHash(hash, out, sizeof(out), 0));
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok ? to_hex(out, sizeof(out)) : std::string();
}

std::wstring new_pin() {
    uint32_t r = 0;
    if (!random_bytes(&r, sizeof(r))) { std::random_device rd; r = rd(); }
    return std::to_wstring(100000 + r % 900000);
}

std::wstring current_pin() {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    return g_pin;
}

// Seconds this address must wait before its next PIN (0 = it may try now).
int pin_wait_seconds(const std::string& ip) {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    const ULONGLONG now = GetTickCount64();
    ULONGLONG until = 0;
    if (auto it = g_pin_guard.find(ip); it != g_pin_guard.end()) until = it->second.until;
    until = std::max(until, g_last_pin_failure + 1000);   // at most one wrong PIN a second from anyone
    return until > now ? static_cast<int>((until - now + 999) / 1000) : 0;
}

void pin_succeeded(const std::string& ip) {
    std::lock_guard<std::mutex> lock(g_guard_mutex);
    g_pin_guard.erase(ip);
}

void pin_failed(const std::string& ip) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_guard_mutex);
        const ULONGLONG now = GetTickCount64();
        g_last_pin_failure = now;
        PinGuard& g = g_pin_guard[ip];
        if (++g.failures % kTriesBeforeLock == 0) {
            const int secs = std::min(30 << std::min(g.lockouts, 5), kMaxLockSeconds);
            ++g.lockouts;
            g.until = now + static_cast<ULONGLONG>(secs) * 1000;
            log("Wrong PINs from " + ip + ": locked for " + std::to_string(secs) + " s");
        }
        if (++g_pin_failures_total >= kFailuresBeforeNewPin) {
            g_pin = new_pin();
            g_pin_failures_total = 0;
            changed = true;
        }
    }
    if (changed) {
        log("Too many wrong PINs: a new PIN was made");
        notify(L"تغيّر رمز الاتصال", L"وصلت محاولات خاطئة كثيرة من جهاز على الشبكة، فصار للاتصال رمز جديد. تجده في نافذة امتداد.");
        refresh_tray();
    }
}

// The device's secret key, or empty if it was never paired (or was paired before 2.0.1).
std::vector<unsigned char> trusted_secret(const std::string& id) {
    if (id.empty() || id.size() > 64) return {};
    BYTE blob[1024];
    DWORD size = sizeof(blob);
    if (RegGetValueW(HKEY_CURRENT_USER, kTrustedKey, widen(id).c_str(), RRF_RT_REG_BINARY, nullptr, blob, &size) != ERROR_SUCCESS) return {};
    DATA_BLOB in{size, blob};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::vector<unsigned char> secret(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return secret;
}

// Pairs the device: makes its secret key, keeps it encrypted, and returns it (hex) to send once.
std::string trust(const std::string& id, const std::string& name) {
    if (id.empty() || id.size() > 64) return {};
    unsigned char secret[32];
    if (!random_bytes(secret, sizeof(secret))) return {};
    DATA_BLOB in{sizeof(secret), secret};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Imtidad", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    bool ok = false;
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kTrustedKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegDeleteValueW(key, widen(id).c_str());   // an older plain entry of the same device
        ok = RegSetValueExW(key, widen(id).c_str(), 0, REG_BINARY, out.pbData, out.cbData) == ERROR_SUCCESS;
        RegCloseKey(key);
    }
    LocalFree(out.pbData);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kTrustedNamesKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        const std::wstring v = widen(name.substr(0, 60));
        RegSetValueExW(key, widen(id).c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>((v.size() + 1) * 2));
        RegCloseKey(key);
    }
    const std::string hex = ok ? to_hex(secret, sizeof(secret)) : std::string();
    SecureZeroMemory(secret, sizeof(secret));
    return hex;
}

void forget_trusted() {
    RegDeleteTreeW(HKEY_CURRENT_USER, kTrustedKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, kTrustedNamesKey);
    log("Trusted devices cleared");
}

void tap_key(WORD vk) {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    const bool ext = (vk >= VK_VOLUME_MUTE && vk <= VK_MEDIA_PLAY_PAUSE);
    in[0].ki.dwFlags = ext ? KEYEVENTF_EXTENDEDKEY : 0;
    in[1].ki.dwFlags = in[0].ki.dwFlags | KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

void press_combo(const std::vector<int>& keys) {
    std::vector<INPUT> ins;
    for (int vk : keys) { INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = static_cast<WORD>(vk); ins.push_back(i); }
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = static_cast<WORD>(*it); i.ki.dwFlags = KEYEVENTF_KEYUP; ins.push_back(i);
    }
    SendInput(static_cast<UINT>(ins.size()), ins.data(), sizeof(INPUT));
}

// Buttons of the control panel on the tablet. Only this fixed list can run: nothing arbitrary.
std::string do_action(const nlohmann::json& j) {
    const std::string name = j.value("name", "");
    if (name == "key") {
        const int vk = j.value("vk", 0);
        if (vk <= 0 || vk > 0xFE) return "bad key";
        tap_key(static_cast<WORD>(vk));
        return {};
    }
    if (name == "combo") {
        std::vector<int> keys;
        if (j.contains("keys") && j["keys"].is_array())
            for (const auto& k : j["keys"]) if (k.is_number_integer() && k.get<int>() > 0 && k.get<int>() <= 0xFE) keys.push_back(k.get<int>());
        if (keys.empty() || keys.size() > 4) return "bad keys";
        press_combo(keys);
        return {};
    }
    if (name == "lock") { LockWorkStation(); return {}; }
    if (name == "screenshot") { std::thread(take_screenshot_for_tablet).detach(); return {}; }
    if (name == "annotate") {
        const std::string a = j.value("mode", "toggle");
        PostMessageW(g_window, WM_ANNOTATE, a == "on" ? 1 : a == "off" ? 0 : a == "clear" ? 2 : 3, 0);
        return {};
    }
    if (name == "sleep") {
        using Fn = BOOLEAN(WINAPI*)(BOOLEAN, BOOLEAN, BOOLEAN);
        HMODULE p = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto fn = p ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(p, "SetSuspendState"))) : nullptr;
        if (!fn) return "sleep unavailable";
        std::thread([fn] { Sleep(800); fn(FALSE, FALSE, FALSE); }).detach();
        return {};
    }
    if (name == "shutdown" || name == "restart") {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        run_hidden(std::wstring(sys) + L"\\shutdown.exe", name == "shutdown" ? L"/s /t 5" : L"/r /t 5", 5000);
        return {};
    }
    if (name == "open") {
        const std::string app = j.value("app", "");
        const wchar_t* target = nullptr;
        if (app == "calc") target = L"calc.exe";
        else if (app == "notepad") target = L"notepad.exe";
        else if (app == "explorer") target = L"explorer.exe";
        else if (app == "paint") target = L"mspaint.exe";
        else if (app == "taskmgr") target = L"taskmgr.exe";
        else if (app == "settings") target = L"ms-settings:";
        else if (app == "browser") target = L"https://www.google.com";
        else return "unknown app";
        ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL);
        return {};
    }
    return "unknown action";
}

// Visible app windows, for "show one window only" on the tablet.
std::string list_windows() {
    nlohmann::json list = nlohmann::json::array();
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto& out = *reinterpret_cast<nlohmann::json*>(lp);
        if (!IsWindowVisible(w) || IsIconic(w) || GetWindow(w, GW_OWNER)) return TRUE;
        if (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;
        wchar_t title[256] = {};
        if (GetWindowTextW(w, title, 256) == 0) return TRUE;
        RECT r{};
        GetWindowRect(w, &r);
        if (r.right - r.left < 200 || r.bottom - r.top < 150) return TRUE;
        wchar_t cls[64] = {};
        GetClassNameW(w, cls, 64);
        if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"ImtidadSettings") == 0) return TRUE;
        out.push_back({{"id", static_cast<long long>(reinterpret_cast<intptr_t>(w))}, {"title", screenlink::to_utf8(title)}});
        return out.size() < 30;
    }, reinterpret_cast<LPARAM>(&list));
    return nlohmann::json{{"type", "WINDOWS"}, {"list", list}}.dump();
}

void load_side() {
    DWORD v = 0, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\ScreenLink", L"Side", RRF_RT_REG_DWORD, nullptr, &v, &size);
    screenlink::set_virtual_side(static_cast<screenlink::Side>(v % 4));
}

void cycle_side() {
    const DWORD v = (static_cast<DWORD>(screenlink::virtual_side()) + 1) % 4;
    screenlink::set_virtual_side(static_cast<screenlink::Side>(v));
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ScreenLink", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"Side", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(key);
    }
}

void client_session(SOCKET s) {
    auto sess = std::make_shared<Session>();
    sess->s = s;
    sess->peer = peer_name(s);
    sess->authed = sess->peer == "127.0.0.1";
    sess->nonce = random_hex(16);
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        g_sessions.push_back(sess);
    }
    const std::string& peer = sess->peer;
    log("Client connected: " + peer);
    const BOOL no_delay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
    screenlink::LineBuffer buffer;
    std::string line;
    char buf[4096];
    bool open = true;
    auto authorize = [&](const std::string& why) {
        sess->authed = true;
        {
            std::lock_guard<std::mutex> lock(g_sessions_mutex);
            g_authorized_ips.insert(peer);
        }
        log("Wi-Fi client allowed (" + why + "): " + peer);
    };
    while (open) {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (!buffer.append(buf, static_cast<size_t>(n))) {
            sess->send(screenlink::make_error("message too long"));
            break;
        }
        while (open && buffer.next_line(line)) {
            const auto j = nlohmann::json::parse(line, nullptr, false);
            const std::string type = j.is_object() ? j.value("type", "") : "";
            if (type != "PING" && type != "CLIP" && type != "AUTH" && type != "INPUT" && type != "VOLUME") log("RX " + peer + " " + line.substr(0, 200));
            if (type == "HELLO") {
                for (const auto& reply : screenlink::handle_message(line)) if (!sess->send(reply)) open = false;
                nlohmann::json info = {{"type", "INFO"}, {"mac", lan_mac()}, {"ip", screenlink::to_utf8(lan_ip())},
                                       {"auth", sess->authed ? "ok" : "pin"}};
                if (!sess->authed) info["nonce"] = sess->nonce;   // a paired device answers with AUTH + proof
                sess->send(info.dump());
                continue;
            }
            if (type == "PING") {
                sess->send(R"({"type":"PONG"})");
                continue;
            }
            if (!sess->authed) {
                if (type != "AUTH") {
                    sess->send(R"({"type":"AUTH_FAIL"})");
                    if (++sess->failures >= 20) open = false;   // keeps sending before the PIN: drop it
                    continue;
                }
                const std::string id = j.value("id", "");
                if (j.contains("proof")) {
                    // A paired device: it signs this connection's random value with its key.
                    // A 256-bit key cannot be guessed, so this path has no waiting (wrong PINs from others never block it).
                    const auto secret = trusted_secret(id);
                    const bool ok = !secret.empty() && !sess->nonce.empty() &&
                                    same_secret(j.value("proof", ""), hmac_sha256_hex(secret, sess->nonce + id));
                    if (ok) {
                        pin_succeeded(peer);
                        authorize("trusted device");
                        sess->send(R"({"type":"AUTH_OK"})");
                        refresh_tray();
                    } else {
                        log("Unknown key from " + peer + ": asking for the PIN");
                        sess->send(R"({"type":"AUTH_FAIL","reset":true})");   // the device forgets its key and asks for the PIN
                    }
                    continue;
                }
                if (const int wait = pin_wait_seconds(peer); wait > 0) {
                    sess->send(nlohmann::json{{"type", "AUTH_FAIL"}, {"wait", wait}}.dump());
                    continue;
                }
                if (same_secret(j.value("pin", ""), screenlink::to_utf8(current_pin()))) {
                    pin_succeeded(peer);
                    authorize("PIN");
                    nlohmann::json ok = {{"type", "AUTH_OK"}};
                    const std::string key = trust(id, j.value("device", "Android"));
                    if (!key.empty()) ok["key"] = key;
                    sess->send(ok.dump());
                    refresh_tray();
                } else {
                    pin_failed(peer);
                    sess->send(nlohmann::json{{"type", "AUTH_FAIL"}, {"wait", pin_wait_seconds(peer)}}.dump());
                }
                continue;
            }
            if (type == "AUTH") { sess->send(R"({"type":"AUTH_OK"})"); continue; }
            if (type == "ACTION" && j.value("name", "") == "color") {
                const unsigned c = screenlink::ui::pixel_under_cursor();
                char hex[8];
                std::snprintf(hex, sizeof(hex), "#%06X", c);
                sess->send(nlohmann::json{{"type", "COLOR"}, {"hex", hex}}.dump());
                continue;
            }
            if (type == "ACTION") {
                const std::string err = do_action(j);
                sess->send(nlohmann::json{{"type", "ACTION_DONE"}, {"name", j.value("name", "")}, {"ok", err.empty()}}.dump());
                if (!err.empty()) log("Action refused: " + err);
                continue;
            }
            if (type == "VOLUMES") {
                std::thread([sess] { sess->send(screenlink::volumes_json()); }).detach();
                continue;
            }
            if (type == "VOLUME") {
                const unsigned long pid = j.value("pid", 0UL);
                const float v = j.value("v", -1.0f);
                const int m = j.contains("m") ? (j.value("m", false) ? 1 : 0) : -1;
                std::thread([pid, v, m] { screenlink::set_volume(pid, v, m); }).detach();
                continue;
            }
            if (type == "INPUT") {
                // Touch/air-mouse input without a picture (main screen).
                const std::string e = j.contains("e") ? (j["e"].is_string() ? j["e"].get<std::string>() : j["e"].dump()) : "";
                if (auto ev = screenlink::parse_input(e)) {
                    MONITORINFO mi{sizeof(mi)};
                    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
                    screenlink::MonitorRect area{mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                                                mi.rcMonitor.bottom - mi.rcMonitor.top};
                    screenlink::inject(*ev, area);
                }
                continue;
            }
            if (type == "WINDOWS") {
                sess->send(list_windows());
                continue;
            }
            if (type == "CLIP") {
                const std::string text = j.value("text", "");
                if (!text.empty() && text.size() <= 3000 && g_window) {
                    PostMessageW(g_window, WM_SET_CLIP, 0, reinterpret_cast<LPARAM>(new std::wstring(widen(text))));
                }
                continue;
            }
            for (const auto& reply : screenlink::handle_message(line)) {
                if (!sess->send(reply)) { open = false; break; }
            }
        }
    }
    closesocket(s);
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        g_sessions.erase(std::remove(g_sessions.begin(), g_sessions.end(), sess), g_sessions.end());
        bool other = false;
        for (auto& o : g_sessions) other = other || (o->peer == peer && o->authed);
        if (!other) g_authorized_ips.erase(peer);
    }
    log("Client disconnected: " + peer);
}

// ------------------------------------------------------------------ files (2.0)
// TCP 48769, one transfer per connection. First line (JSON) then raw bytes:
//   tablet -> PC: {"op":"put","name":"photo.jpg","size":123}  + bytes  -> reply {"ok":true,"name":"..."}
//   PC -> tablet: {"op":"get","id":"..."} -> reply {"name":"...","size":N} + bytes
// The PC announces files for the tablet on the control channel: {"type":"FILE","id","name","size"}.
constexpr unsigned short kFilesPort = 48769;
struct Offer { std::wstring path; std::wstring name; bool temporary = false; };
std::mutex g_offers_mutex;
std::map<std::string, Offer> g_offers;

std::wstring known_folder(REFKNOWNFOLDERID id) {
    wchar_t* p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

std::wstring inbox_dir() {
    const std::wstring d = known_folder(FOLDERID_Downloads) + L"\\Imtidad";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

std::wstring safe_name(std::wstring n) {
    for (auto& c : n) if (wcschr(L"\\/:*?\"<>|", c) || c < 32) c = L'_';
    while (!n.empty() && (n.back() == L'.' || n.back() == L' ')) n.pop_back();
    if (n.empty()) n = L"file";
    return n.substr(0, 150);
}

std::wstring unique_path(const std::wstring& dir, const std::wstring& name) {
    std::wstring base = name, ext;
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) { base = name.substr(0, dot); ext = name.substr(dot); }
    std::wstring p = dir + L"\\" + name;
    for (int i = 2; GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES && i < 1000; ++i)
        p = dir + L"\\" + base + L" (" + std::to_wstring(i) + L")" + ext;
    return p;
}

// Offers a file on this PC to the connected tablets (screenshot, "Send to > Imtidad").
bool offer_file(const std::wstring& path, bool temporary) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const unsigned long long size = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    std::random_device rd;
    char id[17];
    std::snprintf(id, sizeof(id), "%08x%08x", rd(), rd());
    const std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
    {
        std::lock_guard<std::mutex> lock(g_offers_mutex);
        g_offers[id] = Offer{path, name, temporary};
    }
    bool any = false;
    {
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        for (auto& sess : g_sessions) any = any || sess->authed;
    }
    if (!any) {
        notify(L"امتداد", L"لا يوجد جهاز متصل لإرسال الملف إليه.");
        return false;
    }
    broadcast(nlohmann::json{{"type", "FILE"}, {"id", id}, {"name", screenlink::to_utf8(name)}, {"size", size}}.dump());
    log("Offered file " + screenlink::to_utf8(name) + " (" + std::to_string(size) + " bytes)");
    return true;
}

void file_session(SOCKET s) {
    const std::string peer = peer_name(s);
    if (!ip_authorized(peer)) { closesocket(s); return; }
    // Header line, byte by byte (the file data follows directly).
    std::string line;
    char ch;
    while (line.size() < 4096 && recv(s, &ch, 1, 0) == 1 && ch != '\n') line += ch;
    const auto j = nlohmann::json::parse(line, nullptr, false);
    const std::string op = j.is_object() ? j.value("op", "") : "";
    if (op == "put") {
        const std::wstring name = safe_name(widen(j.value("name", "file")));
        const unsigned long long size = j.value("size", 0ULL);
        // Enough room? Keep 512 MB free for Windows, and refuse before anything is written.
        ULARGE_INTEGER free_bytes{};
        constexpr unsigned long long kKeepFree = 512ULL << 20;
        if (GetDiskFreeSpaceExW(inbox_dir().c_str(), &free_bytes, nullptr, nullptr) &&
            (size > free_bytes.QuadPart || free_bytes.QuadPart - size < kKeepFree)) {
            send_line(s, nlohmann::json{{"ok", false}, {"error", "space"}, {"name", screenlink::to_utf8(name)}}.dump());
            log("File refused, not enough disk space: " + screenlink::to_utf8(name) + " (" + std::to_string(size) + " bytes)");
            notify(L"لا توجد مساحة كافية", L"رُفض ملف من الجهاز لأن القرص لا يتسع له: " + name);
            closesocket(s);
            return;
        }
        const std::wstring path = unique_path(inbox_dir(), name);
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool ok = f != INVALID_HANDLE_VALUE;
        std::vector<char> buf(1 << 16);
        unsigned long long left = size;
        while (ok && left > 0) {
            const int want = static_cast<int>(std::min<unsigned long long>(left, buf.size()));
            const int r = recv(s, buf.data(), want, 0);
            if (r <= 0) { ok = false; break; }
            DWORD written = 0;
            ok = WriteFile(f, buf.data(), static_cast<DWORD>(r), &written, nullptr) && written == static_cast<DWORD>(r);
            left -= static_cast<unsigned long long>(r);
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        if (!ok) DeleteFileW(path.c_str());
        const std::string final_name = screenlink::to_utf8(path.substr(path.find_last_of(L'\\') + 1));
        send_line(s, nlohmann::json{{"ok", ok}, {"name", final_name}}.dump());
        log(std::string(ok ? "Received file " : "File transfer failed: ") + final_name);
        if (ok) notify(L"وصل ملف من الجهاز", widen(final_name) + L" — في التنزيلات\\Imtidad");
    } else if (op == "speed") {
        // Network test: send N bytes as fast as possible.
        const long long n = std::clamp(j.value("size", 0LL), 0LL, 64LL << 20);
        std::vector<char> buf(1 << 16, 'x');
        long long left = n;
        while (left > 0) {
            const int k = send(s, buf.data(), static_cast<int>(std::min<long long>(left, static_cast<long long>(buf.size()))), 0);
            if (k <= 0) break;
            left -= k;
        }
    } else if (op == "get") {
        Offer offer;
        {
            std::lock_guard<std::mutex> lock(g_offers_mutex);
            auto it = g_offers.find(j.value("id", ""));
            if (it != g_offers.end()) offer = it->second;
        }
        HANDLE f = offer.path.empty() ? INVALID_HANDLE_VALUE
                                      : CreateFileW(offer.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        LARGE_INTEGER size{};
        if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &size)) {
            send_line(s, R"({"error":"not found"})");
        } else {
            send_line(s, nlohmann::json{{"name", screenlink::to_utf8(offer.name)}, {"size", size.QuadPart}}.dump());
            std::vector<char> buf(1 << 16);
            DWORD r = 0;
            while (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &r, nullptr) && r > 0) {
                size_t off = 0;
                while (off < r) {
                    const int n = send(s, buf.data() + off, static_cast<int>(r - off), 0);
                    if (n <= 0) { r = 0; break; }
                    off += static_cast<size_t>(n);
                }
                if (r == 0) break;
            }
            log("Sent file " + screenlink::to_utf8(offer.name));
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    closesocket(s);
}

void files_listener(SOCKET server) {
    for (;;) {
        SOCKET c = accept(server, nullptr, nullptr);
        if (c != INVALID_SOCKET) std::thread(file_session, c).detach();
    }
}

void take_screenshot_for_tablet() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"Imtidad";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"\\Screenshot %04d-%02d-%02d %02d.%02d.%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::wstring path = dir + name;
    if (screenlink::ui::save_screenshot_png(path)) offer_file(path, true);
    else log("Screenshot failed");
}

// "Send to > Imtidad" in the right-click menu of any file.
void create_sendto_shortcut() {
    const std::wstring dir = known_folder(FOLDERID_SendTo);
    if (dir.empty()) return;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link)))) return;
    link->SetPath(exe);
    link->SetArguments(L"--send");
    link->SetIconLocation(exe, 0);
    link->SetDescription(L"إرسال إلى التابلت أو الجوال");
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
        file->Save((dir + L"\\امتداد (إلى الجوال).lnk").c_str(), TRUE);
        file->Release();
    }
    link->Release();
}

// Tells the tablets which program is in front (the app can show drawing tools for drawing programs).
void foreground_watcher() {
    std::wstring last;
    for (;;) {
        Sleep(1000);
        if (session_count() == 0) continue;
        HWND w = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        std::wstring exe;
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH];
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &n)) { exe = path; exe = exe.substr(exe.find_last_of(L'\\') + 1); }
            CloseHandle(h);
        }
        for (auto& c : exe) c = static_cast<wchar_t>(towlower(c));
        if (exe.empty() || exe == last) continue;
        last = exe;
        broadcast(nlohmann::json{{"type", "APP"}, {"exe", screenlink::to_utf8(exe)}}.dump());
    }
}

const std::vector<std::wstring> kPrivatePrograms = {
    L"keepass.exe", L"keepassxc.exe", L"1password.exe", L"bitwarden.exe", L"lastpass.exe", L"dashlane.exe",
    L"enpass.exe", L"keeper.exe", L"nordpass.exe", L"roboform.exe",
};
bool g_private = true;

void load_privacy() {
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\ScreenLink", L"Private", RRF_RT_REG_DWORD, nullptr, &v, &size);
    g_private = v != 0;
    screenlink::set_private_programs(g_private, kPrivatePrograms);
}

void toggle_privacy() {
    g_private = !g_private;
    screenlink::set_private_programs(g_private, kPrivatePrograms);
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ScreenLink", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        const DWORD v = g_private ? 1 : 0;
        RegSetValueExW(key, L"Private", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(key);
    }
}

// CPU and memory use, sent to connected tablets every 2 seconds.
void stats_sender() {
    auto ft = [](const FILETIME& f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    FILETIME idle0, kern0, user0;
    GetSystemTimes(&idle0, &kern0, &user0);
    for (;;) {
        Sleep(2000);
        FILETIME idle1, kern1, user1;
        GetSystemTimes(&idle1, &kern1, &user1);
        const double idle = static_cast<double>(ft(idle1) - ft(idle0));
        const double total = static_cast<double>((ft(kern1) - ft(kern0)) + (ft(user1) - ft(user0)));
        idle0 = idle1; kern0 = kern1; user0 = user1;
        if (session_count() == 0) continue;
        MEMORYSTATUSEX mem{sizeof(mem)};
        GlobalMemoryStatusEx(&mem);
        const int cpu = total > 0 ? static_cast<int>(100.0 * (1.0 - idle / total) + 0.5) : 0;
        broadcast(nlohmann::json{{"type", "STATS"}, {"cpu", std::clamp(cpu, 0, 100)}, {"ram", static_cast<int>(mem.dwMemoryLoad)},
                                 {"ram_total_gb", static_cast<double>(mem.ullTotalPhys) / (1024.0 * 1024 * 1024)},
                                 {"ram_used_gb", static_cast<double>(mem.ullTotalPhys - mem.ullAvailPhys) / (1024.0 * 1024 * 1024)}}
                      .dump());
    }
}

SOCKET listen_tcp(unsigned short port) {
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) return INVALID_SOCKET;
    const BOOL exclusive = TRUE;
    setsockopt(server, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(server, SOMAXCONN) == SOCKET_ERROR) {
        log("Cannot listen on TCP " + std::to_string(port) + " (error " + std::to_string(WSAGetLastError()) + ")");
        closesocket(server);
        return INVALID_SOCKET;
    }
    return server;
}

void control_listener(SOCKET server) {
    for (;;) {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client != INVALID_SOCKET) std::thread(client_session, client).detach();
    }
}

void video_listener(SOCKET server) {
    screenlink::StreamSettings defaults;
    for (;;) {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        if (!ip_authorized(peer_name(client))) {
            log("Video refused: " + peer_name(client) + " has not entered the PIN");
            const std::string msg = "أدخل رمز الاتصال الظاهر في نافذة امتداد على الكمبيوتر";
            std::string pkt = "SLER";
            const uint32_t n = static_cast<uint32_t>(msg.size());
            pkt += static_cast<char>(n >> 24); pkt += static_cast<char>(n >> 16); pkt += static_cast<char>(n >> 8); pkt += static_cast<char>(n);
            pkt += msg;
            send(client, pkt.data(), static_cast<int>(pkt.size()), 0);
            closesocket(client);
            continue;
        }
        if (g_streaming.exchange(true)) {
            log("Video client rejected: a stream is already running");
            closesocket(client);
            continue;
        }
        log("Video client connected: " + peer_name(client));
        g_video_socket = client;
        std::thread([client, defaults] {
            screenlink::run_video_session(client, defaults, log, [](const screenlink::StreamSettings& s) {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                g_stream_mode = s.pad ? L"لوح رسم" : s.extend ? L"شاشة ثانية" : L"انعكاس الشاشة";
                if (s.extend && s.width > 0) { g_vdd_w = s.width; g_vdd_h = s.height; }
            });
            g_video_socket = INVALID_SOCKET;
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                g_stream_mode.clear();
            }
            g_streaming = false;
            refresh_tray();
            screenlink::ui::refresh();
        }).detach();
        refresh_tray();
    }
}

void audio_listener(SOCKET server) {
    for (;;) {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        if (!ip_authorized(peer_name(client))) {
            closesocket(client);
            continue;
        }
        if (!g_audio_enabled) {
            log("Audio client refused: sound is turned off in the settings window");
            closesocket(client);
            continue;
        }
        log("Audio client connected: " + peer_name(client));
        std::thread([client] { screenlink::run_audio_session(client, log); }).detach();
    }
}

// Answers LAN discovery broadcasts from the tablet app.
void discovery_responder() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kDiscoveryPort);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        log("Discovery: cannot bind UDP " + std::to_string(kDiscoveryPort));
        closesocket(s);
        return;
    }
    char buf[256];
    for (;;) {
        sockaddr_in from{};
        int fromlen = sizeof(from);
        const int n = recvfrom(s, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n <= 0) continue;
        buf[n] = 0;
        if (std::string(buf).rfind("SCREENLINK_DISCOVER", 0) != 0) continue;
        const std::string reply = screenlink::discovery_reply();
        sendto(s, reply.data(), static_cast<int>(reply.size()), 0, reinterpret_cast<sockaddr*>(&from), fromlen);
    }
}

// Keeps every USB-connected tablet linked (adb reverse) without any user action.
void usb_watcher() {
    std::wstring adb = find_adb();
    if (adb.empty()) {
        log("USB: adb not found next to the program; USB linking disabled");
        return;
    }
    log("USB: using " + screenlink::to_utf8(adb));
    ULONGLONG last_refresh = 0;
    for (;;) {
        const std::string out = run_hidden(adb, L"devices", 10000);
        std::set<std::string> now;
        size_t pos = 0;
        while (pos < out.size()) {
            const size_t end = out.find('\n', pos);
            std::string line = out.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? out.size() : end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            const size_t tab = line.find('\t');
            if (tab != std::string::npos && line.substr(tab + 1) == "device") now.insert(line.substr(0, tab));
        }
        const bool periodic = GetTickCount64() - last_refresh > 10000;
        for (const auto& serial : now) {
            const bool is_new = !g_usb_devices.count(serial);
            if (is_new || periodic) {
                const std::wstring w = widen(serial);
                run_hidden(adb, L"-s " + w + L" reverse tcp:48765 tcp:48765");
                run_hidden(adb, L"-s " + w + L" reverse tcp:48766 tcp:48766");
                run_hidden(adb, L"-s " + w + L" reverse tcp:48768 tcp:48768");
                run_hidden(adb, L"-s " + w + L" reverse tcp:48769 tcp:48769");
            }
            if (is_new) {
                std::string model = run_hidden(adb, L"-s " + widen(serial) + L" shell getprop ro.product.marketname");
                if (model.find_first_not_of(" \r\n") == std::string::npos)
                    model = run_hidden(adb, L"-s " + widen(serial) + L" shell getprop ro.product.model");
                log("USB: linked " + serial + " " + model);
                {
                    std::string m = model;
                    while (!m.empty() && (m.back() == '\r' || m.back() == '\n' || m.back() == ' ')) m.pop_back();
                    std::lock_guard<std::mutex> lock(g_state_mutex);
                    g_usb_models[serial] = widen(m.empty() ? "Android" : m);
                }
                const std::string pkgs = run_hidden(adb, L"-s " + widen(serial) + L" shell pm list packages com.screenlink.app");
                if (pkgs.find("com.screenlink.app") == std::string::npos) {
                    notify(L"جهاز جديد موصول", L"التطبيق غير مثبت عليه. من قائمة امتداد اختر «تثبيت/تحديث التطبيق على التابلت».");
                } else {
                    notify(L"تم ربط التابلت عبر USB", L"أصبح جهازك متصلًا وجاهزًا للعرض.");
                }
            }
        }
        for (const auto& serial : g_usb_devices) {
            if (!now.count(serial)) log("USB: disconnected " + serial);
        }
        if (periodic) last_refresh = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            if (now != g_usb_devices) { g_usb_devices = now; refresh_tray(); }
        }
        Sleep(2000);
    }
}

// ------------------------------------------------------------------ settings
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool autostart_enabled() {
    wchar_t value[MAX_PATH * 2];
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"ScreenLink", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS;
}

void set_autostart(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        const std::wstring v = L"\"" + std::wstring(path) + L"\" --tray";  // at sign-in: start quietly
        RegSetValueExW(key, L"ScreenLink", 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"ScreenLink");
    }
    RegCloseKey(key);
}

// First run: start with Windows by default (can be turned off from the menu).
void first_run_defaults() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ScreenLink", 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) return;
    DWORD done = 0, size = sizeof(done);
    if (RegQueryValueExW(key, L"Initialized", nullptr, nullptr, reinterpret_cast<BYTE*>(&done), &size) != ERROR_SUCCESS) {
        set_autostart(true);
        done = 1;
        RegSetValueExW(key, L"Initialized", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&done), sizeof(done));
    }
    RegCloseKey(key);
}

std::atomic<bool> g_wifi_allowed{false};

void check_firewall_rule() {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring netsh = std::wstring(sys) + L"\\netsh.exe";
    g_wifi_allowed = run_hidden(netsh, L"advfirewall firewall show rule name=Imtidad").find("Imtidad") != std::string::npos ||
                     run_hidden(netsh, L"advfirewall firewall show rule name=ScreenLink").find("ScreenLink") != std::string::npos;
}

void allow_wifi() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const std::wstring args = L"advfirewall firewall add rule name=Imtidad dir=in action=allow profile=private enable=yes program=\"" +
                              std::wstring(path) + L"\"";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";  // one administrator approval
    sei.lpFile = L"netsh.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (ShellExecuteExW(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 20000);
        CloseHandle(sei.hProcess);
    }
    check_firewall_rule();
    notify(L"امتداد", g_wifi_allowed ? L"تم السماح بالاتصال عبر Wi-Fi (الشبكات الخاصة)" : L"لم يتم السماح بالاتصال عبر Wi-Fi");
    refresh_tray();
}

void adb_action(const std::wstring& args, const wchar_t* ok_text) {
    std::thread([args, ok_text] {
        const std::wstring adb = find_adb();
        if (adb.empty()) { notify(L"امتداد", L"أداة adb غير موجودة بجانب البرنامج"); return; }
        DWORD code = 1;
        const std::string out = run_hidden(adb, args, 120000, &code);
        log("adb " + screenlink::to_utf8(args) + " -> " + out);
        const bool ok = code == 0 && out.find("Failure") == std::string::npos && out.find("error") == std::string::npos;
        notify(L"امتداد", ok ? ok_text : L"تعذّر التنفيذ: تأكد من توصيل التابلت وتفعيل تصحيح USB");
    }).detach();
}

// Installs/updates the tablet app. A copy signed with another key (older test builds) is removed first.
void install_app_on_device() {
    std::thread([] {
        const std::wstring adb = find_adb();
        const std::wstring apk = find_apk();
        if (adb.empty() || apk.empty()) { notify(L"امتداد", L"ملف التطبيق أو أداة adb غير موجودين بجانب البرنامج"); return; }
        notify(L"امتداد", L"جارٍ تثبيت التطبيق على الجهاز…");
        DWORD code = 1;
        std::string out = run_hidden(adb, L"install -r \"" + apk + L"\"", 180000, &code);
        if (out.find("UPDATE_INCOMPATIBLE") != std::string::npos || out.find("signatures do not match") != std::string::npos ||
            out.find("VERSION_DOWNGRADE") != std::string::npos) {
            log("install: removing the build on the device (other key or newer version) before installing this one");
            run_hidden(adb, L"uninstall com.screenlink.app", 60000);
            out = run_hidden(adb, L"install \"" + apk + L"\"", 180000, &code);
        }
        log("install: " + out);
        const bool ok = out.find("Success") != std::string::npos;
        if (ok) run_hidden(adb, L"shell am start -n com.screenlink.app/.MainActivity");
        notify(L"امتداد", ok ? L"تم تثبيت التطبيق وفتحه على الجهاز" : L"تعذّر التثبيت. على أجهزة Xiaomi فعّل «التثبيت عبر USB» في خيارات المطوّر");
    }).detach();
}

// ------------------------------------------------------------------ PC → device files (window button, menu, drag and drop)
void send_files_to_device(const std::vector<std::wstring>& files) {
    int sent = 0;
    for (const auto& f : files) if (offer_file(f, false)) ++sent;
    if (files.empty()) return;
    if (sent == 0) notify(L"امتداد", L"لا يوجد جهاز متصل. صِل التابلت أو افتح التطبيق عليه أولًا.");
    else notify(L"امتداد", L"أُرسل " + std::to_wstring(sent) + L" ملف إلى الجهاز — في Download/Imtidad أو الصور.");
}

// Opens Windows' file picker (several files allowed) and sends the chosen files.
void pick_and_send_files() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"اختر ملفات لإرسالها إلى الجهاز");
    std::vector<std::wstring> files;
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &item))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { files.emplace_back(path); CoTaskMemFree(path); }
                    item->Release();
                }
            }
            items->Release();
        }
    }
    dlg->Release();
    if (!files.empty()) std::thread([files] { send_files_to_device(files); }).detach();
}

// ------------------------------------------------------------------ tray UI
HICON load_app_icon(int size) {
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, 0));
}

std::wstring short_status() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_stream_mode.empty()) return L"يعرض الآن: " + g_stream_mode;
    if (!g_usb_devices.empty()) return L"التابلت متصل عبر USB";
    return L"جاهز، بانتظار التابلت";
}

std::wstring status_text() { return L"امتداد — " + short_status(); }

void update_tooltip() {
    const std::wstring tip = status_text();
    wcsncpy(g_tray.szTip, tip.c_str(), ARRAYSIZE(g_tray.szTip) - 1);
    g_tray.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void add_item(HMENU m, UINT flags, UINT id, const std::wstring& label, HBITMAP bmp) {
    AppendMenuW(m, flags, id, label.c_str());
    if (bmp) {
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_BITMAP;
        mi.hbmpItem = bmp;
        SetMenuItemInfoW(m, id, FALSE, &mi);
    }
}

void show_menu(HWND hwnd) {
    using namespace screenlink::ui;
    // Menu icons follow the light/dark look (made once per look inside menu_bitmap).
    use_dark_menus();
    static HBITMAP app = app_bitmap();
    HBITMAP open = menu_bitmap(IconOpen), download = menu_bitmap(IconDownload), stop = menu_bitmap(IconStop),
            logb = menu_bitmap(IconLog), power = menu_bitmap(IconPower), settings = menu_bitmap(IconSettings);
    HMENU m = CreatePopupMenu();
    add_item(m, MF_STRING, ID_HEADER, L"امتداد", app);
    bool linked = g_streaming;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        linked = linked || !g_usb_devices.empty();
    }
    add_item(m, MF_STRING | MF_GRAYED, ID_STATUS, (linked ? L"●  " : L"○  ") + short_status(), nullptr);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        const UINT usb = g_usb_devices.empty() ? MF_GRAYED : 0;
        add_item(m, MF_STRING | usb, ID_OPEN_APP, L"فتح التطبيق على التابلت", open);
        add_item(m, MF_STRING | (usb ? MF_GRAYED : 0) | (find_apk().empty() ? MF_GRAYED : 0), ID_INSTALL_APP,
                 L"تثبيت/تحديث التطبيق على التابلت", download);
    }
    {
        // Without USB debugging: the app file goes to the device like any file, and the device opens the installer.
        bool any = false;
        {
            std::lock_guard<std::mutex> lock(g_sessions_mutex);
            for (auto& sess : g_sessions) any = any || sess->authed;
        }
        add_item(m, MF_STRING | (any ? 0 : MF_GRAYED), ID_SEND_FILE, L"إرسال ملف إلى الجهاز…", open);
        add_item(m, MF_STRING | (any && !find_apk().empty() ? 0 : MF_GRAYED), ID_SEND_APP,
                 L"إرسال التطبيق إلى الجهاز (بدون تصحيح USB)", download);
    }
    add_item(m, MF_STRING | (g_streaming ? 0 : MF_GRAYED), ID_STOP_STREAM, L"إيقاف العرض", stop);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_wifi_allowed ? MF_CHECKED | MF_GRAYED : 0), ID_WIFI, L"السماح بالاتصال عبر Wi-Fi");
    AppendMenuW(m, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"التشغيل مع Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    add_item(m, MF_STRING, ID_SETTINGS, L"الإعدادات", settings);
    AppendMenuW(m, MF_STRING | (g_private ? MF_CHECKED : 0), ID_PRIVATE, L"إخفاء نوافذ برامج كلمات المرور عن التابلت");
    AppendMenuW(m, MF_STRING, ID_FORGET, L"نسيان أجهزة Wi-Fi الموثوقة");
    add_item(m, MF_STRING, ID_LOG, L"فتح السجل", logb);
    add_item(m, MF_STRING, ID_EXIT, L"خروج", power);
    SetMenuDefaultItem(m, ID_HEADER, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_LAYOUTRTL, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

std::wstring friendly_encoder(const std::string& name, bool hardware) {
    if (!hardware) return L"المعالج (Software - H.264)\u200F";
    std::string vendor = "GPU";
    for (const char* v : {"AMD", "NVIDIA", "Intel", "Qualcomm"}) {
        std::string up = name, vv = v;
        for (auto& c : up) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        for (auto& c : vv) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if (up.find(vv) != std::string::npos) { vendor = v; break; }
    }
    if (vendor == "GPU" && name.find("NV") == 0) vendor = "NVIDIA";
    return widen(vendor) + L" (Hardware - H.264)";
}

// The PC's address on the local network (the one the tablet would use over Wi-Fi).
std::wstring lan_ip() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return {};
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);  // no packet is sent: this only picks the route
    std::wstring ip;
    if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
        sockaddr_in me{};
        int len = sizeof(me);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
            char buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &me.sin_addr, buf, sizeof(buf));
            ip = widen(buf);
        }
    }
    closesocket(s);
    return ip;
}

// MAC address of the network card that holds the LAN address (for Wake-on-LAN from the tablet).
std::string lan_mac() {
    const std::string ip = screenlink::to_utf8(lan_ip());
    ULONG size = 16 * 1024;
    std::vector<unsigned char> mem(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(mem.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size) ==
        ERROR_BUFFER_OVERFLOW) {
        mem.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(mem.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size) != NO_ERROR)
            return {};
    }
    for (auto* a = list; a; a = a->Next) {
        if (a->PhysicalAddressLength != 6) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            char buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr, buf, sizeof(buf));
            if (ip == buf) {
                char mac[18];
                std::snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", a->PhysicalAddress[0], a->PhysicalAddress[1],
                              a->PhysicalAddress[2], a->PhysicalAddress[3], a->PhysicalAddress[4], a->PhysicalAddress[5]);
                return mac;
            }
        }
    }
    return {};
}

std::wstring detect_os_name() {
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    OSVERSIONINFOW v{sizeof(v)};
    if (fn && fn(&v) == 0) {
        if (v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000) return L"Windows 11";
        if (v.dwMajorVersion == 10) return L"Windows 10";
    }
    return L"Windows";
}

screenlink::ui::State ui_state() {
    screenlink::ui::State st;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        for (const auto& serial : g_usb_devices) {
            auto it = g_usb_models.find(serial);
            st.devices.push_back(it != g_usb_models.end() ? it->second : widen(serial));
        }
        st.device_link = L"USB";
        st.streaming = g_streaming;
        st.fps = g_live.fps;
        st.encoder = g_encoder_text;
        st.vdd_width = g_vdd_w;
        st.vdd_height = g_vdd_h;
    }
    static ULONGLONG last_ip = 0;
    static std::wstring ip;
    if (GetTickCount64() - last_ip > 5000 || ip.empty()) { ip = lan_ip(); last_ip = GetTickCount64(); }
    st.ip = ip;
    st.audio_enabled = g_audio_enabled;
    st.vdd_found = screenlink::find_virtual_display().found;
    st.wifi_allowed = g_wifi_allowed;
    st.os = g_os_name;
    st.pin = current_pin();
    st.side = static_cast<int>(screenlink::virtual_side());
    return st;
}

void set_audio_enabled(bool on) {
    g_audio_enabled = on;
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ScreenLink", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        const DWORD v = on ? 1 : 0;
        RegSetValueExW(key, L"Audio", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(key);
    }
    log(std::string("Sound to the tablet: ") + (on ? "on" : "off"));
}

bool load_audio_enabled() {
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\ScreenLink", L"Audio", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v != 0;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbar_created) {  // Explorer restarted: add the icon again
        Shell_NotifyIconW(NIM_ADD, &g_tray);
        return 0;
    }
    switch (msg) {
        case WM_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT) screenlink::ui::show_settings();
            else if (LOWORD(lp) == WM_CONTEXTMENU) show_menu(hwnd);
            return 0;
        case WM_CLIPBOARDUPDATE:
            // Read a moment later, so the program that is copying finishes first.
            SetTimer(hwnd, 77, 250, nullptr);
            return 0;
        case WM_TIMER: {
            if (wp != 77) return 0;
            KillTimer(hwnd, 77);
            if (!OpenClipboard(hwnd)) return 0;
            std::wstring text;
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) { text = p; GlobalUnlock(h); }
            }
            CloseClipboard();
            if (!text.empty() && text != g_last_clip && text.size() <= 1500) {
                g_last_clip = text;
                const std::wstring copy = text;
                std::thread([copy] { broadcast(nlohmann::json{{"type", "CLIP"}, {"text", screenlink::to_utf8(copy)}}.dump()); }).detach();
            }
            return 0;
        }
        case WM_SET_CLIP: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
            g_last_clip = *text;
            if (OpenClipboard(hwnd)) {
                EmptyClipboard();
                const size_t bytes = (text->size() + 1) * sizeof(wchar_t);
                if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                    memcpy(GlobalLock(h), text->c_str(), bytes);
                    GlobalUnlock(h);
                    if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
                }
                CloseClipboard();
            }
            return 0;
        }
        case WM_COPYDATA: {
            const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (cd && cd->dwData == 1 && cd->lpData) {
                std::wstring all(static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t));
                size_t pos = 0;
                while (pos < all.size()) {
                    const size_t end = all.find(L'\n', pos);
                    std::wstring p = all.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
                    while (!p.empty() && p.back() == L'\0') p.pop_back();
                    if (!p.empty()) std::thread([p] { offer_file(p, false); }).detach();
                    if (end == std::wstring::npos) break;
                    pos = end + 1;
                }
            }
            return TRUE;
        }
        case WM_ANNOTATE:
            screenlink::ui::annotate(static_cast<int>(wp));
            return 0;
        case WM_SHOW_SETTINGS:
            screenlink::ui::show_settings();
            return 0;
        case WM_REFRESH:
            update_tooltip();
            screenlink::ui::refresh();
            return 0;
        case WM_NOTIFY_USER: {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_tray.uFlags = NIF_INFO;
            wcsncpy(g_tray.szInfoTitle, g_pending_balloon_title.c_str(), ARRAYSIZE(g_tray.szInfoTitle) - 1);
            wcsncpy(g_tray.szInfo, g_pending_balloon_text.c_str(), ARRAYSIZE(g_tray.szInfo) - 1);
            g_tray.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
            Shell_NotifyIconW(NIM_MODIFY, &g_tray);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_HEADER:
                case ID_SETTINGS:
                    screenlink::ui::show_settings();
                    break;
                case ID_OPEN_APP:
                    adb_action(L"shell am start -n com.screenlink.app/.MainActivity", L"تم فتح التطبيق على التابلت");
                    break;
                case ID_INSTALL_APP:
                    install_app_on_device();
                    break;
                case ID_STOP_STREAM: {
                    broadcast(R"({"type":"STREAM_STOPPED"})");   // the tablet closes the stream instead of reconnecting
                    const SOCKET s = g_video_socket;
                    if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
                    break;
                }
                case ID_WIFI:
                    std::thread(allow_wifi).detach();
                    break;
                case ID_AUTOSTART:
                    set_autostart(!autostart_enabled());
                    break;
                case ID_LOG:
                    ShellExecuteW(nullptr, L"open", (g_data_dir + L"\\screenlink-host.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                case ID_PRIVATE:
                    toggle_privacy();
                    break;
                case ID_SEND_FILE:
                    pick_and_send_files();
                    break;
                case ID_SEND_APP: {
                    const std::wstring apk = find_apk();
                    if (!apk.empty() && offer_file(apk, false))
                        notify(L"امتداد", L"أُرسل التطبيق إلى الجهاز. اضغط «تثبيت» عندما تظهر نافذة التثبيت عليه.");
                    else
                        notify(L"امتداد", L"لا يوجد جهاز متصل لإرسال التطبيق إليه.");
                    break;
                }
                case ID_FORGET:
                    forget_trusted();
                    notify(L"امتداد", L"ستحتاج أجهزة Wi-Fi لرمز الاتصال مرة أخرى.");
                    break;
                case ID_EXIT:
                    DestroyWindow(hwnd);
                    break;
            }
            return 0;
        case WM_DESTROY:
            screenlink::ui::shutdown();
            Shell_NotifyIconW(NIM_DELETE, &g_tray);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void set_dpi_awareness() {
    using Fn = BOOL(WINAPI*)(HANDLE);
    if (auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))) {
        if (fn(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4)))) return;  // PER_MONITOR_AWARE_V2
    }
    SetProcessDPIAware();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdline, int) {
    const bool quiet = cmdline && wcsstr(cmdline, L"--tray");
    // "Send to > Imtidad": Imtidad.exe --send <file> [<file> ...]
    std::vector<std::wstring> send_paths;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        bool sending = false;
        for (int i = 1; argv && i < argc; ++i) {
            if (wcscmp(argv[i], L"--send") == 0) { sending = true; continue; }
            if (sending) send_paths.emplace_back(argv[i]);
        }
        if (argv) LocalFree(argv);
    }
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\ScreenLinkHost");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: opening it again (Start menu, desktop) shows its window.
        for (int i = 0; i < 20; ++i) {
            if (HWND w = FindWindowExW(HWND_MESSAGE, nullptr, L"ScreenLinkTray", nullptr)) {
                if (!send_paths.empty()) {
                    std::wstring all;
                    for (const auto& p : send_paths) all += p + L"\n";
                    COPYDATASTRUCT cd{1, static_cast<DWORD>((all.size() + 1) * sizeof(wchar_t)), all.data()};
                    SendMessageW(w, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd));
                    return 0;
                }
                if (!quiet) {
                    AllowSetForegroundWindow(ASFW_ANY);
                    PostMessageW(w, WM_SHOW_SETTINGS, 0, 0);
                }
                return 0;
            }
            Sleep(250);
        }
        return 0;
    }
    set_dpi_awareness();

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_exe_dir = path;
    g_exe_dir = g_exe_dir.substr(0, g_exe_dir.find_last_of(L"\\/"));
    wchar_t* local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        g_data_dir = std::wstring(local) + L"\\ScreenLink";
        CoTaskMemFree(local);
        CreateDirectoryW(g_data_dir.c_str(), nullptr);
    } else {
        g_data_dir = g_exe_dir;
    }

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        MessageBoxW(nullptr, L"مكوّن Media Foundation غير متوفر في Windows.", L"امتداد", MB_OK | MB_ICONERROR | MB_RTLREADING);
        return 1;
    }
    {
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
        if (GetComputerNameW(name, &len)) screenlink::set_host_name(screenlink::to_utf8(name));
        g_os_name = detect_os_name();
        screenlink::set_os_name(screenlink::to_utf8(g_os_name));
    }
    g_audio_enabled = load_audio_enabled();
    load_side();
    {
        const std::string hw = screenlink::detect_hardware_encoder();
        g_encoder_text = friendly_encoder(hw, !hw.empty());
        log("Encoder available: " + (hw.empty() ? std::string("software only") : hw));
    }
    screenlink::set_status_hook([](const screenlink::StreamStatus& st) {
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_live = st;
            g_encoder_text = friendly_encoder(st.encoder, st.hardware);
            if (st.hevc) {
                const auto at = g_encoder_text.find(L"H.264");
                if (at != std::wstring::npos) g_encoder_text.replace(at, 5, L"H.265");
            }
        }
        screenlink::ui::refresh();
    });

    SOCKET control = listen_tcp(kPort);
    SOCKET video = listen_tcp(screenlink::kVideoPort);
    SOCKET audio = listen_tcp(screenlink::kAudioPort);
    if (control == INVALID_SOCKET || video == INVALID_SOCKET) {
        MessageBoxW(nullptr, L"تعذّر فتح منافذ الاتصال (48765/48766). قد يكون برنامج آخر يستخدمها.", L"امتداد",
                    MB_OK | MB_ICONERROR | MB_RTLREADING | MB_RIGHT);
        return 1;
    }
    log("Imtidad 2.0 started: control TCP 48765, video TCP 48766, audio TCP 48768, discovery UDP 48767");
    std::thread(control_listener, control).detach();
    std::thread(video_listener, video).detach();
    if (audio != INVALID_SOCKET) std::thread(audio_listener, audio).detach();
    SOCKET files = listen_tcp(kFilesPort);
    if (files != INVALID_SOCKET) std::thread(files_listener, files).detach();
    create_sendto_shortcut();
    std::thread(discovery_responder).detach();
    std::thread(usb_watcher).detach();
    std::thread(check_firewall_rule).detach();
    std::thread(stats_sender).detach();
    std::thread(foreground_watcher).detach();
    load_privacy();
    {
        std::lock_guard<std::mutex> lock(g_guard_mutex);
        g_pin = new_pin();
    }
    first_run_defaults();
    {
        const auto vd = screenlink::find_virtual_display();
        if (vd.found) {
            const std::string err = vd.active ? screenlink::detach_virtual_display() : std::string();
            log(err.empty() ? "Virtual display driver found (detached until extend mode)" : "Detach failed: " + err);
        } else {
            log("Virtual display driver not installed: extend mode unavailable");
        }
    }

    screenlink::ui::use_dark_menus();
    screenlink::ui::init(instance, ui_state, screenlink::ui::Actions{
        [] { adb_action(L"shell am start -n com.screenlink.app/.MainActivity", L"تم فتح التطبيق على التابلت"); },
        [] { set_audio_enabled(!g_audio_enabled); },
        [] { cycle_side(); },
        [] { if (!g_wifi_allowed) std::thread(allow_wifi).detach(); },
        [] { pick_and_send_files(); },
        [](const std::vector<std::wstring>& files) { send_files_to_device(files); },
    });

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"ScreenLinkTray";
    RegisterClassW(&wc);
    g_window = CreateWindowExW(0, wc.lpszClassName, L"ScreenLink", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    AddClipboardFormatListener(g_window);

    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = g_window;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = load_app_icon(GetSystemMetrics(SM_CXSMICON));
    g_tray.hBalloonIcon = load_app_icon(32);
    wcsncpy(g_tray.szTip, L"امتداد", ARRAYSIZE(g_tray.szTip) - 1);
    Shell_NotifyIconW(NIM_ADD, &g_tray);
    g_tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_tray);
    update_tooltip();
    for (const auto& p : send_paths) std::thread([p] { Sleep(4000); offer_file(p, false); }).detach();
    if (quiet || !send_paths.empty()) {
        notify(L"امتداد يعمل", L"البرنامج بجانب الساعة. صِل التابلت أو الجوال بالكيبل، أو افتح التطبيق عليه عبر Wi-Fi.");
    } else {
        screenlink::ui::show_settings();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // Leave the desktop as it was.
    broadcast(R"({"type":"STREAM_STOPPED"})");
    const SOCKET s = g_video_socket;
    if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
    screenlink::detach_virtual_display();
    log("ScreenLink host exiting");
    ReleaseMutex(single);
    return 0;
}
