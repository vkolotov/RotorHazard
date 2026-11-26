// The OSD Timer Backpack on its own UART (S3: UART2, GPIO17/18, 460800).
#pragma once

#include <string>

void backpackInit();
bool backpackWired();  // built with backpack pins
// The backpack's version text, "" without an answer.
std::string backpackVersion();
// Shows `text` centred on `row` of the goggles bound with `phrase`.
bool backpackOsd(const std::string& phrase, int row, const std::string& text);
// Clears that pilot's OSD.
bool backpackOsdClear(const std::string& phrase);
// Runs `text` down that pilot's OSD, top row to bottom (~0.3 s a row), then
// clears it. In the background; false while one is already running.
bool backpackOsdSweep(const std::string& phrase, const std::string& text);
// Reboots the backpack into Wi-Fi mode (its update page).
bool backpackWifi();
// One line for the console and /backpack: wiring and the last result.
std::string backpackSummary();
