// ScreenLink protocol handling — portable (no Winsock), so it can be unit-tested anywhere.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace screenlink {

constexpr int kProtocolVersion = 1;
constexpr std::size_t kMaxLineBytes = 4096;

// Accumulates raw bytes from the socket and splits them into complete lines.
// The size limit applies to a single line, not to everything received in one recv().
class LineBuffer {
public:
    // Appends data. Returns false if an incomplete line has grown past kMaxLineBytes.
    bool append(const char* data, std::size_t size);
    // Moves the next complete line (without '\n' or trailing '\r') into `line`.
    bool next_line(std::string& line);

private:
    std::string pending_;
};

// Parses one JSON line from the client and returns the JSON replies to send (usually one).
std::vector<std::string> handle_message(const std::string& line);

std::string make_error(const std::string& message);

// Name shown on the tablet (set once at startup).
void set_host_name(const std::string& name);
void set_os_name(const std::string& name);  // "Windows 10" / "Windows 11"

// UDP discovery answer: {"type":"HERE","host_name":...,"port":48765,...}
std::string discovery_reply();

}  // namespace screenlink
