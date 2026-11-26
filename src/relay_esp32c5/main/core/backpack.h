// The ExpressLRS Timer Backpack (goggle OSD over ESP-NOW): MSP v2 frames and
// the pilot's UID. Our own code from the protocol facts in
// docs/standalone-gate-relay-design.md (Backpack and VRxC_ELRS are GPL-3).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace relay {

constexpr uint16_t MSP_BACKPACK_VERSION = 0x0010;  // reply payload: the version text
constexpr uint16_t MSP_SET_SEND_UID = 0x00B5;      // 01 + uid[6]: send to that UID; 00: back to its own
constexpr uint16_t MSP_SET_OSD = 0x00B6;           // 02 clear, 03 row col 0 text, 04 display
constexpr uint16_t MSP_BACKPACK_SET_MODE = 0x0380; // 'W' Wi-Fi (update page), 'B' bind
constexpr int OSD_ROWS = 18;
constexpr int OSD_COLS = 50;  // HDZero; text past it is cut

uint8_t crc8DvbS2(const uint8_t* data, size_t n);

// A request ("$X<") frame.
std::vector<uint8_t> mspV2Frame(uint16_t function, const std::vector<uint8_t>& payload = {});

// Finds the first well-formed reply ("$X>") frame in `data`, skipping noise.
bool mspV2FindReply(const std::vector<uint8_t>& data, uint16_t& function, std::vector<uint8_t>& payload);

// The pilot's UID from the first 6 bytes of md5('-DMY_BINDING_PHRASE="<phrase>"'):
// bit 0 of byte 0 cleared (a unicast MAC).
void uidFromDigest(const uint8_t digest[6], uint8_t uid[6]);
// The string the digest is taken of.
std::string bindPhraseHashInput(const std::string& phrase);

// The frames that show `text` on one pilot's goggles: send to their UID, clear,
// write `text` centred on `row` in capitals (the OSD font has no lowercase),
// display, back to the backpack's own UID.
std::vector<std::vector<uint8_t>> osdMessageFrames(const uint8_t uid[6], int row, const std::string& text);

}  // namespace relay
