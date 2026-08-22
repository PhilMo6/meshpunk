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

// Board tail of the power-off path: park board-specific peripherals/rails,
// arm the wake button (GPIO0 on both boards, active LOW), enter deep sleep.
// Never returns; waking is a full reboot (reset reason ESP_RST_DEEPSLEEP).
// The device-neutral teardown (store flush, radio off, BLE/WiFi stop, SD
// unmount) is system_shutdown() in main.cpp and has already run.
// power_dev_init() must release this path's RTC holds on the wake boot.
void power_dev_shutdown(void);

// Standby (light sleep) rail hooks. enter() parks what the board can afford
// to lose while the radio keeps receiving; exit() restores it.
// T-Deck: no-ops — the radio lives on the switched V3V rail (GPIO10 keeps
// driving through light sleep), and the Plus GPS is on the always-on rail
// with no power control. Heltec: VGNSS rail off/on (GPS).
void power_dev_standby_enter(void);
void power_dev_standby_exit(void);
