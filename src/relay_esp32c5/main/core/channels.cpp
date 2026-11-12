#include "channels.h"

#include <cctype>

namespace relay {

namespace {
struct Band {
    char letter;
    int mhz[8];
};
// Raceband first, so a frequency in two bands is named by Raceband.
const Band BANDS[] = {
    {'R', {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917}},
    {'F', {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880}},
    {'E', {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945}},
    {'A', {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725}},
    {'B', {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866}},
    {'L', {5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621}},
};
}  // namespace

int channelFrequency(const std::string& raw)
{
    std::string s;
    for (char c : raw)
        if (!std::isspace((unsigned char)c))
            s += (char)std::toupper((unsigned char)c);
    if (s == "OFF")
        return 0;
    if (s.size() != 2 || !std::isdigit((unsigned char)s[1]))
        return -1;
    const char letter = s[0] == 'H' ? 'R' : s[0];
    const int n = s[1] - '0';
    if (n < 1 || n > 8)
        return -1;
    for (const Band& b : BANDS)
        if (b.letter == letter)
            return b.mhz[n - 1];
    return -1;
}

std::string channelLabel(int frequency)
{
    for (const Band& b : BANDS)
        for (int i = 0; i < 8; i++)
            if (b.mhz[i] == frequency)
                return std::string(1, b.letter) + std::to_string(i + 1);
    return "";
}

}  // namespace relay
