// Wi-Fi as ELRS firmware does it: join the saved network; with none saved, or
// no address after FALLBACK_MS, open a setup hotspot (and keep trying the
// network). The hotspot closes once the relay is online and nobody is on it.
//
// The setup page (http://192.168.4.1 on the hotspot, and the relay's own
// address on the network) shows the relay's status and sets the network,
// the mach1 feed URL, the name and the minimum lap. Saving reboots.
// A tiny DNS responder on the hotspot answers every name with 192.168.4.1,
// so phones open the page by themselves (captive portal).
#include <cstring>
#include <string>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "feed_url.h"
#include "relay.h"
#include "web_form.h"

using namespace relay;

static const char* TAG = "net";

constexpr int64_t FALLBACK_MS = 30000;      // no address this long: open the hotspot
constexpr int64_t RETRY_WITH_AP_MS = 10000;  // while the hotspot is on, retry the network this often
constexpr const char* HOTSPOT_IP = "192.168.4.1";

static Settings g_settings;
static std::string g_uid;
static std::string g_apSsid;
static esp_netif_t* g_sta;
static volatile bool g_haveIp;
static volatile bool g_apOn;
static volatile int g_apClients;
static esp_timer_handle_t g_fallback, g_retry, g_reboot;

bool networkUp() { return g_haveIp; }
bool hotspotOn() { return g_apOn; }
std::string hotspotSsid() { return g_apSsid; }

std::string networkIp()
{
    esp_netif_ip_info_t ip;
    if (!g_haveIp || !g_sta || esp_netif_get_ip_info(g_sta, &ip) != ESP_OK)
        return "";
    char buf[16];
    snprintf(buf, sizeof buf, IPSTR, IP2STR(&ip.ip));
    return buf;
}

static bool staConfigured() { return !g_settings.ssid.empty(); }

static void setHotspot(bool on)
{
    if (on == g_apOn)
        return;
    g_apOn = on;
    if (!on) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "hotspot off");
        return;
    }
    esp_wifi_set_mode(staConfigured() ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    wifi_config_t ap = {};
    strncpy((char*)ap.ap.ssid, g_apSsid.c_str(), sizeof ap.ap.ssid);
    ap.ap.ssid_len = (uint8_t)g_apSsid.size();
    // Open: anyone nearby can join it and change the settings while it is on.
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    ESP_LOGI(TAG, "hotspot %s on (open): http://%s", g_apSsid.c_str(), HOTSPOT_IP);
}

static void onFallback(void*)
{
    if (!g_haveIp)
        setHotspot(true);
}

static void onRetry(void*)
{
    if (!g_haveIp && staConfigured())
        esp_wifi_connect();
}

static void wifiEvent(void*, esp_event_base_t base, int32_t id, void*)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (staConfigured())
                esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            g_haveIp = false;
            if (!esp_timer_is_active(g_fallback) && !g_apOn)
                esp_timer_start_once(g_fallback, FALLBACK_MS * 1000);
            // Scanning for the network disturbs the hotspot, so slower while it is on.
            if (g_apOn) {
                if (!esp_timer_is_active(g_retry))
                    esp_timer_start_once(g_retry, RETRY_WITH_AP_MS * 1000);
            } else {
                esp_wifi_connect();  // the field hotspot comes and goes
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            g_apClients = g_apClients + 1;
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            g_apClients = g_apClients > 0 ? g_apClients - 1 : 0;
            if (g_apClients == 0 && g_haveIp)
                setHotspot(false);
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_haveIp = true;
        esp_timer_stop(g_fallback);
        ESP_LOGI(TAG, "online at %s", networkIp().c_str());
        if (g_apOn && g_apClients == 0)
            setHotspot(false);
    }
}

// ---------------------------------------------------------------------------
// Captive DNS: every A query on the hotspot -> 192.168.4.1

static void dnsTask(void*)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (sock < 0 || bind(sock, (sockaddr*)&addr, sizeof addr) < 0) {
        ESP_LOGW(TAG, "no captive DNS");
        vTaskDelete(nullptr);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        sockaddr_in from;
        socklen_t fromLen = sizeof from;
        const int n = recvfrom(sock, buf, sizeof buf - 16, 0, (sockaddr*)&from, &fromLen);
        // Only hotspot clients (192.168.4.x); never answer for the real network.
        if (n < 12 || (ntohl(from.sin_addr.s_addr) & 0xFFFFFF00) != 0xC0A80400)
            continue;
        int at = 12;
        while (at < n && buf[at])
            at += buf[at] + 1;
        at += 1;
        if (at + 4 > n)
            continue;
        const bool typeA = buf[at] == 0 && buf[at + 1] == 1;
        at += 4;  // qtype, qclass
        buf[2] = 0x81;  // response, recursion desired
        buf[3] = 0x80;  // recursion available, no error
        buf[4] = 0, buf[5] = 1;  // one question
        buf[6] = 0, buf[7] = typeA ? 1 : 0;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = at;
        if (typeA) {
            const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
            memcpy(buf + len, answer, sizeof answer);
            len += sizeof answer;
        }
        sendto(sock, buf, len, 0, (sockaddr*)&from, fromLen);
    }
}

