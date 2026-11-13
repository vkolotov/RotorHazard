// Host tests for the relay's portable core: race engine, node driver (against
// a fake node on a virtual clock), and the mach1 timer protocol.
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "cJSON.h"
#include "channels.h"
#include "fake_node.h"
#include "feed_url.h"
#include "web_form.h"
#include "mach1_timer.h"
#include "node_driver.h"
#include "race_engine.h"
#include "usb_link.h"

using namespace relay;

int relayConstantsChecked();

static int g_checks = 0, g_failed = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        g_checks++;                                                                \
        if (!(cond)) {                                                             \
            g_failed++;                                                            \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)
#define CHECK_EQ(a, b)                                                                                      \
    do {                                                                                                    \
        g_checks++;                                                                                         \
        const auto va = (a);                                                                                \
        const auto vb = (b);                                                                                \
        if (!(va == vb)) {                                                                                  \
            g_failed++;                                                                                     \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                         (long long)va, (long long)vb);                                                     \
        }                                                                                                   \
    } while (0)

static void test(const char* name, const std::function<void()>& body)
{
    const int before = g_failed;
    body();
    std::printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", name);
}

// ---------------------------------------------------------------------------
// Race engine (mach1 raceEngine.test.ts)

static RaceFormat practice() { return RaceFormat{"practice", true, 0, 0, 0, WIN_MOST_PROGRESS, 0}; }
static RaceFormat timed(int seconds, int grace, int win = WIN_MOST_PROGRESS)
{
    RaceFormat f = practice();
    f.unlimitedTime = false;
    f.raceTimeSec = seconds;
    f.lapGraceSec = grace;
    f.winCondition = win;
    return f;
}
static RaceFormat firstTo(int laps)
{
    RaceFormat f = practice();
    f.winCondition = WIN_FIRST_TO_LAP_X;
    f.numberLapsWin = laps;
    return f;
}
constexpr int64_t START = 10000;
static std::map<int, std::string> pilots(std::vector<std::string> names)
{
    std::map<int, std::string> m;
    for (size_t i = 0; i < names.size(); i++)
        m[(int)i] = names[i];
    return m;
}
static std::vector<EventKind> kinds(const std::vector<EngineEvent>& events)
{
    std::vector<EventKind> k;
    for (auto& e : events)
        k.push_back(e.kind);
    return k;
}
static std::vector<Crossing> lapsOf(const std::vector<EngineEvent>& events)
{
    std::vector<Crossing> l;
    for (auto& e : events)
        if (e.kind == EventKind::Lap)
            l.push_back(e.lap);
    return l;
}
static RaceEngine started(const RaceFormat& f, std::vector<std::string> names = {"ace"})
{
    RaceEngine e;
    e.settings = {3000, 500, 400};
    e.stage(pilots(names), f, START);
    CHECK(kinds(e.tick(START)) == std::vector<EventKind>{EventKind::RaceStart});
    return e;
}
static Crossing lap(int n, int64_t t, int64_t stamp) { return Crossing{n, t, stamp}; }

