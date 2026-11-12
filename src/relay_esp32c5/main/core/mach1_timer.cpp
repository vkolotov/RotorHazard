#include "mach1_timer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <set>

#include "cJSON.h"
#include "channels.h"

namespace relay {

namespace {

struct BuiltInFormat {
    int id;
    RaceFormat format;
};

// Formats offered in hello, for a heat started by hand (the run order sends its own).
const std::vector<BuiltInFormat>& builtInFormats()
{
    static const std::vector<BuiltInFormat> formats = [] {
        RaceFormat practice{"Practice · until stopped", true, 0, 0, 0, WIN_MOST_PROGRESS, 0};
        RaceFormat qualifying{"Qualifying · 2 min, finish the lap", false, 120, 60, 0, WIN_FASTEST_CONSECUTIVE, 0};
        RaceFormat race{"Race · first to 3 laps", true, 0, 0, 0, WIN_FIRST_TO_LAP_X, 3};
        return std::vector<BuiltInFormat>{{1, practice}, {2, qualifying}, {3, race}};
    }();
    return formats;
}

bool isInt(const cJSON* item)
{
    return cJSON_IsNumber(item) && item->valuedouble == (double)(int64_t)item->valuedouble;
}

int64_t getInt(const cJSON* obj, const char* key, int64_t fallback)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return isInt(item) ? (int64_t)item->valuedouble : fallback;
}

std::string getStr(const cJSON* obj, const char* key)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

bool getBool(const cJSON* obj, const char* key)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item))
        return cJSON_IsTrue(item);
    return cJSON_IsNumber(item) && item->valuedouble != 0;
}

void addIntOrNull(cJSON* obj, const char* key, int64_t value, int64_t nullValue = -1)
{
    if (value == nullValue)
        cJSON_AddNullToObject(obj, key);
    else
        cJSON_AddNumberToObject(obj, key, (double)value);
}

void addStrOrNull(cJSON* obj, const char* key, const std::string& value)
{
    if (value.empty())
        cJSON_AddNullToObject(obj, key);
    else
        cJSON_AddStringToObject(obj, key, value.c_str());
}

std::string print(cJSON* message)
{
    char* text = cJSON_PrintUnformatted(message);
    std::string out = text ? text : "";
    cJSON_free(text);
    return out;
}

StartPilot parsePilot(const cJSON* p)
{
    StartPilot pilot;
    pilot.pilotId = (int)getInt(p, "pilot_id", -1);
    pilot.callsign = getStr(p, "callsign");
    pilot.band = getStr(p, "band");
    pilot.bandName = getStr(p, "band_name");
    pilot.channel = (int)getInt(p, "channel", -1);
    pilot.frequency = (int)getInt(p, "frequency", 0);
    pilot.seat = (int)getInt(p, "seat", -1);
    const cJSON* levels = cJSON_GetObjectItemCaseSensitive(p, "levels");
    if (cJSON_IsObject(levels) && isInt(cJSON_GetObjectItemCaseSensitive(levels, "enter")) &&
        isInt(cJSON_GetObjectItemCaseSensitive(levels, "exit"))) {
        pilot.hasLevels = true;
        pilot.levels.enter = (int)getInt(levels, "enter", 0);
        pilot.levels.exit = (int)getInt(levels, "exit", 0);
        pilot.levelsFrom = getStr(levels, "from");
    }
    return pilot;
}

RaceFormat parseFormat(const cJSON* f)
{
    RaceFormat format;
    format.name = getStr(f, "name");
    format.unlimitedTime = getBool(f, "unlimited_time");
    format.raceTimeSec = (int)getInt(f, "race_time_sec", 0);
    format.lapGraceSec = (int)getInt(f, "lap_grace_sec", 0);
    format.startBehavior = (int)getInt(f, "start_behavior", 0);
    format.winCondition = (int)getInt(f, "win_condition", WIN_MOST_PROGRESS);
    format.numberLapsWin = (int)getInt(f, "number_laps_win", 0);
    return format;
}

// C5 settings mission control may read and write through the relay
// (frontend/src/lib/browserTimer/c5Config.ts): command -> exact size. Nothing
// else: the timing commands stay the relay's own.
constexpr uint8_t C5_FEATURES = 0x11, C5_READ_DETECTION = 0x41, C5_READ_CALIBRATION = 0x42;
constexpr uint8_t C5_WRITE_DETECTION = 0x73, C5_WRITE_CALIBRATION = 0x74;
constexpr size_t C5_DETECTION_SIZE = 86, C5_CALIBRATION_SIZE = 36;

size_t c5ReadSize(int command)
{
    switch (command) {
    case C5_FEATURES: return 2;
    case C5_READ_DETECTION: return C5_DETECTION_SIZE;
    case C5_READ_CALIBRATION: return C5_CALIBRATION_SIZE;
    default: return 0;
    }
}

size_t c5WriteSize(int command)
{
    switch (command) {
    case C5_WRITE_DETECTION: return 3;
    case C5_WRITE_CALIBRATION: return C5_CALIBRATION_SIZE;
    default: return 0;
    }
}

std::string toHex(const std::vector<uint8_t>& bytes)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

}  // namespace

std::vector<uint8_t> fromHex(const std::string& hex)
{
    std::vector<uint8_t> out;
    auto value = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = value(hex[i]), lo = value(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return {};
        out.push_back((uint8_t)(hi * 16 + lo));
    }
    return out;
}

