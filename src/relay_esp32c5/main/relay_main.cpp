// The C5 relay: drives a RotorHazard node over a UART (the AirPort radio link
// or a wire) and is a mach1 timer over Wi-Fi. No Raspberry Pi, no RotorHazard
// server, no laptop.
//
// Tasks:
//   timer   owns the node link, the node driver and the mach1 timer core
//           (core/), all single-threaded. Everything else reaches it through
//           its event queue.
//   sender  sends what the timer queues for mach1, so a slow network never
//           holds up the node sweep.
//   usb     the native USB port: the console (settings in NVS, status); the
//           USB link (core/usb_link.h), where mission control on a laptop
//           carries the relay's mach1 connection while the relay stays the
//           timer; or serial pass-through, where a USB host (mach1's browser
//           timer over Web Serial) drives the node through the relay's radio
//           link instead of the relay. Wi-Fi stays up meanwhile.
//   websocket client (esp_websocket_client) and Wi-Fi run their own tasks.
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <sys/time.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "hal/uart_ll.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "feed_url.h"
#include "relay.h"
#include "mach1_timer.h"
#include "node_driver.h"
#include "node_protocol.h"
#include "usb_link.h"

using namespace relay;

static const char* TAG = "relay";

constexpr uart_port_t LINK_UART = UART_NUM_1;
constexpr int LINK_BAUD = 921600;  // the node's fixed rate (S32_BPill, C5, AirPort)
constexpr int64_t GATE_RETRY_MS = 3000;
constexpr int LOG_LINES = 40;
// Serial pass-through ends after this long without a byte from the USB host;
// the browser timer polls continuously while it drives the node.
constexpr int64_t SERIAL_IDLE_MS = 3000;

// Who drives the node link. The USB task asks (Requested); the timer task
// stops polling and hands the UART over (Granted); the USB task gives it back
// (Off) and the timer task reconnects to the node.
enum SerialMode : int { SerialOff = 0, SerialRequested = 1, SerialGranted = 2 };
static std::atomic<int> g_serialMode{SerialOff};

// The USB link (core/usb_link.h): one JSON object per line, starting with '{'.
constexpr size_t LINK_LINE_MAX = 16384;
// mach1's messages go to the USB host (link_send) instead of the Wi-Fi socket.
static std::atomic<bool> g_linkRoute{false};
// A page is on the USB link: logs stay off the port.
static std::atomic<bool> g_linkAlive{false};
static SemaphoreHandle_t g_usbLock;  // whole lines to the USB host
// The probe's answer (link_info), kept current by the timer task: the USB task
// answers 0x01 at once, even while the timer task is busy (a gate retry
// blocks it for about a second).
static std::string g_linkInfo;  // under g_statusLock

// ---------------------------------------------------------------------------
// Settings (NVS namespace "relay")

static std::string nvsString(nvs_handle_t h, const char* key, const std::string& fallback)
{
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0)
        return fallback;
    std::string value(len, '\0');
    nvs_get_str(h, key, value.data(), &len);
    value.resize(len - 1);
    return value;
}

Settings loadSettings()
{
    Settings s;
    nvs_handle_t h;
    if (nvs_open("relay", NVS_READONLY, &h) != ESP_OK)
        return s;
    s.ssid = nvsString(h, "ssid", "");
    s.password = nvsString(h, "password", "");
    s.feed = nvsString(h, "feed", "");
    s.name = nvsString(h, "name", s.name);
    int32_t minLap;
    if (nvs_get_i32(h, "minlap", &minLap) == ESP_OK)
        s.minLapMs = minLap;
    int32_t ignore;
    if (nvs_get_i32(h, "ignorems", &ignore) == ESP_OK)
        s.ignoreAfterStartMs = ignore;
    s.c5Detection = nvsString(h, "c5det", "");
    s.c5Input = nvsString(h, "c5in", "");
    s.nodeId = nvsString(h, "nodeid", "");
    int32_t pending;
    if (nvs_get_i32(h, "namepend", &pending) == ESP_OK)
        s.namePending = pending != 0;
    nvs_close(h);
    return s;
}