static void engineTests()
{
    test("engine: starts at the start time, not before", [] {
        RaceEngine e;
        e.stage(pilots({"ace"}), practice(), START);
        CHECK(e.status == RaceStatus::Staging);
        CHECK(e.tick(START - 1).empty());
        auto ev = e.tick(START + 5);
        CHECK(ev.size() == 1 && ev[0].kind == EventKind::RaceStart && ev[0].at == START);
        CHECK(e.status == RaceStatus::Racing);
    });
    test("engine: ignores crossings before the start and just after it", [] {
        RaceEngine e;
        e.settings.ignoreAfterStartMs = 500;
        e.stage(pilots({"ace"}), practice(), START);
        CHECK(lapsOf(e.crossing(0, START - 100)).empty());
        CHECK(lapsOf(e.crossing(0, START - 50, START + 200)).empty());
        CHECK(lapsOf(e.crossing(0, START + 300)).empty());
        CHECK(lapsOf(e.crossing(0, START + 600)).size() == 1);
    });
    test("engine: holeshot is lap 0, laps timed between crossings", [] {
        auto e = started(practice());
        CHECK(lapsOf(e.crossing(0, START + 2500)) == std::vector<Crossing>{lap(0, 2500, 2500)});
        CHECK(lapsOf(e.crossing(0, START + 14000)) == std::vector<Crossing>{lap(1, 11500, 14000)});
        CHECK(lapsOf(e.crossing(0, START + 25250)) == std::vector<Crossing>{lap(2, 11250, 25250)});
    });
    test("engine: drops a crossing under the minimum lap", [] {
        auto e = started(practice());
        e.crossing(0, START + 2000);
        auto ev = e.crossing(0, START + 3500);
        CHECK(kinds(ev) == std::vector<EventKind>{EventKind::Dropped});
        CHECK(lapsOf(e.crossing(0, START + 12000))[0].lapNumber == 1);
    });
    test("engine: ignores seats nobody flies", [] {
        auto e = started(practice());
        CHECK(e.crossing(5, START + 2000).empty());
    });
    test("engine: time limit without grace", [] {
        auto e = started(timed(60, 0));
        e.crossing(0, START + 2000);
        e.crossing(0, START + 30000);
        CHECK(lapsOf(e.crossing(0, START + 60200, START + 60300)).empty());
        CHECK(e.tick(START + 60399).empty());
        auto ev = e.tick(START + 60400);
        CHECK((kinds(ev) == std::vector<EventKind>{EventKind::RaceStop, EventKind::RaceResult}));
        CHECK(ev[0].reason == "time" && ev[0].at == START + 60000);
    });
    test("engine: with grace, each pilot's first crossing after time is their last", [] {
        auto e = started(timed(60, 30), {"ace", "bob"});
        e.crossing(0, START + 2000);
        e.crossing(1, START + 2100);
        e.crossing(0, START + 50000);
        CHECK(lapsOf(e.crossing(0, START + 62000)).size() == 1);
        CHECK(kinds(e.crossing(0, START + 70000)) == std::vector<EventKind>{EventKind::Dropped});
        auto ev = e.crossing(1, START + 75000);
        CHECK((kinds(ev) == std::vector<EventKind>{EventKind::Lap, EventKind::RaceStop, EventKind::RaceResult}));
        CHECK(ev[1].reason == "finished");
    });
    test("engine: with grace, stops when the grace runs out", [] {
        auto e = started(timed(60, 30), {"ace", "bob"});
        e.crossing(0, START + 2000);
        e.crossing(0, START + 62000);
        CHECK(e.tick(START + 89000).empty());
        CHECK((kinds(e.tick(START + 90500)) == std::vector<EventKind>{EventKind::RaceStop, EventKind::RaceResult}));
    });
    test("engine: first to X finishes at the target and stops when all are done", [] {
        auto e = started(firstTo(2), {"ace", "bob"});
        e.crossing(0, START + 2000);
        e.crossing(1, START + 2200);
        e.crossing(0, START + 12000);
        e.crossing(1, START + 13000);
        e.crossing(0, START + 22000);
        CHECK(kinds(e.crossing(0, START + 32000)) == std::vector<EventKind>{EventKind::Dropped});
        auto ev = e.crossing(1, START + 24000);
        CHECK((kinds(ev) == std::vector<EventKind>{EventKind::Lap, EventKind::RaceStop, EventKind::RaceResult}));
        auto& r = ev[2].results;
        CHECK(r.size() == 2 && r[0].callsign == "ace" && r[0].position == 1 && r[0].laps == 2);
        CHECK(r[1].callsign == "bob" && r[1].position == 2 && r[1].laps == 2);
    });
    test("engine: finishers first by finish time, the rest by progress", [] {
        auto e = started(firstTo(2), {"ace", "bob", "cat"});
        const int64_t seq[][2] = {{0, 2000}, {1, 2100}, {2, 2200}, {1, 9000}, {0, 12000}, {1, 16000}, {2, 30000}};
        for (auto& c : seq)
            e.crossing((int)c[0], START + c[1]);
        auto ev = e.stop(START + 40000);
        auto& r = ev[1].results;
        CHECK(r.size() == 3 && r[0].callsign == "bob" && r[1].callsign == "ace" && r[2].callsign == "cat");
    });
    test("engine: results order and fastest lap without the holeshot", [] {
        auto e = started(timed(120, 0, WIN_FASTEST_CONSECUTIVE), {"ace", "bob", "cat"});
        const int64_t seq[][2] = {{0, 2000}, {0, 12000}, {0, 21000}, {1, 1500}, {1, 11000}, {1, 20500}};
        for (auto& c : seq)
            e.crossing((int)c[0], START + c[1]);
        auto r = e.stop(START + 50000)[1].results;
        CHECK(r[0].callsign == "bob" && r[0].laps == 2 && r[0].totalTimeMs == 20500 && r[0].fastestLapMs == 9500);
        CHECK(r[1].callsign == "ace" && r[1].laps == 2 && r[1].totalTimeMs == 21000 && r[1].fastestLapMs == 9000);
        CHECK(r[2].callsign == "cat" && r[2].laps == 0 && r[2].totalTimeMs == 0 && r[2].fastestLapMs == -1);
    });
    test("engine: a crossing from before the stop arriving after it resends results", [] {
        auto e = started(practice());
        e.crossing(0, START + 2000);
        CHECK((kinds(e.stop(START + 20000)) == std::vector<EventKind>{EventKind::RaceStop, EventKind::RaceResult}));
        CHECK((kinds(e.crossing(0, START + 19800, START + 20150)) ==
               std::vector<EventKind>{EventKind::Lap, EventKind::RaceResult}));
        CHECK(e.crossing(0, START + 25000).empty());
    });
    test("engine: stop is a no-op when nothing runs", [] {
        RaceEngine e;
        CHECK(e.stop(0).empty());
    });
}

// ---------------------------------------------------------------------------
// Node driver