std::vector<std::optional<StartPilot>> seatsFromCommand(const std::vector<StartPilot>& pilots, int seats)
{
    std::set<int> chosen;
    bool valid = true;
    for (const StartPilot& p : pilots) {
        if (p.seat < 0 || p.seat >= seats || !chosen.insert(p.seat).second)
            valid = false;
    }
    std::vector<std::optional<StartPilot>> bySeat(seats);
    for (size_t i = 0; i < pilots.size() && (int)i < seats; i++)
        bySeat[valid ? pilots[i].seat : (int)i] = pilots[i];
    return bySeat;
}

// ---------------------------------------------------------------------------
// Plumbing

void Mach1Timer::note(const std::string& line)
{
    if (log)
        log(line);
}

void Mach1Timer::sendJson(cJSON* message)
{
    if (send && connected_)
        send(print(message));
    cJSON_Delete(message);
}

int64_t Mach1Timer::epochOf(int64_t monoMs) const
{
    int64_t epoch = 0;
    if (toEpoch && toEpoch(monoMs, &epoch))
        return epoch;
    return 0;
}

std::string Mach1Timer::isoNow(int64_t now) const
{
    const int64_t ms = epochOf(now);
    const time_t seconds = (time_t)(ms / 1000);
    struct tm t;
    gmtime_r(&seconds, &t);
    char buf[64];
    snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, (int)(ms % 1000));
    return buf;
}

// A race message: numbered and kept until acknowledged.
void Mach1Timer::sendRace(const char* kind, cJSON* fields)
{
    seq_++;
    cJSON* message = fields ? fields : cJSON_CreateObject();
    // The envelope goes first, as the plugin sends it.
    cJSON* envelope = cJSON_CreateObject();
    cJSON_AddStringToObject(envelope, "type", (std::string("rh.") + kind).c_str());
    cJSON_AddStringToObject(envelope, "session", session.c_str());
    cJSON_AddNumberToObject(envelope, "seq", seq_);
    cJSON_AddStringToObject(envelope, "ts", isoNow(now_).c_str());
    addIntOrNull(envelope, "event_id", race_.eventId);
    addStrOrNull(envelope, "race_key", race_.raceKey);
    while (message->child) {
        cJSON* item = cJSON_DetachItemViaPointer(message, message->child);
        cJSON_AddItemToObject(envelope, item->string, item);
    }
    cJSON_Delete(message);
    std::string text = print(envelope);
    cJSON_Delete(envelope);
    pending_[seq_] = text;
    while (pending_.size() > MAX_PENDING)
        pending_.erase(pending_.begin());
    if (send && connected_)
        send(text);
}

// ---------------------------------------------------------------------------
// What the relay reports about itself

std::string Mach1Timer::hello() const
{
    cJSON* m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "hello");
    cJSON_AddStringToObject(m, "name", name.c_str());
    cJSON_AddStringToObject(m, "rh_version", (std::string("c5-relay ") + RELAY_VERSION).c_str());
    cJSON_AddNumberToObject(m, "node_count", driver_ ? driver_->info().seats : 0);
    cJSON_AddStringToObject(m, "plugin_version", PLUGIN_PROTOCOL_VERSION);
    // What mission control may ask of it beyond the plugin's protocol.
    cJSON* features = cJSON_AddArrayToObject(m, "features");
    cJSON_AddItemToArray(features, cJSON_CreateString("node_io"));
    cJSON_AddItemToArray(features, cJSON_CreateString("c5_input"));
    cJSON_AddItemToArray(features, cJSON_CreateString("race_settings"));
    if (!c5Input.empty()) {
        cJSON* input = cJSON_Parse(c5Input.c_str());
        if (cJSON_IsObject(input))
            cJSON_AddItemToObject(m, "c5_input", input);
        else
            cJSON_Delete(input);
    }
    cJSON* race = cJSON_AddObjectToObject(m, "race_settings");
    cJSON_AddNumberToObject(race, "minLapMs", (double)engine.settings.minLapMs);
    cJSON_AddNumberToObject(race, "ignoreAfterStartMs", (double)engine.settings.ignoreAfterStartMs);
    // Levels and channels are saved per gate; a C5 gate is named by its
    // processor, as the browser timer names it, so both share them.
    if (driver_ && !driver_->info().procType.empty())
        cJSON_AddStringToObject(m, "hardware", driver_->info().procType.c_str());
    cJSON* formats = cJSON_AddArrayToObject(m, "formats");
    for (const BuiltInFormat& f : builtInFormats()) {
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "id", f.id);
        cJSON_AddStringToObject(entry, "name", f.format.name.c_str());
        cJSON_AddItemToArray(formats, entry);
    }
    cJSON* frequencies = cJSON_AddArrayToObject(m, "frequencies");
    if (driver_) {
        for (size_t i = 0; i < driver_->seats().size(); i++) {
            const StartPilot* pilot = i < race_.pilots.size() && race_.pilots[i] ? &*race_.pilots[i] : nullptr;
            cJSON* entry = cJSON_CreateObject();
            addStrOrNull(entry, "band", pilot ? pilot->band : "");
            addIntOrNull(entry, "channel", pilot ? pilot->channel : -1);
            cJSON_AddNumberToObject(entry, "frequency", driver_->seats()[i].frequency);
            cJSON_AddItemToArray(frequencies, entry);
        }
    }
    std::string out = print(m);
    cJSON_Delete(m);
    return out;
}

