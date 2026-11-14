// The relay as a mach1 timer: it speaks what the rh_mach1 plugin speaks over
// mach1's timer socket (wss://<host>/ws/timers/<club>/<source>/?key=&uid=),
// so the backend, run order, results, live views and announcements work
// unchanged. The race logic is mach1's browser timer (browserTimer.ts), the
// node handling between races and the live `nodes` stream are the plugin's.
//
//   relay -> mach1: hello, status (every 10 s: the heartbeat), command_result,
//                   rh.race_stage / race_start / lap / race_stop / race_result /
//                   laps_discard, nodes (while mission control watches)
//   mach1 -> relay: welcome, pong, ack {session, seq}, gate_config,
//                   command {id, action: start/stop/discard/set_levels/
//                            set_channel/watch/unwatch/node_io/c5_input/
//                            race_settings, ...}
//
// C5 settings from mission control (anywhere, over mach1): mission control
// runs the C5 settings logic itself (c5Control.ts) and the relay carries its
// node-protocol reads and writes (`node_io`), only the C5 settings commands
// (features, detection, calibration), never during a race. The last
// detection written is kept (NVS on the relay) and written again whenever the
// node (re)connects: the receiver forgets it at power-off. `features` in
// hello says what the relay takes.
//
// Race messages are numbered per boot (`session`, `seq`) and kept until mach1
// acknowledges them, then resent on a reconnect; mach1 stores a resend once.
//
// Single-threaded: the relay's timer task calls everything here, and the
// node driver is called synchronously from it.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "node_driver.h"
#include "race_engine.h"

struct cJSON;

namespace relay {

constexpr const char* RELAY_VERSION = "0.2.10";
// The relay board's family ("c5", "s3"), set by the build; the host tests are "c5".
#ifndef RELAY_BOARD
#define RELAY_BOARD "c5"
#define RELAY_BOARD_UC "C5"
#endif
// The rh_mach1 plugin protocol the relay speaks (hello's plugin_version):
// mission control enables node calibration and channels from 0.5.0. The
// relay's own firmware version goes in rh_version.
constexpr const char* PLUGIN_PROTOCOL_VERSION = "0.5.0";
constexpr int64_t HEARTBEAT_MS = 10000;
// No message from mach1 for this long: the socket is dead even if it looks open.
constexpr int64_t SOCKET_SILENCE_MS = 30000;
// Unacknowledged race messages kept for a resend (a race is ~200 at most).
constexpr size_t MAX_PENDING = 400;
constexpr int64_t DEFAULT_START_DELAY_MS = 9000;
// After a race stops, wait this long before moving nodes back to their own set-up.
constexpr int64_t IDLE_AFTER_STOP_MS = 2000;
// Node readings while mission control watches (as the plugin): sampled this
// often, sent this often, for at most this long after the last `watch`.
constexpr int64_t WATCH_SAMPLE_MS = 50;
constexpr int64_t WATCH_FRAME_MS = 250;
constexpr int64_t WATCH_MAX_TTL_MS = 60000;

struct Levels {
    int enter = 0;
    int exit = 0;
};

// A row of gate_config `levels`: seat or pilot may be absent (-1).
struct LevelsRow {
    int seat = -1;
    int pilotId = -1;
    Levels levels;
};

struct StartPilot {
    int pilotId = -1;
    std::string callsign;
    std::string band;  // "" = null
    int channel = -1;  // -1 = null
    std::string bandName;
    int frequency = 0;
    int seat = -1;
    bool hasLevels = false;
    Levels levels;
    std::string levelsFrom;  // "pilot" / "default"
};

class Mach1Timer {
public:
    // To mach1. The caller drops it while the socket is down; race messages are
    // resent from `pending` on the next connect.
    std::function<void(const std::string& json)> send;
    // Ask the transport to drop and reopen the socket (mach1 went quiet).
    std::function<void()> reconnect;
    // Monotonic ms -> epoch ms; false while the relay has no wall clock (SNTP).
    std::function<bool(int64_t monoMs, int64_t* epochMs)> toEpoch;
    // A line for the console log.
    std::function<void(const std::string& line)> log;
    // Keep a setting across reboots (NVS on the relay): "c5det" (hex), "c5in"
    // (JSON), "name", "nodeid" (hex), "minlap", "ignorems", "namepend" (decimal).
    std::function<void(const std::string& key, const std::string& value)> saveSetting;
    // The relay's health for the status heartbeat's "system" (uptime, heap,
    // reset reason, Wi-Fi signal), added to the object it is given.
    std::function<void(cJSON* system)> systemInfo;

    // Loaded at boot from the saved settings.
    // The last C5 detection write (WRITE 0x73 payload), written again on every (re)connect.
    std::vector<uint8_t> c5Detection;
    // The detection geometry mission control applied it with (JSON), shown there again.
    std::string c5Input;

