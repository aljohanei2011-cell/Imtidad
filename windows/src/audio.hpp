// Computer sound -> tablet: WASAPI loopback capture of the default speakers, sent as 16-bit PCM.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

#include <functional>
#include <string>

namespace screenlink {

constexpr unsigned short kAudioPort = 48768;

// Wire format (server -> client, big-endian):
//   header: "SLA1" u32 sample_rate u32 channels(2) u32 bits(16)
//   chunk:  u32 length <length bytes of interleaved little-endian PCM16>
// Runs until the client disconnects.
void run_audio_session(SOCKET s, const std::function<void(const std::string&)>& log);

}  // namespace screenlink
