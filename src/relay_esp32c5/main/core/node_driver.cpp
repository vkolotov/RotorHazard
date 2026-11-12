#include "node_driver.h"

#include <algorithm>
#include <cstdlib>

#include "node_protocol.h"

namespace relay {

const char* nodeStatusName(NodeStatus s)
{
    switch (s) {
    case NodeStatus::Closed: return "closed";
    case NodeStatus::Syncing: return "syncing";
    case NodeStatus::Running: return "running";
    case NodeStatus::Lost: return "lost";
    }
    return "?";
}

static std::vector<uint8_t> pack8(int v) { return {(uint8_t)(v & 0xFF)}; }
static std::vector<uint8_t> pack16(int v) { return {(uint8_t)((v >> 8) & 0xFF), (uint8_t)(v & 0xFF)}; }

// ---------------------------------------------------------------------------
// One request at a time, as RotorHazard's node I/O lock

bool NodeDriver::readOnce(uint8_t command, size_t size, Reply* out, bool* gotSomething)
{
    // The largest reply is the C5 detection read (86 bytes and the checksum).
    uint8_t buf[128];
    if (size + 1 > sizeof buf)
        return false;
    // Some USB-UART adapters and a radio link that rebooted leave stale bytes.
    link_.flushInput();
    out->sentUs = link_.nowUs();
    link_.write(&command, 1);
    const size_t got = link_.read(buf, size + 1, REQUEST_TIMEOUT_MS);
    out->receivedUs = link_.nowUs();
    *gotSomething = got > 0;
    if (got != size + 1)
        return false;
    if (checksum(buf, size) != buf[size])
        return false;
    out->data.assign(buf, buf + size);
    return true;
}

bool NodeDriver::read(uint8_t command, size_t size, int retries, Reply* out)
{
    for (int attempt = 0; attempt <= retries; attempt++) {
        counters_.reads++;
        bool gotSomething = false;
        if (readOnce(command, size, out, &gotSomething))
            return true;
        // Bytes that do not check out are a checksum error; nothing is a timeout.
        if (gotSomething)
            counters_.checksumErrors++;
        else
            counters_.timeouts++;
        link_.sleepMs(RETRY_PAUSE_MS);
    }
    return false;
}

void NodeDriver::write(uint8_t command, const std::vector<uint8_t>& data)
{
    counters_.writes++;
    std::vector<uint8_t> frame;
    frame.reserve(data.size() + 2);
    frame.push_back(command);
    frame.insert(frame.end(), data.begin(), data.end());
    frame.push_back(checksum(data.data(), data.size()));
    link_.write(frame.data(), frame.size());
}

void NodeDriver::markLost(const std::string& why)
{
    status_ = NodeStatus::Lost;
    detail_ = why;
}

// ---------------------------------------------------------------------------
// Start-up, as RotorHazard's serial_node.discover() and RHInterface.__init__

bool NodeDriver::start(std::string* error)
{
    status_ = NodeStatus::Syncing;
    detail_.clear();
    counters_ = LinkCounters();
    currentIndex_ = -1;
    nextSeat_ = 0;
    lastSeat_ = -1;
    failures_ = 0;
    rtts_.clear();
    minRttUs_ = 0;
    sweepTimes_.clear();
    sweepsPerSecond_ = 0;
    auto fail = [&](const std::string& why) {
        markLost(why);
        if (error)
            *error = why;
        return false;
    };
    // The node, or the radio link in front of it, can take a moment.
    link_.sleepMs(150);
    Reply revision;
    bool ok = false;
    for (int attempt = 0; attempt < 8 && !ok; attempt++) {
        ok = read(READ_REVISION_CODE, 2, 2, &revision) && revision.data[0] == REVISION_MAGIC;
        if (!ok)
            link_.sleepMs(250);
    }
    if (!ok)
        return fail("No RotorHazard node answered. Check the wiring, the radio link and that the node has power.");
    const int apiLevel = revision.data[1];
    if (apiLevel < MIN_API_LEVEL)
        return fail("The node's API level is " + std::to_string(apiLevel) + "; " + std::to_string(MIN_API_LEVEL) +
                    " or later is needed.");
    Reply count;
    const int seats = read(READ_MULTINODE_COUNT, 1, 2, &count) ? count.data[0] : 0;
    if (!seats || seats > 32)
        return fail(seats ? "The node reports " + std::to_string(seats) + " seats." : "The node has no receivers.");
    Reply firmware, proc;
    const bool haveFw = read(READ_FW_VERSION, FW_TEXT_BLOCK_SIZE, 2, &firmware);
    const bool haveProc = read(READ_FW_PROCTYPE, FW_TEXT_BLOCK_SIZE, 2, &proc);
    info_ = NodeInfo();
    info_.apiLevel = apiLevel;
    info_.seats = seats;
    info_.firmware = haveFw ? fwText(firmware.data.data(), firmware.data.size()) : "";
    info_.procType = haveProc ? fwText(proc.data.data(), proc.data.size()) : "";
    info_.wide = hasWideRssi(apiLevel, info_.procType);
    // A C5 node says who it is (firmware 1.2.0-c5.13 on): the node is the timer.
    if (info_.procType == "ESP32C5") {
        Reply features, id, name;
        if (read(READ_RHFEAT_FLAGS, 2, 2, &features) && (((features.data[0] << 8) | features.data[1]) & RHFEAT_C5_NODE_ID) &&
            read(READ_NODE_ID, 6, MAX_RETRIES, &id)) {
            static const char* digits = "0123456789abcdef";
            for (uint8_t b : id.data) {
                info_.nodeId += digits[b >> 4];
                info_.nodeId += digits[b & 15];
            }
            if (read(READ_NODE_NAME, NODE_NAME_SIZE, MAX_RETRIES, &name))
                info_.nodeName = fwText(name.data.data(), name.data.size());
        }
    }
    seats_.assign(seats, SeatState());
    lapIds_.assign(seats, -1);
    pending_.assign(seats, Pending());
    for (int seat = 0; seat < seats; seat++) {
        if (!selectSeat(seat))
            return fail("The node did not switch to seat " + std::to_string(seat + 1) + ".");
        int v;
        if (read16(READ_FREQUENCY, &v))
            seats_[seat].frequency = (v == DISABLED_FREQUENCY) ? 0 : v;
        if (readRssi(READ_ENTER_AT_LEVEL, &v))
            seats_[seat].enter = v;
        if (readRssi(READ_EXIT_AT_LEVEL, &v))
            seats_[seat].exit = v;
    }
    status_ = NodeStatus::Running;
    return true;
}

// ---------------------------------------------------------------------------
// Helpers

bool NodeDriver::selectSeat(int seat)
{
    if (info_.seats <= 1) {
        currentIndex_ = seat;
        return true;
    }
    for (int attempt = 0; attempt <= MAX_RETRIES * 5; attempt++) {
        write(WRITE_CURNODE_INDEX, pack8(seat));
        Reply reply;
        if (read(READ_CURNODE_INDEX, 1, 0, &reply) && reply.data[0] == seat) {
            currentIndex_ = seat;
            return true;
        }
        link_.sleepMs(RETRY_PAUSE_MS);
    }
    currentIndex_ = -1;
    return false;
}

bool NodeDriver::read16(uint8_t command, int* value)
{
    Reply r;
    if (!read(command, 2, MAX_RETRIES, &r))
        return false;
    *value = unpack16(r.data.data());
    return true;
}

bool NodeDriver::readRssi(uint8_t command, int* value)
{
    Reply r;
    if (!read(command, info_.wide ? 2 : 1, MAX_RETRIES, &r))
        return false;
    *value = info_.wide ? unpack16(r.data.data()) : r.data[0];
    return true;
}

bool NodeDriver::setAndValidate8(uint8_t w, uint8_t r, int value)
{
    for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        write(w, pack8(value));
        Reply reply;
        if (read(r, 1, MAX_RETRIES, &reply) && reply.data[0] == value)
            return true;
    }
    return false;
}