    // Who this is to mach1. With a C5 node that says who it is (1.2.0-c5.13 on),
    // the node: "c5-<node id>", the same timer whether a laptop or this relay
    // drives it. Otherwise the relay itself (relayUid).
    std::string uid;
    std::string session;
    std::string name = RELAY_BOARD_UC " relay";
    // The relay's own id ("relay-<mac>"), reported in hello.
    std::string relayUid;
    // The node is another (or the first) node with an id: be it in mach1. The
    // caller reconnects the socket with the new uid.
    std::function<void(const std::string& uid)> identity;
    // A rename made while the node was away: written to the node when it connects.
    bool namePending = false;
    // A new name (relay console, setup page, or mach1's set_name): the node
    // keeps it, so it follows the node. Returns an error, or "".
    std::string rename(const std::string& next);

    RaceEngine engine;
    // How often a watched gate's readings go out: faster while the USB link carries them.
    int64_t watchFrameMs = WATCH_FRAME_MS;
    // How mach1 reaches this timer, in hello: "relay" (its own Wi-Fi) or
    // "relay_usb" (through mission control on a laptop, over USB).
    std::string route = "relay";
    // Where pages on the same network reach it ("ip:port"), in hello.
    std::vector<std::string> addresses;

    // The gate node is up (driver Running) or gone (nullptr).
    void gateUp(NodeDriver* driver, int64_t now);
    void gateDown(int64_t now);

    void socketOpen(int64_t now);
    void socketClosed();
    void message(const char* json, int64_t now);
    // A crossing from the node driver, at `atMs` (monotonic ms).
    void crossing(int seat, int64_t atMs, int64_t now);
    // Clock work: race start/end, idle set-up, heartbeat, watch frames.
    void tick(int64_t now);

    // For the console.
    bool socketConnected() const { return connected_; }
    const std::string& club() const { return club_; }
    size_t pendingCount() const { return pending_.size(); }
    // When mach1 was last heard (any message, or the socket opening), ms.
    int64_t lastHeardMs() const { return lastHeard_; }
    bool watching(int64_t now) const { return now < watchUntil_; }
    const std::string& raceKey() const { return race_.raceKey; }
    std::string seatCallsign(int seat) const;

    // Exposed for tests.
    std::string hello() const;
    std::string status() const;
    std::optional<Levels> resolveLevels(int seat, int pilotId, std::string* from = nullptr) const;

private:
    struct RaceContext {
        std::string raceKey;
        int eventId = -1;
        std::string phase;
        std::string raceType;
        int round = -1;
        int race = -1;
        std::string heatName;
        std::vector<std::optional<StartPilot>> pilots;  // by seat
    };
    struct GateNode {
        bool hasFrequency = false;
        int frequency = 0;
        std::string channel;
    };

    void note(const std::string& line);
    void sendJson(cJSON* message);
    void sendRace(const char* kind, cJSON* fields);
    void emit(const std::vector<EngineEvent>& events, int64_t now);
    void runCommand(cJSON* command, int64_t now);
    std::string start(cJSON* command, int64_t now);
    std::string stopRace(int64_t now);
    std::string discard(int64_t now);
    std::string setLevels(cJSON* command);
    std::string setChannel(cJSON* command);
    std::string nodeIo(cJSON* command, cJSON* result);
    std::string rememberC5Input(cJSON* command);
    std::string raceSettings(cJSON* command);
    void adoptNode();
    void applyRaceSettings(const cJSON* race, bool save);
    void restoreC5Detection();
    void gateConfig(cJSON* message, int64_t now);
    void applyLevels(int seat, int pilotId, const StartPilot* given);
    void applyIdle();
    int seatPilotId(int seat) const;
    void watch(cJSON* command, int64_t now);
    void sendWatchFrame(int64_t now);
    std::string isoNow(int64_t now) const;
    int64_t epochOf(int64_t monoMs) const;

    NodeDriver* driver_ = nullptr;
    int64_t now_ = 0;  // the time of the call being handled
    bool connected_ = false;
    std::string club_;
    int timerId_ = -1;
    uint32_t seq_ = 0;
    std::map<uint32_t, std::string> pending_;
    std::deque<std::string> seen_;
    int64_t lastHeartbeat_ = 0;
    int64_t lastHeard_ = 0;
    RaceContext race_;
    bool hasStartedEpoch_ = false;
    int64_t startedEpochMs_ = 0;
    // The gate just came up: the next gate_config sets the nodes up.
    bool setUpOnConfig_ = false;
    bool haveGateConfig_ = false;
    std::vector<LevelsRow> gateLevels_;
    std::map<int, GateNode> gateNodes_;
    int64_t idleAt_ = -1;
    // Watching.
    int64_t watchUntil_ = 0;
    int64_t nextSample_ = 0;
    int64_t nextFrame_ = 0;
    std::map<int, std::vector<int>> samples_;
    std::vector<std::pair<int, int64_t>> marks_;
};

// The pilots by seat, as mach1 seated them (`seat` on each). A start from an
// older mach1, or with seats that do not fit this gate, puts pilot n on node n.
std::vector<std::optional<StartPilot>> seatsFromCommand(const std::vector<StartPilot>& pilots, int seats);

// Hex text (as the relay keeps the C5 detection in NVS) to bytes; empty if not hex.
std::vector<uint8_t> fromHex(const std::string& hex);

}  // namespace relay