std::string Mach1Timer::status() const
{
    cJSON* m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "status");
    cJSON* state = cJSON_AddObjectToObject(m, "state");
    cJSON_AddNumberToObject(state, "race_status", raceStatusCode(engine.status));
    cJSON_AddStringToObject(state, "race_status_name", raceStatusName(engine.status));
    cJSON_AddNullToObject(state, "heat_id");
    cJSON_AddStringToObject(state, "format", engine.hasFormat ? engine.format.name.c_str() : "");
    addIntOrNull(state, "event_id", race_.eventId);
    addStrOrNull(state, "race_key", race_.raceKey);
    addStrOrNull(state, "phase", race_.phase);
    addIntOrNull(state, "round", race_.round);
    addIntOrNull(state, "race", race_.race);
    cJSON* pilots = cJSON_AddArrayToObject(state, "pilots");
    for (const auto& [seat, callsign] : engine.pilots())
        cJSON_AddItemToArray(pilots, cJSON_CreateString(callsign.c_str()));
    if (hasStartedEpoch_)
        cJSON_AddNumberToObject(state, "started_epoch_ms", (double)startedEpochMs_);
    else
        cJSON_AddNullToObject(state, "started_epoch_ms");
    cJSON_AddNumberToObject(state, "pending", (double)pending_.size());
    std::string out = print(m);
    cJSON_Delete(m);
    return out;
}

std::string Mach1Timer::seatCallsign(int seat) const
{
    const auto pilots = engine.pilots();
    auto it = pilots.find(seat);
    return it == pilots.end() ? "" : it->second;
}

// ---------------------------------------------------------------------------
// Socket

void Mach1Timer::socketOpen(int64_t now)
{
    now_ = now;
    connected_ = true;
    lastHeard_ = now;
    lastHeartbeat_ = now;
    // What a fresh connection gets, as the plugin: hello, status, every
    // unacknowledged message.
    if (send) {
        send(hello());
        send(status());
        for (const auto& [seq, text] : pending_)
            send(text);
    }
}

void Mach1Timer::socketClosed()
{
    if (connected_)
        note("Lost mach1; races go on here and are sent when it is back");
    connected_ = false;
}

void Mach1Timer::message(const char* json, int64_t now)
{
    now_ = now;
    lastHeard_ = now;
    cJSON* m = cJSON_Parse(json);
    if (!cJSON_IsObject(m)) {
        cJSON_Delete(m);
        return;
    }
    const std::string type = getStr(m, "type");
    if (type == "welcome") {
        club_ = getStr(m, "club");
        timerId_ = (int)getInt(m, "timer_id", -1);
        note("Connected to " + (club_.empty() ? std::string("mach1") : club_));
    } else if (type == "ack") {
        if (getStr(m, "session") == session) {
            const int64_t seq = getInt(m, "seq", -1);
            if (seq >= 0 && pending_.erase((uint32_t)seq) && pending_.empty() && send && connected_)
                send(status());
        }
    } else if (type == "gate_config") {
        gateConfig(m, now);
    } else if (type == "command") {
        const std::string id = getStr(m, "id");
        if (!id.empty() && std::find(seen_.begin(), seen_.end(), id) == seen_.end()) {
            seen_.push_back(id);
            while (seen_.size() > 100)
                seen_.pop_front();
            runCommand(m, now);
        }
    }
    cJSON_Delete(m);
}

// ---------------------------------------------------------------------------
// Clock