static void driverTests()
{
    test("driver: syncs like RotorHazard (8-bit C5 at API 36)", [] {
        FakeNode node(4);
        node.seats[2].frequency = 5732;
        NodeDriver d(node);
        std::string error;
        CHECK(d.start(&error));
        CHECK(d.info().apiLevel == 36 && d.info().seats == 4 && d.info().procType == "ESP32C5" && !d.info().wide);
        CHECK(d.info().firmware == "1.2.0-c5.4");
        CHECK(d.seats()[2].frequency == 5732 && d.seats()[0].frequency == 5800);
        CHECK(d.seats()[0].enter == 120 && d.seats()[0].exit == 90);
    });
    test("driver: 16-bit RSSI on an STM32 at API 38", [] {
        FakeNode node(2, 38, "STM32F4");
        node.seats[0].enter = 900;
        NodeDriver d(node);
        CHECK(d.start(nullptr));
        CHECK(d.info().wide && d.seats()[0].enter == 900);
        CHECK(d.setThresholds(1, 1200, 1000));
        CHECK(node.seats[1].enter == 1200 && node.seats[1].exit == 1000);
    });
    test("driver: sets levels on a seat", [] {
        FakeNode node(3);
        NodeDriver d(node);
        d.start(nullptr);
        CHECK(d.setThresholds(1, 140, 100));
        CHECK(node.seats[1].enter == 140 && node.seats[1].exit == 100);
        CHECK(d.seats()[1].enter == 140);
        CHECK(!d.setThresholds(1, 300, 100));  // past the 8-bit limit
    });
    test("driver: reports a crossing once, timed from the request", [] {
        FakeNode node(2);
        NodeDriver d(node);
        std::vector<std::pair<int, int64_t>> got;
        d.onCrossing = [&](int seat, int64_t at, int) { got.push_back({seat, at}); };
        d.start(nullptr);
        for (int i = 0; i < 10; i++)
            d.poll();
        const int64_t at = node.clockUs - 40000;
        node.cross(1, at);
        for (int i = 0; i < 20; i++)
            d.poll();
        CHECK_EQ(got.size(), 1u);
        CHECK(got[0].first == 1);
        CHECK(std::llabs(got[0].second - at) < 1500);
    });
    test("driver: times a crossing right when replies were lost and retried", [] {
        FakeNode node(2);
        NodeDriver d(node);
        d.readExtremums = true;
        std::vector<int64_t> got;
        d.onCrossing = [&](int, int64_t at, int) { got.push_back(at); };
        d.start(nullptr);
        for (int i = 0; i < 10; i++)
            d.poll();
        const int64_t at = node.clockUs - 40000;
        node.cross(1, at);
        node.dropCommands[READ_LAP_EXTREMUMS] = 3;
        node.dropCommands[READ_LAP_PASS_STATS] = 2;
        for (int i = 0; i < 20; i++)
            d.poll();
        CHECK(got.size() == 1);
        CHECK(d.counters().timeouts >= 5);
        CHECK(!got.empty() && std::llabs(got[0] - at) < 1500);
    });
    test("driver: does not take a garbage reply for a crossing", [] {
        FakeNode node(1);
        node.seats[0].lapId = 5;
        NodeDriver d(node);
        int crossings = 0;
        d.onCrossing = [&](int, int64_t, int) { crossings++; };
        d.start(nullptr);
        for (int i = 0; i < 6; i++)
            d.poll();
        node.garbageNext = 1;  // all zeros pass the checksum and read as lap id 0
        for (int i = 0; i < 6; i++)
            d.poll();
        CHECK(crossings == 0);
    });
    test("driver: retries a reply that never came", [] {
        FakeNode node(1);
        NodeDriver d(node);
        d.start(nullptr);
        node.dropNext = 2;
        for (int i = 0; i < 6; i++)
            d.poll();
        CHECK(d.counters().timeouts == 2);
        CHECK(d.status() == NodeStatus::Running);
    });
    test("driver: sets frequencies and switches unused seats off", [] {
        FakeNode node(3);
        NodeDriver d(node);
        d.start(nullptr);
        CHECK(d.setFrequencies({5658, 5695}).empty());
        CHECK(node.seats[0].frequency == 5658 && node.seats[1].frequency == 5695 && node.seats[2].frequency == 1111);
        CHECK(d.seats()[2].frequency == 0 && d.seats()[1].frequency == 5695);
    });
    test("driver: reads only seats that are on, counts sweeps", [] {
        FakeNode node(4);
        NodeDriver d(node);
        d.start(nullptr);
        d.setFrequencies({0, 5658, 0, 0});
        for (int i = 0; i < 50; i++)
            CHECK(d.poll());
        CHECK(d.sweepsPerSecond() > 0);
        d.setFrequencies({0, 0, 0, 0});
        CHECK(!d.poll());
    });
    test("driver: fails start-up clearly when nothing answers", [] {
        FakeNode node(1);
        node.dropNext = 100000;
        NodeDriver d(node);
        std::string error;
        CHECK(!d.start(&error));
        CHECK(error.find("No RotorHazard node answered") == 0);
        CHECK(d.status() == NodeStatus::Lost);
    });
    test("driver: a node that stops answering is lost", [] {
        FakeNode node(2);
        NodeDriver d(node);
        d.start(nullptr);
        node.dropNext = 100000;
        for (int i = 0; i < 60 && d.status() == NodeStatus::Running; i++)
            d.poll();
        CHECK(d.status() == NodeStatus::Lost);
    });
    test("feed url: as the plugin's socket_url", [] {
        CHECK(socketUrl("https://mach1.org.nz/akl/feed/relay/?key=K1", "relay-ab") ==
              "wss://mach1.org.nz/ws/timers/akl/relay/?key=K1&uid=relay-ab");
        CHECK(socketUrl(" http://192.168.8.211:8000/akl/feed/rh?x=1&key=K2 ", "u") ==
              "ws://192.168.8.211:8000/ws/timers/akl/rh/?key=K2&uid=u");
        CHECK(socketUrl("https://mach1.org.nz/akl/feed/relay/", "u").empty());
        CHECK(socketUrl("https://mach1.org.nz/akl/relay/?key=K", "u").empty());
        CHECK(socketUrl("ftp://mach1.org.nz/akl/feed/relay/?key=K", "u").empty());
    });
    test("web form: decoding and escaping", [] {
        auto f = parseForm("ssid=My+Net%21&password=a%26b%3Dc&feed=https%3A%2F%2Fx%2Fc%2Ffeed%2Frelay%2F%3Fkey%3DK&empty=&flag");
        CHECK(f["ssid"] == "My Net!" && f["password"] == "a&b=c" && f["empty"].empty() && f.count("flag"));
        CHECK(f["feed"] == "https://x/c/feed/relay/?key=K");
        CHECK(urlDecode("100%") == "100%" && urlDecode("%4") == "%4" && urlDecode("%zz") == "%zz");
        CHECK(htmlEscape("<a href=\"x\">'&'</a>") == "&lt;a href=&quot;x&quot;&gt;&#39;&amp;&#39;&lt;/a&gt;");
    });
    test("channels: labels and frequencies", [] {
        CHECK(channelFrequency("R1") == 5658 && channelFrequency(" r8 ") == 5917 && channelFrequency("off") == 0);
        CHECK(channelFrequency("X1") == -1 && channelFrequency("R9") == -1 && channelFrequency("H2") == 5695);
        CHECK(channelLabel(5880) == "R7" && channelLabel(5740) == "F1" && channelLabel(1234).empty());
    });
}

// ---------------------------------------------------------------------------
// mach1 timer protocol

struct Rig {
    FakeNode node{8};
    NodeDriver driver{node};
    Mach1Timer timer;
    std::vector<cJSON*> sent;
    std::vector<std::string> lines;
    bool clock = true;
    int reconnects = 0;
    int64_t epochBase = 1762800000000LL;

    Rig()
    {
        timer.uid = "relay-test";
        timer.session = "s1";
        timer.send = [this](const std::string& text) { sent.push_back(cJSON_Parse(text.c_str())); };
        timer.toEpoch = [this](int64_t mono, int64_t* epoch) {
            *epoch = epochBase + mono;
            return clock;
        };
        timer.log = [this](const std::string& l) { lines.push_back(l); };
        timer.reconnect = [this] { reconnects++; };
        driver.onCrossing = [this](int seat, int64_t atUs, int) { timer.crossing(seat, atUs / 1000, now()); };
    }
    ~Rig() { clearSent(); }
    void clearSent()
    {
        for (cJSON* m : sent)
            cJSON_Delete(m);
        sent.clear();
    }
    int64_t now() { return node.nowUs() / 1000; }
    void up()
    {
        driver.start(nullptr);
        timer.socketOpen(now());
        timer.message(R"({"type":"welcome","timer_id":7,"club":"Mach1"})", now());
        timer.gateUp(&driver, now());
    }
    // Sweep the node and tick the timer for `ms` of virtual time.
    void run(int64_t ms)
    {
        const int64_t until = now() + ms;
        while (now() < until) {
            if (!driver.poll())
                node.sleepMs(10);
            timer.tick(now());
        }
    }
    std::vector<cJSON*> ofType(const std::string& type)
    {
        std::vector<cJSON*> out;
        for (cJSON* m : sent) {
            const cJSON* t = cJSON_GetObjectItem(m, "type");
            if (cJSON_IsString(t) && type == t->valuestring)
                out.push_back(m);
        }
        return out;
    }
    cJSON* last(const std::string& type)
    {
        auto all = ofType(type);
        return all.empty() ? nullptr : all.back();
    }
    void command(const std::string& json) { timer.message(json.c_str(), now()); }
};

