#include "protocol.hpp"

#include <nlohmann/json.hpp>

namespace screenlink {

using nlohmann::json;

namespace {
std::string g_host_name = "PC";
std::string g_os_name = "Windows";
}

void set_host_name(const std::string& name) { g_host_name = name; }
void set_os_name(const std::string& name) { g_os_name = name; }

std::string discovery_reply() {
    return json{{"type", "HERE"}, {"host_name", g_host_name}, {"os", g_os_name}, {"port", 48765},
                {"video_port", 48766}, {"version", kProtocolVersion}}.dump();
}

bool LineBuffer::append(const char* data, std::size_t size) {
    pending_.append(data, size);
    // Only the part after the last newline is an unfinished line; complete lines are
    // checked individually in next_line().
    const auto last_newline = pending_.rfind('\n');
    const std::size_t unfinished =
        last_newline == std::string::npos ? pending_.size() : pending_.size() - last_newline - 1;
    return unfinished <= kMaxLineBytes;
}

bool LineBuffer::next_line(std::string& line) {
    const auto end = pending_.find('\n');
    if (end == std::string::npos) return false;
    line.assign(pending_, 0, end);
    pending_.erase(0, end + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

std::string make_error(const std::string& message) {
    return json{{"type", "ERROR"}, {"message", message}}.dump();
}

std::vector<std::string> handle_message(const std::string& line) {
    if (line.size() > kMaxLineBytes) return {make_error("message too long")};
    if (line.empty()) return {};

    const json msg = json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (msg.is_discarded() || !msg.is_object()) return {make_error("invalid JSON")};

    const auto type_it = msg.find("type");
    if (type_it == msg.end() || !type_it->is_string()) return {make_error("missing type")};
    const std::string type = type_it->get<std::string>();

    if (type == "HELLO") {
        const int version = msg.value("version", 0);
        if (version != kProtocolVersion) return {make_error("unsupported protocol version")};
        return {json{{"type", "WELCOME"}, {"server", "screenlink-windows"}, {"version", kProtocolVersion},
                     {"host_name", g_host_name}, {"os", g_os_name}, {"video_port", 48766}}.dump()};
    }
    if (type == "PING") {
        return {json{{"type", "PONG"}}.dump()};
    }
    if (type == "SET_MODE") {
        const auto mode_it = msg.find("mode");
        if (mode_it == msg.end() || !mode_it->is_string()) return {make_error("invalid mode")};
        const std::string mode = mode_it->get<std::string>();
        if (mode != "MIRROR" && mode != "EXTEND" && mode != "REMOTE") return {make_error("invalid mode")};
        return {json{{"type", "MODE_ACK"}, {"mode", mode}, {"implemented", false}}.dump()};
    }
    return {make_error("unknown command")};
}

}  // namespace screenlink