void Mach1Timer::tick(int64_t now)
{
    now_ = now;
    emit(engine.tick(now), now);
    if (idleAt_ >= 0 && now >= idleAt_) {
        idleAt_ = -1;
        applyIdle();
    }
    if (connected_ && now - lastHeartbeat_ >= HEARTBEAT_MS) {
        lastHeartbeat_ = now;
        if (send)
            send(status());
        if (now - lastHeard_ > SOCKET_SILENCE_MS) {
            note("mach1 went quiet; reconnecting");
            if (reconnect)
                reconnect();
        }
    }
    if (watchUntil_ > 0) {
        if (now >= watchUntil_ || !driver_) {
            watchUntil_ = 0;
            samples_.clear();
            marks_.clear();
            if (driver_)
                driver_->readExtremums = false;
        } else {
            if (now >= nextSample_) {
                nextSample_ = now + WATCH_SAMPLE_MS;
                for (size_t i = 0; i < driver_->seats().size(); i++)
                    samples_[(int)i].push_back(driver_->seats()[i].rssi);
            }
            if (now >= nextFrame_) {
                nextFrame_ = now + WATCH_FRAME_MS;
                sendWatchFrame(now);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The gate

void Mach1Timer::gateUp(NodeDriver* driver, int64_t now)
{
    now_ = now;
    driver_ = driver;
    setUpOnConfig_ = true;
    const NodeInfo& info = driver->info();
    note("Gate: " + std::to_string(info.seats) + " seats, " + (info.procType.empty() ? "node" : info.procType) + " " +
         info.firmware + ", API " + std::to_string(info.apiLevel));
    if (watching(now))
        driver_->readExtremums = true;
    restoreC5Detection();
    // mach1 answers hello with gate_config, which sets the nodes up.
    if (send && connected_)
        send(hello());
}

void Mach1Timer::gateDown(int64_t now)
{
    now_ = now;
    driver_ = nullptr;
    if (send && connected_)
        send(hello());
}

void Mach1Timer::crossing(int seat, int64_t atMs, int64_t now)
{
    now_ = now;
    if (watching(now))
        marks_.push_back({seat, atMs});
    emit(engine.crossing(seat, atMs, now), now);
}

// ---------------------------------------------------------------------------
// Engine outcomes -> race messages

void Mach1Timer::emit(const std::vector<EngineEvent>& events, int64_t now)
{
    for (const EngineEvent& e : events) {
        switch (e.kind) {
        case EventKind::RaceStart: {
            startedEpochMs_ = epochOf(e.at);
            hasStartedEpoch_ = true;
            cJSON* f = cJSON_CreateObject();
            cJSON_AddNumberToObject(f, "start_epoch_ms", (double)startedEpochMs_);
            sendRace("race_start", f);
            if (send && connected_)
                send(status());
            note("Race started");
            break;
        }
        case EventKind::Lap: {
            cJSON* f = cJSON_CreateObject();
            cJSON_AddNumberToObject(f, "seat", e.seat);
            cJSON_AddStringToObject(f, "callsign", e.callsign.c_str());
            cJSON_AddNumberToObject(f, "start_epoch_ms",
                                    (double)(hasStartedEpoch_ ? startedEpochMs_ : epochOf(engine.startAt)));
            cJSON_AddNumberToObject(f, "lap_number", e.lap.lapNumber);
            cJSON_AddNumberToObject(f, "lap_time", (double)e.lap.lapTime);
            cJSON_AddNumberToObject(f, "lap_time_stamp", (double)e.lap.lapTimeStamp);
            cJSON_AddFalseToObject(f, "deleted");
            sendRace("lap", f);
            char line[96];
            snprintf(line, sizeof line, "%s: %s %.3f s", e.callsign.c_str(),
                     e.lap.lapNumber == 0 ? "holeshot" : ("lap " + std::to_string(e.lap.lapNumber)).c_str(),
                     e.lap.lapTime / 1000.0);
            note(line);
            break;
        }
        case EventKind::RaceStop:
            // Nodes back to their own set-up once late crossings have had time to arrive.
            idleAt_ = now + IDLE_AFTER_STOP_MS;
            sendRace("race_stop", nullptr);
            if (send && connected_)
                send(status());
            note(e.reason == "command" ? "Race stopped" : e.reason == "time" ? "Time is up: race over"
                                                                               : "Everyone finished");
            break;
        case EventKind::RaceResult: {
            cJSON* f = cJSON_CreateObject();
            cJSON* results = cJSON_AddArrayToObject(f, "results");
            for (const ResultLine& r : e.results) {
                cJSON* line = cJSON_CreateObject();
                cJSON_AddStringToObject(line, "callsign", r.callsign.c_str());
                cJSON_AddNumberToObject(line, "position", r.position);
                cJSON_AddNumberToObject(line, "laps", r.laps);
                cJSON_AddNumberToObject(line, "total_time_ms", (double)r.totalTimeMs);
                addIntOrNull(line, "fastest_lap_ms", r.fastestLapMs);
                cJSON_AddItemToArray(results, line);
            }
            sendRace("race_result", f);
            break;
        }
        case EventKind::Dropped:
            note("Seat " + std::to_string(e.seat + 1) + ": crossing ignored (" + e.reason + ")");
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Commands from mach1

void Mach1Timer::runCommand(cJSON* command, int64_t now)
{
    const std::string action = getStr(command, "action");
    // Watching is renewed every few seconds: no answer, no status (as the plugin).
    if (action == "watch") {
        watch(command, now);
        return;
    }
    if (action == "unwatch") {
        watchUntil_ = now;  // the next tick stops it
        return;
    }
    std::string error;
    cJSON* result = cJSON_CreateObject();
    if (action == "node_io")
        error = nodeIo(command, result);
    else if (action == "c5_input")
        error = rememberC5Input(command);
    else if (action == "race_settings")
        error = raceSettings(command);
    else if (action == "start")
        error = start(command, now);
    else if (action == "stop")
        error = stopRace(now);
    else if (action == "discard")
        error = discard(now);
    else if (action == "set_levels")
        error = setLevels(command);
    else if (action == "set_channel")
        error = setChannel(command);
    else
        error = "Unknown command " + action;
    cJSON_AddStringToObject(result, "type", "command_result");
    cJSON_AddStringToObject(result, "id", getStr(command, "id").c_str());
    cJSON_AddBoolToObject(result, "ok", error.empty());
    if (!error.empty()) {
        cJSON_AddStringToObject(result, "error", error.c_str());
        note(action + " failed: " + error);
    }
    sendJson(result);
    if (send && connected_)
        send(status());
}

std::string Mach1Timer::start(cJSON* command, int64_t now)
{
    if (!driver_ || driver_->status() != NodeStatus::Running)
        return "The gate node is not connected to the relay.";
    int64_t epoch;
    if (!toEpoch || !toEpoch(now, &epoch))
        return "The relay has no wall clock yet (waiting for network time).";
    std::vector<StartPilot> pilots;
    const cJSON* list = cJSON_GetObjectItemCaseSensitive(command, "pilots");
    const cJSON* item;
    cJSON_ArrayForEach(item, list)
    {
        if (cJSON_IsObject(item))
            pilots.push_back(parsePilot(item));
    }
    const int seats = driver_->info().seats;
    if (pilots.empty())
        return "The heat has no pilots.";
    if ((int)pilots.size() > seats)
        return "This timer has " + std::to_string(seats) + " nodes.";

    // Finish whatever is on the timer first; a finished race is just cleared.
    emit(engine.stop(now), now);
    if (engine.status == RaceStatus::Done)
        engine.clear();
    idleAt_ = -1;

    // Each pilot onto the node mach1 chose (gate.py: their own levels first,
    // then the node's channel), tuned to the pilot's channel; nodes nobody
    // flies are switched off, so they cannot record a stray crossing.
    auto bySeat = seatsFromCommand(pilots, seats);
    std::vector<int> frequencies;
    for (const auto& p : bySeat)
        frequencies.push_back(p ? p->frequency : 0);
    const std::vector<int> failed = driver_->setFrequencies(frequencies);
    if (!failed.empty()) {
        std::string list;
        for (int s : failed)
            list += (list.empty() ? "" : ", ") + std::to_string(s + 1);
        note("Seat " + list + " did not confirm its frequency");
    }
    // The levels mach1 sent for each pilot's node, else what gate_config has.
    for (int seat = 0; seat < seats; seat++)
        if (bySeat[seat])
            applyLevels(seat, bySeat[seat]->pilotId, bySeat[seat]->hasLevels ? &*bySeat[seat] : nullptr);

    RaceFormat format;
    const cJSON* f = cJSON_GetObjectItemCaseSensitive(command, "format");
    if (cJSON_IsObject(f) && !getStr(f, "name").empty()) {
        format = parseFormat(f);
    } else {
        const int formatId = (int)getInt(command, "format_id", -1);
        const std::string raceType = getStr(command, "race_type");
        const int fallbackId = raceType == "TimeTrial" ? 2 : raceType == "Race" ? 3 : 1;
        const BuiltInFormat* chosen = nullptr;
        for (const BuiltInFormat& b : builtInFormats())
            if (b.id == formatId)
                chosen = &b;
        if (!chosen)
            for (const BuiltInFormat& b : builtInFormats())
                if (b.id == fallbackId)
                    chosen = &b;
        format = chosen->format;
    }
    const int64_t requested = getInt(command, "start_delay_ms", -1);
    const int64_t delay = requested >= 1000 && requested <= 60000 ? requested : DEFAULT_START_DELAY_MS;
    const int64_t startAt = now + delay;

    race_ = RaceContext();
    race_.raceKey = getStr(command, "race_key");
    if (race_.raceKey.empty()) {
        char key[33];
        for (int i = 0; i < 32; i++)
            key[i] = "0123456789abcdef"[rand() & 15];
        key[32] = 0;
        race_.raceKey = key;
    }
    race_.eventId = (int)getInt(command, "event_id", -1);
    race_.phase = getStr(command, "phase");
    race_.raceType = getStr(command, "race_type");
    race_.round = (int)getInt(command, "round", -1);
    race_.race = (int)getInt(command, "race", -1);
    race_.heatName = getStr(command, "heat_name");
    race_.pilots = bySeat;
    if (send && connected_)
        send(hello());
    hasStartedEpoch_ = false;

    std::map<int, std::string> seated;
    for (int seat = 0; seat < seats; seat++)
        if (bySeat[seat])
            seated[seat] = bySeat[seat]->callsign;
    engine.stage(seated, format, startAt);

    cJSON* fields = cJSON_CreateObject();
    cJSON_AddStringToObject(fields, "race_type", race_.raceType.c_str());
    addStrOrNull(fields, "phase", race_.phase);
    addIntOrNull(fields, "round", race_.round);
    addIntOrNull(fields, "race", race_.race);
    addStrOrNull(fields, "heat_name", race_.heatName);
    addIntOrNull(fields, "target_laps", format.winCondition == WIN_FIRST_TO_LAP_X ? format.numberLapsWin : -1);
    cJSON_AddNumberToObject(fields, "race_time_sec", format.raceTimeSec);
    cJSON_AddBoolToObject(fields, "unlimited_time", format.unlimitedTime);
    cJSON_AddNumberToObject(fields, "start_epoch_ms", (double)epochOf(startAt));
    cJSON* out = cJSON_AddArrayToObject(fields, "pilots");
    std::string summary;
    for (int seat = 0; seat < seats; seat++) {
        if (!bySeat[seat])
            continue;
        const StartPilot& p = *bySeat[seat];
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "callsign", p.callsign.c_str());
        cJSON_AddNumberToObject(entry, "seat", seat);
        addStrOrNull(entry, "band", p.band);
        addIntOrNull(entry, "channel", p.channel);
        cJSON_AddNumberToObject(entry, "frequency", p.frequency);
        addStrOrNull(entry, "band_name", p.bandName);
        cJSON_AddItemToArray(out, entry);
        const std::string label = channelLabel(p.frequency);
        summary += (summary.empty() ? "" : ", ") + p.callsign + " " +
                   (label.empty() ? std::to_string(p.frequency) : label) + " on node " + std::to_string(seat + 1);
    }
    sendRace("race_stage", fields);
    note("Staged " + (race_.heatName.empty() ? std::string("a heat") : race_.heatName) + ": " + summary);
    return "";
}

std::string Mach1Timer::stopRace(int64_t now)
{
    if (engine.status != RaceStatus::Staging && engine.status != RaceStatus::Racing)
        return "No race is running.";
    emit(engine.stop(now), now);
    return "";
}

std::string Mach1Timer::discard(int64_t now)
{
    if (engine.status == RaceStatus::Ready)
        return "There is no race to discard.";
    emit(engine.stop(now), now);
    engine.clear();
    hasStartedEpoch_ = false;
    sendRace("laps_discard", nullptr);
    note("Race discarded");
    // Back to the nodes' own set-up.
    idleAt_ = -1;
    applyIdle();
    return "";
}

std::string Mach1Timer::setLevels(cJSON* command)
{
    const int seat = (int)getInt(command, "seat", -1);
    const int enter = (int)getInt(command, "enter", 0);
    const int exit = (int)getInt(command, "exit", 0);
    if (!driver_ || seat < 0 || seat >= (int)driver_->seats().size())
        return "No such node.";
    if (!(enter > exit && exit > 0))
        return "Enter must be above exit.";
    if (!driver_->setThresholds(seat, enter, exit))
        return "The node did not confirm the new levels.";
    note("Node " + std::to_string(seat + 1) + ": enter " + std::to_string(enter) + ", exit " + std::to_string(exit) +
         " (from mission control)");
    return "";
}

std::string Mach1Timer::setChannel(cJSON* command)
{
    const int seat = (int)getInt(command, "seat", -1);
    if (!driver_ || seat < 0 || seat >= (int)driver_->seats().size())
        return "No such node.";
    // Mission control sends {label, frequency, band, channel}; frequency 0 is off.
    int frequency = (int)getInt(command, "frequency", -1);
    const std::string label = getStr(command, "label");
    if (frequency < 0)
        frequency = channelFrequency(label);
    if (frequency < 0)
        return "No such channel.";
    const bool racing = engine.status == RaceStatus::Staging || engine.status == RaceStatus::Racing;
    if (racing && engine.pilots().count(seat))
        return "Node " + std::to_string(seat + 1) + " is in the race; change it after.";
    if (!driver_->setSeatFrequency(seat, frequency))
        note("Node " + std::to_string(seat + 1) + " did not confirm " + (label.empty() ? std::to_string(frequency) : label));
    if (frequency && seatPilotId(seat) < 0)
        applyLevels(seat, -1, nullptr);
    note("Node " + std::to_string(seat + 1) + (frequency ? " on " + (label.empty() ? std::to_string(frequency) : label) : " off"));
    if (send && connected_)
        send(hello());
    return "";
}

void Mach1Timer::watch(cJSON* command, int64_t now)
{
    const cJSON* ttl = cJSON_GetObjectItemCaseSensitive(command, "ttl_s");
    int64_t ttlMs = 20000;
    if (cJSON_IsNumber(ttl) && ttl->valuedouble > 0 && ttl->valuedouble * 1000 <= WATCH_MAX_TTL_MS)
        ttlMs = (int64_t)(ttl->valuedouble * 1000);
    if (watchUntil_ <= now) {
        samples_.clear();
        marks_.clear();
        nextSample_ = now;
        nextFrame_ = now + WATCH_FRAME_MS;
    }
    watchUntil_ = now + ttlMs;
    if (driver_)
        driver_->readExtremums = true;
}

void Mach1Timer::sendWatchFrame(int64_t now)
{
    cJSON* m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "nodes");
    cJSON_AddNumberToObject(m, "dt_ms", WATCH_SAMPLE_MS);
    cJSON* seats = cJSON_AddArrayToObject(m, "seats");
    for (size_t i = 0; i < driver_->seats().size(); i++) {
        const SeatState& s = driver_->seats()[i];
        const int pilotId = seatPilotId((int)i);
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "seat", (double)i);
        cJSON_AddNumberToObject(entry, "frequency", s.frequency);
        cJSON_AddNumberToObject(entry, "enter", s.enter);
        cJSON_AddNumberToObject(entry, "exit", s.exit);
        cJSON* rssi = cJSON_AddArrayToObject(entry, "rssi");
        for (int v : samples_[(int)i])
            cJSON_AddItemToArray(rssi, cJSON_CreateNumber(v));
        cJSON_AddBoolToObject(entry, "crossing", s.crossing);
        addIntOrNull(entry, "pilot_id", pilotId);
        addStrOrNull(entry, "callsign", seatCallsign((int)i));
        cJSON_AddItemToArray(seats, entry);
    }
    cJSON* marks = cJSON_AddArrayToObject(m, "marks");
    for (const auto& [seat, at] : marks_) {
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "seat", seat);
        cJSON_AddNumberToObject(entry, "ago_ms", (double)std::max<int64_t>(0, now - at));
        cJSON_AddItemToArray(marks, entry);
    }
    samples_.clear();
    marks_.clear();
    sendJson(m);
}

// ---------------------------------------------------------------------------
// Levels and channels between races (gate.py's rules)

void Mach1Timer::gateConfig(cJSON* m, int64_t now)
{
    (void)now;
    haveGateConfig_ = true;
    gateLevels_.clear();
    const cJSON* item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(m, "levels"))
    {
        LevelsRow row;
        row.seat = (int)getInt(item, "seat", -1);
        row.pilotId = (int)getInt(item, "pilot_id", -1);
        row.levels.enter = (int)getInt(item, "enter", 0);
        row.levels.exit = (int)getInt(item, "exit", 0);
        if (row.levels.enter > 0 && row.levels.exit > 0)
            gateLevels_.push_back(row);
    }
    gateNodes_.clear();
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(m, "nodes"))
    {
        const int seat = (int)getInt(item, "seat", -1);
        if (seat < 0)
            continue;
        GateNode node;
        const cJSON* frequency = cJSON_GetObjectItemCaseSensitive(item, "frequency");
        node.hasFrequency = isInt(frequency);
        node.frequency = node.hasFrequency ? (int)frequency->valuedouble : 0;
        node.channel = getStr(item, "channel");
        gateNodes_[seat] = node;
    }
    // The settings saved on the gate this relay serves in the club's setup (race: minimum lap, ...).
    const cJSON* setup = cJSON_GetObjectItemCaseSensitive(m, "setup");
    const cJSON* settings = cJSON_GetObjectItemCaseSensitive(setup, "settings");
    const cJSON* race = cJSON_GetObjectItemCaseSensitive(settings, "race");
    if (cJSON_IsObject(race))
        applyRaceSettings(race, true);
    if (setUpOnConfig_) {
        setUpOnConfig_ = false;
        applyIdle();
    }
}

// ---------------------------------------------------------------------------
// Settings from mission control

std::string Mach1Timer::nodeIo(cJSON* command, cJSON* result)
{
    if (!driver_ || driver_->status() != NodeStatus::Running)
        return "The gate node is not connected to the relay.";
    if (engine.status == RaceStatus::Staging || engine.status == RaceStatus::Racing)
        return "C5 settings cannot change during staging or racing.";
    const cJSON* ops = cJSON_GetObjectItemCaseSensitive(command, "ops");
    if (!cJSON_IsArray(ops) || cJSON_GetArraySize(ops) < 1 || cJSON_GetArraySize(ops) > 8)
        return "node_io needs 1 to 8 operations.";
    // Check everything before touching the node.
    const cJSON* op;
    cJSON_ArrayForEach(op, ops)
    {
        const cJSON* w = cJSON_GetObjectItemCaseSensitive(op, "w");
        const cJSON* r = cJSON_GetObjectItemCaseSensitive(op, "r");
        if (isInt(w)) {
            const cJSON* d = cJSON_GetObjectItemCaseSensitive(op, "d");
            const size_t size = c5WriteSize((int)w->valuedouble);
            if (!size || !cJSON_IsArray(d) || (size_t)cJSON_GetArraySize(d) != size)
                return "Only the C5 settings can be written through the relay.";
            const cJSON* b;
            cJSON_ArrayForEach(b, d)
            {
                if (!isInt(b) || b->valuedouble < 0 || b->valuedouble > 255)
                    return "Bad bytes in node_io.";
            }
        } else if (isInt(r)) {
            const size_t size = c5ReadSize((int)r->valuedouble);
            if (!size || getInt(op, "n", -1) != (int64_t)size)
                return "Only the C5 settings can be read through the relay.";
        } else {
            return "Bad node_io operation.";
        }
    }
    cJSON* data = cJSON_AddArrayToObject(result, "data");
    std::vector<uint8_t> detectionWritten;
    cJSON_ArrayForEach(op, ops)
    {
        const cJSON* w = cJSON_GetObjectItemCaseSensitive(op, "w");
        if (isInt(w)) {
            std::vector<uint8_t> bytes;
            const cJSON* b;
            cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(op, "d"))
                bytes.push_back((uint8_t)b->valuedouble);
            driver_->writeRaw((uint8_t)w->valuedouble, bytes);
            if ((uint8_t)w->valuedouble == C5_WRITE_DETECTION)
                detectionWritten = bytes;
            continue;
        }
        const uint8_t r = (uint8_t)getInt(op, "r", 0);
        Reply reply;
        if (driver_->read(r, c5ReadSize(r), MAX_RETRIES, &reply)) {
            cJSON_AddItemToArray(data, cJSON_CreateString(toHex(reply.data).c_str()));
            // A detection setting read back as applied: keep it, to write again after a reconnect.
            if (r == C5_READ_DETECTION && !detectionWritten.empty() && reply.data.size() == C5_DETECTION_SIZE &&
                reply.data[0] == 3 && reply.data[13] == 0 && reply.data[2] == detectionWritten[1] &&
                reply.data[3] == detectionWritten[2] && detectionWritten != c5Detection) {
                c5Detection = detectionWritten;
                if (saveSetting)
                    saveSetting("c5det", toHex(c5Detection));
                note("C5 detection " + std::to_string(detectionWritten[1] * 256 + detectionWritten[2]) + " ms from mach1, kept");
            }
        } else {
            cJSON_AddItemToArray(data, cJSON_CreateNull());
        }
    }
    return "";
}