static int64_t num(const cJSON* obj, const char* key)
{
    const cJSON* v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(v) ? (int64_t)v->valuedouble : -999;
}
static std::string str(const cJSON* obj, const char* key)
{
    const cJSON* v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static bool isNull(const cJSON* obj, const char* key) { return cJSON_IsNull(cJSON_GetObjectItem(obj, key)); }

static const char* START_CMD = R"({"type":"command","id":"c1","action":"start","event_id":12,"race_key":"rk1",
  "phase":"race","race_type":"Race","round":2,"race":3,"heat_name":"Race R2 · Race 3",
  "format":{"name":"mach1 race","unlimited_time":true,"race_time_sec":0,"lap_grace_sec":0,"start_behavior":0,
            "win_condition":1,"number_laps_win":0},
  "start_delay_ms":9000,
  "pilots":[{"pilot_id":101,"callsign":"ace","band":"R","band_name":"Raceband","channel":1,"frequency":5658,"seat":2,
             "levels":{"enter":140,"exit":100,"from":"pilot"}},
            {"pilot_id":102,"callsign":"bob","band":"R","band_name":"Raceband","channel":8,"frequency":5917,"seat":0}]})";

static void timerTests()
{
    test("timer: connect sends hello and status; hello names the gate", [] {
        Rig r;
        r.up();
        cJSON* hello = r.last("hello");
        CHECK(hello && str(hello, "hardware") == "ESP32C5" && num(hello, "node_count") == 8);
        CHECK(str(hello, "rh_version").find("c5-relay") == 0);
        // mission control enables node calibration from plugin 0.5.0
        CHECK(str(hello, "plugin_version") == "0.5.0");
        CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(hello, "frequencies")) == 8);
        CHECK(r.ofType("status").size() == 1);
        CHECK(r.timer.club() == "Mach1");
    });
    test("timer: first gate_config puts nodes on their preferred channels and default levels", [] {
        Rig r;
        r.up();
        r.command(R"({"type":"gate_config","hardware":"ESP32C5",
          "levels":[{"seat":0,"pilot_id":null,"enter":130,"exit":95},{"seat":null,"pilot_id":null,"enter":125,"exit":92}],
          "channels":{"0":"R1","1":"off"},
          "nodes":[{"seat":0,"channel":"R1","frequency":5658,"levels":{"enter":130,"exit":95}},
                   {"seat":1,"channel":"off","frequency":0,"levels":null},
                   {"seat":2,"channel":null,"frequency":null,"levels":null}]})");
        CHECK(r.node.seats[0].frequency == 5658 && r.node.seats[0].enter == 130 && r.node.seats[0].exit == 95);
        CHECK(r.node.seats[1].frequency == 1111);
        // No preference: stays where it is, with the any-node default.
        CHECK(r.node.seats[2].frequency == 5800 && r.node.seats[2].enter == 125);
        // A second gate_config (mission control saved something) does not move nodes.
        r.node.seats[0].frequency = 5732;
        r.command(R"({"type":"gate_config","levels":[],"nodes":[{"seat":0,"channel":"R1","frequency":5658}]})");
        CHECK(r.node.seats[0].frequency == 5732);
    });
    test("timer: start seats pilots, switches unused nodes off, stages, and answers", [] {
        Rig r;
        r.up();
        const int64_t t0 = r.now();
        r.command(START_CMD);
        CHECK(r.node.seats[2].frequency == 5658 && r.node.seats[0].frequency == 5917);
        CHECK(r.node.seats[1].frequency == 1111 && r.node.seats[7].frequency == 1111);
        CHECK(r.node.seats[2].enter == 140 && r.node.seats[2].exit == 100);
        cJSON* result = r.last("command_result");
        CHECK(result && str(result, "id") == "c1" && cJSON_IsTrue(cJSON_GetObjectItem(result, "ok")));
        cJSON* stage = r.last("rh.race_stage");
        CHECK(stage && str(stage, "session") == "s1" && num(stage, "seq") == 1 && str(stage, "race_key") == "rk1");
        CHECK(num(stage, "event_id") == 12 && str(stage, "phase") == "race" && isNull(stage, "target_laps"));
        CHECK(num(stage, "start_epoch_ms") == r.epochBase + t0 + 9000);
        CHECK(str(stage, "ts").size() == 24 && str(stage, "ts").back() == 'Z');
        cJSON* ps = cJSON_GetObjectItem(stage, "pilots");
        CHECK(cJSON_GetArraySize(ps) == 2);
        CHECK(str(cJSON_GetArrayItem(ps, 0), "callsign") == "bob" && num(cJSON_GetArrayItem(ps, 0), "seat") == 0);
        CHECK(str(cJSON_GetArrayItem(ps, 1), "callsign") == "ace" && num(cJSON_GetArrayItem(ps, 1), "seat") == 2);
        cJSON* status = r.last("status");
        CHECK(num(cJSON_GetObjectItem(status, "state"), "race_status") == 3);
        CHECK(r.timer.pendingCount() == 1);
    });
    test("timer: a full race: start, holeshot, laps, stop, results", [] {
        Rig r;
        r.up();
        const int64_t startAt = r.now() + 9000;
        r.command(START_CMD);
        r.run(9100);
        cJSON* start = r.last("rh.race_start");
        CHECK(start && num(start, "start_epoch_ms") == r.epochBase + startAt);
        CHECK(num(cJSON_GetObjectItem(r.last("status"), "state"), "race_status") == 1);
        // ace (seat 2) crosses, then again 11.5 s later.
        r.run(2000);
        const int64_t first = r.now() - 20;
        r.node.cross(2, first * 1000);
        r.run(11500);
        const int64_t second = r.now() - 20;
        r.node.cross(2, second * 1000);
        r.run(500);
        auto laps = r.ofType("rh.lap");
        CHECK(laps.size() == 2);
        if (laps.size() == 2) {
            CHECK(num(laps[0], "seat") == 2 && str(laps[0], "callsign") == "ace" && num(laps[0], "lap_number") == 0);
            CHECK(std::llabs(num(laps[0], "lap_time_stamp") - (first - startAt)) <= 2);
            CHECK(num(laps[1], "lap_number") == 1 && std::llabs(num(laps[1], "lap_time") - (second - first)) <= 2);
            CHECK(num(laps[1], "start_epoch_ms") == r.epochBase + startAt);
            CHECK(cJSON_IsFalse(cJSON_GetObjectItem(laps[1], "deleted")));
        }
        r.command(R"({"type":"command","id":"c2","action":"stop"})");
        CHECK(r.last("rh.race_stop") != nullptr);
        cJSON* res = r.last("rh.race_result");
        cJSON* lines = cJSON_GetObjectItem(res, "results");
        CHECK(cJSON_GetArraySize(lines) == 2 && str(cJSON_GetArrayItem(lines, 0), "callsign") == "ace");
        CHECK(num(cJSON_GetArrayItem(lines, 0), "laps") == 1 && isNull(cJSON_GetArrayItem(lines, 1), "fastest_lap_ms"));
        // A second stop fails, and says why.
        r.command(R"({"type":"command","id":"c3","action":"stop"})");
        cJSON* result = r.last("command_result");
        CHECK(str(result, "id") == "c3" && cJSON_IsFalse(cJSON_GetObjectItem(result, "ok")));
        CHECK(str(result, "error") == "No race is running.");
    });
    test("timer: acks clear pending; a reconnect resends the rest", [] {
        Rig r;
        r.up();
        r.command(START_CMD);
        r.run(9100);
        CHECK_EQ(r.timer.pendingCount(), 2u);
        r.command(R"({"type":"ack","session":"s1","seq":1})");
        r.command(R"({"type":"ack","session":"other","seq":2})");
        CHECK(r.timer.pendingCount() == 1);
        r.run(1000);  // past the ignore window after the start
        r.timer.socketClosed();
        r.clearSent();
        r.node.cross(0, r.node.clockUs - 1000);  // bob's holeshot while mach1 is away
        r.run(300);
        CHECK(r.sent.empty());
        CHECK(r.timer.pendingCount() == 2);
        r.timer.socketOpen(r.now());
        CHECK(r.sent.size() == 4);  // hello, status, race_start, lap
        CHECK(str(r.sent[0], "type") == "hello" && str(r.sent[1], "type") == "status");
        CHECK(num(r.sent[2], "seq") == 2 && str(r.sent[3], "type") == "rh.lap");
    });
    test("timer: commands are de-duplicated by id", [] {
        Rig r;
        r.up();
        r.command(START_CMD);
        const size_t stages = r.ofType("rh.race_stage").size();
        r.command(START_CMD);
        CHECK(r.ofType("rh.race_stage").size() == stages);
    });
    test("timer: start fails clearly without a gate or a clock", [] {
        Rig r;
        r.timer.socketOpen(0);
        r.command(START_CMD);
        cJSON* result = r.last("command_result");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(result, "ok")));
        CHECK(str(result, "error").find("not connected") != std::string::npos);
        Rig c;
        c.clock = false;
        c.up();
        c.command(START_CMD);
        CHECK(str(c.last("command_result"), "error").find("wall clock") != std::string::npos);
        CHECK(c.ofType("rh.race_stage").empty());
    });
    test("timer: discard clears the race and puts nodes back", [] {
        Rig r;
        r.up();
        r.command(R"({"type":"gate_config","levels":[],"nodes":[{"seat":1,"channel":"R2","frequency":5695}]})");
        r.command(START_CMD);
        CHECK(r.node.seats[1].frequency == 1111);
        r.command(R"({"type":"command","id":"d1","action":"discard"})");
        CHECK(r.last("rh.laps_discard") != nullptr);
        CHECK(r.timer.engine.status == RaceStatus::Ready);
        CHECK(r.node.seats[1].frequency == 5695);
        r.command(R"({"type":"command","id":"d2","action":"discard"})");
        CHECK(str(r.last("command_result"), "error") == "There is no race to discard.");
    });
    test("timer: after a race stops, nodes go back once late crossings had time", [] {
        Rig r;
        r.up();
        r.command(R"({"type":"gate_config","levels":[],"nodes":[{"seat":1,"channel":"R2","frequency":5695}]})");
        r.command(START_CMD);
        r.run(9100);
        r.command(R"({"type":"command","id":"s1","action":"stop"})");
        CHECK(r.node.seats[1].frequency == 1111);
        r.run(1000);
        CHECK(r.node.seats[1].frequency == 1111);
        r.run(1500);
        CHECK(r.node.seats[1].frequency == 5695);
    });
    test("timer: set_levels and set_channel from mission control", [] {
        Rig r;
        r.up();
        r.command(R"({"type":"command","id":"l1","action":"set_levels","seat":3,"enter":150,"exit":110})");
        CHECK(r.node.seats[3].enter == 150 && r.node.seats[3].exit == 110);
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r.last("command_result"), "ok")));
        r.command(R"({"type":"command","id":"l2","action":"set_levels","seat":3,"enter":100,"exit":110})");
        CHECK(str(r.last("command_result"), "error") == "Enter must be above exit.");
        r.command(R"({"type":"command","id":"h1","action":"set_channel","seat":4,"label":"R5","frequency":5806,"band":"R","channel":5})");
        CHECK(r.node.seats[4].frequency == 5806);
        r.command(R"({"type":"command","id":"h2","action":"set_channel","seat":4,"label":"off","frequency":0})");
        CHECK(r.node.seats[4].frequency == 1111);
        // A node in the race stays on its pilot's channel.
        r.command(START_CMD);
        r.command(R"({"type":"command","id":"h3","action":"set_channel","seat":2,"label":"R3","frequency":5732})");
        CHECK(str(r.last("command_result"), "error").find("in the race") != std::string::npos);
        CHECK(r.node.seats[2].frequency == 5658);
    });
    test("timer: watch streams nodes frames with readings and crossings, then stops", [] {
        Rig r;
        r.up();
        const size_t results = r.ofType("command_result").size();
        r.command(R"({"type":"command","id":"w1","action":"watch","ttl_s":2})");
        CHECK(r.driver.readExtremums);
        CHECK(r.ofType("command_result").size() == results);  // no answer to a watch
        r.node.seats[0].rssi = 77;
        r.run(600);
        r.node.cross(0, r.node.clockUs - 5000);
        r.run(600);
        auto frames = r.ofType("nodes");
        CHECK(frames.size() >= 3);
        cJSON* seats = cJSON_GetObjectItem(frames.back(), "seats");
        CHECK(cJSON_GetArraySize(seats) == 8);
        cJSON* rssi = cJSON_GetObjectItem(cJSON_GetArrayItem(seats, 0), "rssi");
        CHECK(cJSON_GetArraySize(rssi) >= 3);
        CHECK(cJSON_GetArrayItem(rssi, 0)->valueint == 77);
        bool marked = false;
        for (cJSON* f : frames)
            marked = marked || cJSON_GetArraySize(cJSON_GetObjectItem(f, "marks")) > 0;
        CHECK(marked);
        r.run(1500);
        const size_t count = r.ofType("nodes").size();
        r.run(1000);
        CHECK(r.ofType("nodes").size() == count);
        CHECK(!r.driver.readExtremums);
    });
    test("timer: heartbeat every 10 s; a silent mach1 is reconnected", [] {
        Rig r;
        r.up();
        const size_t before = r.ofType("status").size();
        r.node.seats.assign(8, FakeSeat());
        r.driver.setFrequencies({});
        r.run(10100);
        CHECK(r.ofType("status").size() == before + 1);
        CHECK(r.reconnects == 0);
        r.run(25000);
        CHECK(r.reconnects >= 1);
    });
    test("timer: levels resolve as gate.py: own, node default, any-node own, any-node default", [] {
        Rig r;
        r.command(R"({"type":"gate_config","levels":[
          {"seat":1,"pilot_id":5,"enter":201,"exit":101},{"seat":1,"pilot_id":null,"enter":202,"exit":102},
          {"seat":null,"pilot_id":6,"enter":203,"exit":103},{"seat":null,"pilot_id":null,"enter":204,"exit":104}]})");
        std::string from;
        CHECK(r.timer.resolveLevels(1, 5, &from)->enter == 201 && from == "pilot");
        CHECK(r.timer.resolveLevels(1, 6, &from)->enter == 202 && from == "default");
        CHECK(r.timer.resolveLevels(2, 6, &from)->enter == 203 && from == "pilot");
        CHECK(r.timer.resolveLevels(2, -1, &from)->enter == 204 && from == "default");
    });
    test("timer: seatsFromCommand falls back to pilot n on node n", [] {
        StartPilot a, b;
        a.seat = 3;
        b.seat = 3;
        auto s = seatsFromCommand({a, b}, 4);
        CHECK(s[0] && s[1] && !s[2]);
        a.seat = 9;
        b.seat = 1;
        s = seatsFromCommand({a, b}, 4);
        CHECK(s[0] && s[1]);
        a.seat = 3;
        s = seatsFromCommand({a, b}, 4);
        CHECK(s[3] && s[1] && !s[0]);
    });
}

