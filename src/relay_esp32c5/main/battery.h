// The relay's own battery voltage through a divider on an ADC1 pin.
#pragma once

namespace relay {

struct Battery {
    bool wired = false;  // the pin reads a divider (not floating low)
    int pinMv = 0, batteryMv = 0, ratioMilli = 0;
};

void batteryInit();
// Once a second, from the timer task.
void batterySample();
Battery batteryRead();
// Battery mV per pin mV x1000, saved; 0 restores the default (2.000). False if out of range.
bool batterySetRatio(int ratioMilli);

}  // namespace relay