static bool saveString(const char* key, const std::string& value)
{
    nvs_handle_t h;
    if (nvs_open("relay", NVS_READWRITE, &h) != ESP_OK)
        return false;
    const bool ok = nvs_set_str(h, key, value.c_str()) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static bool saveInt(const char* key, int32_t value)
{
    nvs_handle_t h;
    if (nvs_open("relay", NVS_READWRITE, &h) != ESP_OK)
        return false;
    const bool ok = nvs_set_i32(h, key, value) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool saveSettings(const Settings& s)
{
    return saveString("ssid", s.ssid) && saveString("password", s.password) && saveString("feed", s.feed) &&
           saveString("name", s.name) && saveInt("minlap", s.minLapMs) && saveInt("namepend", s.namePending ? 1 : 0);
}

static SemaphoreHandle_t g_statusLock;
static std::string g_statusJson = "{}";

void setStatusJson(const std::string& json)
{
    xSemaphoreTake(g_statusLock, portMAX_DELAY);
    g_statusJson = json;
    xSemaphoreGive(g_statusLock);
}

std::string statusJson()
{
    xSemaphoreTake(g_statusLock, portMAX_DELAY);
    std::string copy = g_statusJson;
    xSemaphoreGive(g_statusLock);
    return copy;
}

// ---------------------------------------------------------------------------
// The node link: a UART

// Raw bytes over the node link since boot, for the `link` console command.
static std::atomic<uint32_t> g_uartTx{0}, g_uartRx{0};

class UartLink : public NodeLink {
public:
    void begin()
    {
        uart_config_t config = {};
        config.baud_rate = LINK_BAUD;
        config.data_bits = UART_DATA_8_BITS;
        config.parity = UART_PARITY_DISABLE;
        config.stop_bits = UART_STOP_BITS_1;
        config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        config.source_clk = UART_SCLK_DEFAULT;
        // ESP-IDF's order: parameters (which start the UART's clock), pins,
        // then the driver. Installing first hung in uart_ll_update() after a
        // software restart (interrupt watchdog, boot loop).
        ESP_ERROR_CHECK(uart_param_config(LINK_UART, &config));
        ESP_ERROR_CHECK(uart_set_pin(LINK_UART, RELAY_LINK_TX, RELAY_LINK_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 2048, 0, 0, nullptr, 0));
    }
    void flushInput() override { uart_flush_input(LINK_UART); }
    void write(const uint8_t* data, size_t n) override
    {
        const int w = uart_write_bytes(LINK_UART, data, n);
        if (w > 0)
            g_uartTx += w;
    }
    size_t read(uint8_t* data, size_t n, uint32_t timeoutMs) override
    {
        const int64_t deadline = esp_timer_get_time() + (int64_t)timeoutMs * 1000;
        size_t got = 0;
        while (got < n) {
            const int64_t left = deadline - esp_timer_get_time();
            if (left <= 0)
                break;
            const TickType_t ticks = pdMS_TO_TICKS((left + 999) / 1000);
            const int r = uart_read_bytes(LINK_UART, data + got, n - got, ticks ? ticks : 1);
            if (r > 0)
                got += r;
        }
        g_uartRx += got;
        return got;
    }
    int64_t nowUs() override { return esp_timer_get_time(); }
    void sleepMs(uint32_t ms) override { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }
};

// ---------------------------------------------------------------------------
// Queues between the tasks

enum class QueueKind : uint8_t { SocketOpen, SocketClosed, SocketText, Console, Refused, Link };
struct Event {
    QueueKind kind;
    char* text;  // malloc'd or null; the timer task frees it
};

static QueueHandle_t g_events;  // -> timer task
// timer task -> sender task: malloc'd strings, the first byte says where to:
// 'M' a message for mach1 (Wi-Fi socket or USB link, whichever is the route
// when it is sent), 'U' a line for the USB host.
static QueueHandle_t g_outbox;
static esp_websocket_client_handle_t g_ws;
static volatile bool g_wsConnected;

static void post(QueueKind kind, const char* text, size_t len)
{
    Event e{kind, nullptr};
    if (text) {
        e.text = (char*)malloc(len + 1);
        if (!e.text)
            return;
        memcpy(e.text, text, len);
        e.text[len] = 0;
    }
    if (xQueueSend(g_events, &e, pdMS_TO_TICKS(100)) != pdTRUE)
        free(e.text);
}

static void postText(QueueKind kind, const char* text) { post(kind, text, text ? strlen(text) : 0); }

// ---------------------------------------------------------------------------
// mach1 socket

static std::string g_frame;  // a text message arriving in pieces

static void wsEvent(void*, esp_event_base_t, int32_t id, void* data)
{
    auto* d = (esp_websocket_event_data_t*)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        g_wsConnected = true;
        post(QueueKind::SocketOpen, nullptr, 0);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (g_wsConnected) {
            g_wsConnected = false;
            post(QueueKind::SocketClosed, nullptr, 0);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x08 && d->data_len >= 2) {
            // Close: 4403 is mach1 refusing the club key.
            const int code = ((uint8_t)d->data_ptr[0] << 8) | (uint8_t)d->data_ptr[1];
            if (code == 4403)
                post(QueueKind::Refused, nullptr, 0);
        } else if (d->op_code == 0x01 || d->op_code == 0x00) {
            if (d->payload_offset == 0)
                g_frame.clear();
            g_frame.append(d->data_ptr, d->data_len);
            if (d->payload_offset + d->data_len >= d->payload_len) {
                post(QueueKind::SocketText, g_frame.data(), g_frame.size());
                g_frame.clear();
            }
        }
        break;
    default:
        break;
    }
}

// One line to the USB host, whole: a page skips a line that is not JSON.
static void usbLine(const std::string& line)
{
    if (g_serialMode.load() != SerialOff || !usb_serial_jtag_is_connected())
        return;
    xSemaphoreTake(g_usbLock, portMAX_DELAY);
    usb_serial_jtag_write_bytes(line.data(), line.size(), pdMS_TO_TICKS(200));
    usb_serial_jtag_write_bytes("\n", 1, pdMS_TO_TICKS(200));
    xSemaphoreGive(g_usbLock);
}

static void queueOut(char where, const std::string& text)
{
    char* copy = (char*)malloc(text.size() + 2);
    if (!copy)
        return;
    copy[0] = where;
    memcpy(copy + 1, text.c_str(), text.size() + 1);
    if (xQueueSend(g_outbox, &copy, 0) != pdTRUE)
        free(copy);
}

static void senderTask(void*)
{
    for (;;) {
        char* item;
        if (xQueueReceive(g_outbox, &item, portMAX_DELAY) != pdTRUE)
            continue;
        const char* text = item + 1;
        if (item[0] == 'U') {
            usbLine(text);
        } else if (g_linkRoute.load()) {
            usbLine(UsbLink::wrap(text));
        } else if (g_ws && g_wsConnected && esp_websocket_client_is_connected(g_ws)) {
            // Dropped while mach1 is away: race messages are resent from
            // `pending` on the next connect, everything else is sent afresh.
            esp_websocket_client_send_text(g_ws, text, strlen(text), pdMS_TO_TICKS(5000));
        }
        free(item);
    }
}

static void startSocket(const std::string& url)
{
    esp_websocket_client_config_t config = {};
    config.uri = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 4096;
    config.reconnect_timeout_ms = 3000;
    config.network_timeout_ms = 10000;
    config.task_stack = 6144;
    g_ws = esp_websocket_client_init(&config);
    esp_websocket_register_events(g_ws, WEBSOCKET_EVENT_ANY, wsEvent, nullptr);
    esp_websocket_client_start(g_ws);
}

// ---------------------------------------------------------------------------
// Wi-Fi and the wall clock

static bool clockValid() { return time(nullptr) > 1700000000; }

// The page's clock on the USB link, for a relay that has none (no Wi-Fi, no SNTP).
static void clockFromHost(int64_t epochMs)
{
    if (clockValid())
        return;
    struct timeval tv;
    tv.tv_sec = epochMs / 1000;
    tv.tv_usec = (epochMs % 1000) * 1000;
    settimeofday(&tv, nullptr);
}

static bool toEpoch(int64_t monoMs, int64_t* epochMs)
{
    if (!clockValid())
        return false;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const int64_t nowEpoch = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    *epochMs = nowEpoch - (esp_timer_get_time() / 1000 - monoMs);
    return true;
}

// ---------------------------------------------------------------------------
// The timer task

static std::deque<std::string> g_log;  // timer task only

static void addLog(const std::string& line)
{
    ESP_LOGI(TAG, "%s", line.c_str());
    char stamp[16] = "--:--:--";
    if (clockValid()) {
        time_t t = time(nullptr);
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(stamp, sizeof stamp, "%H:%M:%SZ", &tm);
    }
    g_log.push_front(std::string(stamp) + " " + line);
    while (g_log.size() > LOG_LINES)
        g_log.pop_back();
}

static std::string hexId(int bytes)
{
    std::string out;
    for (int i = 0; i < bytes; i++) {
        char b[3];
        snprintf(b, sizeof b, "%02x", (unsigned)(esp_random() & 0xFF));
        out += b;
    }
    return out;
}

struct TimerContext {
    Settings settings;
    std::string uid;
    std::string socketUrl;
};

static UsbLink* g_link;  // timer task only

static void printStatus(const TimerContext& ctx, NodeDriver& driver, Mach1Timer& timer)
{
    const int64_t now = esp_timer_get_time() / 1000;
    printf(RELAY_BOARD "-relay %s  uid %s  name \"%s\"\n", RELAY_VERSION, ctx.uid.c_str(), ctx.settings.name.c_str());
    printf("link     UART1 tx %d rx %d @ %d\n", RELAY_LINK_TX, RELAY_LINK_RX, LINK_BAUD);
    printf("wifi     %s %s  ssid \"%s\"%s   clock %s\n", networkUp() ? "up" : "down", networkIp().c_str(), ctx.settings.ssid.c_str(), hotspotOn() ? ("  hotspot " + hotspotSsid()).c_str() : "",
           clockValid() ? "synced" : "not synced");
    printf("mach1    %s%s  %s  pending %u%s\n", timer.socketConnected() ? "connected" : "not connected",
           g_linkRoute.load() ? " through USB" : "", timer.club().c_str(), (unsigned)timer.pendingCount(),
           timer.watching(now) ? "  (watched)" : "");
    printf("         %s\n", ctx.socketUrl.empty() ? "no feed URL: set one with `feed`" : ctx.socketUrl.c_str());
    const NodeInfo& info = driver.info();
    printf("node     %s %s  %s %s API %d, %d seats  %.1f sweeps/s  rtt %.1f ms\n", nodeStatusName(driver.status()),
           driver.detail().c_str(), info.procType.c_str(), info.firmware.c_str(), info.apiLevel, info.seats,
           driver.sweepsPerSecond(), driver.minRttUs() / 1000.0);
    const LinkCounters& c = driver.counters();
    printf("         reads %lu writes %lu timeouts %lu checksum %lu\n", (unsigned long)c.reads, (unsigned long)c.writes,
           (unsigned long)c.timeouts, (unsigned long)c.checksumErrors);
    printf("race     %s  %s  min lap %lld ms\n", raceStatusName(timer.engine.status),
           timer.engine.hasFormat ? timer.engine.format.name.c_str() : "", (long long)timer.engine.settings.minLapMs);
    for (size_t i = 0; i < driver.seats().size(); i++) {
        const SeatState& s = driver.seats()[i];
        const std::string callsign = timer.seatCallsign((int)i);
        printf("  node %u  %4d MHz  enter %4d exit %4d  rssi %4d  peak %4d%s  %s  laps %u\n", (unsigned)i + 1,
               s.frequency, s.enter, s.exit, s.rssi, s.passPeak, s.crossing ? " X" : "  ", callsign.c_str(),
               (unsigned)timer.engine.laps((int)i).size());
    }
}

// What the setup page shows (`/status.json`).
static void publishStatus(const TimerContext& ctx, NodeDriver& driver, Mach1Timer& timer)
{
    const int64_t now = esp_timer_get_time() / 1000;
    cJSON* m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "version", RELAY_VERSION);
    cJSON_AddStringToObject(m, "uid", ctx.uid.c_str());
    cJSON_AddStringToObject(m, "name", ctx.settings.name.c_str());
    cJSON* wifi = cJSON_AddObjectToObject(m, "wifi");
    cJSON_AddBoolToObject(wifi, "up", networkUp());
    cJSON_AddStringToObject(wifi, "ip", networkIp().c_str());
    cJSON_AddStringToObject(wifi, "ssid", ctx.settings.ssid.c_str());
    cJSON_AddBoolToObject(wifi, "hotspot", hotspotOn());
    cJSON_AddStringToObject(wifi, "hotspot_ssid", hotspotSsid().c_str());
    cJSON_AddBoolToObject(m, "clock", clockValid());
    cJSON* mach1 = cJSON_AddObjectToObject(m, "mach1");
    cJSON_AddBoolToObject(mach1, "configured", !ctx.socketUrl.empty());
    cJSON_AddBoolToObject(mach1, "connected", timer.socketConnected());
    cJSON_AddStringToObject(mach1, "via", g_linkRoute.load() ? "usb" : "wifi");
    cJSON_AddStringToObject(mach1, "club", timer.club().c_str());
    cJSON_AddNumberToObject(mach1, "pending", (double)timer.pendingCount());
    cJSON_AddBoolToObject(mach1, "watched", timer.watching(now));
    const NodeInfo& info = driver.info();
    const LinkCounters& c = driver.counters();
    cJSON* node = cJSON_AddObjectToObject(m, "node");
    cJSON_AddStringToObject(node, "status", nodeStatusName(driver.status()));
    cJSON_AddStringToObject(node, "detail", driver.detail().c_str());
    cJSON_AddStringToObject(node, "proc", info.procType.c_str());
    cJSON_AddStringToObject(node, "firmware", info.firmware.c_str());
    cJSON_AddNumberToObject(node, "api", info.apiLevel);
    cJSON_AddNumberToObject(node, "sweeps", driver.sweepsPerSecond());
    cJSON_AddNumberToObject(node, "rtt_ms", driver.minRttUs() / 1000.0);
    cJSON_AddNumberToObject(node, "reads", c.reads);
    cJSON_AddNumberToObject(node, "timeouts", c.timeouts);
    cJSON_AddNumberToObject(node, "checksum", c.checksumErrors);
    cJSON* race = cJSON_AddObjectToObject(m, "race");
    cJSON_AddStringToObject(race, "status", raceStatusName(timer.engine.status));
    cJSON_AddStringToObject(race, "format", timer.engine.hasFormat ? timer.engine.format.name.c_str() : "");
    cJSON_AddNumberToObject(race, "min_lap_ms", (double)timer.engine.settings.minLapMs);
    cJSON* seats = cJSON_AddArrayToObject(m, "seats");
    if (driver.status() == NodeStatus::Running) {
        for (size_t i = 0; i < driver.seats().size(); i++) {
            const SeatState& s = driver.seats()[i];
            cJSON* seat = cJSON_CreateObject();
            cJSON_AddNumberToObject(seat, "frequency", s.frequency);
            cJSON_AddNumberToObject(seat, "rssi", s.rssi);
            cJSON_AddNumberToObject(seat, "enter", s.enter);
            cJSON_AddNumberToObject(seat, "exit", s.exit);
            cJSON_AddStringToObject(seat, "callsign", timer.seatCallsign((int)i).c_str());
            cJSON_AddNumberToObject(seat, "laps", (double)timer.engine.laps((int)i).size());
            cJSON_AddItemToArray(seats, seat);
        }
    }
    if (g_link) {
        const std::string info = g_link->info(g_wsConnected);
        xSemaphoreTake(g_statusLock, portMAX_DELAY);
        g_linkInfo = info;
        xSemaphoreGive(g_statusLock);
    }
    cJSON* log = cJSON_AddArrayToObject(m, "log");
    for (const std::string& line : g_log)
        cJSON_AddItemToArray(log, cJSON_CreateString(line.c_str()));
    char* text = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    if (text) {
        setStatusJson(text);
        cJSON_free(text);
    }
}