bool NodeDriver::setAndValidate16(uint8_t w, uint8_t r, int value)
{
    for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        write(w, pack16(value));
        int got;
        if (read16(r, &got) && got == value)
            return true;
    }
    return false;
}

bool NodeDriver::setAndValidateRssi(uint8_t w, uint8_t r, int value)
{
    return info_.wide ? setAndValidate16(w, r, value) : setAndValidate8(w, r, value);
}

void NodeDriver::resetLapTracking(int seat)
{
    // A frequency change restarts the seat's lap count on some nodes.
    lapIds_[seat] = -1;
    pending_[seat] = Pending();
}

bool NodeDriver::setFrequency(int seat, int frequency)
{
    SeatState& state = seats_[seat];
    for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        if (!frequency) {
            setAndValidate16(WRITE_FREQUENCY, READ_FREQUENCY, info_.apiLevel >= 24 ? DISABLED_FREQUENCY : 5800);
            state.frequency = 0;
            return true;
        }
        const bool set = setAndValidate16(WRITE_FREQUENCY, READ_FREQUENCY, frequency);
        if (set)
            state.frequency = frequency;
        if (info_.apiLevel < 36)
            return set;
        // RX5808: the VCO needs ~20 ms to settle before its register reads true.
        link_.sleepMs(30);
        Reply test;
        if (set && read(TEST_RX_REGISTER, 1, MAX_RETRIES, &test) && test.data[0])
            return true;
    }
    return false;
}

