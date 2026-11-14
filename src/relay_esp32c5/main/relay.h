// What the relay's ESP-IDF files share.
#pragma once
#include <string>

#ifndef RELAY_BOARD  // set by the build (main/CMakeLists.txt)
#define RELAY_BOARD "c5"
#define RELAY_BOARD_UC "C5"
#endif

struct Settings {
    std::string ssid, password, feed, name = RELAY_BOARD_UC " relay";
    int minLapMs = 4000;
    int ignoreAfterStartMs = 500;
    // Kept from mission control: the last C5 detection write (hex) and its geometry (JSON).
    std::string c5Detection, c5Input;
    // The C5 node last driven (hex id): mach1 knows this relay as that node's timer.
    std::string nodeId;
    // The name was changed here while the node was away: written to the node when it connects.
    bool namePending = false;
};

Settings loadSettings();
bool saveSettings(const Settings& s);

// The status the web page shows, as JSON: the timer task refreshes it.
void setStatusJson(const std::string& json);
std::string statusJson();

// network.cpp: Wi-Fi (station, and the setup hotspot ELRS-style), the wall
// clock, the setup page.
void startNetwork(const Settings& s, const std::string& uid);
bool networkUp();       // joined the configured network, with an address
std::string networkIp();  // that address, "" without one
bool hotspotOn();
std::string hotspotSsid();
int networkRssi();
// Open connections to the web server: all clients, and how many are page links (/link WebSockets).
int webClients(int* links);        // the joined access point's signal (dBm), 0 when not joined

// The local link: pages on the same network open a WebSocket at /link on the
// web server and speak what the USB link speaks (core/usb_link.h). Text
// frames go to `onText` with the client's socket; `onClosed` when one closes.
// Pages only probing (link_probe) and the one carrying the relay share it.
void setLocalLinkHandlers(void (*onText)(const char* text, size_t len, int fd), void (*onClosed)(int fd));
bool localLinkOpen(int fd);
// A message to that page (copied; sent from the web server's task).
void localLinkSend(int fd, const std::string& text);