std::string Mach1Timer::rememberC5Input(cJSON* command)
{
    const cJSON* input = cJSON_GetObjectItemCaseSensitive(command, "input");
    if (!cJSON_IsObject(input))
        return "c5_input needs the detection geometry.";
    cJSON* copy = cJSON_CreateObject();
    for (const char* key : {"speed", "distance", "angle"}) {
        const cJSON* value = cJSON_GetObjectItemCaseSensitive(input, key);
        if (!cJSON_IsNumber(value)) {
            cJSON_Delete(copy);
            return "c5_input needs speed, distance and angle.";
        }
        cJSON_AddNumberToObject(copy, key, value->valuedouble);
    }
    char* text = cJSON_PrintUnformatted(copy);
    cJSON_Delete(copy);
    c5Input = text ? text : "";
    cJSON_free(text);
    if (saveSetting)
        saveSetting("c5in", c5Input);
    if (send && connected_)
        send(hello());
    return "";
}

std::string Mach1Timer::raceSettings(cJSON* command)
{
    const cJSON* settings = cJSON_GetObjectItemCaseSensitive(command, "settings");
    if (!cJSON_IsObject(settings))
        return "race_settings needs settings.";
    applyRaceSettings(settings, true);
    // Kept on the gate this relay serves too, so the club's setup shows them.
    cJSON* m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "gate_settings");
    cJSON* s = cJSON_AddObjectToObject(m, "settings");
    cJSON* race = cJSON_AddObjectToObject(s, "race");
    cJSON_AddNumberToObject(race, "minLapMs", (double)engine.settings.minLapMs);
    cJSON_AddNumberToObject(race, "ignoreAfterStartMs", (double)engine.settings.ignoreAfterStartMs);
    sendJson(m);
    if (send && connected_)
        send(hello());
    return "";
}

