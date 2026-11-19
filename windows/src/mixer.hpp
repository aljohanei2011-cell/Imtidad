// Per-program sound levels (Windows volume mixer), for the sliders on the tablet.
#pragma once

#include <string>

namespace screenlink {

// {"type":"VOLUMES","master":{"v":0.5,"m":false},"apps":[{"pid":123,"name":"chrome","v":0.8,"m":false}]}
std::string volumes_json();

// pid 0 = the speakers themselves (master). v: 0..1, or < 0 to keep. mute: 0/1, or -1 to keep.
bool set_volume(unsigned long pid, float v, int mute);

}  // namespace screenlink
