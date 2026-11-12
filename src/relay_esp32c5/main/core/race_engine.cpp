#include "race_engine.h"

#include <algorithm>
#include <cmath>

namespace relay {

int raceStatusCode(RaceStatus s)
{
    switch (s) {
    case RaceStatus::Ready: return 0;
    case RaceStatus::Racing: return 1;
    case RaceStatus::Done: return 2;
    case RaceStatus::Staging: return 3;
    }
    return 0;
}

const char* raceStatusName(RaceStatus s)
{
    switch (s) {
    case RaceStatus::Ready: return "ready";
    case RaceStatus::Racing: return "racing";
    case RaceStatus::Done: return "done";
    case RaceStatus::Staging: return "staging";
    }
    return "ready";
}

std::map<int, std::string> RaceEngine::pilots() const
{
    std::map<int, std::string> out;
    for (const auto& [seat, run] : seats_)
        out[seat] = run.callsign;
    return out;
}

std::vector<Crossing> RaceEngine::laps(int seat) const
{
    auto it = seats_.find(seat);
    return it == seats_.end() ? std::vector<Crossing>() : it->second.crossings;
}

void RaceEngine::stage(const std::map<int, std::string>& pilots, const RaceFormat& f, int64_t at)
{
    seats_.clear();
    for (const auto& [seat, callsign] : pilots) {
        SeatRun run;
        run.callsign = callsign;
        seats_[seat] = run;
    }
    format = f;
    hasFormat = true;
    startAt = at;
    stopped = false;
    stoppedAt = 0;
    status = RaceStatus::Staging;
}

void RaceEngine::clear()
{
    seats_.clear();
    format = RaceFormat();
    hasFormat = false;
    stopped = false;
    stoppedAt = 0;
    status = RaceStatus::Ready;
}

bool RaceEngine::timeUpAt(int64_t* at) const
{
    if (!hasFormat || format.unlimitedTime || !(format.raceTimeSec > 0))
        return false;
    *at = startAt + (int64_t)format.raceTimeSec * 1000;
    return true;
}

bool RaceEngine::graceEndsAt(int64_t* at) const
{
    int64_t timeUp;
    if (!timeUpAt(&timeUp))
        return false;
    *at = timeUp + (int64_t)std::max(0, format.lapGraceSec) * 1000;
    return true;
}

int RaceEngine::lapTarget() const
{
    return hasFormat && format.winCondition == WIN_FIRST_TO_LAP_X && format.numberLapsWin > 0 ? format.numberLapsWin
                                                                                               : 0;
}

std::vector<EngineEvent> RaceEngine::tick(int64_t now)
{
    std::vector<EngineEvent> events;
    if (status == RaceStatus::Staging && now >= startAt) {
        status = RaceStatus::Racing;
        EngineEvent e{EventKind::RaceStart};
        e.at = startAt;
        events.push_back(e);
    }
    if (status == RaceStatus::Racing) {
        int64_t deadline;
        if (graceEndsAt(&deadline) && now >= deadline + settings.settleMs) {
            auto more = finish(deadline, "time");
            events.insert(events.end(), more.begin(), more.end());
        }
    }
    return events;
}

std::vector<EngineEvent> RaceEngine::crossing(int seat, int64_t at, int64_t now)
{
    // A crossing just before a stop may arrive just after it; the start must
    // have happened for it to count either way.
    std::vector<EngineEvent> events = tick(now);
    auto it = seats_.find(seat);
    const bool lateForDone = status == RaceStatus::Done && stopped && at <= stoppedAt;
    if (it == seats_.end() || (status != RaceStatus::Racing && !lateForDone))
        return events;
    SeatRun& run = it->second;
    const int64_t stamp = at - startAt;
    auto dropped = [&](const std::string& why) {
        EngineEvent e{EventKind::Dropped};
        e.seat = seat;
        e.at = at;
        e.reason = why;
        events.push_back(e);
        return events;
    };
    if (stamp < settings.ignoreAfterStartMs)
        return dropped(stamp < 0 ? "before the start" : "just after the start");
    if (stopped && at > stoppedAt)
        return events;
    if (run.finished)
        return dropped("already finished");
    const Crossing* previous = run.crossings.empty() ? nullptr : &run.crossings.back();
    const int64_t lapTime = previous ? stamp - previous->lapTimeStamp : stamp;
    if (previous && lapTime < settings.minLapMs)
        return dropped(std::to_string(lapTime) + " ms after the last crossing");
    // A crossing after the deadline (the grace period) is beyond any lap.
    int64_t deadline;
    if (graceEndsAt(&deadline) && at > deadline)
        return events;
    Crossing lap;
    lap.lapNumber = (int)run.crossings.size();
    lap.lapTime = lapTime;
    lap.lapTimeStamp = stamp;
    run.crossings.push_back(lap);
    EngineEvent e{EventKind::Lap};
    e.seat = seat;
    e.at = at;
    e.callsign = run.callsign;
    e.lap = lap;
    events.push_back(e);
    if (status == RaceStatus::Done) {
        // It happened before the stop but arrived after the results went out.
        EngineEvent r{EventKind::RaceResult};
        r.results = results();
        events.push_back(r);
        return events;
    }
    int64_t timeUp;
    const bool hasTimeUp = timeUpAt(&timeUp);
    const int target = lapTarget();
    if (target && lap.lapNumber >= target) {
        run.finished = true;
        run.hasFinishedStamp = true;
        run.finishedStamp = stamp;
    } else if (hasTimeUp && at >= timeUp && format.lapGraceSec > 0) {
        // Time is up: this was the pilot's last lap.
        run.finished = true;
        run.hasFinishedStamp = true;
        run.finishedStamp = stamp;
    }
    if (status == RaceStatus::Racing &&
        std::all_of(seats_.begin(), seats_.end(), [](const auto& kv) { return kv.second.finished; })) {
        auto more = finish(at, "finished");
        events.insert(events.end(), more.begin(), more.end());
    }
    return events;
}

std::vector<EngineEvent> RaceEngine::stop(int64_t now)
{
    if (status != RaceStatus::Staging && status != RaceStatus::Racing)
        return {};
    // A time-limited race whose deadline passed ends at the deadline.
    int64_t deadline;
    const bool limited = graceEndsAt(&deadline);
    return finish(limited && status == RaceStatus::Racing ? std::min(now, deadline) : now, "command");
}

std::vector<EngineEvent> RaceEngine::finish(int64_t at, const char* reason)
{
    status = RaceStatus::Done;
    stopped = true;
    stoppedAt = at;
    EngineEvent s{EventKind::RaceStop};
    s.at = at;
    s.reason = reason;
    EngineEvent r{EventKind::RaceResult};
    r.results = results();
    return {s, r};
}

std::vector<ResultLine> RaceEngine::results() const
{
    struct Line {
        const SeatRun* run;
        ResultLine out;
        bool crossed;
    };
    std::vector<Line> lines;
    for (const auto& [seat, run] : seats_) {
        Line l{&run, ResultLine(), !run.crossings.empty()};
        l.out.callsign = run.callsign;
        int64_t fastest = -1;
        for (const Crossing& c : run.crossings) {
            if (c.lapNumber < 1)
                continue;
            l.out.laps++;
            if (fastest < 0 || c.lapTime < fastest)
                fastest = c.lapTime;
        }
        l.out.fastestLapMs = fastest;
        l.out.totalTimeMs = run.crossings.empty() ? 0 : run.crossings.back().lapTimeStamp;
        lines.push_back(l);
    }
    const int target = lapTarget();
    auto progress = [](const Line& a, const Line& b) {
        if (a.crossed != b.crossed)
            return a.crossed;
        if (a.out.laps != b.out.laps)
            return a.out.laps > b.out.laps;
        return a.out.totalTimeMs < b.out.totalTimeMs;
    };
    std::stable_sort(lines.begin(), lines.end(), [&](const Line& a, const Line& b) {
        if (target) {
            const bool aDone = a.out.laps >= target, bDone = b.out.laps >= target;
            if (aDone && bDone) {
                const int64_t at = a.run->hasFinishedStamp ? a.run->finishedStamp : a.out.totalTimeMs;
                const int64_t bt = b.run->hasFinishedStamp ? b.run->finishedStamp : b.out.totalTimeMs;
                return at < bt;
            }
            if (aDone != bDone)
                return aDone;
        }
        return progress(a, b);
    });
    std::vector<ResultLine> out;
    for (size_t i = 0; i < lines.size(); i++) {
        lines[i].out.position = (int)i + 1;
        out.push_back(lines[i].out);
    }
    return out;
}

}  // namespace relay