static void settingsTests()
{
    test("timer: hello says what mission control may ask, and the race settings", [] {
        Rig r;
        r.timer.c5Input = R"({"speed":200,"distance":1,"angle":90})";
        r.up();
        cJSON* hello = r.last("hello");
        cJSON* features = cJSON_GetObjectItem(hello, "features");
        CHECK(cJSON_GetArraySize(features) == 4 && std::string(cJSON_GetArrayItem(features, 0)->valuestring) == "node_io");
        CHECK(num(cJSON_GetObjectItem(hello, "c5_input"), "angle") == 90);
        CHECK(num(cJSON_GetObjectItem(hello, "race_settings"), "minLapMs") == 4000);
    });
    test("timer: node_io carries C5 settings reads and writes, keeps the detection, and nothing else", [] {
        Rig r;
        std::map<std::string, std::string> saved;
        r.timer.saveSetting = [&](const std::string& k, const std::string& v) { saved[k] = v; };
        r.up();
        r.command(R"({"type":"command","id":"n1","action":"node_io","ops":[{"r":17,"n":2},{"w":115,"d":[3,0,61]},{"r":65,"n":86}]})");
        cJSON* result = r.last("command_result");
        CHECK(str(result, "id") == "n1" && cJSON_IsTrue(cJSON_GetObjectItem(result, "ok")));
        cJSON* data = cJSON_GetObjectItem(result, "data");
        CHECK(cJSON_GetArraySize(data) == 2);
        CHECK(std::string(cJSON_GetArrayItem(data, 0)->valuestring) == "00e0");
        CHECK(std::string(cJSON_GetArrayItem(data, 1)->valuestring).substr(0, 8) == "0307003d");
        CHECK(r.node.minPassMs == 61);
        CHECK(saved["c5det"] == "03003d" && r.timer.c5Detection == std::vector<uint8_t>({3, 0, 61}));
        // Only the C5 settings: not a frequency, not a seat's levels.
        const int before = r.node.seats[0].frequency;
        r.command(R"({"type":"command","id":"n2","action":"node_io","ops":[{"w":81,"d":[22,98]}]})");
        result = r.last("command_result");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(result, "ok")) && str(result, "error").find("Only the C5") == 0);
        CHECK(r.node.seats[0].frequency == before);
        r.command(R"({"type":"command","id":"n3","action":"node_io","ops":[{"r":65,"n":10}]})");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(r.last("command_result"), "ok")));
        // Never during a race.
        r.command(START_CMD);
        r.command(R"({"type":"command","id":"n4","action":"node_io","ops":[{"r":66,"n":36}]})");
        CHECK(str(r.last("command_result"), "error") == "C5 settings cannot change during staging or racing.");
    });
    test("timer: the kept detection is written again whenever the node (re)connects", [] {
        Rig r;
        r.timer.c5Detection = {3, 0, 61};
        r.up();
        CHECK(r.node.minPassMs == 61);
        r.node.minPassMs = 0;  // the receiver was power-cycled
        r.timer.gateDown(r.now());
        r.driver.start(nullptr);
        r.timer.gateUp(&r.driver, r.now());
        CHECK(r.node.minPassMs == 61);
        bool restored = false;
        for (const auto& l : r.lines)
            restored = restored || l.find("C5 detection 61 ms restored") != std::string::npos;
        CHECK(restored);
        // A node that is not a C5 is left alone.
        Rig other;
        other.node.proc = "STM32F1";
        other.node.c5 = false;
        other.timer.c5Detection = {3, 0, 61};
        other.up();
        CHECK(other.node.writes.empty() || other.node.writes.back()[0] != 0x73);
    });
    test("timer: race settings from mission control apply, are kept, and go on the gate", [] {
        Rig r;
        std::map<std::string, std::string> saved;
        r.timer.saveSetting = [&](const std::string& k, const std::string& v) { saved[k] = v; };
        r.up();
        r.command(R"({"type":"command","id":"s1","action":"race_settings","settings":{"minLapMs":6000,"ignoreAfterStartMs":800}})");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r.last("command_result"), "ok")));
        CHECK(r.timer.engine.settings.minLapMs == 6000 && r.timer.engine.settings.ignoreAfterStartMs == 800);
        CHECK(saved["minlap"] == "6000" && saved["ignorems"] == "800");
        cJSON* gate = r.last("gate_settings");
        CHECK(num(cJSON_GetObjectItem(cJSON_GetObjectItem(gate, "settings"), "race"), "minLapMs") == 6000);
        // Saved on the club's setup: applied from gate_config.
        r.command(R"({"type":"gate_config","levels":[],"nodes":[],"setup":{"gate":"start","confirm":false,"settings":{"race":{"minLapMs":5000}}}})");
        CHECK(r.timer.engine.settings.minLapMs == 5000);
        // The geometry mission control applied a detection with.
        r.command(R"({"type":"command","id":"s2","action":"c5_input","input":{"speed":180,"distance":1,"angle":90}})");
        CHECK(saved["c5in"] == R"({"speed":180,"distance":1,"angle":90})");
        CHECK(num(cJSON_GetObjectItem(r.last("hello"), "c5_input"), "speed") == 180);
    });
}