std::vector<int> NodeDriver::setFrequencies(const std::vector<int>& frequencies)
{
    std::vector<int> failed;
    for (int seat = 0; seat < (int)seats_.size(); seat++) {
        const int wanted = seat < (int)frequencies.size() ? frequencies[seat] : 0;
        if (!selectSeat(seat) || !setFrequency(seat, wanted))
            failed.push_back(seat);
        resetLapTracking(seat);
    }
    return failed;
}

bool NodeDriver::setSeatFrequency(int seat, int frequency)
{
    if (seat < 0 || seat >= (int)seats_.size())
        return false;
    const bool ok = selectSeat(seat) && setFrequency(seat, frequency);
    resetLapTracking(seat);
    return ok;
}

bool NodeDriver::setNodeName(const std::string& name)
{
    if (info_.nodeId.empty())
        return false;
    // At most 32 bytes, not cutting a UTF-8 character in two.
    size_t n = std::min(name.size(), NODE_NAME_SIZE);
    while (n > 0 && n < name.size() && ((uint8_t)name[n] & 0xC0) == 0x80)
        n--;
    std::vector<uint8_t> data(NODE_NAME_SIZE, 0);
    std::copy(name.begin(), name.begin() + n, data.begin());
    write(WRITE_NODE_NAME, data);
    Reply reply;
    if (!read(READ_NODE_NAME, NODE_NAME_SIZE, MAX_RETRIES, &reply) || reply.data != data)
        return false;
    info_.nodeName = std::string(name.begin(), name.begin() + n);
    return true;
}

bool NodeDriver::setThresholds(int seat, int enter, int exit)
{
    if (seat < 0 || seat >= (int)seats_.size() || !selectSeat(seat))
        return false;
    const int limit = maxRssi(info_.wide);
    const bool okEnter = enter > 0 && enter < limit && setAndValidateRssi(WRITE_ENTER_AT_LEVEL, READ_ENTER_AT_LEVEL, enter);
    const bool okExit = exit > 0 && exit < limit && setAndValidateRssi(WRITE_EXIT_AT_LEVEL, READ_EXIT_AT_LEVEL, exit);
    if (okEnter)
        seats_[seat].enter = enter;
    if (okExit)
        seats_[seat].exit = exit;
    return okEnter && okExit;
}

// ---------------------------------------------------------------------------
// Sweep

