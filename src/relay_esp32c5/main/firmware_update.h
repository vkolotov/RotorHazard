#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace relay {

class NodeDriver;

struct FirmwareRequest {
    bool pending = false;
    std::string target;
    std::string url;
    std::string sha256;
    size_t size = 0;
};

using FirmwareProgress = std::function<void(const char* phase, size_t done, size_t total)>;

// Download an approved image over HTTPS and update either the node through
// the acknowledged AirPort protocol, or the S3's inactive OTA partition.
bool updateNodeFirmware(NodeDriver& driver, const FirmwareRequest& request,
                        const FirmwareProgress& progress, std::string* error);
bool updateRelayFirmware(const FirmwareRequest& request, const FirmwareProgress& progress,
                         std::string* error);

} // namespace relay
