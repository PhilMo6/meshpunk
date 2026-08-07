// board_pins.h — neutral names for the pins the common firmware still wires
// directly (shared SPI bus, chip selects, boot button, radio).
//
// Everything else is behind a subsystem backend (input/, display/, audio/,
// power/, gps/). This header exists for the wiring that legitimately belongs
// to the common code: the SPI bus the radio + SD + panel share, and the
// RadioLib Module constructor.
//
// Per-board block below; the board define comes from platformio.ini.

#pragma once

#if defined(BOARD_TDECK)

#include "../utilities.h"
#include "../tdeck-pins.h"

// Board identity, reported to Lua via _device_caps().
#define MESHPUNK_BOARD_NAME "tdeck"

// LoRa radio (SX1262) — RadioLib Module pins
#define PIN_LORA_CS    RADIO_CS_PIN
#define PIN_LORA_DIO1  RADIO_DIO1_PIN
#define PIN_LORA_RST   RADIO_RST_PIN
#define PIN_LORA_BUSY  RADIO_BUSY_PIN

// Shared SPI bus (radio + SD + display)
#define PIN_SPI_SCK    BOARD_SPI_SCK
#define PIN_SPI_MISO   BOARD_SPI_MISO
#define PIN_SPI_MOSI   BOARD_SPI_MOSI

// Chip selects parked high at boot
#define PIN_SD_CS      BOARD_SDCARD_CS
#define PIN_TFT_CS     BOARD_TFT_CS

// Boot button
#define PIN_BOOT_BTN   BOARD_BOOT_PIN

#else
#error "No board selected: define BOARD_<NAME> in platformio.ini build_flags"
#endif
