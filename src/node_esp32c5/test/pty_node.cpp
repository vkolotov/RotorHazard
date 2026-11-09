// The C5 node firmware's core (RotorHazard's RssiNode and commands, the C5
// scanner and meter) on a pseudo-terminal, with a simulated radio, in real
// time. test_rh_e2e.py points RotorHazard's own serial discovery and
// interface code at it.
//
// Prints the pty's path on stdout, then serves until killed. Test hooks on
// the console ('!' lines): "pass <MHz> <amplitude> <half-width ms>" flies a
// simulated quad past that frequency (a triangle peaking half-width ms from
// now); "vtx <MHz> <amplitude>" leaves a VTX on.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "c5_rhglue.h"
#include "c5_scan.h"
#include "sim_radio.h"

using namespace c5;

static int64_t nowUs64()
{
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration_cast<microseconds>(steady_clock::now() - t0).count();
}
extern "C" uint32_t rhc5_millis(void) { return (uint32_t)(nowUs64() / 1000); }
extern "C" uint32_t rhc5_micros(void) { return (uint32_t)nowUs64(); }
extern "C" void rhc5_delay_ms(uint32_t ms) { usleep(ms * 1000); }
static uint32_t nowUs() { return (uint32_t)nowUs64(); }

class RealtimeRadio : public SimRadio {
public:
    bool capture(int n, int64_t* tUs) override
    {
        const int64_t t = nowUs64();
        emitters = steady;
        if (passMhz && t < passEndUs) {
            const double dt = (double)(t - passPeakUs) / passHalfUs;
            const double a = passAmp * (1.0 - (dt < 0 ? -dt : dt));
            if (a > 0.5)
                emitters.push_back({(double)passMhz, a});
        }
        const bool ok = SimRadio::capture(n, tUs);
        *tUs = t;
        return ok;
    }
    std::vector<Emitter> steady;
    int passMhz = 0;
    double passAmp = 0;
    int64_t passPeakUs = 0, passEndUs = 0, passHalfUs = 1;
};

static RealtimeRadio g_radio;

class PtyOut : public Transport {
public:
    int fd = -1;
    void write(const uint8_t* d, size_t n) override
    {
        while (n) {
            const ssize_t w = ::write(fd, d, n);
            if (w <= 0)
                return;
            d += w;
            n -= (size_t)w;
        }
    }
};

static void console(const char* line, Transport& out)
{
    char cmd[16] = {};
    double a = 0, b = 0, c = 0;
    sscanf(line, "%15s %lf %lf %lf", cmd, &a, &b, &c);
    if (!strcmp(cmd, "pass")) {
        const int64_t t = nowUs64();
        g_radio.passMhz = (int)a;
        g_radio.passAmp = b;
        g_radio.passHalfUs = (int64_t)(c * 1000);
        g_radio.passPeakUs = t + g_radio.passHalfUs;
        g_radio.passEndUs = t + 2 * g_radio.passHalfUs;
        char reply[64];
        const int n = snprintf(reply, sizeof(reply), "peak %lu\n", (unsigned long)(g_radio.passPeakUs / 1000));
        out.write((const uint8_t*)reply, (size_t)n);
    } else if (!strcmp(cmd, "vtx")) {
        g_radio.steady = {{a, b}};
    }
}

int main()
{
    const int fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (fd < 0 || grantpt(fd) || unlockpt(fd))
        return 1;
    printf("%s\n", ptsname(fd));
    fflush(stdout);
    fcntl(fd, F_SETFL, O_NONBLOCK);

    Scanner scanner(g_radio);
    rhBegin(scanner, 8);
    PtyOut out;
    out.fd = fd;
    RhLink link(out);
    link.setConsole(console);
    // pace cycles like the C5: ~1 ms for 8 visits
    for (;;) {
        const int64_t start = nowUs64();
        scanner.cycle(nowUs);
        uint8_t buf[256];
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0)
            link.feed(buf, (size_t)n);
        const int64_t spent = nowUs64() - start;
        if (spent < 1000)
            usleep((useconds_t)(1000 - spent));
    }
}