void Mach1Timer::applyRaceSettings(const cJSON* race, bool save)
{
    const cJSON* minLap = cJSON_GetObjectItemCaseSensitive(race, "minLapMs");
    const cJSON* ignore = cJSON_GetObjectItemCaseSensitive(race, "ignoreAfterStartMs");
    if (cJSON_IsNumber(minLap) && minLap->valuedouble >= 0 && minLap->valuedouble <= 120000 &&
        (int64_t)minLap->valuedouble != engine.settings.minLapMs) {
        engine.settings.minLapMs = (int64_t)minLap->valuedouble;
        if (save && saveSetting)
            saveSetting("minlap", std::to_string(engine.settings.minLapMs));
    }
    if (cJSON_IsNumber(ignore) && ignore->valuedouble >= 0 && ignore->valuedouble <= 60000 &&
        (int64_t)ignore->valuedouble != engine.settings.ignoreAfterStartMs) {
        engine.settings.ignoreAfterStartMs = (int64_t)ignore->valuedouble;
        if (save && saveSetting)
            saveSetting("ignorems", std::to_string(engine.settings.ignoreAfterStartMs));
    }
}

void Mach1Timer::restoreC5Detection()
{
    // The receiver forgets its detection at power-off: write the kept one again.
    if (!driver_ || c5Detection.size() != 3 || driver_->info().procType != "ESP32C5")
        return;
    driver_->writeRaw(C5_WRITE_DETECTION, c5Detection);
    Reply reply;
    const bool ok = driver_->read(C5_READ_DETECTION, C5_DETECTION_SIZE, MAX_RETRIES, &reply) &&
                    reply.data.size() == C5_DETECTION_SIZE && reply.data[0] == 3 && reply.data[13] == 0 &&
                    reply.data[2] == c5Detection[1] && reply.data[3] == c5Detection[2];
    note(std::string("C5 detection ") + std::to_string(c5Detection[1] * 256 + c5Detection[2]) + " ms " +
         (ok ? "restored" : "not confirmed by the node"));
}

