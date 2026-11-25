#include "firmware_update.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "core/node_driver.h"

namespace relay {
namespace {

constexpr uint8_t READ_UPDATE_INFO = 0x46;
constexpr uint8_t READ_UPDATE_STATUS = 0x47;
constexpr uint8_t WRITE_UPDATE_BEGIN = 0x76;
constexpr uint8_t WRITE_UPDATE_DATA = 0x77;
constexpr uint8_t WRITE_UPDATE_FINISH = 0x7B;
constexpr uint8_t WRITE_UPDATE_ABORT = 0x7C;
constexpr uint8_t WRITE_UPDATE_CONFIRM = 0x7D;
constexpr uint8_t UPDATE_PROTOCOL = 1;
constexpr size_t UPDATE_CHUNK = 64;
constexpr int STATE_RECEIVING = 1;
constexpr int STATE_READY = 2;
constexpr int STATE_PENDING_CONFIRM = 3;

void put32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(value >> 24);
    out.push_back(value >> 16);
    out.push_back(value >> 8);
    out.push_back(value);
}

uint32_t get32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool parseHex(const std::string& text, uint8_t out[32])
{
    if (text.size() != 64)
        return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < 32; i++) {
        const int hi = digit(text[i * 2]), lo = digit(text[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

struct Status {
    int state = 0;
    int error = 0;
    uint32_t session = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
};

bool status(NodeDriver& driver, Status* out, int retries = 3)
{
    Reply reply;
    if (!driver.read(READ_UPDATE_STATUS, 16, retries, &reply) || reply.data[0] != UPDATE_PROTOCOL)
        return false;
    out->state = reply.data[1];
    out->error = (reply.data[2] << 8) | reply.data[3];
    out->session = get32(reply.data.data() + 4);
    out->offset = get32(reply.data.data() + 8);
    out->size = get32(reply.data.data() + 12);
    return true;
}

using ChunkWriter = std::function<bool(const uint8_t*, size_t, size_t, std::string*)>;

bool download(const FirmwareRequest& request, const FirmwareProgress& progress,
              const ChunkWriter& writer, std::string* error)
{
    uint8_t expected[32];
    if (request.size == 0 || !parseHex(request.sha256, expected)) {
        *error = "The firmware manifest has an invalid size or SHA-256.";
        return false;
    }
    if (request.url.rfind("https://", 0) != 0) {
        *error = "Firmware downloads must use HTTPS.";
        return false;
    }
    esp_http_client_config_t config = {};
    config.url = request.url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 15000;
    config.keep_alive_enable = true;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client || esp_http_client_open(client, 0) != ESP_OK) {
        if (client) esp_http_client_cleanup(client);
        *error = "Could not open the firmware download.";
        return false;
    }
    const int64_t contentLength = esp_http_client_fetch_headers(client);
    const int statusCode = esp_http_client_get_status_code(client);
    if (statusCode != 200 || (contentLength >= 0 && (size_t)contentLength != request.size)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        *error = "Firmware server returned an unexpected status or size.";
        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    std::vector<uint8_t> buffer(4096);
    size_t offset = 0;
    bool ok = true;
    while (offset < request.size) {
        const int got = esp_http_client_read(client, (char*)buffer.data(),
                                             std::min(buffer.size(), request.size - offset));
        if (got <= 0) {
            *error = "Firmware download ended early.";
            ok = false;
            break;
        }
        mbedtls_sha256_update(&sha, buffer.data(), got);
        if (!writer(buffer.data(), got, offset, error)) {
            ok = false;
            break;
        }
        offset += got;
        if (progress) progress("downloading", offset, request.size);
    }
    uint8_t actual[32];
    mbedtls_sha256_finish(&sha, actual);
    mbedtls_sha256_free(&sha);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (ok && memcmp(actual, expected, sizeof actual)) {
        *error = "Downloaded firmware SHA-256 does not match the manifest.";
        ok = false;
    }
    return ok;
}

} // namespace

bool updateNodeFirmware(NodeDriver& driver, const FirmwareRequest& request,
                        const FirmwareProgress& progress, std::string* error)
{
    Reply info;
    if (!driver.read(READ_UPDATE_INFO, 16, 4, &info) || info.data[0] != UPDATE_PROTOCOL ||
        info.data[1] != UPDATE_CHUNK || info.data[2] != 1) {
        *error = "The connected node does not support safe firmware update.";
        return false;
    }
    if (request.size > get32(info.data.data() + 4)) {
        *error = "The node firmware does not fit its OTA slot.";
        return false;
    }
    uint8_t digest[32];
    if (!parseHex(request.sha256, digest)) {
        *error = "The node firmware SHA-256 is invalid.";
        return false;
    }
    uint32_t session = esp_random();
    if (!session) session = 1;
    std::vector<uint8_t> begin{UPDATE_PROTOCOL};
    put32(begin, session);
    put32(begin, request.size);
    begin.insert(begin.end(), digest, digest + 32);
    driver.writeRaw(WRITE_UPDATE_BEGIN, begin);
    Status current;
    if (!status(driver, &current) || current.state != STATE_RECEIVING || current.session != session || current.error) {
        *error = "The node rejected the firmware update.";
        return false;
    }

    std::vector<uint8_t> pending;
    size_t pendingOffset = 0;
    auto sendChunk = [&](const uint8_t* data, size_t length, size_t offset) -> bool {
        std::vector<uint8_t> payload;
        put32(payload, session);
        put32(payload, offset);
        payload.push_back(length);
        payload.insert(payload.end(), data, data + length);
        payload.resize(9 + UPDATE_CHUNK, 0);
        for (int attempt = 0; attempt < 6; attempt++) {
            driver.writeRaw(WRITE_UPDATE_DATA, payload);
            if (status(driver, &current, 1) && current.session == session &&
                current.offset >= offset + length && !current.error)
                return true;
        }
        return false;
    };
    const bool downloaded = download(request, progress,
        [&](const uint8_t* data, size_t length, size_t offset, std::string* why) {
            size_t at = 0;
            while (at < length) {
                const size_t take = std::min(UPDATE_CHUNK - pending.size(), length - at);
                if (pending.empty()) pendingOffset = offset + at;
                pending.insert(pending.end(), data + at, data + at + take);
                at += take;
                if (pending.size() == UPDATE_CHUNK && !sendChunk(pending.data(), pending.size(), pendingOffset)) {
                    *why = "The node stopped acknowledging firmware data at byte " + std::to_string(pendingOffset) + ".";
                    return false;
                }
                if (pending.size() == UPDATE_CHUNK) pending.clear();
            }
            return true;
        }, error);
    if (!downloaded || (!pending.empty() && !sendChunk(pending.data(), pending.size(), pendingOffset))) {
        driver.writeRaw(WRITE_UPDATE_ABORT, {(uint8_t)(session >> 24), (uint8_t)(session >> 16),
                                             (uint8_t)(session >> 8), (uint8_t)session});
        if (downloaded) *error = "The node did not acknowledge the final firmware block.";
        return false;
    }
    if (current.state != STATE_READY || current.offset != request.size) {
        *error = "The node did not receive the complete firmware image.";
        return false;
    }
    std::vector<uint8_t> finish;
    put32(finish, session);
    driver.writeRaw(WRITE_UPDATE_FINISH, finish);
    if (progress) progress("restarting node", request.size, request.size);
    for (int attempt = 0; attempt < 60; attempt++) {
        if (progress) progress("restarting node", attempt, 60);
        driver.writeRaw(WRITE_UPDATE_CONFIRM, {UPDATE_PROTOCOL});
        Status post;
        if (status(driver, &post, 0) && post.state == 0 && !post.error)
            return true;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    *error = "The updated node did not return for confirmation.";
    return false;
}

bool updateRelayFirmware(const FirmwareRequest& request, const FirmwareProgress& progress,
                         std::string* error)
{
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (!target || request.size > target->size) {
        *error = "No relay OTA slot can hold this firmware.";
        return false;
    }
    esp_ota_handle_t ota = 0;
    if (esp_ota_begin(target, request.size, &ota) != ESP_OK) {
        *error = "Could not begin the relay OTA write.";
        return false;
    }
    const bool ok = download(request, progress,
        [&](const uint8_t* data, size_t length, size_t, std::string* why) {
            if (esp_ota_write(ota, data, length) == ESP_OK)
                return true;
            *why = "Writing the relay OTA partition failed.";
            return false;
        }, error);
    if (!ok) {
        esp_ota_abort(ota);
        return false;
    }
    if (esp_ota_end(ota) != ESP_OK) {
        *error = "The downloaded relay image is invalid.";
        return false;
    }
    esp_app_desc_t desc = {};
    if (esp_ota_get_partition_description(target, &desc) != ESP_OK || strcmp(desc.project_name, "rhrelay_esp32c5")) {
        *error = "The image is not S3 relay firmware.";
        return false;
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        *error = "Could not select the new relay firmware.";
        return false;
    }
    if (progress) progress("restarting relay", request.size, request.size);
    return true;
}

} // namespace relay
