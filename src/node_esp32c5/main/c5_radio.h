// The ESP32-C5's own radio as a receive-only 5 GHz power meter.
//
// None of this is documented by Espressif. The facts it relies on (which
// PHY calls retune the PLL, the capture engine's registers, the sample
// layout, the gain register) are listed in README.md ("Where the radio facts come from") with how
// each was established: our own disassembly of Espressif's Apache-2.0
// PHY libraries, the ESP-IDF sources, and bench checks. tools/check_phy.py
// re-checks the library facts against the libraries a build links.
//
// Receive only: Wi-Fi runs in NULL mode (no interface, nothing to send),
// and the capture trigger is only ever called with its receive-dump
// arguments (c5_radio.cpp, stockCapture()).
#ifndef RHNODE_C5_RADIO_H
#define RHNODE_C5_RADIO_H

#include <stdint.h>

#include "c5_scan.h"

namespace c5 {

struct RadioStatus {
    bool wifi = false;          // Wi-Fi started (keeps the PHY powered)
    bool live = false;          // boot check: captures carry signal and the right gain field
    bool rxOnNeeded = false;    // the extra RX-on calls were needed to get live captures
    int firstCheck = -1;        // boot check before / after TX off: 0 live, 1 incomplete,
    int afterTxOff = -1;        //  2 gain field wrong, 3 flat
    int gainMax = 0;            // highest calibrated gain index
    uint32_t stockCaptures = 0, directCaptures = 0, timeouts = 0, incomplete = 0;
};

class C5Radio : public RadioPort {
public:
    // Wi-Fi up, 5 GHz band setup, TX path off, boot checks.
    bool begin(int gain);

    void hop(int loMhz) override;
    void ensureGain(int index) override;
    bool capture(int n, int64_t* tUs) override;
    const volatile uint32_t* words() override;

    // Full retune through Espressif's channel code (calibrates; ~1 ms).
    void fullTune(int loMhz);
    // Hop skip mask: bit 0 leaves out the 5 GHz clock update, bit 1 the
    // frequency-memory update.
    void setHopSkip(int mask) { skip_ = mask; }
    int hopSkip() const { return skip_; }
    // Use the stock (slow, ~1.7 ms) trigger for every capture.
    void setStockOnly(bool on) { stockOnly_ = on; }

    const RadioStatus& status() const { return st_; }
    uint32_t gainReg() const;

private:
    RadioStatus st_;
    int skip_ = 0;
    bool stockOnly_ = false;
    bool seeded_ = false;
    int curLo_ = 0;

    bool stockCapture(int n);
    bool directCapture(int n);
    void arm(int n);
    bool check(int n) const;
    bool bootCheck(int gain);
    int liveCheck(int gain);
};

}  // namespace c5

#endif
