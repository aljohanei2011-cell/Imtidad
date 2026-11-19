// Portable unit tests for the protocol logic. Build with -DSCREENLINK_BUILD_TESTS=ON.
#include <cstdlib>
#include <iostream>
#include <string>

#include "../src/input.hpp"
#include "../src/nv12.hpp"
#include "../src/protocol.hpp"

#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& name) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    if (!ok) ++failures;
}

bool single_reply_contains(const std::string& line, const std::string& needle) {
    const auto replies = screenlink::handle_message(line);
    return replies.size() == 1 && replies[0].find(needle) != std::string::npos;
}

}  // namespace

int main() {
    using screenlink::LineBuffer;

    check(single_reply_contains(R"({"type":"HELLO","client":"android","version":1})", R"("type":"WELCOME")"), "HELLO -> WELCOME");
    check(single_reply_contains(R"({"type":"HELLO","version":2})", "unsupported protocol version"), "HELLO wrong version -> ERROR");
    check(single_reply_contains(R"({"type":"PING"})", R"("type":"PONG")"), "PING -> PONG");
    check(single_reply_contains(R"({ "type" : "PING" })", R"("type":"PONG")"), "PING with spaces -> PONG");
    check(single_reply_contains(R"({"type":"SET_MODE","mode":"EXTEND"})", R"("mode":"EXTEND")"), "SET_MODE EXTEND -> MODE_ACK");
    check(single_reply_contains(R"({"type":"SET_MODE","mode":"BAD"})", "invalid mode"), "SET_MODE bad -> ERROR");
    check(single_reply_contains(R"({"type":"SET_MODE"})", "invalid mode"), "SET_MODE missing -> ERROR");
    check(single_reply_contains("not json", "invalid JSON"), "garbage -> ERROR");
    check(single_reply_contains(R"({"mode":"MIRROR"})", "missing type"), "no type -> ERROR");
    check(single_reply_contains(R"({"type":"NOPE"})", "unknown command"), "unknown -> ERROR");
    check(screenlink::handle_message("").empty(), "empty line ignored");

    // Several short messages in one chunk whose total exceeds the limit must still be accepted.
    {
        LineBuffer b;
        std::string chunk;
        const std::string ping = R"({"type":"PING"})";
        while (chunk.size() < screenlink::kMaxLineBytes * 2) chunk += ping + "\n";
        const bool ok = b.append(chunk.data(), chunk.size());
        std::string line;
        int count = 0;
        while (b.next_line(line)) { if (line == ping) ++count; }
        check(ok && count > 0 && static_cast<size_t>(count) * (ping.size() + 1) == chunk.size(), "many small lines in one chunk accepted");
    }
    // A single unfinished line over the limit is rejected.
    {
        LineBuffer b;
        const std::string big(screenlink::kMaxLineBytes + 1, 'x');
        check(!b.append(big.data(), big.size()), "oversized single line rejected");
    }
    // A line split across two recv() calls, with CRLF, is reassembled.
    {
        LineBuffer b;
        std::string line;
        b.append(R"({"type":"PI)", 11);
        const bool none_yet = !b.next_line(line);
        b.append("NG\"}\r\n", 6);
        check(none_yet && b.next_line(line) && line == R"({"type":"PING"})", "split line + CRLF reassembled");
    }

    // Input parsing.
    {
        using K = screenlink::InputEvent::Kind;
        auto d = screenlink::parse_input(R"({"t":"p","a":"d","x":0.25,"y":1.5})");
        check(d && d->kind == K::PointerDown && d->x == 0.25 && d->y == 1.0, "pointer down parsed, y clamped");
        auto t = screenlink::parse_input("{\"t\":\"k\",\"text\":\"\u0645\u0631\u062d\u0628\u0627 hi\"}");
        check(t && t->kind == K::Text && t->text == L"\u0645\u0631\u062d\u0628\u0627 hi", "Arabic text parsed to UTF-16");
        auto c = screenlink::parse_input(R"({"t":"c","keys":[17,67]})");
        check(c && c->kind == K::Combo && c->keys.size() == 2, "combo parsed");
        check(!screenlink::parse_input(R"({"t":"v","vk":999})"), "bad VK rejected");
        check(!screenlink::parse_input(R"({"t":"s","dy":0})"), "zero scroll ignored");
        auto pen = screenlink::parse_input(R"({"t":"pen","a":"m","x":0.5,"y":0.5,"p":0.75,"tx":30,"ty":-120,"e":1})");
        check(pen && pen->kind == K::PenMove && pen->pressure == 0.75 && pen->tilt_x == 30 && pen->tilt_y == -90 && pen->eraser,
              "pen move parsed, tilt clamped");
        check(screenlink::discovery_reply().find("\"HERE\"") != std::string::npos, "discovery reply");
        check(screenlink::utf8_to_wide("\xF0\x9F\x98\x80").size() == 2, "emoji becomes surrogate pair");
    }
    // NV12 conversion of white / black / red.
    {
        std::vector<uint8_t> bgra = {255,255,255,255, 0,0,0,255, 255,255,255,255, 0,0,0,255};  // 2x2
        std::vector<uint8_t> nv(6);
        screenlink::bgra_to_nv12(bgra.data(), 8, 2, 2, nv.data());
        check(nv[0] == 235 && nv[1] == 16 && nv[4] == 128 && nv[5] == 128, "white/black -> Y 235/16, neutral chroma");
        std::vector<uint8_t> red = {0,0,255,255, 0,0,255,255, 0,0,255,255, 0,0,255,255};
        screenlink::bgra_to_nv12(red.data(), 8, 2, 2, nv.data());
        check(nv[0] == 82 && nv[4] == 90 && nv[5] == 240, "red -> Y82 U90 V240 (BT.601)");
    }

    std::cout << (failures == 0 ? "ALL PASSED" : "FAILURES: " + std::to_string(failures)) << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