// ---------------------------------------------------------------------------
// The setup page

static const char PAGE_HEAD[] = R"(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>C5 relay</title>
<style>body{font-family:system-ui,sans-serif;max-width:40rem;margin:1rem auto;padding:0 1rem;color:#222}
h1{font-size:1.4rem}h2{font-size:1.1rem;margin-top:1.5rem}label{display:block;margin:.6rem 0 .2rem;font-weight:600}
input{width:100%;box-sizing:border-box;padding:.5rem;font-size:1rem}button{margin-top:1rem;padding:.6rem 1.2rem;font-size:1rem}
small{color:#666}table{border-collapse:collapse;width:100%;font-size:.9rem}td,th{padding:.2rem .4rem;text-align:left;border-bottom:1px solid #eee}
.ok{color:#080}.bad{color:#b00}</style></head><body><h1>C5 relay</h1>
)";

static const char PAGE_SCRIPT[] = R"(<script>
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const yes=(b,t,f)=>`<span class="${b?'ok':'bad'}">${esc(b?t:f)}</span>`;
async function poll(){try{const s=await (await fetch('/status.json')).json();
let h=`<table><tr><th>Wi-Fi</th><td>${yes(s.wifi.up,'online '+s.wifi.ip,'not connected')} ${esc(s.wifi.ssid)}${s.wifi.hotspot?' · hotspot '+esc(s.wifi.hotspot_ssid):''}</td></tr>
<tr><th>Clock</th><td>${yes(s.clock,'synced','not synced')}</td></tr>
<tr><th>mach1</th><td>${yes(s.mach1.connected,'connected '+s.mach1.club,'not connected')} · pending ${s.mach1.pending}${s.mach1.watched?' · watched':''}</td></tr>
<tr><th>Node</th><td>${yes(s.node.status=='running',s.node.status,s.node.status)} ${esc(s.node.proc)} ${esc(s.node.firmware)} API ${s.node.api} · ${s.node.sweeps.toFixed(1)} sweeps/s · ${esc(s.node.detail)}</td></tr>
<tr><th>Link</th><td>${s.node.reads} reads · ${s.node.timeouts} timeouts · ${s.node.checksum} checksum errors</td></tr>
<tr><th>Race</th><td>${esc(s.race.status)} ${esc(s.race.format)}</td></tr></table>`;
if(s.seats.length){h+=`<table><tr><th>Node</th><th>MHz</th><th>RSSI</th><th>Enter/exit</th><th>Pilot</th><th>Laps</th></tr>`+
s.seats.map((x,i)=>`<tr><td>${i+1}</td><td>${x.frequency||'off'}</td><td>${x.rssi}</td><td>${x.enter}/${x.exit}</td><td>${esc(x.callsign)}</td><td>${x.laps}</td></tr>`).join('')+'</table>';}
document.getElementById('status').innerHTML=h;}catch(e){document.getElementById('status').textContent='No status: '+e;}}
poll();setInterval(poll,1000);</script>)";

static std::string page()
{
    std::string h = PAGE_HEAD;
    h += "<div id=status>Loading status…</div><h2>Settings</h2><form method=post action=/settings>";
    h += "<label>Wi-Fi network</label><input name=ssid maxlength=32 value=\"" + htmlEscape(g_settings.ssid) + "\">";
    h += "<label>Wi-Fi password</label><input name=password type=password maxlength=63 placeholder=\"";
    h += g_settings.password.empty() ? "none" : "unchanged";
    h += "\"><small>Leave empty to keep the saved one. The relay remembers one network; "
         "use the same name and password for the home and field hotspot.</small>";
    h += "<label>mach1 feed URL</label><input name=feed value=\"" + htmlEscape(g_settings.feed) +
         "\" placeholder=\"https://mach1.org.nz/&lt;club&gt;/feed/relay/?key=…\">"
         "<small>The club feed URL from mach1, with the last part set to e.g. <b>relay</b>.</small>";
    h += "<label>Timer name</label><input name=name maxlength=32 value=\"" + htmlEscape(g_settings.name) + "\">"
         "<small>Kept on the C5 node too, so it follows the node; mach1 shows it.</small>";
    h += "<label>Minimum lap (ms)</label><input name=minlap inputmode=numeric value=\"" +
         std::to_string(g_settings.minLapMs) + "\">";
    h += "<button>Save and restart</button></form>";
    h += "<p><small>Relay " + htmlEscape(g_uid) + ". Setup hotspot " + htmlEscape(g_apSsid) + " (open), http://" + HOTSPOT_IP + "</small></p>";
    h += PAGE_SCRIPT;
    h += "</body></html>";
    return h;
}

static esp_err_t sendPage(httpd_req_t* req, const std::string& body)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.data(), (ssize_t)body.size());
}

static esp_err_t getRoot(httpd_req_t* req) { return sendPage(req, page()); }

static esp_err_t getStatus(httpd_req_t* req)
{
    const std::string json = statusJson();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.data(), (ssize_t)json.size());
}