std::optional<Levels> Mach1Timer::resolveLevels(int seat, int pilotId, std::string* from) const
{
    auto find = [&](int s, int p) -> const LevelsRow* {
        for (const LevelsRow& r : gateLevels_)
            if (r.seat == s && r.pilotId == p)
                return &r;
        return nullptr;
    };
    struct Candidate {
        const LevelsRow* row;
        const char* source;
    };
    const Candidate candidates[] = {
        {pilotId >= 0 ? find(seat, pilotId) : nullptr, "pilot"},
        {find(seat, -1), "default"},
        {pilotId >= 0 ? find(-1, pilotId) : nullptr, "pilot"},
        {find(-1, -1), "default"},
    };
    for (const Candidate& c : candidates) {
        if (c.row) {
            if (from)
                *from = c.source;
            return c.row->levels;
        }
    }
    return std::nullopt;
}

void Mach1Timer::applyLevels(int seat, int pilotId, const StartPilot* given)
{
    if (!driver_)
        return;
    std::optional<Levels> levels;
    if (given && given->hasLevels)
        levels = given->levels;
    else
        levels = resolveLevels(seat, pilotId);
    if (!levels)
        return;  // nothing saved: the node keeps its own
    const SeatState& state = driver_->seats()[seat];
    if (state.enter == levels->enter && state.exit == levels->exit)
        return;
    if (!driver_->setThresholds(seat, levels->enter, levels->exit))
        note("Node " + std::to_string(seat + 1) + " did not confirm its levels");
}

