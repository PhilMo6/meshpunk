// wio_l2_board.h — Seeed Wio Tracker L2 board layer: the shared I2C bus,
// the PCA9555 I/O expander and the LP5814 backlight driver. Every
// subsystem backend for this board (display, input, power, GPS, audio)
// reaches its expander-controlled lines through here.
//
// Sources: Seeed wiki pin table, the Meshtastic seeed_wio_tracker_L2
// variant (expander bit map, addresses) and the wadamesh
// variants/wio_tracker_l2 port (power-up sequence, backlight registers —
// hardware-tested on this board).

#pragma once

#if defined(BOARD_WIO_L2)

#include <stdint.h>

// ── Shared I2C bus (expander, touch, backlight, battery ADC, codec) ────────
#define WIO_L2_I2C_SDA        47
#define WIO_L2_I2C_SCL        48
#define WIO_L2_I2C_FREQ       100000

// ── PCA9555 I/O expander ────────────────────────────────────────────────────
#define WIO_L2_EXP_ADDR       0x21
#define WIO_L2_EXP_INT_PIN    45     // open-drain, active LOW

// Bit numbers: P00-P07 = 0-7, P10-P17 = 8-15 (Meshtastic EXPANDS_* values,
// identical to wadamesh's constants).
#define WIO_L2_EXP_WAKE_BTN   0      // input
#define WIO_L2_EXP_I2C_INT    1      // input
#define WIO_L2_EXP_SD_DETECT  2      // input
#define WIO_L2_EXP_TP_INT     3      // GT911 INT, held LOW
#define WIO_L2_EXP_LCD_CTRL   4      // Meshtastic: EXPANDS_LCD_CS
#define WIO_L2_EXP_LCD_PWR    5
#define WIO_L2_EXP_LCD_RST    6      // active LOW
#define WIO_L2_EXP_GROVE_PWR  7
#define WIO_L2_EXP_TP_RST     8      // active LOW
#define WIO_L2_EXP_GNSS_RST   9      // reset asserted HIGH, released LOW
#define WIO_L2_EXP_USER_LED   10
#define WIO_L2_EXP_USB_OTG    11
#define WIO_L2_EXP_AUDIO_PA   12
#define WIO_L2_EXP_GNSS_PWR   13
#define WIO_L2_EXP_SD_PWR     14
#define WIO_L2_EXP_BAT_SENSE  15

// ── Display: NV3031B, 240x320 native portrait, QSPI on SPI3 ────────────────
#define WIO_L2_LCD_SCLK       42
#define WIO_L2_LCD_IO0        41
#define WIO_L2_LCD_IO1        40
#define WIO_L2_LCD_IO2        39
#define WIO_L2_LCD_IO3        38
#define WIO_L2_LCD_CS         46

// ── LP5814 backlight LED driver ─────────────────────────────────────────────
#define WIO_L2_LIGHT_ADDR     0x2C

// ── GT911 touch ─────────────────────────────────────────────────────────────
#define WIO_L2_TOUCH_ADDR     0x5D   // INT held LOW through reset selects 0x5D

// ── ADS1115 battery ADC ─────────────────────────────────────────────────────
#define WIO_L2_BATT_ADC_ADDR  0x48

// ── AW35615 USB-C port controller (CC pins, VBUS sense) ─────────────────────
#define WIO_L2_TYPEC_ADDR     0x22

// Bring the board up: I2C bus, expander probe, then the power-up sequence
// (LCD rail + reset pulse, GNSS rail + reset, touch reset, battery divider
// enable ON, every other switched line parked OFF). Runs once, before any
// other peripheral access. Returns false (after logging which step failed)
// if the expander does not answer — nothing downstream works without it.
bool wio_l2_board_init(void);

// True once wio_l2_board_init() has completed.
bool wio_l2_board_ready(void);

// Drive one expander line as an output; false = I2C write failed.
bool wio_l2_exp_set(uint8_t bit, bool level);

// Read one expander line's input level; false = I2C read failed. Reading
// the input port also releases the expander's INT line (GPIO45).
bool wio_l2_exp_get(uint8_t bit, bool* level);

// LP5814 backlight: register setup (all four channels), then PWM duty
// 0-255 on all channels. init() applies `pwm` as its first duty.
bool wio_l2_backlight_init(uint8_t pwm);
void wio_l2_backlight_set(uint8_t pwm);

// Run fn(arg) in a task on core 1 and block until it returns; false (logged)
// if the task could not be created. esp_intr_alloc() binds a driver's
// interrupt to the core that calls it, and setup() runs on core 0. The L2's
// display bus (LovyanGFX's SPI3 through the ESP-IDF SPI driver) and SDMMC
// host initialize through here so their interrupts land on core 1: with
// them on core 0, core 0 had no free interrupt line left when the WiFi
// task's first hardware-AES use (WPA2 handshake) allocated its own, and
// esp-aes aborts on that failure.
bool wio_l2_run_on_core1(void (*fn)(void*), void* arg);

#endif // BOARD_WIO_L2
