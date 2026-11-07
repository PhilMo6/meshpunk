// Shared bring-up plumbing: the I2C bus + XL9535 expander access that
// rung 2 establishes, and the SPI2 bus state from rung 5, reused by later
// rungs. IO numbering is the wadamesh/LilyGo v1 map (IOs 10-17 = port-1
// bits 0-7; no IO8/9 exists).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

#define IO_3V3_EN      0
#define IO_RF_SWITCH   1
#define IO_SCREEN_RST  2
#define IO_TOUCH_RST   3
#define IO_TOUCH_INT   4   // input
#define IO_5V_EN       6
#define IO_SENSOR_INT  7   // input
#define IO_VCCA_EN     10
#define IO_GPS_WAKE    11
#define IO_RTC_INT     12  // input
#define IO_C6_EN       14
#define IO_SD_EN       15  // active LOW
#define IO_SX1262_RST  16  // radio reset on both SKUs (SX1262 and LR2021)
#define IO_SX1262_DIO1 17  // input

#ifdef __cplusplus
extern "C" {
#endif

// NULL until rung 2 finds the XL9535 on the v1 pins.
extern i2c_master_bus_handle_t g_p4_i2c0;

// True once rung 5 has initialized the SPI2 bus (radio SPI).
extern bool g_p4_spi2_ready;

// Drive one expander output; false = the I2C write failed.
bool p4_xl_set(uint8_t io, bool level);

// SDIO host + slot 1 for the C6 link; configures once, later calls return
// true. false = init failed (reason printed).
bool p4_c6_sdio_init(void);

#ifdef __cplusplus
}
#endif