static void identityTests()
{
    test("identity: the relay is the node's timer in mach1, under the node's name", [] {
        Rig r;
        r.timer.relayUid = "relay-aabbccddeeff";
        std::string reconnectedAs;
        std::map<std::string, std::string> saved;
        r.timer.identity = [&](const std::string& uid) { reconnectedAs = uid; };
        r.timer.saveSetting = [&](const std::string& k, const std::string& v) { saved[k] = v; };
        r.up();
        CHECK(r.driver.info().nodeId == "3c84270a1b2c" && r.driver.info().nodeName == "Start gate");
        CHECK(r.timer.uid == "c5-3c84270a1b2c" && reconnectedAs == "c5-3c84270a1b2c");
        CHECK(r.timer.name == "Start gate" && saved["name"] == "Start gate" && saved["nodeid"] == "3c84270a1b2c");
        cJSON* hello = cJSON_Parse(r.timer.hello().c_str());
        CHECK(str(hello, "node_id") == "3c84270a1b2c" && str(hello, "relay_uid") == "relay-aabbccddeeff");
        CHECK(str(hello, "name") == "Start gate" && str(hello, "route") == "relay");
        cJSON_Delete(hello);
    });
    test("identity: a rename from mach1 goes to the node and back in hello", [] {
        Rig r;
        r.up();
        r.command(R"({"type":"command","id":"n1","action":"set_name","name":"Finish gate"})");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r.last("command_result"), "ok")));
        CHECK(r.node.nodeName == "Finish gate" && r.timer.name == "Finish gate");
        CHECK(str(r.last("hello"), "name") == "Finish gate");
        // The node keeps 32 bytes; a longer name is cut there, never inside a character.
        r.command(R"({"type":"command","id":"n2","action":"set_name","name":"A very long timer name that goes on and on"})");
        CHECK(r.node.nodeName == "A very long timer name that goes" && r.timer.name == r.node.nodeName);
        r.command(R"({"type":"command","id":"n3","action":"set_name","name":"  "})");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(r.last("command_result"), "ok")));
    });
    test("identity: a rename on the relay while the node is away reaches the node when it connects", [] {
        Rig r;
        r.timer.socketOpen(r.now());
        CHECK(r.timer.rename("Pit gate").empty() && r.timer.namePending);
        r.driver.start(nullptr);
        r.timer.gateUp(&r.driver, r.now());
        CHECK(r.node.nodeName == "Pit gate" && !r.timer.namePending);
    });
    test("identity: a node without an id leaves the relay as itself", [] {
        Rig r;
        r.node.hasIdentity = false;
        r.timer.uid = "relay-aabbccddeeff";
        r.up();
        CHECK(r.timer.uid == "relay-aabbccddeeff" && r.driver.info().nodeId.empty());
        CHECK(str(r.last("hello"), "node_id").empty());
    });
}

