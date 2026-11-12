#include "node_protocol.h"

#include <cctype>

namespace relay {

bool hasWideRssi(int apiLevel, const std::string& procType)
{
    std::string kind;
    for (char c : procType)
        kind += (char)std::toupper((unsigned char)c);
    return apiLevel >= 38 && (kind.rfind("STM32", 0) == 0 || kind.rfind("ESP32", 0) == 0);
}

LapPassStats parseLapPassStats(const uint8_t* d, bool wide)
{
    const int width = wide ? 2 : 1;
    auto rssiAt = [&](int offset) { return wide ? (int)unpack16(d + offset) : (int)d[offset]; };
    LapPassStats s;
    s.lapId = d[0];
    s.msSince = unpack16(d + 1);
    s.rssi = rssiAt(3);
    s.nodePeakRssi = rssiAt(3 + width);
    s.passPeakRssi = rssiAt(3 + 2 * width);
    s.loopTimeUs = unpack16(d + 3 + 3 * width);
    return s;
}

std::string fwText(const uint8_t* data, size_t n)
{
    size_t end = 0;
    while (end < n && data[end])
        end++;
    std::string s((const char*)data, end);
    while (!s.empty() && std::isspace((unsigned char)s.back()))
        s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace((unsigned char)s[start]))
        start++;
    return s.substr(start);
}

}  // namespace relay
