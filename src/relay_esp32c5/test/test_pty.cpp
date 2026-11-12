// The relay's node driver against the C5 node firmware's real core
// (RotorHazard's RssiNode and commands, the C5 scanner and meter) running on
// a pseudo-terminal in real time: ../../node_esp32c5/test/pty_node.
//
// Syncs, tunes eight seats, sets levels, then flies two simulated passes and
// checks that the relay's lap time (crossing 2 - crossing 1) matches the true
// time between the two peaks.
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "node_driver.h"

using namespace relay;

static int failures = 0;
#define EXPECT(cond, ...)                                  \
    do {                                                   \
        if (!(cond)) {                                     \
            failures++;                                    \
            std::fprintf(stderr, "FAIL %s: ", #cond);      \
            std::fprintf(stderr, __VA_ARGS__);             \
            std::fprintf(stderr, "\n");                    \
        }                                                  \
    } while (0)

static int64_t monoUs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

class PtyLink : public NodeLink {
public:
    explicit PtyLink(int fd) : fd(fd) {}
    int fd;
    void flushInput() override
    {
        uint8_t buf[256];
        while (::read(fd, buf, sizeof buf) > 0) {
        }
    }
    void write(const uint8_t* data, size_t n) override
    {
        while (n) {
            const ssize_t w = ::write(fd, data, n);
            if (w > 0) {
                data += w;
                n -= (size_t)w;
            }
        }
    }
    size_t read(uint8_t* data, size_t n, uint32_t timeoutMs) override
    {
        const int64_t deadline = monoUs() + (int64_t)timeoutMs * 1000;
        size_t got = 0;
        while (got < n) {
            const int64_t left = deadline - monoUs();
            if (left <= 0)
                break;
            struct pollfd p = {fd, POLLIN, 0};
            if (poll(&p, 1, (int)((left + 999) / 1000)) <= 0)
                continue;
            const ssize_t r = ::read(fd, data + got, n - got);
            if (r > 0)
                got += (size_t)r;
        }
        return got;
    }
    int64_t nowUs() override { return monoUs(); }
    void sleepMs(uint32_t ms) override { usleep(ms * 1000); }

    // A '!' console line to the node, and its one-line answer.
    std::string console(const std::string& line)
    {
        flushInput();
        const std::string out = "!" + line + "\n";
        write((const uint8_t*)out.data(), out.size());
        std::string reply;
        uint8_t c;
        while (read(&c, 1, 500) == 1 && c != '\n')
            reply += (char)c;
        return reply;
    }
};

int main(int argc, char** argv)
{
    const char* node = argc > 1 ? argv[1] : "../../node_esp32c5/test/pty_node";
    int pipefd[2];
    if (pipe(pipefd))
        return 2;
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(pipefd[1], 1);
        execl(node, node, (char*)nullptr);
        _exit(127);
    }
    close(pipefd[1]);
    char path[128] = {};
    for (size_t i = 0; i < sizeof path - 1; i++) {
        if (::read(pipefd[0], path + i, 1) != 1 || path[i] == '\n') {
            path[i] = 0;
            break;
        }
    }
    const int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        std::fprintf(stderr, "cannot open %s\n", path);
        kill(pid, SIGKILL);
        return 2;
    }
    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    tcsetattr(fd, TCSANOW, &t);

    PtyLink link(fd);
    NodeDriver driver(link);
    std::vector<std::pair<int, int64_t>> crossings;
    driver.onCrossing = [&](int seat, int64_t at, int) { crossings.push_back({seat, at}); };

    std::string error;
    EXPECT(driver.start(&error), "%s", error.c_str());
    const NodeInfo& info = driver.info();
    EXPECT(info.apiLevel == 36 && info.seats == 8 && info.procType == "ESP32C5" && !info.wide,
           "API %d, %d seats, %s", info.apiLevel, info.seats, info.procType.c_str());
    std::printf("node: %s %s, API %d, %d seats\n", info.procType.c_str(), info.firmware.c_str(), info.apiLevel,
                info.seats);

    const std::vector<int> raceband = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};
    const auto failed = driver.setFrequencies(raceband);
    EXPECT(failed.empty(), "%zu seats did not confirm", failed.size());
    for (int i = 0; i < 8; i++)
        EXPECT(driver.setThresholds(i, 150, 100), "seat %d levels", i);

    auto sweep = [&](int64_t ms) {
        const int64_t until = monoUs() + ms * 1000;
        while (monoUs() < until)
            driver.poll();
    };
    sweep(1000);  // fill the node's median filters
    EXPECT(driver.sweepsPerSecond() > 20, "%.1f sweeps/s", driver.sweepsPerSecond());
    std::printf("sweeping: %.1f sweeps/s, min rtt %.2f ms, floor R4 %d\n", driver.sweepsPerSecond(),
                driver.minRttUs() / 1000.0, driver.seats()[3].rssi);
    EXPECT(crossings.empty(), "%zu crossings before any pass", crossings.size());

    long peaks[2];
    for (int pass = 0; pass < 2; pass++) {
        const std::string reply = link.console("pass 5769 178 300");
        EXPECT(reply.rfind("peak ", 0) == 0, "console said '%s'", reply.c_str());
        peaks[pass] = atol(reply.c_str() + 5);
        sweep(pass == 0 ? 3200 : 1500);
    }
    EXPECT(crossings.size() == 2, "%zu crossings for 2 passes", crossings.size());
    if (crossings.size() == 2) {
        EXPECT(crossings[0].first == 3 && crossings[1].first == 3, "seats %d, %d", crossings[0].first,
               crossings[1].first);
        const double relayLapMs = (crossings[1].second - crossings[0].second) / 1000.0;
        const double trueLapMs = (double)(peaks[1] - peaks[0]);
        std::printf("lap: relay %.1f ms, true %.1f ms, error %+.1f ms\n", relayLapMs, trueLapMs,
                    relayLapMs - trueLapMs);
        EXPECT(std::fabs(relayLapMs - trueLapMs) < 10, "lap error %.1f ms", relayLapMs - trueLapMs);
    }
    const LinkCounters& c = driver.counters();
    std::printf("link: %u reads, %u writes, %u timeouts, %u checksum errors\n", c.reads, c.writes, c.timeouts,
                c.checksumErrors);
    EXPECT(c.timeouts == 0 && c.checksumErrors == 0, "link errors");

    kill(pid, SIGKILL);
    std::printf(failures ? "pty test FAILED\n" : "pty test ok\n");
    return failures ? 1 : 0;
}
