#include "backpack.h"

#include <algorithm>

namespace relay {

uint8_t crc8DvbS2(const uint8_t* data, size_t n)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0xD5) : (uint8_t)(crc << 1);
    }
    return crc;
}

std::vector<uint8_t> mspV2Frame(uint16_t function, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> f = {'$', 'X', '<'};
    const size_t body = f.size();
    f.push_back(0);  // flags
    f.push_back(function & 0xFF);
    f.push_back(function >> 8);
    f.push_back(payload.size() & 0xFF);
    f.push_back((payload.size() >> 8) & 0xFF);
    f.insert(f.end(), payload.begin(), payload.end());
    f.push_back(crc8DvbS2(f.data() + body, f.size() - body));
    return f;
}

bool mspV2FindReply(const std::vector<uint8_t>& data, uint16_t& function, std::vector<uint8_t>& payload)
{
    for (size_t i = 0; i + 9 <= data.size(); i++) {
        if (data[i] != '$' || data[i + 1] != 'X' || data[i + 2] != '>')
            continue;
        const uint8_t* body = data.data() + i + 3;
        const size_t len = body[3] | (body[4] << 8);
        if (i + 3 + 5 + len + 1 > data.size())
            continue;
        if (crc8DvbS2(body, 5 + len) != body[5 + len])
            continue;
        function = body[1] | (body[2] << 8);
        payload.assign(body + 5, body + 5 + len);
        return true;
    }
    return false;
}

void uidFromDigest(const uint8_t digest[6], uint8_t uid[6])
{
    std::copy(digest, digest + 6, uid);
    uid[0] &= 0xFE;
}

std::string bindPhraseHashInput(const std::string& phrase)
{
    return "-DMY_BINDING_PHRASE=\"" + phrase + "\"";
}

std::vector<std::vector<uint8_t>> osdMessageFrames(const uint8_t uid[6], int row, const std::string& text)
{
    // Goggle OSD fonts (Betaflight layout) have capitals only: the lowercase
    // codes are icons (arrows), so "Mach1" would show as M and three arrows.
    std::string shown = text.substr(0, OSD_COLS);
    for (char& c : shown)
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
    row = std::clamp(row, 0, OSD_ROWS - 1);
    const int col = (OSD_COLS - (int)shown.size()) / 2;
    std::vector<uint8_t> to = {1};
    to.insert(to.end(), uid, uid + 6);
    std::vector<uint8_t> write = {3, (uint8_t)row, (uint8_t)col, 0};
    write.insert(write.end(), shown.begin(), shown.end());
    return {
        mspV2Frame(MSP_SET_SEND_UID, to),
        mspV2Frame(MSP_SET_OSD, {2}),
        mspV2Frame(MSP_SET_OSD, write),
        mspV2Frame(MSP_SET_OSD, {4}),
        mspV2Frame(MSP_SET_SEND_UID, {0}),
    };
}

}  // namespace relay
