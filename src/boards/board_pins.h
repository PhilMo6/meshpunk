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

class SPIClass;

// The SPI bus the microSD slot lives on: the shared FSPI bus on the T-Deck,
// the display's HSPI bus on the Heltec kit. Defined by the board's display
// backend (which owns that bus's lifecycle); lazily begun on first call.
SPIClass& board_sd_spi(void);

#if defined(BOARD_TDECK)

#include "../utilities.h"
#include "../tdeck-pins.h"

// Board identity, reported to Lua via _device_caps().
#define MESHPUNK_BOARD_NAME "tdeck"
// Display form of the same, used to build the default node name.
#define MESHPUNK_BOARD_LABEL "T-Deck"

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

#elif defined(BOARD_HELTEC_V4)

// Heltec WiFi LoRa 32 V4-R8 + Expansion Kit V2. Pin sources: Heltec's
// V4_Touch_TFT example, the Meshtastic heltec_v4 variant, and the vendored
// MeshCore variants/heltec_v4 — all three agree on the pins below.

// Board identity, reported to Lua via _device_caps().
#define MESHPUNK_BOARD_NAME "heltec_v4"
// Display form of the same, used to build the default node name.
#define MESHPUNK_BOARD_LABEL "Heltec V4"

// LoRa radio (SX1262) — RadioLib Module pins. The radio has FSPI to itself
// on this board (the panel is on HSPI); the FEM control lines are separate
// build defines consumed by src/boards/heltec_fem.cpp.
// All hw-verified: CS/SCK/MOSI/MISO by the GetStatus probe (0x2A on 10/11),
// BUSY by the reset-pulse sampler (GPIO13 pulses high through POR, GPIO14
// stays low) — same assignments as the base V4 after all.
#define PIN_LORA_CS    8
#define PIN_LORA_DIO1  14
#define PIN_LORA_RST   12
#define PIN_LORA_BUSY  13

// FSPI bus (radio only; SD is probed on the display's HSPI bus)
#define PIN_SPI_SCK    9
#define PIN_SPI_MISO   11
#define PIN_SPI_MOSI   10

// microSD (Expansion Kit V2 slot): CS hw-confirmed by the boot probe
// (mounted a 32GB card); the slot shares the display's HSPI bus
// (SCK 16 / MOSI 15 / MISO 45 — see display_heltec.cpp). Parking this CS
// high at boot also prevents the card from ever seeing clocks CS-less
// (which wedges it until power-cycle). PIN_TFT_CS stays undefined: the
// panel CS belongs to the display backend.
#define PIN_SD_CS      3

// Buttons, both hw-probed 2026-08-11: USER = GPIO0 active LOW (shares the
// line with the mainboard PRG/boot button — one electrical input), IO =
// GPIO46 ACTIVE HIGH with an external pull-down (rides the R8's LED_Write
// line). GPIO35/36/37 must NEVER be driven on this board (octal PSRAM).
#define PIN_BOOT_BTN   0
#define PIN_IO_BTN     46

#else
#error "No board selected: define BOARD_<NAME> in platformio.ini build_flags"
#endif
