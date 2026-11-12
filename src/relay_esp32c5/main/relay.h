// What the relay's ESP-IDF files share.
#pragma once
#include <string>

struct Settings {
    std::string ssid, password, feed, name = "C5 relay";
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
