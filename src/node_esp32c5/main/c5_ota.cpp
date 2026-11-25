// Loss-tolerant, transport-independent application update for the C5 node.
// RotorHazard uses it over a wired serial port; the S3 relay uses the same
// messages over AirPort. The running application remains selected until the
// complete ESP image and its caller-supplied SHA-256 have both verified.
#include "c5_rhglue.h"

#include <string.h>

#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

namespace c5 {
namespace {

enum State : uint8_t { Idle = 0, Receiving = 1, Ready = 2, PendingConfirm = 3, Failed = 4 };
enum Error : uint16_t {
    Ok = 0, BadProtocol = 1, BadSize = 2, NoPartition = 3, BeginFailed = 4,
    WrongSession = 5, WrongOffset = 6, BadLength = 7, WriteFailed = 8,
    Incomplete = 9, BadDigest = 10, InvalidImage = 11, BootSelectFailed = 12,
    ConfirmFailed = 13,
};

State state = Idle;
uint16_t error = Ok;
uint32_t session = 0, nextOffset = 0, totalSize = 0;
uint8_t expectedSha[32] = {};
const esp_partition_t* target = nullptr;
esp_ota_handle_t handle = 0;
mbedtls_sha256_context sha;
bool shaLive = false;
int64_t confirmDeadlineUs = 0;

uint32_t get32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void put16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
void put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

void reply(Transport& out, uint8_t* data, size_t n)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum += data[i];
    data[n] = sum;
    out.write(data, n + 1);
}

void fail(Error e)
{
    error = e;
    state = Failed;
    if (handle) {
        esp_ota_abort(handle);
        handle = 0;
    }
    if (shaLive) {
        mbedtls_sha256_free(&sha);
        shaLive = false;
    }
}

void abortUpdate()
{
    if (handle) esp_ota_abort(handle);
    handle = 0;
    if (shaLive) mbedtls_sha256_free(&sha);
    shaLive = false;
    state = Idle;
    error = Ok;
    session = nextOffset = totalSize = 0;
    target = nullptr;
}

void begin(const uint8_t* d)
{
    const uint8_t protocol = d[0];
    const uint32_t newSession = get32(d + 1);
    const uint32_t size = get32(d + 5);
    if (protocol != kFwUpdateProtocol) { fail(BadProtocol); return; }
    target = esp_ota_get_next_update_partition(nullptr);
    if (!target) { fail(NoPartition); return; }
    if (!size || size > target->size) { fail(BadSize); return; }

    // BEGIN is idempotent while its transfer is alive.
    if (state == Receiving && session == newSession && totalSize == size && !memcmp(expectedSha, d + 9, 32))
        return;
    abortUpdate();
    target = esp_ota_get_next_update_partition(nullptr);
    session = newSession;
    totalSize = size;
    memcpy(expectedSha, d + 9, 32);
    if (esp_ota_begin(target, size, &handle) != ESP_OK) { fail(BeginFailed); return; }
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0) { fail(BeginFailed); return; }
    shaLive = true;
    state = Receiving;
    error = Ok;
}

void data(const uint8_t* d)
{
    const uint32_t gotSession = get32(d);
    const uint32_t offset = get32(d + 4);
    const uint8_t length = d[8];
    if (state != Receiving || gotSession != session) { error = WrongSession; return; }
    if (!length || length > kFwUpdateChunkSize || offset + length > totalSize) { error = BadLength; return; }
    // The host may repeat the last chunk when STATUS was lost.
    if (offset < nextOffset && offset + length == nextOffset) { error = Ok; return; }
    if (offset != nextOffset) { error = WrongOffset; return; }
    if (esp_ota_write(handle, d + 9, length) != ESP_OK || mbedtls_sha256_update(&sha, d + 9, length) != 0) {
        fail(WriteFailed);
        return;
    }
    nextOffset += length;
    error = Ok;
    if (nextOffset == totalSize) state = Ready;
}

void finish(const uint8_t* d)
{
    if (get32(d) != session) { error = WrongSession; return; }
    if (state != Ready || nextOffset != totalSize) { error = Incomplete; return; }
    uint8_t actual[32];
    if (!shaLive || mbedtls_sha256_finish(&sha, actual) != 0) { fail(BadDigest); return; }
    mbedtls_sha256_free(&sha);
    shaLive = false;
    if (memcmp(actual, expectedSha, sizeof actual)) { fail(BadDigest); return; }
    esp_err_t result = esp_ota_end(handle);
    handle = 0;
    if (result != ESP_OK) { fail(InvalidImage); return; }
    esp_app_desc_t desc = {};
    if (esp_ota_get_partition_description(target, &desc) != ESP_OK || strcmp(desc.project_name, "rhnode_esp32c5")) {
        fail(InvalidImage);
        return;
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) { fail(BootSelectFailed); return; }
    state = PendingConfirm;
    error = Ok;
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

} // namespace

void fwUpdateWriteInfo(Transport& out)
{
    uint8_t d[kFwUpdateInfoSize + 1] = {};
    d[0] = kFwUpdateProtocol;
    d[1] = kFwUpdateChunkSize;
    d[2] = 1; // target: ESP32-C5 node
    d[3] = 1; // rollback confirmation supported
    const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
    put32(d + 4, p ? p->size : 0);
    const esp_partition_t* running = esp_ota_get_running_partition();
    put32(d + 8, running ? running->address : 0);
    put32(d + 12, p ? p->address : 0);
    reply(out, d, kFwUpdateInfoSize);
}

void fwUpdateWriteStatus(Transport& out)
{
    uint8_t d[kFwUpdateStatusSize + 1] = {};
    d[0] = kFwUpdateProtocol;
    d[1] = state;
    put16(d + 2, error);
    put32(d + 4, session);
    put32(d + 8, nextOffset);
    put32(d + 12, totalSize);
    reply(out, d, kFwUpdateStatusSize);
}

int fwUpdatePayloadSize(uint8_t command)
{
    switch (command) {
    case kWriteFwUpdateBegin: return kFwUpdateBeginSize;
    case kWriteFwUpdateData: return kFwUpdateDataSize;
    case kWriteFwUpdateFinish:
    case kWriteFwUpdateAbort: return 4;
    case kWriteFwUpdateConfirm: return 1;
    default: return -1;
    }
}

void fwUpdateHandle(uint8_t command, const uint8_t* d, size_t)
{
    switch (command) {
    case kWriteFwUpdateBegin: begin(d); break;
    case kWriteFwUpdateData: data(d); break;
    case kWriteFwUpdateFinish: finish(d); break;
    case kWriteFwUpdateAbort:
        if (!session || get32(d) == session) abortUpdate();
        break;
    case kWriteFwUpdateConfirm:
        if (d[0] == kFwUpdateProtocol) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                state = Idle;
                error = Ok;
                confirmDeadlineUs = 0;
            } else {
                error = ConfirmFailed;
            }
        }
        break;
    }
}

bool fwUpdateBusy() { return state == Receiving || state == Ready; }

void fwUpdateInit()
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t imageState;
    if (running && esp_ota_get_state_partition(running, &imageState) == ESP_OK && imageState == ESP_OTA_IMG_PENDING_VERIFY) {
        state = PendingConfirm;
        confirmDeadlineUs = esp_timer_get_time() + 120LL * 1000000LL;
    }
}

void fwUpdateTick()
{
    if (state == PendingConfirm && confirmDeadlineUs && esp_timer_get_time() >= confirmDeadlineUs) {
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

} // namespace c5