static std::string message(const std::string& title, const std::string& text)
{
    return std::string(PAGE_HEAD) + "<h2>" + htmlEscape(title) + "</h2><p>" + text +
           "</p><p><a href=/>Back</a></p></body></html>";
}

static esp_err_t postSettings(httpd_req_t* req)
{
    if (req->content_len > 4096)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too long");
    std::string body(req->content_len, '\0');
    size_t got = 0;
    while (got < body.size()) {
        const int r = httpd_req_recv(req, body.data() + got, body.size() - got);
        if (r <= 0)
            return ESP_FAIL;
        got += (size_t)r;
    }
    auto form = parseForm(body);
    Settings next = g_settings;
    std::string error;
    next.ssid = form["ssid"];
    if (!form["password"].empty() || next.ssid != g_settings.ssid)
        next.password = form["password"];
    next.feed = form["feed"];
    next.name = form["name"].empty() ? "C5 relay" : form["name"];
    const std::string minlap = form["minlap"];
    if (next.ssid.size() > 32)
        error = "The network name is longer than 32 characters.";
    else if (!next.password.empty() && (next.password.size() < 8 || next.password.size() > 63))
        error = "A Wi-Fi password has 8 to 63 characters.";
    else if (!next.feed.empty() && socketUrl(next.feed, "x").empty())
        error = "That is not a mach1 feed URL: it needs /&lt;club&gt;/feed/&lt;source&gt;/ and ?key=.";
    else if (minlap.empty() || minlap.size() > 6 || minlap.find_first_not_of("0123456789") != std::string::npos ||
             atoi(minlap.c_str()) > 120000)
        error = "Minimum lap must be a whole number of milliseconds up to 120000.";
    if (error.empty()) {
        next.minLapMs = atoi(minlap.c_str());
        next.name = next.name.substr(0, 32);
        // A new name goes to the node when it connects (after the restart).
        if (next.name != g_settings.name)
            next.namePending = true;
        if (!saveSettings(next))
            error = "Could not save the settings.";
    }
    if (!error.empty())
        return sendPage(req, message("Not saved", error));
    std::string text = "The relay restarts now";
    if (next.ssid.empty())
        text += " and opens its setup hotspot again.";
    else
        text += " and joins <b>" + htmlEscape(next.ssid) + "</b>. If it cannot, its setup hotspot <b>" +
                htmlEscape(g_apSsid) + "</b> comes back within a minute.";
    sendPage(req, message("Saved", text));
    esp_timer_start_once(g_reboot, 1500 * 1000);
    return ESP_OK;
}

// Anything else: phones probing for a captive portal, or a typo. Send them to the page.
static esp_err_t getOther(httpd_req_t* req)
{
    char host[64] = {};
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
    const std::string ip = networkIp();
    const bool ours = !strcmp(host, HOTSPOT_IP) || (!ip.empty() && ip == host);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", ours ? "/" : "http://192.168.4.1/");
    return httpd_resp_send(req, nullptr, 0);
}

static void startHttp()
{
    httpd_handle_t server = nullptr;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "no web server");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = getRoot, .user_ctx = nullptr},
        {.uri = "/status.json", .method = HTTP_GET, .handler = getStatus, .user_ctx = nullptr},
        {.uri = "/settings", .method = HTTP_POST, .handler = postSettings, .user_ctx = nullptr},
        {.uri = "/*", .method = HTTP_GET, .handler = getOther, .user_ctx = nullptr},
    };
    for (const auto& u : uris)
        httpd_register_uri_handler(server, &u);
}

// ---------------------------------------------------------------------------

void startNetwork(const Settings& s, const std::string& uid)
{
    g_settings = s;
    g_uid = uid;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    snprintf(ssid, sizeof ssid, "C5-Relay-%02X%02X", mac[4], mac[5]);
    g_apSsid = ssid;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    g_sta = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(g_sta, "c5-relay");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEvent, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEvent, nullptr);

    esp_timer_create_args_t args = {};
    args.callback = onFallback;
    args.name = "wifi-fallback";
    esp_timer_create(&args, &g_fallback);
    args.callback = onRetry;
    args.name = "wifi-retry";
    esp_timer_create(&args, &g_retry);
    args.callback = [](void*) { esp_restart(); };
    args.name = "reboot";
    esp_timer_create(&args, &g_reboot);

    if (staConfigured()) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        wifi_config_t sta = {};
        strncpy((char*)sta.sta.ssid, s.ssid.c_str(), sizeof sta.sta.ssid);
        strncpy((char*)sta.sta.password, s.password.c_str(), sizeof sta.sta.password);
        // Never a BSSID: the same SSID is the home APs and the field hotspot.
        sta.sta.threshold.authmode = s.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    }
    // Lower latency than modem sleep; the relay is on USB power.
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_wifi_start());
    if (staConfigured())
        esp_timer_start_once(g_fallback, FALLBACK_MS * 1000);
    else
        setHotspot(true);

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    xTaskCreate(dnsTask, "dns", 3072, nullptr, 4, nullptr);
    startHttp();
}
