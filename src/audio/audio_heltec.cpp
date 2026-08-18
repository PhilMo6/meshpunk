// audio_heltec.cpp — Heltec V4 Expansion Kit audio backend
// (contract: audio_dev.h).
//
// The kit has a piezo buzzer behind a PAM8904 driver (boost level selected
// by a hardware slide switch; the GPIO drives the square wave). No I2S DAC:
// audio_dev_init returns nullptr, the sound.cpp mixer runs tone-less, and
// music/module PCM audio is unavailable on this board.
//
// Buzzer DIN = GPIO4, hw-identified: SD-probe chip-select toggles on 4
// clicked the piezo, and the 2kHz tone of the 3-pitch boot probe was the
// audible one.

#if defined(BOARD_HELTEC_V4)

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "audio_dev.h"
#include "Audio.h"
#include "../meshpunk_sync.h"   // SLog

#define HELTEC_BUZZER_PIN       4    // hw-identified: SD-probe CS toggles on 4
                                     // clicked the piezo, and the mid pitch of
                                     // the 3-tone probe was the audible one
#define HELTEC_BUZZ_LEDC_CH     2    // display backlight uses channel 1

static bool s_ledc_ready = false;

static void buzzer_attach(int pin) {
  ledcSetup(HELTEC_BUZZ_LEDC_CH, 2000, 10);
  ledcAttachPin(pin, HELTEC_BUZZ_LEDC_CH);
  s_ledc_ready = true;
}

Audio* audio_dev_init(void) {
  buzzer_attach(HELTEC_BUZZER_PIN);
  return nullptr;   // no I2S player on this board
}

AudioDevKind audio_dev_kind(void) { return AUDIO_DEV_BUZZER; }

// Sound-mixer tone translation — the ONLY path to this speaker. Every sound
// the board makes, notifications included, arrives as mixer note changes.
void audio_dev_buzzer_tone(uint32_t freq_hz) {
  if (!s_ledc_ready) return;
  ledcWriteTone(HELTEC_BUZZ_LEDC_CH, freq_hz);
}

void audio_dev_buzzer_off(void) {
  if (!s_ledc_ready) return;
  ledcWriteTone(HELTEC_BUZZ_LEDC_CH, 0);
}

// ── Decode-only player ─────────────────────────────────────────────────────
// No DAC is wired to I2S here, so this instance exists purely to decode: its
// PCM is captured by sound.cpp's staging hook and pushed to the USB sink.
// setPinout() is deliberately NOT called — i2s_set_pin never runs, so no GPIO
// is claimed and the peripheral clocks silence into nothing.
//
// DMA ring: the stock 8x512 costs 16KB of INTERNAL SRAM (the pool USB host
// and ELF module stacks compete for) purely to buffer samples nobody reads,
// since the hook makes the library skip its own i2s_write. 4x64 = 1KB
// instead. The library ignores i2s_driver_install's return value, and an
// uninstalled driver faults inside the IDF's own validity checks, so the
// MESHPUNK i2sDriverInstalled() accessor is checked here and the stock size
// retried before giving up: worst case is today's cost, never a broken
// driver. (The IDF lower bounds are undocumented in the shipped precompiled
// SDK, which is exactly why this verifies rather than trusts.)
#define HELTEC_DEC_DMA_COUNT 4
#define HELTEC_DEC_DMA_LEN   64

Audio* audio_dev_decoder_open(void) {
  Audio* a = new Audio(false, 3, I2S_NUM_0,
                       HELTEC_DEC_DMA_COUNT, HELTEC_DEC_DMA_LEN);
  if (a && !a->i2sDriverInstalled()) {
    SLog.println("[AUDIO] decoder: minimal DMA rejected, retrying stock size");
    delete a;
    a = new Audio(false, 3, I2S_NUM_0);          // library defaults
    if (a && !a->i2sDriverInstalled()) { delete a; a = nullptr; }
  }
  if (!a) { SLog.println("[AUDIO] decoder open FAILED"); return nullptr; }
  SLog.printf("[AUDIO] decoder open (internal free %u)\n",
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return a;
}

void audio_dev_decoder_close(Audio* a) {
  if (!a) return;
  delete a;   // destructor uninstalls the I2S driver and frees its buffers
  SLog.printf("[AUDIO] decoder closed (internal free %u)\n",
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

#endif // BOARD_HELTEC_V4
