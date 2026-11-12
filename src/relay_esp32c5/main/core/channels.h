// Video channels by label, as mach1's backend apps/integrations/rotorhazard/channels.py.
#pragma once
#include <string>

namespace relay {

// "R1" -> 5658; "off" -> 0; anything else -> -1.
int channelFrequency(const std::string& label);
// 5658 -> "R1" (Raceband first); "" for one in no band.
std::string channelLabel(int frequency);

}  // namespace relay
