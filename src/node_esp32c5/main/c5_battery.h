// Battery voltage through a divider on an ADC1 pin (ESP-IDF side). The link
// protocol for it (READ 0x48, WRITE 0x7F) is in c5_rhglue.
#pragma once

#include "c5_rhglue.h"

namespace c5 {

void batteryInit();
// Once a second, between scan cycles.
void batterySample();
void batteryRead(NodeBattery* out);
// Battery mV per pin mV x1000, saved; 0 restores the default. False if out of range.
bool batterySetRatio(uint16_t ratioMilli);

}  // namespace c5
