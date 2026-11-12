// The race itself, as RotorHazard runs it. A port of mach1's browser timer
// engine (frontend/src/lib/browserTimer/raceEngine.ts).
//
// Pure logic: every call is given the time (monotonic ms), and every outcome
// is returned as events for the caller to send. Nothing here waits on a
// timer, so a late tick only delays when an event is sent, never its times.
//
// Laps, holeshot start (RotorHazard's default):
//   the first crossing after the start is lap 0, lap_time = its stamp;
//   crossing n is lap n, lap_time = stamp_n - stamp_(n-1);
//   stamp = ms from the start to the crossing.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace relay {

enum class RaceStatus { Ready, Staging, Racing, Done };
// RotorHazard's RaceStatus values, which mach1's status reports carry.
int raceStatusCode(RaceStatus s);
const char* raceStatusName(RaceStatus s);

constexpr int WIN_MOST_PROGRESS = 1;
constexpr int WIN_FIRST_TO_LAP_X = 2;
constexpr int WIN_FASTEST_CONSECUTIVE = 4;

struct RaceFormat {
    std::string name;
    bool unlimitedTime = true;
    int raceTimeSec = 0;
    int lapGraceSec = 0;
    int startBehavior = 0;
    int winCondition = WIN_MOST_PROGRESS;
    int numberLapsWin = 0;
};

struct EngineSettings {
    // A crossing sooner than this after the pilot's previous one is dropped.
    int64_t minLapMs = 4000;
    // Crossings this soon after the start are ignored.
    int64_t ignoreAfterStartMs = 500;
    // After a time-up or grace deadline, wait this long for crossings still
    // on their way from the node.
    int64_t settleMs = 400;
};

struct Crossing {
    int lapNumber = 0;
    int64_t lapTime = 0;
    int64_t lapTimeStamp = 0;
    bool operator==(const Crossing& o) const
    {
        return lapNumber == o.lapNumber && lapTime == o.lapTime && lapTimeStamp == o.lapTimeStamp;
    }
};

struct ResultLine {
    std::string callsign;
    int position = 0;
    int laps = 0;
    int64_t totalTimeMs = 0;
    int64_t fastestLapMs = -1;  // -1: none
};

enum class EventKind { RaceStart, Lap, RaceStop, RaceResult, Dropped };

struct EngineEvent {
    explicit EngineEvent(EventKind k) : kind(k) {}
    EventKind kind;
    int seat = -1;
    int64_t at = 0;  // RaceStart: the start; RaceStop: when it stopped; Dropped: the crossing
    std::string callsign;
    Crossing lap;
    std::string reason;  // RaceStop: command/time/finished; Dropped: why
    std::vector<ResultLine> results;
};

class RaceEngine {
public:
    RaceStatus status = RaceStatus::Ready;
    RaceFormat format;
    bool hasFormat = false;
    int64_t startAt = 0;
    bool stopped = false;
    int64_t stoppedAt = 0;
    EngineSettings settings;

    // Seat -> callsign for the staged race.
    std::map<int, std::string> pilots() const;
    std::vector<Crossing> laps(int seat) const;

    // Stage a race to start at `startAt`. The caller stops any running race first.
    void stage(const std::map<int, std::string>& pilots, const RaceFormat& format, int64_t startAt);
    // Back to ready, forgetting the race.
    void clear();
    // Advance the clock: the start, a time limit, the end of the grace period.
    std::vector<EngineEvent> tick(int64_t now);
    // A crossing on `seat` at time `at`, reported by the node (maybe a little late).
    std::vector<EngineEvent> crossing(int seat, int64_t at, int64_t now);
    std::vector<EngineEvent> crossing(int seat, int64_t at) { return crossing(seat, at, at); }
    // Stop now (a command, or a restage). Staging or racing only.
    std::vector<EngineEvent> stop(int64_t now);
    // Results in RotorHazard's leaderboard order for the format.
    std::vector<ResultLine> results() const;

private:
    struct SeatRun {
        std::string callsign;
        std::vector<Crossing> crossings;
        bool finished = false;
        bool hasFinishedStamp = false;
        int64_t finishedStamp = 0;
    };
    bool timeUpAt(int64_t* at) const;
    bool graceEndsAt(int64_t* at) const;
    int lapTarget() const;  // 0: none
    std::vector<EngineEvent> finish(int64_t at, const char* reason);
    std::map<int, SeatRun> seats_;
};

}  // namespace relay