// ---------------------------------------------------------------------------
// USB link: mission control on a laptop carries the relay's mach1 connection

struct LinkRig : Rig {
    UsbLink link{timer};
    std::vector<std::string> toHost;
    std::vector<bool> routes;
    int64_t hostClock = 0;
    LinkRig()
    {
        link.toHost = [this](const std::string& line) { toHost.push_back(line); };
        link.routeChanged = [this](bool via) { routes.push_back(via); };
        link.hostClock = [this](int64_t ms) { hostClock = ms; };
        link.log = [this](const std::string& l) { lines.push_back(l); };
        link.rhVersion = "c5-relay test";
    }
    void line(const std::string& json, bool wifi = true) { link.received(json.c_str(), now(), wifi); }
    cJSON* lastToHost() { return toHost.empty() ? nullptr : cJSON_Parse(toHost.back().c_str()); }
};

static void usbLinkTests()
{
    test("usb link: link_open answers link_info with the timer's uid and takes the page's clock", [] {
        LinkRig r;
        r.timer.uid = "c5-3c84270a1b2c";
        r.timer.name = "Start gate";
        r.line(R"({"type":"link_open","time_ms":1762800123456})");
        CHECK(r.link.alive() && !r.link.viaLink());
        CHECK(r.hostClock == 1762800123456LL);
        cJSON* info = r.lastToHost();
        CHECK(str(info, "type") == "link_info" && str(info, "uid") == "c5-3c84270a1b2c");
        CHECK(str(info, "name") == "Start gate" && cJSON_IsTrue(cJSON_GetObjectItem(info, "cloud")));
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(info, "via_link")));
        cJSON_Delete(info);
        // Not link messages: ignored.
        r.line(R"({"type":"command","id":"x","action":"stop"})");
        r.line("not json");
        CHECK(r.routes.empty());
    });
    test("usb link: a race started over Wi-Fi goes on through USB and back, nothing lost", [] {
        LinkRig r;
        r.up();  // Wi-Fi socket open
        r.command(START_CMD);
        r.run(9100);
        r.command(R"({"type":"ack","session":"s1","seq":1})");  // race_stage acked
        CHECK_EQ(r.timer.pendingCount(), 1u);                    // race_start not yet
        r.run(1000);
        // Plugged in: the page opens the link, then its own socket to mach1.
        r.line(R"({"type":"link_open","time_ms":1762800000000})");
        r.clearSent();
        r.line(R"({"type":"link_up","prefer":true})");
        CHECK(r.link.viaLink() && r.routes.size() == 1 && r.routes[0]);
        // What a fresh connection gets: hello, status, the unacknowledged race_start.
        CHECK(r.sent.size() == 3 && str(r.sent[0], "type") == "hello" && str(r.sent[2], "type") == "rh.race_start");
        CHECK(str(r.sent[0], "route") == "relay_usb");
        CHECK(r.timer.socketConnected());
        // The race goes on: a lap is sent (through the page), acks come back through it.
        r.clearSent();
        r.node.cross(0, r.node.clockUs - 1000);
        r.run(300);
        CHECK(r.ofType("rh.lap").size() == 1);
        r.line(R"({"type":"link_cloud","message":{"type":"ack","session":"s1","seq":2}})");
        CHECK_EQ(r.timer.pendingCount(), 1u);  // the lap is left
        // Commands from mach1 arrive through the page too.
        r.line(R"({"type":"link_cloud","message":{"type":"command","id":"c9","action":"set_levels","seat":3,"enter":150,"exit":110}})");
        CHECK(r.node.seats[3].enter == 150);
        // Unplugged: straight back to Wi-Fi; the reopened socket resends the lap.
        r.link.tick(r.now(), false, true);
        CHECK(!r.link.viaLink() && !r.link.alive() && r.routes.size() == 2 && !r.routes[1]);
        CHECK(!r.timer.socketConnected());
        r.clearSent();
        r.timer.socketOpen(r.now());
        CHECK(r.sent.size() == 3 && str(r.sent[2], "type") == "rh.lap");
        CHECK(str(r.sent[0], "route") == "relay");
        CHECK(r.timer.engine.status == RaceStatus::Racing);
    });
    test("usb link: a quiet page or a closed page socket hands mach1 back to Wi-Fi", [] {
        LinkRig r;
        r.up();
        r.line(R"({"type":"link_open"})");
        r.line(R"({"type":"link_up","prefer":true})");
        CHECK(r.link.viaLink());
        // Still heard from within the timeout: stays.
        r.run(4000);
        r.line(R"({"type":"link_open"})");
        r.run(4000);
        r.link.tick(r.now(), true, true);
        CHECK(r.link.viaLink());
        r.run(USB_LINK_TIMEOUT_MS + 100);
        r.link.tick(r.now(), true, true);
        CHECK(!r.link.viaLink() && !r.link.alive());
        // The page's socket to mach1 closes: link_down.
        r.line(R"({"type":"link_open"})");
        r.line(R"({"type":"link_up","prefer":true})");
        CHECK(r.link.viaLink());
        r.line(R"({"type":"link_down"})");
        CHECK(!r.link.viaLink() && r.link.alive());
        // A second link_up on the same route resends what a fresh socket gets.
        r.line(R"({"type":"link_up","prefer":true})");
        r.clearSent();
        r.line(R"({"type":"link_up","prefer":true})");
        CHECK(r.ofType("hello").size() == 1);
    });
    test("usb link: without prefer the page stands in only while Wi-Fi is down", [] {
        LinkRig r;
        r.up();
        r.line(R"({"type":"link_open"})");
        r.line(R"({"type":"link_up"})", true);
        CHECK(!r.link.viaLink() && r.routes.empty());
        r.link.tick(r.now(), true, false);  // Wi-Fi socket lost
        CHECK(r.link.viaLink());
    });
    test("usb link: mach1 quiet through the page asks it to reconnect, Wi-Fi meanwhile", [] {
        LinkRig r;
        r.up();
        r.line(R"({"type":"link_open"})");
        r.line(R"({"type":"link_up","prefer":true})");
        r.link.drop(r.now(), "mach1 went quiet", true);
        CHECK(!r.link.viaLink() && r.routes.back() == false);
        CHECK(r.toHost.back() == R"({"type":"link_reconnect"})");
        // link_cloud is ignored while the route is Wi-Fi.
        r.line(R"({"type":"link_cloud","message":{"type":"command","id":"c8","action":"set_levels","seat":3,"enter":151,"exit":111}})");
        CHECK(r.node.seats[3].enter != 151);
    });
    test("usb link: a probe is answered with link_info and nothing else; USB watch frames are faster", [] {
        LinkRig r;
        r.up();
        r.line(R"({"type":"link_probe"})");
        cJSON* info = r.lastToHost();
        CHECK(str(info, "type") == "link_info" && !r.link.alive());
        cJSON_Delete(info);
        CHECK(r.timer.watchFrameMs == WATCH_FRAME_MS);
        r.line(R"({"type":"link_open"})");
        r.line(R"({"type":"link_up","prefer":true})");
        CHECK(r.timer.watchFrameMs == USB_WATCH_FRAME_MS);
        r.link.tick(r.now(), false, true);
        CHECK(r.timer.watchFrameMs == WATCH_FRAME_MS);
    });
    test("usb link: link_send wraps a mach1 message", [] {
        CHECK(UsbLink::wrap(R"({"type":"status"})") == R"({"type":"link_send","message":{"type":"status"}})");
    });
}

int main()
{
    CHECK(relayConstantsChecked() == 1);
    engineTests();
    driverTests();
    timerTests();
    settingsTests();
    identityTests();
    usbLinkTests();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
