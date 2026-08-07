// power_dev.h — per-device power/battery backend contract.
//
// One backend per build via the board define. Owns the peripheral power
// rail and the battery measurement path (divider ratio, ADC setup,
// averaging, or a PMU chip on boards that have one).

#pragma once

#include <stdint.h>

// Enable the board's peripheral power rail. Called EARLY in setup(), before
// any peripheral that needs the rail is touched.
void power_dev_init(void);

// Battery voltage in millivolts, or 0 on boards with no measurement path.
// The single battery read for the whole firmware — UI topbar, Lua
// (_get_battery_mv) and the BLE companion all come through here so they
// can never report differently-scaled values.
uint16_t power_dev_battery_mv(void);
