// The USB link: mach1 mission control, open in a browser on a laptop with the
// relay plugged into it, carries the relay's mach1 connection over the USB
// serial port. The relay stays the timer (its node driver and race engine run
// on, nothing restarts); only the path to mach1 changes. Unplugged, or with the
// page closed, it goes back to its own Wi-Fi connection, resending whatever
// mach1 has not acknowledged.
//
// The messages are the rh_mach1 plugin's local link (socket.io event
// `mach1_link`), one JSON object per line instead of socket.io events:
//
//   page -> relay: 0x01 (one byte, a probe: answered with link_info; a plain
//                  RotorHazard node ignores it, an undefined read command),
//                  link_open {time_ms} (every 2 s; also the page's clock),
//                  link_up {prefer}, link_cloud {message}, link_down
//   relay -> page: link_info {uid, name, cloud, via_link, ...},
//                  link_send {message}, link_reconnect (mach1 went quiet)
//
// With `prefer` (the page always asks: a cable beats Wi-Fi) the link carries
// everything even while the relay has Wi-Fi. While it does, the relay's own
// socket to mach1 is closed, so mach1 sees one connection for the timer.
//
// Single-threaded, like Mach1Timer: the timer task calls everything here.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "mach1_timer.h"

namespace relay {

// The page sends link_open every 2 s; this long without a line from it, it is gone.
constexpr int64_t USB_LINK_TIMEOUT_MS = 6000;
// Watched readings while the link carries them (a local cable, not the internet).
constexpr int64_t USB_WATCH_FRAME_MS = 100;

class UsbLink {
public:
    explicit UsbLink(Mach1Timer& timer) : timer_(timer) {}

    // One line (no newline) to the USB host.
    std::function<void(const std::string& line)> toHost;
    // Where mach1's messages go from now: true = through the page, false = the
    // relay's own Wi-Fi socket. The caller closes or reopens that socket; a
    // reopened socket says hello itself (Mach1Timer::socketOpen).
    std::function<void(bool viaLink)> routeChanged;
    // The page's wall clock (epoch ms), for a relay without one (no SNTP).
    std::function<void(int64_t epochMs)> hostClock;
    std::function<void(const std::string& line)> log;
    // What the page needs beyond the timer's name and uid.
    std::string rhVersion;

    // A JSON line from the USB host.
    void received(const char* line, int64_t now, bool wifiConnected);
    // Every loop: the page went quiet, or USB went away.
    void tick(int64_t now, bool usbConnected, bool wifiConnected);
    // Stop using the link: the USB port is taken for node pass-through, or
    // mach1 went quiet through it (the page is asked to reconnect).
    void drop(int64_t now, const std::string& why, bool askReconnect);
    // The timer's uid changed (another node): the page reconnects as it.
    void identityChanged(bool wifiConnected);

    bool viaLink() const { return via_; }
    bool alive() const { return alive_; }
    std::string info(bool wifiConnected) const;
    // A mach1 message wrapped for the page.
    static std::string wrap(const std::string& mach1Json);

private:
    void update(int64_t now, bool wifiConnected, bool fresh);
    void note(const std::string& line);

    Mach1Timer& timer_;
    bool alive_ = false;   // the page has been heard from recently
    bool up_ = false;      // the page's socket to mach1 is open
    bool prefer_ = false;
    bool via_ = false;     // mach1's messages go through the page
    int64_t seen_ = 0;
};

}  // namespace relay
