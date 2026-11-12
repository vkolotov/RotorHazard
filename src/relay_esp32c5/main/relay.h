// What the relay's ESP-IDF files share.
#pragma once
#include <string>

struct Settings {
    std::string ssid, password, feed, name = "C5 relay";
    int minLapMs = 4000;
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
