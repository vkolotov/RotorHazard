// The timer socket address for a club feed URL, as the rh_mach1 plugin's
// socket_url():
//   https://mach1.org.nz/<club>/feed/<source>/?key=K
//     -> wss://mach1.org.nz/ws/timers/<club>/<source>/?key=K&uid=<uid>
#pragma once
#include <string>

namespace relay {

// Empty when the feed URL is not one mach1 gave (no /<club>/feed/<source>/, no key).
std::string socketUrl(const std::string& feed, const std::string& uid);

}  // namespace relay
