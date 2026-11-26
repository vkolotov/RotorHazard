#include "osd_backpack.h"

#include <cstdio>
#include <vector>

#include "backpack.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md5.h"
#include "soc/soc_caps.h"

using namespace relay;

// The last HP UART: UART2 on the S3 (the C5 has none spare and builds without a backpack).
constexpr uart_port_t BACKPACK_UART = (uart_port_t)(SOC_UART_HP_NUM - 1);
constexpr int BACKPACK_BAUD = 460800;  // the Timer Backpack's fixed MSP rate

static SemaphoreHandle_t g_lock;
static bool g_ready = false;
static std::string g_last = "not used yet";

bool backpackWired() { return RELAY_BACKPACK_TX >= 0 && RELAY_BACKPACK_RX >= 0; }

void backpackInit()
{
    g_lock = xSemaphoreCreateMutex();
    if (!backpackWired())
        return;
    uart_config_t config = {};
    config.baud_rate = BACKPACK_BAUD;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    // Same order as the node link: parameters, pins, driver.
    if (uart_param_config(BACKPACK_UART, &config) != ESP_OK ||
        uart_set_pin(BACKPACK_UART, RELAY_BACKPACK_TX, RELAY_BACKPACK_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK ||
        uart_driver_install(BACKPACK_UART, 1024, 0, 0, nullptr, 0) != ESP_OK) {
        g_last = "UART setup failed";
        return;
    }
    g_ready = true;
}

static void sendFrame(const std::vector<uint8_t>& frame)
{
    uart_write_bytes(BACKPACK_UART, frame.data(), frame.size());
    uart_wait_tx_done(BACKPACK_UART, pdMS_TO_TICKS(50));
}

static void pilotUid(const std::string& phrase, uint8_t uid[6])
{
    const std::string input = bindPhraseHashInput(phrase);
    uint8_t digest[16];
    mbedtls_md5((const unsigned char*)input.data(), input.size(), digest);
    uidFromDigest(digest, uid);
}

static std::string uidText(const uint8_t uid[6])
{
    char b[18];
    snprintf(b, sizeof b, "%02x%02x%02x%02x%02x%02x", uid[0], uid[1], uid[2], uid[3], uid[4], uid[5]);
    return b;
}

std::string backpackVersion()
{
    if (!g_ready)
        return "";
    xSemaphoreTake(g_lock, portMAX_DELAY);
    uart_flush_input(BACKPACK_UART);
    sendFrame(mspV2Frame(MSP_BACKPACK_VERSION));
    std::vector<uint8_t> got;
    std::string version;
    const TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(300);
    while (xTaskGetTickCount() < end) {
        uint8_t buf[64];
        const int r = uart_read_bytes(BACKPACK_UART, buf, sizeof buf, pdMS_TO_TICKS(20));
        if (r > 0)
            got.insert(got.end(), buf, buf + r);
        uint16_t fn;
        std::vector<uint8_t> payload;
        if (mspV2FindReply(got, fn, payload) && fn == MSP_BACKPACK_VERSION) {
            for (uint8_t c : payload)
                if (c)
                    version += (char)c;
            break;
        }
    }
    g_last = version.empty() ? "no answer to the version request (" + std::to_string(got.size()) + " bytes)" : "version " + version;
    xSemaphoreGive(g_lock);
    return version;
}

static bool sendOsd(const std::vector<std::vector<uint8_t>>& frames, const std::string& what)
{
    if (!g_ready)
        return false;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    for (const auto& f : frames) {
        sendFrame(f);
        vTaskDelay(pdMS_TO_TICKS(5));  // the backpack queues each ESP-NOW send
    }
    g_last = what;
    xSemaphoreGive(g_lock);
    return true;
}

bool backpackOsd(const std::string& phrase, int row, const std::string& text)
{
    uint8_t uid[6];
    pilotUid(phrase, uid);
    return sendOsd(osdMessageFrames(uid, row, text), "OSD \"" + text + "\" row " + std::to_string(row) + " to uid " + uidText(uid));
}

bool backpackOsdClear(const std::string& phrase)
{
    uint8_t uid[6];
    pilotUid(phrase, uid);
    std::vector<uint8_t> to = {1};
    to.insert(to.end(), uid, uid + 6);
    return sendOsd({mspV2Frame(MSP_SET_SEND_UID, to), mspV2Frame(MSP_SET_OSD, {2}), mspV2Frame(MSP_SET_OSD, {4}),
                    mspV2Frame(MSP_SET_SEND_UID, {0})},
                   "OSD cleared for uid " + uidText(uid));
}

struct Sweep {
    uint8_t uid[6];
    std::string text;
};
static volatile bool g_sweeping = false;

static void sweepTask(void* arg)
{
    Sweep* sweep = static_cast<Sweep*>(arg);
    for (int row = 0; row < OSD_ROWS; row++) {
        sendOsd(osdMessageFrames(sweep->uid, row, sweep->text), "OSD sweep row " + std::to_string(row) + " to uid " + uidText(sweep->uid));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    std::vector<uint8_t> to = {1};
    to.insert(to.end(), sweep->uid, sweep->uid + 6);
    sendOsd({mspV2Frame(MSP_SET_SEND_UID, to), mspV2Frame(MSP_SET_OSD, {2}), mspV2Frame(MSP_SET_OSD, {4}), mspV2Frame(MSP_SET_SEND_UID, {0})},
            "OSD sweep done to uid " + uidText(sweep->uid));
    delete sweep;
    g_sweeping = false;
    vTaskDelete(nullptr);
}

bool backpackOsdSweep(const std::string& phrase, const std::string& text)
{
    if (!g_ready || g_sweeping)
        return false;
    Sweep* sweep = new Sweep;
    pilotUid(phrase, sweep->uid);
    sweep->text = text;
    g_sweeping = true;
    if (xTaskCreate(sweepTask, "osdsweep", 4096, sweep, 3, nullptr) != pdPASS) {
        delete sweep;
        g_sweeping = false;
        return false;
    }
    return true;
}

bool backpackWifi()
{
    return sendOsd({mspV2Frame(MSP_BACKPACK_SET_MODE, {'W'})}, "asked to start Wi-Fi");
}

std::string backpackSummary()
{
    if (!backpackWired())
        return "backpack not built in";
    return "backpack UART2 tx " + std::to_string(RELAY_BACKPACK_TX) + " rx " + std::to_string(RELAY_BACKPACK_RX) + " @ " +
           std::to_string(BACKPACK_BAUD) + ": " + (g_ready ? g_last : "not ready (" + g_last + ")");
}