bool NodeDriver::poll()
{
    if (status_ != NodeStatus::Running || seats_.empty())
        return false;
    const int n = (int)seats_.size();
    int seat = -1;
    for (int i = 0; i < n; i++) {
        const int s = (nextSeat_ + i) % n;
        if (seats_[s].frequency) {
            seat = s;
            break;
        }
    }
    if (seat < 0)
        return false;
    // Back at or before the last seat read: a sweep is complete.
    const bool wrapped = seat <= lastSeat_;
    lastSeat_ = seat;
    nextSeat_ = seat + 1;
    if (nextSeat_ >= n)
        nextSeat_ = 0;
    if (wrapped) {
        const int64_t now = link_.nowUs();
        sweepTimes_.push_back(now);
        while (!sweepTimes_.empty() && now - sweepTimes_.front() > 5000000)
            sweepTimes_.pop_front();
        sweepsPerSecond_ = sweepTimes_.size() > 1
                               ? (double)(sweepTimes_.size() - 1) * 1e6 / (double)(now - sweepTimes_.front())
                               : 0;
    }
    const bool ok = readSeat(seat);
    failures_ = ok ? 0 : failures_ + 1;
    if (failures_ > LOST_AFTER_FAILURES)
        markLost("The node stopped answering.");
    else if (failures_ > 20)
        detail_ = "The node is not answering reliably.";
    else if (ok)
        detail_.clear();
    return true;
}

bool NodeDriver::readSeat(int seat)
{
    if (currentIndex_ != seat || info_.seats > 1) {
        if (!selectSeat(seat))
            return false;
    }
    const size_t size = lapStatsSize(info_.wide);
    Reply reply;
    if (!read(READ_LAP_PASS_STATS, size, MAX_RETRIES, &reply))
        return false;
    const LapPassStats stats = parseLapPassStats(reply.data.data(), info_.wide);
    SeatState& state = seats_[seat];
    if (readExtremums) {
        Reply ext;
        if (read(READ_LAP_EXTREMUMS, size, MAX_RETRIES, &ext))
            state.crossing = extremumsCrossing(ext.data.data());
    }
    noteRtt(reply.receivedUs - reply.sentUs);
    const int limit = maxRssi(info_.wide);
    if (stats.rssi > 0 && stats.rssi < limit)
        state.rssi = stats.rssi;
    if (stats.passPeakRssi > 0 && stats.passPeakRssi < limit)
        state.passPeak = stats.passPeakRssi;
    if (stats.nodePeakRssi > 0 && stats.nodePeakRssi < limit)
        state.nodePeak = stats.nodePeakRssi;
    lapStats(seat, stats.lapId, reply.sentUs + minRttUs_ / 2 - (int64_t)stats.msSince * 1000, state.passPeak);
    return true;
}

void NodeDriver::noteRtt(int64_t rttUs)
{
    rtts_.push_back(rttUs);
    if (rtts_.size() > RTT_WINDOW)
        rtts_.pop_front();
    minRttUs_ = *std::min_element(rtts_.begin(), rtts_.end());
}

// A new lap id is a crossing, but only once a second read agrees: a reply of
// garbage can pass the checksum (three zero bytes do), and a phantom crossing
// would put a lap in somebody's race.
void NodeDriver::lapStats(int seat, int lapId, int64_t crossingUs, int peak)
{
    const int known = lapIds_[seat];
    if (known == -1) {
        lapIds_[seat] = lapId;  // the first read only sets where counting starts
        return;
    }
    if (lapId == known) {
        pending_[seat] = Pending();
        return;
    }
    Pending& p = pending_[seat];
    if (!p.set || p.lapId != lapId) {
        p.set = true;
        p.lapId = lapId;
        p.atUs = crossingUs;
        return;
    }
    const int64_t first = p.atUs;
    pending_[seat] = Pending();
    lapIds_[seat] = lapId;
    // Both reads time the same crossing; far apart, the first was the bad one.
    const int64_t at = std::llabs(first - crossingUs) <= 50000 ? first : crossingUs;
    if (onCrossing)
        onCrossing(seat, at, peak);
}

}  // namespace relay
