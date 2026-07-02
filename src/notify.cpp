// notify.cpp — C-side message notification alerts (melody + keyboard blink).
//
// Replaces the old Lua-side path (topbar.lua's kbd_blink_notify/sound_notify),
// which went dark whenever Lua was torn down for an ELF run. Everything here
// runs on always-alive infrastructure:
//   - notify_message_alert() fires from the mesh RX handlers (mesh task, Core 1)
//   - the melody is a C-owned SoundObject; the Core-1 sound task mixes TONE
//     objects into I2S itself, so it is audible even mid-Doom (same path as
//     module audio)
//   - the blink state machine is stepped by notify_tick() from mesh_task_body
//
// Concurrency: the blink state is single-task by construction — armed by
// notify_message_alert() and stepped by notify_tick(), both on the mesh task.
// The only shared resource is the I2C bus (Wire), whose per-transaction HAL
// lock serializes the brightness writes against the keyboard matrix reads
// (loopTask normally, the elf_input task during ELF runs).

#include "notify.h"

#include <Arduino.h>
#include "sound.h"
#include "meshpunk_sync.h"

// Host accessors implemented in main.cpp (the pref/backlight globals are
// file-static there).
extern void    setKeyboardBrightness(uint8_t value);
extern bool    firmware_notify_kbd_enabled();
extern bool    firmware_notify_sound_enabled();
extern uint8_t firmware_kbd_brightness();
extern bool    firmware_kbd_timed_out();

// C-owned notification melody (the same C-major arpeggio topbar.lua used to
// build). Pre-rendered once at boot so the ~148KB PCM buffer lands low in the
// PSRAM heap (no mid-heap fragmentation wall) and survives lua_close() on ELF
// launch.
static int s_melody_id = -1;

// ── Keyboard blink state machine ────────────────────────────────────────────

#define BLINK_STEP_MS  150
#define BLINK_STEPS    6      // 3 on/off cycles, then restore

static bool     s_blink_active  = false;
static uint8_t  s_blink_step    = 0;
static uint32_t s_blink_next_ms = 0;
static uint8_t  s_blink_restore = 0;

void notify_init() {
    static const MelodyNote NOTIFY_MELODY[] = {
        { 523, 150}, {0, 30},
        { 659, 150}, {0, 30},
        { 784, 150}, {0, 30},
        {1047, 300},
    };
    ToneParams base{};
    base.attack_ms     = 5;
    base.decay_ms      = 30;
    base.sustain_level = 0.6f;
    base.release_ms    = 30;
    s_melody_id = sound_create_melody_notes(
        NOTIFY_MELODY, sizeof(NOTIFY_MELODY) / sizeof(NOTIFY_MELODY[0]), base);
    if (s_melody_id < 0)
        SLog.println("[notify] melody pre-render failed - sound alerts disabled");
}

void notify_message_alert() {
    if (firmware_notify_sound_enabled() && s_melody_id >= 0)
        sound_play(s_melody_id);   // mutex-guarded; the Core-1 sound task mixes it

    if (firmware_notify_kbd_enabled() && !s_blink_active) {
        // Restore target: the user's brightness, or 0 if the backlight is
        // currently timed out (mirror of the old Lua blink).
        s_blink_restore = firmware_kbd_timed_out() ? 0 : firmware_kbd_brightness();
        s_blink_step    = 0;
        s_blink_next_ms = millis();   // first toggle on the next tick
        s_blink_active  = true;
    }
}

void notify_tick() {
    if (!s_blink_active) return;
    uint32_t now = millis();
    if ((int32_t)(now - s_blink_next_ms) < 0) return;
    s_blink_step++;
    if (s_blink_step > BLINK_STEPS) {
        setKeyboardBrightness(s_blink_restore);
        s_blink_active = false;
        return;
    }
    setKeyboardBrightness((s_blink_step % 2 == 1) ? 255 : 0);
    s_blink_next_ms = now + BLINK_STEP_MS;
}
