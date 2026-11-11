// A simulated ESP32-C5 receiver for the host tests: emitters at RF
// frequencies, noise, DC offset, I/Q imbalance and ADC clipping, captured
// into words laid out like the C5's (10-bit I, 10-bit Q, gain field).
#ifndef RHNODE_C5_SIM_RADIO_H
#define RHNODE_C5_SIM_RADIO_H

#include <complex>
#include <cmath>
#include <random>
#include <vector>

#include "c5_scan.h"

struct Emitter {
    double mhz;
    double amplitude;   // LSB at the ADC
};

class SimRadio : public c5::RadioPort {
public:
    std::vector<Emitter> emitters;
    double noise = 1.0;                 // LSB rms per I and Q
    std::complex<double> dc = {0, 0};   // LSB
    std::complex<double> nu = {0, 0};   // I/Q imbalance: x' = x + nu * conj(x)
    int gain = 30;
    int64_t clockUs = 0;
    int64_t stepUs = 125;               // time per capture
    int lo = 5800;
    int hops = 0;
    bool failNext = false;
    bool frontEnd = true;               // apply Scanner::frontEndDb() to each emitter

    void hop(int loMhz) override { lo = loMhz; ++hops; }
    void ensureGain(int index) override { gain = index; }
    const volatile uint32_t* words() override { return buf_.data(); }

    bool capture(int n, int64_t* tUs) override
    {
        buf_.assign(n + 4, 0);
        std::normal_distribution<double> g(0.0, noise);
        std::uniform_real_distribution<double> ph(0, 2 * M_PI);
        std::vector<double> phase(emitters.size());
        for (auto& p : phase)
            p = ph(rng_);
        for (int k = 0; k < n; ++k) {
            std::complex<double> x(g(rng_), g(rng_));
            for (size_t e = 0; e < emitters.size(); ++e) {
                // RF above the LO appears at negative baseband frequency
                const double f = c5::kSpectrumSign * (emitters[e].mhz - lo) / 80.0;
                double a = emitters[e].amplitude;
                if (frontEnd)
                    a *= std::pow(10.0, c5::Scanner::frontEndDb((float)(emitters[e].mhz - lo)) / 20.0);
                x += std::polar(a, 2 * M_PI * f * k + phase[e]);
            }
            x = x + nu * std::conj(x) + dc;
            int i = (int)std::lround(x.real()), q = (int)std::lround(x.imag());
            i = i > 511 ? 511 : (i < -512 ? -512 : i);
            q = q > 511 ? 511 : (q < -512 ? -512 : q);
            buf_[k] = ((uint32_t)i & 0x3FF) | (((uint32_t)q & 0x3FF) << 10) | ((uint32_t)(gain & 0x7F) << 20);
        }
        clockUs += stepUs;
        *tUs = clockUs;
        if (failNext) {
            failNext = false;
            return false;
        }
        return true;
    }

private:
    std::vector<uint32_t> buf_;
    std::mt19937 rng_{12345};
};

#endif