int Mach1Timer::seatPilotId(int seat) const
{
    if (!engine.pilots().count(seat) || seat >= (int)race_.pilots.size() || !race_.pilots[seat])
        return -1;
    return race_.pilots[seat]->pilotId;
}

// No race on: each node back on its preferred channel (or off) with its
// levels: the last race's pilot's on their node, the node's default elsewhere.
// Nodes with no preference stay as they are.
void Mach1Timer::applyIdle()
{
    if (!driver_ || driver_->status() != NodeStatus::Running || !haveGateConfig_ ||
        engine.status == RaceStatus::Staging || engine.status == RaceStatus::Racing)
        return;
    for (int seat = 0; seat < (int)driver_->seats().size(); seat++) {
        auto it = gateNodes_.find(seat);
        if (it != gateNodes_.end() && it->second.hasFrequency && driver_->seats()[seat].frequency != it->second.frequency)
            driver_->setSeatFrequency(seat, it->second.frequency);
        if (driver_->seats()[seat].frequency == 0)
            continue;
        const int pilotId = seatPilotId(seat);
        const StartPilot* flown = pilotId >= 0 ? &*race_.pilots[seat] : nullptr;
        applyLevels(seat, pilotId, flown && flown->hasLevels ? flown : nullptr);
    }
    if (send && connected_)
        send(hello());
}

}  // namespace relay
