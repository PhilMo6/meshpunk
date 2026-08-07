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
#include <string.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "sound.h"
#include "meshpunk_sync.h"

// Keyboard backlight commands live in the board input backend.
#include "input/input_dev.h"

// Host accessors implemented in main.cpp (the pref globals are file-static
// there).
extern bool    firmware_notify_kbd_enabled();
extern bool    firmware_notify_sound_enabled();
extern uint8_t firmware_kbd_brightness();
extern bool    firmware_kbd_timed_out();
extern uint32_t firmware_rtc_epoch();

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
static uint8_t  s_blink_steps   = BLINK_STEPS;   // this run's length

// ── Generic notification store ──────────────────────────────────────────────
// Ring of pre-formatted single-string records (see notify.h). Pre-allocated in
// PSRAM by notify_init() — which runs in setup() BEFORE luaBringUp()/
// lua_arena_create(), same rationale as the melody pre-render above: the block
// lands low in the PSRAM heap, never fragments the arena gap, and survives
// every Lua teardown. Guarded by s_log_mutex: writers can be any task (mesh
// task today, USB/battery/etc. later), readers are the Core-0 Lua bindings.

#define NOTIFY_LOG_CAP       32
#define NOTIFY_LOG_TEXT_MAX  192

struct NotifyRec {
    uint32_t ts;                        // our RTC clock at post time
    char     text[NOTIFY_LOG_TEXT_MAX];
};

static NotifyRec*        s_log        = nullptr;  // PSRAM, notify_init()
static uint8_t           s_log_head   = 0;        // next slot to write
static uint8_t           s_log_count  = 0;        // valid records (<= CAP)
static uint16_t          s_log_unseen = 0;        // posts since last _seen
static SemaphoreHandle_t s_log_mutex  = nullptr;

void notify_init() {
    s_log_mutex = xSemaphoreCreateMutex();
    s_log = (NotifyRec*)heap_caps_calloc(NOTIFY_LOG_CAP, sizeof(NotifyRec),
                                         MALLOC_CAP_SPIRAM);
    if (!s_log)
        SLog.println("[notify] log alloc failed - notification history disabled");

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
        s_blink_steps   = BLINK_STEPS;
        s_blink_next_ms = millis();   // first toggle on the next tick
        s_blink_active  = true;
    }
}

// Same state machine, one on/off cycle, no sound and no notification pref:
// UI acknowledgement for an explicit user action (the ELF mode toggles).
// Reusing this rather than open-coding a second blink matters because
// notify_tick() is the only place the backlight is driven from, so two
// blinkers could not fight over the restore value.
void notify_kbd_blink() {
    if (s_blink_active) return;
    s_blink_restore = firmware_kbd_timed_out() ? 0 : firmware_kbd_brightness();
    s_blink_step    = 0;
    s_blink_steps   = 2;              // on, off, restore
    s_blink_next_ms = millis();
    s_blink_active  = true;
}

void notify_tick() {
    if (!s_blink_active) return;
    uint32_t now = millis();
    if ((int32_t)(now - s_blink_next_ms) < 0) return;
    s_blink_step++;
    if (s_blink_step > s_blink_steps) {
        input_dev_kbd_backlight(s_blink_restore);
        s_blink_active = false;
        return;
    }
    input_dev_kbd_backlight((s_blink_step % 2 == 1) ? 255 : 0);
    s_blink_next_ms = now + BLINK_STEP_MS;
}

// ── Generic notification store ──────────────────────────────────────────────

void notify_post(const char* text) {
    if (s_log && s_log_mutex && text && text[0] &&
        xSemaphoreTake(s_log_mutex, portMAX_DELAY) == pdTRUE) {
        NotifyRec* rec = &s_log[s_log_head];
        rec->ts = firmware_rtc_epoch();
        strncpy(rec->text, text, NOTIFY_LOG_TEXT_MAX - 1);
        rec->text[NOTIFY_LOG_TEXT_MAX - 1] = '\0';
        s_log_head = (s_log_head + 1) % NOTIFY_LOG_CAP;
        if (s_log_count < NOTIFY_LOG_CAP) s_log_count++;
        if (s_log_unseen < 0xFFFF) s_log_unseen++;
        xSemaphoreGive(s_log_mutex);
    }
    notify_message_alert();   // record even when both delivery prefs are off
}

int notify_log_count() {
    if (!s_log_mutex) return 0;
    xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    int n = s_log_count;
    xSemaphoreGive(s_log_mutex);
    return n;
}

uint16_t notify_log_unseen() {
    if (!s_log_mutex) return 0;
    xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    uint16_t n = s_log_unseen;
    xSemaphoreGive(s_log_mutex);
    return n;
}

bool notify_log_get(int i, uint32_t* ts, char* buf, size_t buflen) {
    if (!s_log || !s_log_mutex || i < 0 || !buf || buflen == 0) return false;
    bool ok = false;
    xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    if (i < s_log_count) {
        // i=0 is the newest record: one slot behind the write head.
        int slot = (s_log_head - 1 - i + 2 * NOTIFY_LOG_CAP) % NOTIFY_LOG_CAP;
        if (ts) *ts = s_log[slot].ts;
        strncpy(buf, s_log[slot].text, buflen - 1);
        buf[buflen - 1] = '\0';
        ok = true;
    }
    xSemaphoreGive(s_log_mutex);
    return ok;
}

void notify_log_seen() {
    if (!s_log_mutex) return;
    xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    s_log_unseen = 0;
    xSemaphoreGive(s_log_mutex);
}

void notify_log_clear() {
    if (!s_log_mutex) return;
    xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    s_log_head = 0;
    s_log_count = 0;
    s_log_unseen = 0;
    xSemaphoreGive(s_log_mutex);
}