static void consoleCommand(const std::string& line, TimerContext& ctx, NodeDriver& driver, Mach1Timer& timer)
{
    if (line == "status") {
        printStatus(ctx, driver, timer);
    } else if (line == "log") {
        for (auto it = g_log.rbegin(); it != g_log.rend(); ++it)
            printf("%s\n", it->c_str());
    } else if (line.rfind("name ", 0) == 0) {
        const std::string error = timer.rename(line.substr(5));
        printf("%s\n", error.empty() ? ("name \"" + timer.name + "\"").c_str() : error.c_str());
    } else if (line.rfind("minlap ", 0) == 0) {
        const int ms = atoi(line.c_str() + 7);
        timer.engine.settings.minLapMs = ms;
        ctx.settings.minLapMs = ms;
        printf("minimum lap %d ms\n", ms);
    }
}

#ifdef RELAY_DIAG
static volatile int g_diagStage = 0;
static void diagBeat(void*)
{
    for (int n = 0;; n++) {
        printf("hb %d stage %d heap %u\n", n, g_diagStage, (unsigned)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

static void timerTask(void* arg)
{
    auto* ctx = (TimerContext*)arg;
    static UartLink link;
#ifdef RELAY_DIAG
    printf("diag: link.begin()\n");
#endif
    link.begin();
#ifdef RELAY_DIAG
    printf("diag: link up\n");
#endif
    static NodeDriver driver(link);
    static Mach1Timer timer;
    timer.uid = ctx->uid;
    timer.session = hexId(16);
    timer.name = ctx->settings.name;
    timer.engine.settings.minLapMs = ctx->settings.minLapMs;
    timer.engine.settings.ignoreAfterStartMs = ctx->settings.ignoreAfterStartMs;
    timer.c5Detection = fromHex(ctx->settings.c5Detection);
    // mach1 knows the relay as the node it drives (c5-<node id>), else as itself.
    timer.relayUid = ctx->uid;
    timer.uid = ctx->settings.nodeId.empty() ? ctx->uid : "c5-" + ctx->settings.nodeId;
    timer.namePending = ctx->settings.namePending;
    // The USB link: mission control on a laptop carries the mach1 connection.
    static UsbLink usbLink(timer);
    g_link = &usbLink;
    usbLink.rhVersion = std::string(RELAY_BOARD "-relay ") + RELAY_VERSION;
    usbLink.toHost = [](const std::string& line) { queueOut('U', line); };
    usbLink.hostClock = [](int64_t epochMs) {
        if (!clockValid()) {
            clockFromHost(epochMs);
            addLog("Clock set from the laptop");
        }
    };
    usbLink.log = addLog;
    usbLink.routeChanged = [](bool viaLink) {
        g_linkRoute.store(viaLink);
        if (!g_ws)
            return;
        if (viaLink) {
            // mach1 gets one connection for this timer: the page's.
            esp_websocket_client_stop(g_ws);
            g_wsConnected = false;
        } else if (g_serialMode.load() == SerialOff || timer.uid.rfind("c5-", 0) != 0) {
            // (In pass-through a C5 node's timer is the USB host's, so not then.)
            esp_websocket_client_start(g_ws);
        }
    };
    timer.identity = [ctx](const std::string& uid) {
        // Another node: reconnect to mach1 as that node's timer.
        ctx->socketUrl = socketUrl(ctx->settings.feed, uid);
        if (g_ws && !ctx->socketUrl.empty()) {
            esp_websocket_client_stop(g_ws);
            esp_websocket_client_set_uri(g_ws, ctx->socketUrl.c_str());
            if (!g_linkRoute.load())
                esp_websocket_client_start(g_ws);
        }
        // A page on the USB link reconnects as it.
        g_link->identityChanged(g_wsConnected);
    };
    timer.c5Input = ctx->settings.c5Input;
    timer.toEpoch = toEpoch;
    timer.log = addLog;
    // Settings from mission control, kept across reboots.
    timer.saveSetting = [ctx](const std::string& key, const std::string& value) {
        if (key == "namepend") {
            saveInt("namepend", atoi(value.c_str()));
        } else if (key == "name" || key == "nodeid") {
            saveString(key.c_str(), value);
            if (key == "name")
                ctx->settings.name = value;
            else
                ctx->settings.nodeId = value;
        } else if (key == "minlap" || key == "ignorems") {
            saveInt(key.c_str(), atoi(value.c_str()));
            if (key == "minlap")
                ctx->settings.minLapMs = atoi(value.c_str());
            else
                ctx->settings.ignoreAfterStartMs = atoi(value.c_str());
        } else if (key == "c5det" || key == "c5in") {
            saveString(key.c_str(), value);
        }
    };
    timer.send = [](const std::string& text) { queueOut('M', text); };
    timer.reconnect = [] {
        if (g_linkRoute.load()) {
            // Through the page: it reopens its socket; Wi-Fi meanwhile.
            g_link->drop(esp_timer_get_time() / 1000, "mach1 went quiet through the laptop", true);
        } else if (g_ws) {
            esp_websocket_client_stop(g_ws);
            esp_websocket_client_start(g_ws);
        }
    };
    driver.onCrossing = [](int seat, int64_t atUs, int) {
        timer.crossing(seat, atUs / 1000, esp_timer_get_time() / 1000);
    };

    bool gateRunning = false;
    bool handedOver = false;  // the USB task has the link (serial pass-through)
    int64_t nextGateTry = 0;
    auto gateStep = [&](int64_t now) {
        if (gateRunning && driver.status() != NodeStatus::Running) {
            gateRunning = false;
            addLog("Gate link lost: " + driver.detail() + "; reconnecting");
            timer.gateDown(now);
            nextGateTry = now + GATE_RETRY_MS;
        }
        if (!gateRunning && now >= nextGateTry) {
            std::string error;
            if (driver.start(&error)) {
                gateRunning = true;
                timer.gateUp(&driver, esp_timer_get_time() / 1000);
            } else {
                static std::string lastError;
                if (error != lastError)
                    addLog("Gate: " + error);
                lastError = error;
                nextGateTry = esp_timer_get_time() / 1000 + GATE_RETRY_MS;
            }
        }
        if (gateRunning) {
            if (!driver.poll())
                vTaskDelay(pdMS_TO_TICKS(10));  // every seat off
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    };
    for (;;) {
        Event e;
        while (xQueueReceive(g_events, &e, 0) == pdTRUE) {
            const int64_t now = esp_timer_get_time() / 1000;
            // The Wi-Fi socket's events, from before it was closed for the USB link, are stale.
            const bool viaUsb = g_linkRoute.load();
            switch (e.kind) {
            case QueueKind::SocketOpen: if (!viaUsb) timer.socketOpen(now); break;
            case QueueKind::SocketClosed: if (!viaUsb) timer.socketClosed(); break;
            case QueueKind::SocketText: if (!viaUsb) timer.message(e.text, now); break;
            case QueueKind::Refused: addLog("mach1 refused the club key: check the feed URL"); break;
            case QueueKind::Console: consoleCommand(e.text, *ctx, driver, timer); break;
            case QueueKind::Link:
                if (g_serialMode.load() == SerialOff)
                    usbLink.received(e.text, now, g_wsConnected);
                break;
            }
            free(e.text);
        }
        const int64_t now = esp_timer_get_time() / 1000;
        // Serial pass-through: stop using the link and hand it to the USB task.
        const int serial = g_serialMode.load();
        if (serial == SerialRequested) {
            // The USB port now carries the node protocol, not the link.
            usbLink.drop(now, "the USB host drives the node now", false);
            if (gateRunning) {
                gateRunning = false;
                timer.gateDown(now);
            }
            handedOver = true;
            addLog("Serial: a USB host drives the node");
            // The host (mach1 in a browser) is now this node's timer in mach1: step aside.
            if (g_ws && timer.uid.rfind("c5-", 0) == 0) {
                esp_websocket_client_stop(g_ws);
                timer.socketClosed();
                addLog("mach1: the USB host is the timer now; reconnecting when it lets go");
            }
            g_serialMode.store(SerialGranted);
        } else if (serial == SerialOff && handedOver) {
            handedOver = false;
            addLog("Serial ended; reconnecting to the node");
            nextGateTry = now;
            if (g_ws && !esp_websocket_client_is_connected(g_ws))
                esp_websocket_client_start(g_ws);
        }
        if (!handedOver) {
            static int64_t nextUsbCheck = 0;
            if (now >= nextUsbCheck) {
                nextUsbCheck = now + 100;
                usbLink.tick(now, usb_serial_jtag_is_connected(), g_wsConnected);
            }
        }
        g_linkAlive.store(usbLink.alive());
        if (handedOver)
            vTaskDelay(pdMS_TO_TICKS(20));
        else
            gateStep(now);
        timer.tick(esp_timer_get_time() / 1000);
        static int64_t nextStatus = 0;
        if (esp_timer_get_time() / 1000 >= nextStatus) {
            nextStatus = esp_timer_get_time() / 1000 + 500;
            publishStatus(*ctx, driver, timer);
        }
    }
}

// ---------------------------------------------------------------------------
// Console

static std::string joinArgs(int argc, char** argv, int from)
{
    std::string out;
    for (int i = from; i < argc; i++)
        out += (i > from ? " " : "") + std::string(argv[i]);
    return out;
}

static int cmdStatus(int, char**)
{
    postText(QueueKind::Console, "status");
    vTaskDelay(pdMS_TO_TICKS(100));
    return 0;
}

static int cmdLog(int, char**)
{
    postText(QueueKind::Console, "log");
    vTaskDelay(pdMS_TO_TICKS(100));
    return 0;
}

static int cmdWifi(int argc, char** argv)
{
    if (argc < 2) {
        printf("wifi <ssid> [password]\n");
        return 1;
    }
    const bool ok = saveString("ssid", argv[1]) && saveString("password", argc > 2 ? argv[2] : "");
    printf(ok ? "Saved; `reboot` to use it.\n" : "Could not save.\n");
    return ok ? 0 : 1;
}

static int cmdFeed(int argc, char** argv)
{
    if (argc != 2) {
        printf("feed <club feed URL from mach1, e.g. https://mach1.org.nz/<club>/feed/relay/?key=...>\n");
        return 1;
    }
    if (socketUrl(argv[1], "x").empty()) {
        printf("Not a mach1 feed URL: it needs /<club>/feed/<source>/ and ?key=.\n");
        return 1;
    }
    const bool ok = saveString("feed", argv[1]);
    printf(ok ? "Saved; `reboot` to use it.\n" : "Could not save.\n");
    return ok ? 0 : 1;
}

static int cmdName(int argc, char** argv)
{
    if (argc < 2) {
        printf("name <text>\n");
        return 1;
    }
    // The node keeps it too (and mach1 shows it): applied now, no reboot.
    postText(QueueKind::Console, ("name " + joinArgs(argc, argv, 1)).c_str());
    vTaskDelay(pdMS_TO_TICKS(100));
    return 0;
}

static int cmdMinLap(int argc, char** argv)
{
    const int ms = argc == 2 ? atoi(argv[1]) : -1;
    if (ms < 0 || ms > 120000) {
        printf("minlap <ms>: crossings sooner than this after a pilot's last one are dropped\n");
        return 1;
    }
    saveInt("minlap", ms);
    postText(QueueKind::Console, ("minlap " + std::to_string(ms)).c_str());
    vTaskDelay(pdMS_TO_TICKS(50));
    return 0;
}

// What the node link's pins and UART are doing: which side of the link is silent.
static int cmdLink(int, char**)
{
    printf("UART%d tx %d rx %d @ %d: sent %lu bytes, received %lu since boot; %u waiting\n", (int)LINK_UART,
           RELAY_LINK_TX, RELAY_LINK_RX, LINK_BAUD, (unsigned long)g_uartTx.load(), (unsigned long)g_uartRx.load(),
           [] { size_t n = 0; uart_get_buffered_data_len(LINK_UART, &n); return (unsigned)n; }());
    printf("rx pin level now %d\n", gpio_get_level((gpio_num_t)RELAY_LINK_RX));
    gpio_dump_io_configuration(stdout, (1ULL << RELAY_LINK_TX) | (1ULL << RELAY_LINK_RX));
    return 0;
}

static int cmdReboot(int, char**)
{
    esp_restart();
    return 0;
}

static const esp_console_cmd_t kCommands[] = {
    {.command = "status", .help = "Wi-Fi, mach1, node link, race and every seat", .hint = nullptr, .func = cmdStatus, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
    {.command = "log", .help = "Recent events", .hint = nullptr, .func = cmdLog, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
    {.command = "wifi", .help = "wifi <ssid> [password]: the network to join", .hint = nullptr, .func = cmdWifi, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
    {.command = "feed", .help = "feed <url>: the club feed URL from mach1 (source segment e.g. relay)", .hint = nullptr, .func = cmdFeed, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
    {.command = "name", .help = "name <text>: what mach1 calls this timer", .hint = nullptr, .func = cmdName, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
    {.command = "minlap", .help = "minlap <ms>: minimum lap time", .hint = nullptr, .func = cmdMinLap, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
    {.command = "link", .help = "The node link's pins and byte counts", .hint = nullptr, .func = cmdLink, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
    {.command = "reboot", .help = "Restart (applies saved settings)", .hint = nullptr, .func = cmdReboot, .argtable = nullptr, .func_w_context = nullptr, .context = nullptr},
};

// Logs must not reach the USB host while it speaks the node protocol.
static int quietVprintf(const char* fmt, va_list args)
{
    if (g_serialMode.load() != SerialOff || g_linkAlive.load())
        return 0;
    return vprintf(fmt, args);
}

static void runConsoleLine(const std::string& line)
{
    if (!line.empty()) {
        int ret = 0;
        const esp_err_t err = esp_console_run(line.c_str(), &ret);
        if (err == ESP_ERR_NOT_FOUND)
            printf("Unknown command; `help` lists them.\n");
    }
    printf("relay> ");
    fflush(stdout);
}

// The native USB port. A line starting with a letter is a console command;
// one starting with '{' is the USB link (core/usb_link.h: mission control
// carrying the relay's mach1 connection). Anything else is a USB host
// speaking RotorHazard's node protocol (it never
// starts with a letter: mach1's browser timer opens with READ_REVISION_CODE
// 0x22, and its C5 settings are '!' lines): the relay hands it the node link
// and forwards bytes both ways until the host has been quiet for
// SERIAL_IDLE_MS.
static void usbTask(void*)
{
    uint8_t in[256];
    uint8_t out[256];
    std::string line;
    bool json = false;       // the line is the USB link's
    bool tooLong = false;    // ... and too long: dropped at its end
    std::string pending;     // bytes from the host while the link is handed over
    int64_t lineStarted = 0;
    int64_t lastHostByte = 0;
    for (;;) {
        const int mode = g_serialMode.load();
        // 1 ms turnaround while forwarding; the console can wait longer, which
        // keeps this task (above the timer task) from stealing its sweep time.
        const int n = usb_serial_jtag_read_bytes(in, sizeof in, pdMS_TO_TICKS(mode == SerialOff ? 20 : 1));
        const int64_t nowMs = esp_timer_get_time() / 1000;
        if (mode == SerialOff) {
            for (int i = 0; i < n; i++) {
                const uint8_t b = in[i];
                if (line.empty() && !json && b == 0x01) {
                    // A page asking which kind of board this is (core/usb_link.h).
                    xSemaphoreTake(g_statusLock, portMAX_DELAY);
                    const std::string info = g_linkInfo;
                    xSemaphoreGive(g_statusLock);
                    if (!info.empty())
                        usbLine(info);
                    else
                        postText(QueueKind::Link, "{\"type\":\"link_probe\"}");
                    continue;
                }
                if (line.empty() && !json && b == '{') {
                    json = true;
                    tooLong = false;
                    lineStarted = nowMs;
                }
                if (json) {
                    if (b == '\r' || b == '\n') {
                        if (!tooLong)
                            post(QueueKind::Link, line.data(), line.size());
                        line.clear();
                        json = false;
                    } else if (line.size() < LINK_LINE_MAX) {
                        line += (char)b;
                    } else {
                        tooLong = true;
                    }
                    continue;
                }
                if (line.empty() && b != '\r' && b != '\n' && !isalpha(b)) {
                    // The node protocol: hand over the link, keep the rest.
                    pending.assign((const char*)in + i, n - i);
                    lastHostByte = nowMs;
                    g_serialMode.store(SerialRequested);
                    break;
                }
                if (b == '\r' || b == '\n') {
                    if (!line.empty() || b == '\r') {
                        printf("\n");
                        runConsoleLine(line);
                    }
                    line.clear();
                } else if (b == 0x08 || b == 0x7f) {
                    if (!line.empty())
                        line.pop_back();
                } else if (line.size() < 255) {
                    if (line.empty())
                        lineStarted = nowMs;
                    line += (char)b;
                }
            }
            // A stray letter must not hold the port as console for ever.
            if (!line.empty() && nowMs - lineStarted > 30000) {
                line.clear();
                json = false;
            }
            continue;
        }
        if (n > 0) {
            lastHostByte = nowMs;
            if (mode == SerialGranted)
                uart_write_bytes(LINK_UART, in, n);
            else if (pending.size() + n < 4096)
                pending.append((const char*)in, n);
        }
        if (mode == SerialGranted) {
            if (!pending.empty()) {
                uart_flush_input(LINK_UART);
                uart_write_bytes(LINK_UART, pending.data(), pending.size());
                pending.clear();
            }
            const int r = uart_read_bytes(LINK_UART, out, sizeof out, 0);
            if (r > 0)
                usb_serial_jtag_write_bytes(out, r, pdMS_TO_TICKS(20));
        }
        if (nowMs - lastHostByte > SERIAL_IDLE_MS) {
            pending.clear();
            line.clear();
            json = false;
            g_serialMode.store(SerialOff);
        }
    }
}

static void startUsb()
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb.rx_buffer_size = 4096;
    usb.tx_buffer_size = 4096;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    usb_serial_jtag_vfs_use_driver();
    g_usbLock = xSemaphoreCreateMutex();
    setvbuf(stdout, nullptr, _IONBF, 0);
    esp_log_set_vprintf(quietVprintf);

    esp_console_config_t console = ESP_CONSOLE_CONFIG_DEFAULT();
    console.max_cmdline_length = 256;
    ESP_ERROR_CHECK(esp_console_init(&console));
    esp_console_register_help_command();
    for (const auto& c : kCommands)
        ESP_ERROR_CHECK(esp_console_cmd_register(&c));
    xTaskCreate(usbTask, "usb", 6144, nullptr, 7, nullptr);
}

// ---------------------------------------------------------------------------

extern "C" void app_main(void)
{
    // Keep UART0's clocks on. With the console on USB, ESP-IDF's start-up
    // (esp_perip_clk_init) turns them off, and the C5's ROM then hangs on every
    // later non-power-on reset (watchdog, panic, esp_restart, a USB-serial
    // reset), polling PCR_UART0_CONF for a UART0 that has no clock: the board
    // looks dead until it is power-cycled. Seen on two boards on 2026-11-12;
    // switching the clock on over JTAG let the stuck ROM boot at once.
    // The S3's ROM does not have this problem.
#if CONFIG_IDF_TARGET_ESP32C5
    uart_ll_enable_bus_clock(UART_NUM_0, true);
    uart_ll_sclk_enable(&UART0);
#endif
    printf("relay boot: reset reason %d\n", (int)esp_reset_reason());
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    static TimerContext ctx;
    ctx.settings = loadSettings();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char uid[32];
    snprintf(uid, sizeof uid, "relay-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ctx.uid = uid;
    // mach1 knows the relay as the node it last drove; the timer task reconnects if that changes.
    ctx.socketUrl = socketUrl(ctx.settings.feed, ctx.settings.nodeId.empty() ? ctx.uid : "c5-" + ctx.settings.nodeId);

    g_events = xQueueCreate(64, sizeof(Event));
    g_outbox = xQueueCreate(64, sizeof(char*));

    g_statusLock = xSemaphoreCreateMutex();
#ifdef RELAY_DIAG
    // Staged start: console, then the node UART, then Wi-Fi, 8 s apart, with
    // a heartbeat, to see which step stops the board.
    xTaskCreate(diagBeat, "diag", 3072, nullptr, 10, nullptr);
    g_diagStage = 1; printf("diag: stage 1 console\n");
    startUsb();
    vTaskDelay(pdMS_TO_TICKS(8000));
    g_diagStage = 2; printf("diag: stage 2 sender+timer (UART link)\n");
    xTaskCreate(senderTask, "sender", 4096, nullptr, 5, nullptr);
    xTaskCreate(timerTask, "timer", 8192, &ctx, 6, nullptr);
    vTaskDelay(pdMS_TO_TICKS(8000));
    g_diagStage = 3; printf("diag: stage 3 network\n");
    startNetwork(ctx.settings, ctx.uid);
    g_diagStage = 4; printf("diag: stage 4 network started\n");
    if (!ctx.settings.ssid.empty() && !ctx.socketUrl.empty())
        startSocket(ctx.socketUrl);
    g_diagStage = 5;
#else
    // USB first: a browser opening the port resets the C5 and starts probing
    // for the node within ~2.5 s.
    startUsb();
    xTaskCreate(timerTask, "timer", 8192, &ctx, 6, nullptr);
    // Always: with no network saved it opens the setup hotspot.
    startNetwork(ctx.settings, ctx.uid);
    if (!ctx.settings.ssid.empty() && !ctx.socketUrl.empty())
        startSocket(ctx.socketUrl);
    xTaskCreate(senderTask, "sender", 4096, nullptr, 5, nullptr);
#endif
    if (ctx.settings.ssid.empty())
        printf("\nNo Wi-Fi set: join hotspot %s (open) and open http://192.168.4.1,\n"
               "or `wifi <ssid> <password>`, `feed <url>`, then `reboot`.\n", hotspotSsid().c_str());
    else if (ctx.socketUrl.empty())
        printf("\nNo mach1 feed URL: `feed <url>`, then `reboot`.\n");
}
