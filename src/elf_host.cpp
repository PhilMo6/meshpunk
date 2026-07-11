#include "elf_host.h"
#include "elf_loader.h"
#include "meshpunk_fs.h"
#include "meshpunk_sync.h"
#include "sound.h"
#include "tdeck-pins.h"
#include "ble_companion.h"
#include "punkmesh.h"

#include <Arduino.h>
#include <Ticker.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <esp_rom_sys.h>   // esp_rom_printf — safe to call from exception context
#include <esp_attr.h>      // IRAM_ATTR — the ELF fault handler runs from IRAM
#include <soc/timer_group_reg.h>  // TG1 MWDT (interrupt watchdog) register access
#include <lvgl.h>
#include <math.h>
#include <setjmp.h>
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/reent.h>
#include <errno.h>
#include <time.h>
#include <locale.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

// From main.cpp
extern TFT_eSPI tft;
extern Ticker lvgl_ticker;
extern void wake_activity();  // reset inactivity timer + restore backlights

// GCC runtime builtins (soft-float, 64-bit division, double-precision)
extern "C" {
    long long __divdi3(long long, long long);
    float __divsf3(float, float);
    double __extendsfdf2(float);
    int __ltdf2(double, double);
    float __truncdfsf2(double);
    // Double-precision arithmetic
    double __adddf3(double, double);
    double __subdf3(double, double);
    double __muldf3(double, double);
    double __divdf3(double, double);
    int    __gtdf2(double, double);
    int    __gedf2(double, double);
    int    __ledf2(double, double);
    int    __nedf2(double, double);
    int    __eqdf2(double, double);
    double __floatsidf(int);
    double __floatunsidf(unsigned);
    int    __fixdfsi(double);
    unsigned __fixunsdfsi(double);
    long long __fixdfdi(double);
    double __floatdidf(long long);
    unsigned long long __udivdi3(unsigned long long, unsigned long long);
    unsigned long long __umoddi3(unsigned long long, unsigned long long);
    long long __moddi3(long long, long long);
}

// Trackball ISR counters (from main.cpp)
extern volatile int trackball_click;
extern volatile int trackball_up;
extern volatile int trackball_down;
extern volatile int trackball_left;
extern volatile int trackball_right;

// Keyboard constants (mirrored from main.cpp)
#define KB_SLAVE_ADDR  0x55
#define KB_COLS_N      5
#define KB_ROWS_N      7

static const char kb_map[KB_COLS_N][KB_ROWS_N] = {
  {'q','w',  0, 'a',  0, ' ',  0 },
  {'e','s','d','p','x','z',  0 },
  {'r','g','t',  0, 'v','c','f'},
  {'u','h','y',  0, 'b','n','j'},
  {'o','l','i',  0, '$','m','k'},
};

// SYM-held layer — digits and symbols. Mirrors main.cpp's kb_matrix_symbol so
// the ELF-module keyboard path can produce numbers/symbols (essential for DOS
// and other modules; without this only letters were reachable).
static const char kb_map_sym[KB_COLS_N][KB_ROWS_N] = {
  {'#','1',  0, '*',  0,   0, '0'},
  {'2','4','5','@','8','7',  0 },
  {'3','/','(',  0, '?','9','6'},
  {'_',':',')',  0, '!',',',';'},
  {'+','"','-',  0,   0, '.','\''},
};

// Modifier positions in the matrix
#define MOD_LSHIFT_COL 1
#define MOD_LSHIFT_ROW 6
#define MOD_RSHIFT_COL 2
#define MOD_RSHIFT_ROW 3
#define MOD_SYM_COL    0
#define MOD_SYM_ROW    2
#define MOD_ALT_COL    0
#define MOD_ALT_ROW    4
#define KEY_ENTER_COL  3
#define KEY_ENTER_ROW  3
#define KEY_BS_COL     4
#define KEY_BS_ROW     3

// ---------------------------------------------------------------------------
// Key queue for host_get_key()
// ---------------------------------------------------------------------------

#define KEY_QUEUE_SIZE 32

struct KeyEvent {
    unsigned char key;
    int pressed;
};

static KeyEvent key_queue[KEY_QUEUE_SIZE];
static volatile int kq_head = 0;
static volatile int kq_tail = 0;

// Core 1 ELF input task: samples the keyboard at a fixed rate independent of
// the game loop (which monopolizes Core 0). See elf_input_start/stop.
static TaskHandle_t   s_input_task     = nullptr;
static volatile bool  s_input_task_run = false;

// Key ring: producers are the Core 1 input task (poll_input) and the USB HID
// keyboard driver (usb_task, also Core 1, via elf_input_inject) — the spinlock
// serializes them. Consumer is the game's scanInput on Core 0 (kq_pop). The
// barriers make the slot's data writes visible before kq_head advances (and
// vice-versa on the read side), which matters with the ends on different cores.
static portMUX_TYPE s_kq_mux = portMUX_INITIALIZER_UNLOCKED;

static void kq_push(unsigned char key, int pressed) {
    portENTER_CRITICAL(&s_kq_mux);
    int next = (kq_head + 1) % KEY_QUEUE_SIZE;
    if (next != kq_tail) {      // else full: drop
        key_queue[kq_head].key = key;
        key_queue[kq_head].pressed = pressed;
        __sync_synchronize();   // publish the slot before advancing head
        kq_head = next;
    }
    portEXIT_CRITICAL(&s_kq_mux);
}

static bool kq_pop(KeyEvent* out) {
    if (kq_head == kq_tail) return false;
    __sync_synchronize();       // see the slot writes that preceded head's advance
    *out = key_queue[kq_tail];
    kq_tail = (kq_tail + 1) % KEY_QUEUE_SIZE;
    return true;
}

// Previous key states for edge detection (136 to cover trackball pseudo-codes 0x80-0x87)
// 256 so the full byte range is addressable — the module extension codes for
// F1-F10 live at 0xB0-0xB9 (Alt+digit), above the old 136 ceiling.
#define INPUT_STATE_SIZE 256
static bool prev_key_state[INPUT_STATE_SIZE] = {0};
static bool esc_held = false;
static uint32_t esc_hold_start = 0;
#define ESC_EXIT_HOLD_MS 1500

// USB keyboard Backspace held (set by elf_input_inject, pre-keymap) — OR'd
// into the exit-hold check so a USB keyboard can leave a module too.
static volatile bool s_usb_bs_down = false;

// Raw-delta mode: set by host_trackball_read() (module emulates a mouse and
// owns the ISR counters); reset before each module run.
static volatile bool s_trk_raw_mode = false;

// Trackball momentum state
static float trk_vel_x = 0, trk_vel_y = 0;
static float trk_impulse  = 1.5f;   // velocity added per ISR tick
static float trk_friction = 0.82f;  // multiplied each poll cycle
static float trk_thresh   = 0.4f;   // below this, key released
static bool  trk_momentum = true;   // false = legacy one-tick-per-poll

// ---------------------------------------------------------------------------
// Data-driven keymap: T-Deck physical key → module keycode.
// Populated by parse_keymap_arg() when the launcher passes -keymap.
// PURE REMAPPER: a zero entry means "no remap" and the key passes through
// unchanged (modules ignore codes they don't know). Only mapped keys are
// translated. Default is passthrough mode (no table lookup at all).
// ---------------------------------------------------------------------------
static uint8_t keymap_table[INPUT_STATE_SIZE];
static bool keymap_passthrough = true;   // default: all keys pass through as-is

// Parse a keymap string: "AD=77+81,AF=73+82,A0=61,..."
// Format: OUTPUT_HEX=INPUT_HEX[+ALT_INPUT_HEX], comma-separated.
static void parse_keymap_arg(const char* str) {
    memset(keymap_table, 0, sizeof(keymap_table));
    if (!str || !*str) return;

    const char* p = str;
    while (*p) {
        // Parse output keycode and primary input key (2 hex digits each)
        unsigned out = 0, key1 = 0, key2 = 0;
        if (sscanf(p, "%2x=%2x", &out, &key1) < 2) break;
        if (key1 < INPUT_STATE_SIZE)
            keymap_table[key1] = (uint8_t)out;

        // Advance past "OO=KK"
        const char* plus = strchr(p, '+');
        const char* comma = strchr(p, ',');

        // Check for +ALT before the next comma (or end of string)
        if (plus && (!comma || plus < comma)) {
            if (sscanf(plus + 1, "%2x", &key2) == 1) {
                if (key2 < INPUT_STATE_SIZE)
                    keymap_table[key2] = (uint8_t)out;
            }
        }

        // Advance to next entry
        if (comma) p = comma + 1;
        else break;
    }
}

// ---------------------------------------------------------------------------
// Poll keyboard + trackball, push key events onto queue for loaded module
// ---------------------------------------------------------------------------

// kb_only=true: read the keyboard matrix and queue key edges, but skip the
// trackball velocity integration (and preserve its previous state so no
// spurious trackball edge is generated). Used by the high-rate keyboard
// pump in host_sleep_ms — the trackball momentum model is tuned for the
// once-per-frame poll rate and must not be advanced ~200 times a second.
static void poll_input(bool kb_only = false) {
    bool cur_state[INPUT_STATE_SIZE] = {0};

    // Read keyboard matrix via I2C
    uint8_t matrix[KB_COLS_N] = {0};
    Wire.requestFrom(KB_SLAVE_ADDR, KB_COLS_N);
    for (int c = 0; c < KB_COLS_N && Wire.available(); c++) {
        matrix[c] = Wire.read();
    }

    bool shift = (matrix[MOD_LSHIFT_COL] & (1 << MOD_LSHIFT_ROW)) ||
                 (matrix[MOD_RSHIFT_COL] & (1 << MOD_RSHIFT_ROW));
    bool sym = (matrix[MOD_SYM_COL] & (1 << MOD_SYM_ROW));
    bool alt = (matrix[MOD_ALT_COL] & (1 << MOD_ALT_ROW));

    // Decode matrix into character states
    for (int c = 0; c < KB_COLS_N; c++) {
        if (matrix[c] == 0) continue;
        for (int r = 0; r < KB_ROWS_N; r++) {
            if (!(matrix[c] & (1 << r))) continue;
            // Skip modifiers
            if (c == MOD_LSHIFT_COL && r == MOD_LSHIFT_ROW) continue;
            if (c == MOD_RSHIFT_COL && r == MOD_RSHIFT_ROW) continue;
            if (c == MOD_SYM_COL && r == MOD_SYM_ROW) continue; // sym
            if (c == MOD_ALT_COL && r == MOD_ALT_ROW) continue; // alt

            if (c == KEY_ENTER_COL && r == KEY_ENTER_ROW) {
                cur_state[0x0D] = true;
            } else if (c == KEY_BS_COL && r == KEY_BS_ROW) {
                cur_state[0x08] = true; // backspace (BS)
            } else {
                // Layer select: Alt + a digit-position key -> F1..F10 (module
                // maps 0xB0-0xB9); Sym -> number/symbol layer; else base.
                char ch;
                if (alt) {
                    char s = kb_map_sym[c][r];
                    if (s >= '1' && s <= '9')  ch = (char)(0xB0 + (s - '1'));
                    else if (s == '0')         ch = (char)0xB9; // F10
                    else if (s == '/')         ch = '\\';       // Alt+/ (G key) -> backslash for DOS paths
                    else                       ch = kb_map[c][r];
                } else if (sym) {
                    ch = kb_map_sym[c][r];
                } else {
                    ch = kb_map[c][r];
                }
                if (ch) cur_state[(uint8_t)ch] = true;
            }
        }
    }

    // Shift state
    if (shift) cur_state[0x80] = true; // pseudo-code for shift

    if (!kb_only && s_trk_raw_mode) {
        // Module reads raw deltas via host_trackball_read() (e.g. PC-XT's
        // serial mouse) — leave the ISR counters alone, emit no 0x81-0x85.
    } else if (!kb_only) {
        // Trackball — momentum or legacy mode
        if (trk_momentum) {
            // Accumulate all pending ISR ticks into velocity
            int up = trackball_up;    trackball_up = 0;
            int dn = trackball_down;  trackball_down = 0;
            int lt = trackball_left;  trackball_left = 0;
            int rt = trackball_right; trackball_right = 0;

            trk_vel_y -= up * trk_impulse;
            trk_vel_y += dn * trk_impulse;
            trk_vel_x -= lt * trk_impulse;
            trk_vel_x += rt * trk_impulse;

            // Apply friction
            trk_vel_x *= trk_friction;
            trk_vel_y *= trk_friction;

            // Snap to zero below threshold
            if (fabsf(trk_vel_x) < trk_thresh) trk_vel_x = 0;
            if (fabsf(trk_vel_y) < trk_thresh) trk_vel_y = 0;

            // Report as held keys while velocity is above threshold
            if (trk_vel_y < -trk_thresh) cur_state[0x81] = true;  // up
            if (trk_vel_y >  trk_thresh) cur_state[0x82] = true;  // down
            if (trk_vel_x < -trk_thresh) cur_state[0x83] = true;  // left
            if (trk_vel_x >  trk_thresh) cur_state[0x84] = true;  // right
        } else {
            // Legacy: one tick per poll cycle
            if (trackball_up > 0)    { trackball_up--;    cur_state[0x81] = true; }
            if (trackball_down > 0)  { trackball_down--;  cur_state[0x82] = true; }
            if (trackball_left > 0)  { trackball_left--;  cur_state[0x83] = true; }
            if (trackball_right > 0) { trackball_right--; cur_state[0x84] = true; }
        }
        if (trackball_click > 0) { trackball_click = 0; cur_state[0x85] = true; }
    } else {
        // Keyboard-only tick: carry the trackball's previous state forward so
        // edge detection sees no change (no spurious press/release) and the
        // momentum integration stays exclusively on the lower-rate full poll.
        for (int i = 0x81; i <= 0x85; i++) cur_state[i] = prev_key_state[i];
    }

    // Edge detection: generate press/release events for changed keys.
    // In passthrough mode (default), all keys are pushed as-is. In keymap
    // mode the table is a pure remapper: mapped keys translate, everything
    // else passes through unchanged (unknown codes are ignored by modules).
    for (int i = 0; i < INPUT_STATE_SIZE; i++) {
        if (cur_state[i] != prev_key_state[i]) {
            uint8_t out;
            if (keymap_passthrough) {
                out = (uint8_t)i;  // pass through as-is
            } else {
                out = keymap_table[i];
                if (!out) out = (uint8_t)i;   // unmapped: pass through
            }
            if (out)
                kq_push(out, cur_state[i] ? 1 : 0);
        }
    }
    memcpy(prev_key_state, cur_state, INPUT_STATE_SIZE);

    // Exit hold detection: hold backspace for 1.5s to return to launcher.
    // T-Deck has no physical ESC key; backspace (0x08) is the exit key.
    // s_usb_bs_down folds in a USB keyboard's Backspace (this poll runs at
    // 100Hz and would otherwise clear esc_held from the matrix every tick).
    if (cur_state[0x08] || s_usb_bs_down) {
        if (!esc_held) { esc_held = true; esc_hold_start = millis(); }
    } else {
        esc_held = false;
    }
}

// ---------------------------------------------------------------------------
// Core 1 ELF input task
// ---------------------------------------------------------------------------
// While an ELF module runs, the game owns Core 0 entirely (the elf_run task,
// and loopTask blocked behind it), so the only way to sample input is once
// per game frame — which on a heavy cart collapses to ~10 reads/sec and drops
// any keypress shorter than a frame. This task runs on Core 1 and reads the
// keyboard at a fixed 100Hz regardless of what the game is doing, queuing
// edges into the same kq ring host_get_key drains. A tap is now caught and
// latched even while Core 0 is mid-frame.
//
// I2C safety: _launch_elf blocks loopTask for the whole module run, so the
// firmware's own keyboard scanning (the loop() path — untouched) is dormant;
// this task is the only keyboard-matrix READER between elf_input_start/stop,
// and stop() returns only once it has fully exited, before loopTask resumes.
// The notification blink (notify.cpp, mesh task) may WRITE the backlight
// brightness concurrently — Wire's per-transaction HAL lock serializes it
// against the matrix reads here.
//
// The keyboard is read every tick (100Hz); the trackball's momentum model is
// integrated only every 3rd tick (~33Hz) so its tuned feel is unchanged by
// the higher keyboard rate (poll_input's kb_only path skips the trackball).

#define ELF_INPUT_PERIOD_MS   10   // 100 Hz keyboard sampling
#define ELF_INPUT_TRK_EVERY   3    // integrate trackball every Nth tick (~33 Hz)

static void elf_input_task_body(void* param) {
    (void)param;
    TickType_t last = xTaskGetTickCount();
    uint32_t tick = 0;
    while (s_input_task_run) {
        bool do_trackball = (tick % ELF_INPUT_TRK_EVERY) == 0;
        poll_input(/*kb_only=*/!do_trackball);
        tick++;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(ELF_INPUT_PERIOD_MS));
    }
    s_input_task = nullptr;   // signal stop() that we've exited (no more I2C)
    vTaskDelete(nullptr);
}

// Start the Core 1 input task. Resets the queue + edge state so the module
// starts from a clean slate. Called from _launch_elf just before the module
// runs; idempotent if already running.
static void elf_input_start() {
    if (s_input_task) return;
    kq_head = kq_tail = 0;
    memset(prev_key_state, 0, INPUT_STATE_SIZE);
    esc_held = false;
    s_usb_bs_down = false;
    s_input_task_run = true;
    // Priority 5: ABOVE usb_mgr (4), sound_task (3) and elf_blit (3). The
    // keyboard poll is a tiny, latency-critical task (one I2C read every 10ms);
    // when USB host is streaming, those higher-priority core-1 tasks were
    // starving it at priority 2 — the game saw laggy/missed/doubled keys. Input
    // responsiveness beats a few ms of audio/blit jitter (absorbed by their
    // buffers), and the poll yields immediately so it can't starve them.
    if (xTaskCreatePinnedToCore(elf_input_task_body, "elf_input", 3072,
                                nullptr, 5, &s_input_task, 1 /* Core 1 */) != pdPASS) {
        // Couldn't spawn — fall back to host_get_key's own polling.
        s_input_task = nullptr;
        s_input_task_run = false;
        SLog.println("[elf_host] WARN: input task spawn failed, using per-frame polling");
    }
}

// Stop the Core 1 input task and wait for it to fully exit before returning,
// so no I2C read is in flight when loopTask resumes its own keyboard scan.
static void elf_input_stop() {
    if (!s_input_task) return;
    s_input_task_run = false;
    for (int i = 0; i < 100 && s_input_task; i++) {  // ~200ms safety cap
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    s_input_task = nullptr;
}

// ── Firmware-internal injection (USB HID keyboard; see elf_host.h) ─────────

bool elf_input_active(void) { return s_input_task_run; }

void elf_input_inject(unsigned char key, int pressed) {
    if (key == 0x08) s_usb_bs_down = (pressed != 0);   // exit-hold, pre-keymap
    if (!s_input_task_run) return;
    uint8_t out;
    if (keymap_passthrough) {
        out = key;
    } else {
        out = keymap_table[key];
        if (!out) out = key;    // unmapped: pass through (as poll_input)
    }
    if (out) kq_push(out, pressed ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Host function implementations
// ---------------------------------------------------------------------------

extern "C" {

// Count of fread/fwrite transfers we bounced because dst/src was in PSRAM.
static volatile uint32_t g_bounce_reads = 0;

void host_blit_frame(const uint16_t* rgb565, int w, int h) {
    SPI_LOCK();
    tft.startWrite();
    int x_offset = (320 - w) / 2;
    if (x_offset < 0) x_offset = 0;
    int y_offset = (240 - h) / 2;
    if (y_offset < 0) y_offset = 0;
    tft.setAddrWindow(x_offset, y_offset, w, h);
    tft.pushColors((uint16_t*)rgb565, w * h, false);
    tft.endWrite();
    SPI_UNLOCK();
}

// ── Async blit ───────────────────────────────────────────────────────────────
// A Core-1 task owns the (blocking) SPI push so the module keeps running on
// Core 0 during the transfer. The module double-buffers and hands over a
// frame pointer; back-pressure is one frame deep.
//
// Priority 3 (tied with the sound task, above the mesh task at 2). At the old
// priority 2 the push lost CPU to both synth (3) and the radio task (2) and to
// the shared spi_bus_mutex, so an ~11.5 ms transfer couldn't finish inside a
// 50-72 ms Core-0 step — Core 0 then stalled ~a full transfer every frame
// (blitwait ~= 11.5 ms). At 3 it preempts the radio task and, via the mutex's
// priority inheritance, gets the SPI bus released to it sooner; tied with synth
// (which is mostly blocked waiting on I2S) keeps audio fed. DMA isn't an option
// here: the TFT shares one register-level SPI bus with the radio and SD, so
// TFT_eSPI initDMA() would install the esp-idf driver on it and break them.
// If audio underruns/crackles after this, drop back to 2.
static TaskHandle_t      s_blit_task = nullptr;
static SemaphoreHandle_t s_blit_idle = nullptr;   // given when no push in flight
static const uint16_t* volatile s_blit_buf = nullptr;
static volatile int s_blit_w = 0, s_blit_h = 0;

static void blit_task_body(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint16_t* buf = s_blit_buf;
        if (buf) host_blit_frame(buf, s_blit_w, s_blit_h);
        xSemaphoreGive(s_blit_idle);
    }
}

void host_blit_frame_async(const uint16_t* rgb565, int w, int h) {
    if (!s_blit_task) {
        if (!s_blit_idle) {
            s_blit_idle = xSemaphoreCreateBinary();
            if (!s_blit_idle) { host_blit_frame(rgb565, w, h); return; }
            xSemaphoreGive(s_blit_idle);
        }
        xTaskCreatePinnedToCore(blit_task_body, "elf_blit", 4096, nullptr,
                                3, &s_blit_task, 1 /* Core 1 */);
        if (!s_blit_task) { host_blit_frame(rgb565, w, h); return; }
    }
    // Wait until the previous frame is on the wire — after this the caller's
    // other buffer is free to render into.
    xSemaphoreTake(s_blit_idle, portMAX_DELAY);
    s_blit_buf = rgb565;
    s_blit_w = w;
    s_blit_h = h;
    xTaskNotifyGive(s_blit_task);
}

// Wait for any in-flight async push. Must be called before the module's
// memory is unmapped — the blit task reads the frame straight from module BSS.
static void blit_drain(void) {
    if (!s_blit_task) return;
    xSemaphoreTake(s_blit_idle, portMAX_DELAY);
    s_blit_buf = nullptr;
    xSemaphoreGive(s_blit_idle);
}

void host_clear_screen(void) {
    SPI_LOCK();
    tft.startWrite();
    tft.fillScreen(TFT_BLACK);
    tft.endWrite();
    SPI_UNLOCK();
}

uint32_t host_get_ticks_ms(void) {
    return (uint32_t)millis();
}

uint32_t host_get_ticks_us(void) {
    return (uint32_t)micros();
}

void host_sleep_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

int host_get_key(int* pressed, unsigned char* key) {
    // Normally the Core 1 input task fills the queue; only self-poll if that
    // task isn't running (spawn failed), so the queue never has two producers.
    if (!s_input_task) poll_input();
    KeyEvent ev;
    if (kq_pop(&ev)) {
        *pressed = ev.pressed;
        *key = ev.key;
        return 1;
    }
    return 0;
}

// Raw trackball deltas for modules that emulate a pointing device (PC-XT's
// serial mouse). First call opts the module in: the Core 1 input task stops
// consuming the ISR counters and stops emitting 0x81-0x85 pseudo-keys.
// Subtract-what-was-read (not =0) so ticks landing between the read and the
// write survive — same tolerance as the input task's own consumption.
void host_trackball_read(int* dx, int* dy, int* click) {
    s_trk_raw_mode = true;
    int up = trackball_up;    trackball_up    -= up;
    int dn = trackball_down;  trackball_down  -= dn;
    int lt = trackball_left;  trackball_left  -= lt;
    int rt = trackball_right; trackball_right -= rt;
    int ck = trackball_click; trackball_click -= ck;
    if (dx)    *dx = rt - lt;
    if (dy)    *dy = dn - up;
    if (click) *click = ck;
}

// Track the sample rate across calls so we only reconfigure the mixer when
// the rate changes. Reset to 0 by the cleanup path after module exit so the
// next module's first push always sets the rate correctly.
static int s_audio_last_rate = 0;

void host_audio_push(const int16_t* samples, int count, int sample_rate) {
    // Route through the firmware's sound mixer — samples are mixed alongside
    // notification tones, with firmware volume/mute applied automatically.
    // Set the upsample factor so the mixer resamples correctly for this
    // module's rate (e.g. 11025 Hz, 22050 Hz, or 44100 Hz).
    if (sample_rate != s_audio_last_rate) {
        sound_extern_set_rate(sample_rate);
        s_audio_last_rate = sample_rate;
    }
    sound_extern_push(samples, count);
}

// Pull-model audio: the module registers a synth callback that the sound
// task (Core 1) invokes for exactly the samples the I2S pipeline needs,
// replacing per-frame host_audio_push() pacing from the game loop (Core 0).
// Constraints on the callback: it runs on the firmware's sound task, so it
// must not block and must not call the module's malloc/free (the alloc
// tracker is single-task). Pass cb=NULL to unregister; that blocks until
// the mixer is outside the callback, making it safe to free module memory.
void host_audio_set_pull(void (*cb)(int16_t* out, int count), int sample_rate) {
    // Relocations targeting .text arrive instruction-side (0x42/0x43) from
    // the loader, but remap defensively in case the pointer came through a
    // data-side path — a 0x3C/0x3D PSRAM alias is never executable.
    uint32_t addr = (uint32_t)cb;
    if (addr >= 0x3C000000 && addr < 0x3E000000)
        cb = (void (*)(int16_t*, int))(addr + 0x06000000);
    sound_extern_set_pull(cb, sample_rate);
    // Force the next host_audio_push() to reprogram the mixer rate —
    // unregistering resets the upsample factor behind s_audio_last_rate.
    s_audio_last_rate = 0;
}

int host_should_exit(void) {
    return (esc_held && (millis() - esc_hold_start > ESC_EXIT_HOLD_MS)) ? 1 : 0;
}

void* host_read_file(const char* path, uint32_t* out_size) {
    // Default to SD for ELF module file access
    return meshpunk_read_all(path, out_size, /*default_sd=*/true);
}

int host_write_file(const char* path, const void* data, uint32_t size) {
    MeshpunkFile mf = meshpunk_open(path, "w", /*default_sd=*/true);
    if (!mf.valid) {
        // First write into a directory that doesn't exist yet (e.g. a cart's
        // cdata/ folder) — create the parents and retry once.
        meshpunk_mkdirs(path, /*default_sd=*/true);
        mf = meshpunk_open(path, "w", /*default_sd=*/true);
        if (!mf.valid) return -1;
    }
    size_t written = mf.file.write((const uint8_t*)data, size);
    meshpunk_close(mf);
    return (written == size) ? 0 : -1;
}

void host_log(const char* msg) {
    SLog.printf("[elf_mod] %s\n", msg);
}

// Module-callable internal-heap integrity probe. Prints the tag FIRST, so if
// the heap is corrupt and the walk loops/faults (silent TG1WDT), the last
// "[heapchk] <tag>:" line on the wire names the step that did the damage.
void host_check_heap(const char* tag) {
    // Also report this task's remaining stack — if it plummets toward 0, the
    // module is overflowing the task stack (spilling into adjacent internal-RAM
    // heap = the "corruption").
    esp_rom_printf("[heapchk] %s (stk_free=%u): int=", tag ? tag : "?",
                   (unsigned)uxTaskGetStackHighWaterMark(NULL));
    // Print incrementally: if a walk hangs/faults on a corrupt heap, the last
    // token on the wire names which heap broke.
    bool ok = heap_caps_check_integrity(MALLOC_CAP_INTERNAL, false);
    esp_rom_printf("%s psram=", ok ? "ok" : "CORRUPT");
    // The module heap (Lua's entire world) lives in PSRAM — this is the one
    // that matters for cart-corruption hunts.
    bool ok_ps = heap_caps_check_integrity(MALLOC_CAP_SPIRAM, false);
    esp_rom_printf("%s\n", ok_ps ? "ok" : "CORRUPT");
}

// File I/O wrappers for loaded modules.
//
// CRITICAL: SD transfers use SPI DMA, and on the ESP32-S3 DMA CANNOT target
// PSRAM. Modules that stream large files into PSRAM-allocated buffers via fread
// would have the SD DMA write to a bogus address, corrupting internal RAM (heap
// metadata, DMA descriptors). We bounce any PSRAM-destined read/write through
// an internal DMA-capable buffer so the DMA only ever touches internal RAM.
static inline bool elf_is_psram(const void* p) {
    uint32_t a = (uint32_t)p;
    return a >= 0x3C000000u && a < 0x3E000000u;   // PSRAM data bus window
}

static uint8_t* s_io_bounce = nullptr;            // lazily allocated, reused
#define ELF_IO_BOUNCE_SZ 8192

static uint8_t* elf_io_bounce() {
    if (!s_io_bounce)
        s_io_bounce = (uint8_t*)heap_caps_malloc(ELF_IO_BOUNCE_SZ, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    return s_io_bounce;
}

// fread that never DMAs into PSRAM (called only by the loaded module, which is
// single-threaded on the elf_run task — so the shared bounce buffer is safe).
// SPI-locked file wrappers — SD card shares the SPI bus with the radio,
// so every file operation must hold the bus mutex.

// Module file descriptor tracker — same pattern as the PSRAM allocation tracker.
// Modules may keep files open for streaming reads; if the module exits via exit()
// or abort(), those handles leak and exhaust FATFS file descriptors.
#define MAX_MODULE_FILES 16
static FILE* s_mod_files[MAX_MODULE_FILES];
static int   s_mod_file_count = 0;

static void mod_track_file(FILE* f) {
    if (!f) return;
    if (s_mod_file_count < MAX_MODULE_FILES)
        s_mod_files[s_mod_file_count++] = f;
    else
        printf("[elf_host] WARNING: module file tracker full\n");
}

static void mod_untrack_file(FILE* f) {
    if (!f) return;
    for (int i = s_mod_file_count - 1; i >= 0; i--) {
        if (s_mod_files[i] == f) {
            s_mod_files[i] = s_mod_files[--s_mod_file_count];
            return;
        }
    }
}

static void mod_close_tracked_files() {
    int n = s_mod_file_count;
    for (int i = 0; i < n; i++) {
        if (s_mod_files[i])
            fclose(s_mod_files[i]);
    }
    s_mod_file_count = 0;
    if (n > 0)
        printf("[elf_host] closed %d leaked module file descriptors\n", n);
}

FILE* elf_fopen(const char* path, const char* mode) {
    SPI_LOCK();
    FILE* f = fopen(path, mode);
    SPI_UNLOCK();
    mod_track_file(f);
    return f;
}

int elf_fclose(FILE* f) {
    mod_untrack_file(f);
    SPI_LOCK();
    int r = fclose(f);
    SPI_UNLOCK();
    return r;
}

int elf_fseek(FILE* f, long offset, int whence) {
    SPI_LOCK();
    int r = fseek(f, offset, whence);
    SPI_UNLOCK();
    return r;
}

long elf_ftell(FILE* f) {
    SPI_LOCK();
    long r = ftell(f);
    SPI_UNLOCK();
    return r;
}

size_t elf_fread(void* dst, size_t size, size_t nmemb, FILE* f) {
    size_t total = size * nmemb;
    uint8_t* bb;
    if (total == 0) return 0;
    if (!elf_is_psram(dst) || !(bb = elf_io_bounce())) {
        SPI_LOCK();
        size_t r = fread(dst, size, nmemb, f);
        SPI_UNLOCK();
        return r;
    }
    if (++g_bounce_reads == 1)
        esp_rom_printf("[elf_host] elf_fread: bouncing PSRAM reads (first dst=0x%x total=%u)\n",
                       (unsigned)(uint32_t)dst, (unsigned)total);
    uint8_t* d = (uint8_t*)dst;
    size_t done = 0;
    while (done < total) {
        size_t chunk = (total - done < ELF_IO_BOUNCE_SZ) ? (total - done) : ELF_IO_BOUNCE_SZ;
        SPI_LOCK();
        size_t r = fread(bb, 1, chunk, f);         // SD DMA lands in internal RAM
        SPI_UNLOCK();
        if (r) { memcpy(d + done, bb, r); done += r; }
        if (r < chunk) break;                      // short read / EOF
    }
    return (size ? done / size : 0);
}

size_t elf_fwrite(const void* src, size_t size, size_t nmemb, FILE* f) {
    size_t total = size * nmemb;
    uint8_t* bb;
    if (total == 0) return 0;
    if (!elf_is_psram(src) || !(bb = elf_io_bounce())) {
        SPI_LOCK();
        size_t r = fwrite(src, size, nmemb, f);
        SPI_UNLOCK();
        return r;
    }
    const uint8_t* s = (const uint8_t*)src;
    size_t done = 0;
    while (done < total) {
        size_t chunk = (total - done < ELF_IO_BOUNCE_SZ) ? (total - done) : ELF_IO_BOUNCE_SZ;
        memcpy(bb, s + done, chunk);
        SPI_LOCK();
        size_t w = fwrite(bb, 1, chunk, f);
        SPI_UNLOCK();
        done += w;
        if (w < chunk) break;
    }
    return (size ? done / size : 0);
}

// ---------------------------------------------------------------------------
// Module PSRAM allocation tracker.
// The loaded module allocates PSRAM through our psram_malloc/calloc/realloc
// wrappers.  When the module exits — often via exit() → longjmp, skipping
// normal cleanup — those allocations leak.  We track every psram_*
// allocation and bulk-free survivors after module exit.
// ---------------------------------------------------------------------------
// Allocation tracker — an open-addressing hash set in PSRAM. Lua-based
// modules (PICO-8) route their entire GC heap through these wrappers:
// tens of thousands of live allocations and thousands of track/untrack
// calls per frame, so both operations must be O(1). The previous linear
// array degraded to a full-table scan per free() once it filled, and
// printed a warning per allocation — both visibly dragged the frame rate.
#define MOD_ALLOC_HASH_SIZE 65536              // power of 2, 256KB PSRAM
#define MOD_ALLOC_MAX_LIVE  (MOD_ALLOC_HASH_SIZE / 2)
#define MOD_ALLOC_TOMB      ((void*)1)

static void**   s_mod_allocs = nullptr;        // hash table, lazily allocated
static int      s_mod_alloc_count = 0;         // live entries
static uint32_t s_mod_alloc_occupied = 0;      // live + tombstones
static bool     s_mod_tracking = false;
static bool     s_mod_track_warned = false;

static inline uint32_t mod_hash(void* p) {
    uint32_t x = (uint32_t)p;
    x ^= x >> 16; x *= 0x7feb352d;
    x ^= x >> 15; x *= 0x846ca68b;
    x ^= x >> 16;
    return x & (MOD_ALLOC_HASH_SIZE - 1);
}

static void mod_track_insert(void* ptr) {
    uint32_t i = mod_hash(ptr);
    uint32_t first_tomb = UINT32_MAX;
    for (;;) {
        void* v = s_mod_allocs[i];
        if (v == NULL) {
            if (first_tomb != UINT32_MAX) {
                s_mod_allocs[first_tomb] = ptr;   // reuse tombstone
            } else {
                s_mod_allocs[i] = ptr;
                s_mod_alloc_occupied++;
            }
            s_mod_alloc_count++;
            return;
        }
        if (v == MOD_ALLOC_TOMB && first_tomb == UINT32_MAX) first_tomb = i;
        if (v == ptr) return;                     // already tracked
        i = (i + 1) & (MOD_ALLOC_HASH_SIZE - 1);
    }
}

// Tombstones eventually exhaust the NULL slots probes terminate on;
// rebuild when 3/4 of the table is live+tomb (live alone is capped at 1/2).
static void mod_track_rebuild(void) {
    if (s_mod_alloc_count == 0) {   // all tombstones — just wipe
        memset(s_mod_allocs, 0, MOD_ALLOC_HASH_SIZE * sizeof(void*));
        s_mod_alloc_occupied = 0;
        return;
    }
    void** live = (void**)heap_caps_malloc(
        (size_t)s_mod_alloc_count * sizeof(void*), MALLOC_CAP_SPIRAM);
    if (!live) {            // can't rebuild — stop tracking rather than risk
        s_mod_tracking = false;  // unterminated probe loops
        printf("[elf_host] WARNING: alloc tracker rebuild failed, tracking disabled\n");
        return;
    }
    int n = 0;
    for (uint32_t i = 0; i < MOD_ALLOC_HASH_SIZE; i++) {
        void* v = s_mod_allocs[i];
        if (v && v != MOD_ALLOC_TOMB) live[n++] = v;
    }
    memset(s_mod_allocs, 0, MOD_ALLOC_HASH_SIZE * sizeof(void*));
    s_mod_alloc_count = 0;
    s_mod_alloc_occupied = 0;
    for (int i = 0; i < n; i++) mod_track_insert(live[i]);
    heap_caps_free(live);
}

static void mod_track(void* ptr) {
    if (!s_mod_tracking || !ptr) return;
    if (!s_mod_allocs) {
        s_mod_allocs = (void**)heap_caps_calloc(
            MOD_ALLOC_HASH_SIZE, sizeof(void*), MALLOC_CAP_SPIRAM);
        if (!s_mod_allocs) {
            printf("[elf_host] WARNING: can't allocate alloc tracker\n");
            return;
        }
    }
    if (s_mod_alloc_count >= MOD_ALLOC_MAX_LIVE) {
        if (!s_mod_track_warned) {
            s_mod_track_warned = true;
            printf("[elf_host] WARNING: module alloc tracker full (%d live) — "
                   "further allocations leak if the module aborts\n", s_mod_alloc_count);
        }
        return;
    }
    if (s_mod_alloc_occupied > (MOD_ALLOC_HASH_SIZE / 4) * 3) {
        mod_track_rebuild();
        if (!s_mod_tracking) return;
    }
    mod_track_insert(ptr);
}

static void mod_untrack(void* ptr) {
    if (!ptr || !s_mod_allocs) return;
    uint32_t i = mod_hash(ptr);
    for (;;) {
        void* v = s_mod_allocs[i];
        if (v == NULL) return;                    // not tracked
        if (v == ptr) {
            s_mod_allocs[i] = MOD_ALLOC_TOMB;
            s_mod_alloc_count--;
            return;
        }
        i = (i + 1) & (MOD_ALLOC_HASH_SIZE - 1);
    }
}

static void mod_free_tracked() {
    int n = s_mod_alloc_count;
    if (s_mod_allocs) {
        for (uint32_t i = 0; i < MOD_ALLOC_HASH_SIZE; i++) {
            void* v = s_mod_allocs[i];
            if (v && v != MOD_ALLOC_TOMB)
                heap_caps_free(v);
        }
        heap_caps_free(s_mod_allocs);
        s_mod_allocs = nullptr;
    }
    s_mod_alloc_count = 0;
    s_mod_alloc_occupied = 0;
    s_mod_tracking = false;
    s_mod_track_warned = false;
    if (n > 0)
        printf("[elf_host] freed %d leaked module allocations\n", n);
}

// PSRAM-only memory allocation for loaded modules.
// All module memory goes to PSRAM — never internal RAM.
// Allocations are tracked so they can be freed on module exit.
void* psram_malloc(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    mod_track(p);
    return p;
}

void* psram_calloc(size_t nmemb, size_t size) {
    void* p = heap_caps_calloc(nmemb, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    mod_track(p);
    return p;
}

void* psram_realloc(void* ptr, size_t size) {
    mod_untrack(ptr);
    void* p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) mod_track(p);
    // Failed grow: the old block is still live and the module keeps using it —
    // re-track it or it escapes the leak sweep if the module exits via longjmp.
    // (size == 0 means realloc freed the block; it must stay untracked.)
    else if (ptr && size) mod_track(ptr);
    return p;
}

void psram_free(void* ptr) {
    mod_untrack(ptr);
    free(ptr);
}

// Query how much PSRAM is available for the module to allocate.
uint32_t host_psram_largest_free(void) {
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

} // extern "C"

// ---------------------------------------------------------------------------
// Stubs for functions that don't make sense or are unsafe on ESP32
// ---------------------------------------------------------------------------
static FILE* stub_tmpfile(void) { return NULL; }
static char* stub_tmpnam(char* s) { (void)s; return NULL; }
static FILE* stub_freopen(const char* p, const char* m, FILE* f) {
    (void)p; (void)m; (void)f; return NULL;
}
static char* stub_getenv(const char* n) { (void)n; return NULL; }
static int   stub_atexit(void(*f)(void)) { (void)f; return 0; }

// ---------------------------------------------------------------------------
// Symbol export table — these are the functions the loaded ELF can call
// ---------------------------------------------------------------------------

static const elf_symbol_t host_exports[] = {
    { "host_blit_frame",    (void*)host_blit_frame },
    { "host_blit_frame_async", (void*)host_blit_frame_async },
    { "host_clear_screen",  (void*)host_clear_screen },
    { "host_get_ticks_ms",  (void*)host_get_ticks_ms },
    { "host_get_ticks_us",  (void*)host_get_ticks_us },
    { "host_sleep_ms",      (void*)host_sleep_ms },
    { "host_get_key",       (void*)host_get_key },
    { "host_trackball_read", (void*)host_trackball_read },
    { "host_audio_push",    (void*)host_audio_push },
    { "host_audio_set_pull", (void*)host_audio_set_pull },
    { "host_should_exit",   (void*)host_should_exit },
    { "host_read_file",     (void*)host_read_file },
    { "host_write_file",    (void*)host_write_file },
    { "host_log",           (void*)host_log },
    { "host_check_heap",    (void*)host_check_heap },

    // C library: stdio
    { "printf",             (void*)printf },
    { "fprintf",            (void*)fprintf },
    { "sprintf",            (void*)sprintf },
    { "snprintf",           (void*)snprintf },
    { "vsnprintf",          (void*)vsnprintf },
    { "vfprintf",           (void*)vfprintf },
    { "puts",               (void*)puts },
    { "putchar",            (void*)putchar },
    { "fputc",              (void*)fputc },
    { "fputs",              (void*)fputs },
    { "fopen",              (void*)elf_fopen },   // SPI-locked
    { "fclose",             (void*)elf_fclose },  // SPI-locked
    { "fread",              (void*)elf_fread },   // SPI-locked + PSRAM bounce
    { "fwrite",             (void*)elf_fwrite },  // SPI-locked + PSRAM bounce
    { "fseek",              (void*)elf_fseek },   // SPI-locked
    { "ftell",              (void*)elf_ftell },   // SPI-locked
    { "fflush",             (void*)fflush },
    { "sscanf",             (void*)sscanf },
    { "fscanf",             (void*)fscanf },
    { "fgets",              (void*)fgets },
    { "getc",               (void*)getc },
    { "ungetc",             (void*)ungetc },
    { "feof",               (void*)feof },
    { "ferror",             (void*)ferror },
    { "clearerr",           (void*)clearerr },
    { "setvbuf",            (void*)setvbuf },
    { "remove",             (void*)remove },
    { "rename",             (void*)rename },
    { "tmpfile",            (void*)stub_tmpfile },
    { "tmpnam",             (void*)stub_tmpnam },
    { "freopen",            (void*)stub_freopen },

    // C library: string
    { "memcpy",             (void*)memcpy },
    { "memset",             (void*)memset },
    { "memmove",            (void*)memmove },
    { "memcmp",             (void*)memcmp },
    { "strcmp",              (void*)strcmp },
    { "strncmp",            (void*)strncmp },
    { "strcasecmp",         (void*)strcasecmp },
    { "strncasecmp",        (void*)strncasecmp },
    { "strlen",             (void*)strlen },
    { "strcpy",             (void*)strcpy },
    { "strncpy",            (void*)strncpy },
    { "strcat",             (void*)strcat },
    { "strchr",             (void*)strchr },
    { "strrchr",            (void*)strrchr },
    { "strstr",             (void*)strstr },
    { "strdup",             (void*)strdup },
    { "strtol",             (void*)strtol },
    { "strtoul",            (void*)strtoul },
    { "strtod",             (void*)strtod },
    { "atoi",               (void*)atoi },
    { "atof",               (void*)atof },
    { "memchr",             (void*)memchr },
    { "strpbrk",            (void*)strpbrk },
    { "strspn",             (void*)strspn },
    { "strcspn",            (void*)strcspn },
    { "strcoll",            (void*)strcoll },
    { "strerror",           (void*)strerror },
    { "strtok",             (void*)strtok },

    // C library: memory (PSRAM-aware wrappers)
    { "malloc",             (void*)psram_malloc },
    { "free",               (void*)psram_free },
    { "calloc",             (void*)psram_calloc },
    { "realloc",            (void*)psram_realloc },
    { "host_psram_largest_free", (void*)host_psram_largest_free },

    // C library: math/utility
    { "abs",                (void*)(int(*)(int))abs },
    { "qsort",              (void*)qsort },
    { "rand",               (void*)rand },
    { "srand",              (void*)srand },

    // C library: math (float + double)
    { "sinf",               (void*)sinf },
    { "cosf",               (void*)cosf },
    { "tanf",               (void*)tanf },
    { "asinf",              (void*)asinf },
    { "acosf",              (void*)acosf },
    { "atan2f",             (void*)atan2f },
    { "powf",               (void*)powf },
    { "sqrtf",              (void*)sqrtf },
    { "fmodf",              (void*)fmodf },
    { "fabsf",              (void*)fabsf },
    { "floorf",             (void*)floorf },
    { "ceilf",              (void*)ceilf },
    { "roundf",             (void*)roundf },
    { "logf",               (void*)logf },
    { "log2f",              (void*)log2f },
    { "log10f",             (void*)log10f },
    { "expf",               (void*)expf },
    { "exp2f",              (void*)exp2f },
    { "sin",                (void*)(double(*)(double))sin },
    { "cos",                (void*)(double(*)(double))cos },
    { "tan",                (void*)(double(*)(double))tan },
    { "asin",               (void*)(double(*)(double))asin },
    { "acos",               (void*)(double(*)(double))acos },
    { "atan2",              (void*)(double(*)(double,double))atan2 },
    { "pow",                (void*)(double(*)(double,double))pow },
    { "sqrt",               (void*)(double(*)(double))sqrt },
    { "fmod",               (void*)(double(*)(double,double))fmod },
    { "fabs",               (void*)(double(*)(double))fabs },
    { "floor",              (void*)(double(*)(double))floor },
    { "ceil",               (void*)(double(*)(double))ceil },
    { "round",              (void*)(double(*)(double))round },
    { "log",                (void*)(double(*)(double))log },
    { "log2",               (void*)(double(*)(double))log2 },
    { "log10",              (void*)(double(*)(double))log10 },
    { "exp",                (void*)(double(*)(double))exp },
    { "ldexp",              (void*)(double(*)(double,int))ldexp },
    { "ldexpf",             (void*)ldexpf },
    { "frexp",              (void*)(double(*)(double,int*))frexp },
    { "frexpf",             (void*)frexpf },
    { "modf",               (void*)(double(*)(double,double*))modf },

    // C library: ctype
    { "_ctype_",            (void*)_ctype_ },
    { "toupper",            (void*)toupper },
    { "tolower",            (void*)tolower },
    { "isalpha",            (void*)isalpha },
    { "isdigit",            (void*)isdigit },
    { "isalnum",            (void*)isalnum },
    { "isspace",            (void*)isspace },
    { "isupper",            (void*)isupper },
    { "islower",            (void*)islower },
    { "ispunct",            (void*)ispunct },
    { "isxdigit",           (void*)isxdigit },
    { "isgraph",            (void*)isgraph },
    { "iscntrl",            (void*)iscntrl },
    { "isprint",            (void*)isprint },

    // C library: POSIX
    { "mkdir",              (void*)mkdir },
    { "system",             (void*)system },

    // C library: newlib internals
    { "__errno",            (void*)__errno },
    { "__getreent",         (void*)__getreent },

    // GCC soft-float / 64-bit math builtins
    { "__divdi3",           (void*)__divdi3 },
    { "__divsf3",           (void*)__divsf3 },
    { "__extendsfdf2",      (void*)__extendsfdf2 },
    { "__ltdf2",            (void*)__ltdf2 },
    { "__truncdfsf2",       (void*)__truncdfsf2 },
    // Double-precision builtins
    { "__adddf3",           (void*)__adddf3 },
    { "__subdf3",           (void*)__subdf3 },
    { "__muldf3",           (void*)__muldf3 },
    { "__divdf3",           (void*)__divdf3 },
    { "__gtdf2",            (void*)__gtdf2 },
    { "__gedf2",            (void*)__gedf2 },
    { "__ledf2",            (void*)__ledf2 },
    { "__nedf2",            (void*)__nedf2 },
    { "__eqdf2",            (void*)__eqdf2 },
    { "__floatsidf",        (void*)__floatsidf },
    { "__floatunsidf",      (void*)__floatunsidf },
    { "__fixdfsi",          (void*)__fixdfsi },
    { "__fixunsdfsi",       (void*)__fixunsdfsi },
    { "__fixdfdi",          (void*)__fixdfdi },
    { "__floatdidf",        (void*)__floatdidf },
    { "__udivdi3",          (void*)__udivdi3 },
    { "__umoddi3",          (void*)__umoddi3 },
    { "__moddi3",           (void*)__moddi3 },

    // C library: time
    { "time",               (void*)time },
    { "clock",              (void*)clock },
    { "mktime",             (void*)mktime },
    { "gmtime",             (void*)gmtime },
    { "localtime",          (void*)localtime },
    { "difftime",           (void*)difftime },
    { "strftime",           (void*)strftime },

    // C library: misc stubs
    { "getenv",             (void*)stub_getenv },
    { "atexit",             (void*)stub_atexit },
    { "setlocale",          (void*)setlocale },
    { "localeconv",         (void*)localeconv },

    // C library: setjmp (for exit() trap)
    { "setjmp",             (void*)setjmp },
    { "longjmp",            (void*)longjmp },

    ELF_SYMBOL_END
};

// ---------------------------------------------------------------------------
// Dedicated execution task for loaded modules.
//
// Heavy native modules use far more stack than the 16KB Arduino
// loopTask provides — and _launch_elf is itself invoked deep inside the Lua
// VM's C call chain, so even less is actually free. Overflowing the loopTask
// stack corrupts it; the CPU then faults and the panic handler cannot unwind
// the trashed stack, so instead of a Guru Meditation backtrace we get a
// silent TG1 (interrupt) watchdog reset. Running the module on its own large,
// freshly-allocated stack removes that failure mode entirely.
// ---------------------------------------------------------------------------

// Module task stack. Internal RAM, not PSRAM: a task stack must stay
// accessible while the cache is disabled (e.g. during flash writes) and the
// module does file I/O. We try 64KB first for maximum headroom, falling back
// to 48KB or 32KB if not enough contiguous internal RAM is available.
// (ESP-IDF stack sizes are in bytes — StackType_t is 1 byte.)
static const uint32_t ELF_TASK_STACK_CANDIDATES[] = { 64*1024, 48*1024, 32*1024 };

struct elf_run_ctx {
    elf_module_t*     mod;
    int               argc;
    char**            argv;
    int               result;
    SemaphoreHandle_t done;   // given by the module task when it returns
};

// ---------------------------------------------------------------------------
// CPU-exception interception for loaded modules.
//
// The default ESP-IDF dual-core panic handler deadlocks in this context — it
// hangs in panic_handler.c (stalling the other core) and the raw TG1 hardware
// watchdog then resets the chip before any Guru Meditation backtrace prints.
// So a fault inside the module is invisible: all we see is "TG1WDT_SYS_RST".
//
// While the module runs we install our own handler for the common fatal
// exception causes. It prints the exact cause / PC / faulting address via the
// ROM console (the same path the boot log uses, so it reaches the USB console)
// then spins so the watchdog resets cleanly — at least now WITH the diagnostic
// info on the wire. This pinpoints a bad relocation or wild pointer.
//
// XtExcFrame, xt_exc_handler and xt_set_exception_handler come from
// <xtensa/xtensa_api.h>, already pulled in transitively via FreeRTOS.h.
// ---------------------------------------------------------------------------

// Xtensa EXCCAUSE values a misbehaving native module is likely to trigger.
// (Excludes 1=Syscall and 4=Level1Interrupt — those are NORMAL operations and
// hooking them would break the system.) Includes the PIF/cache-bus error causes
// 12-15, which PSRAM access can raise and which we were previously missing.
static const int kElfFaultCauses[] = {
    0,   // IllegalInstruction
    2,   // InstructionFetchError
    3,   // LoadStoreError
    6,   // IntegerDivideByZero
    8,   // Privileged
    9,   // LoadStoreAlignment
    12,  // InstrPIFDataError
    13,  // LoadStorePIFDataError
    14,  // InstrPIFAddrError
    15,  // LoadStorePIFAddrError
    20,  // InstrFetchProhibited
    26,  // LoadStorePrivilege
    28,  // LoadProhibited
    29,  // StoreProhibited
};
#define ELF_FAULT_NCAUSES (sizeof(kElfFaultCauses) / sizeof(kElfFaultCauses[0]))

static void IRAM_ATTR elf_exception_handler(XtExcFrame* f) {
    esp_rom_printf("\n\n*** [ELF FAULT] core=%d exccause=%d PC=0x%x excvaddr=0x%x PS=0x%x\n",
                   (int)xPortGetCoreID(), (int)f->exccause, (unsigned)f->pc,
                   (unsigned)f->excvaddr, (unsigned)f->ps);
    esp_rom_printf("*** a0 =0x%x a1 =0x%x a2 =0x%x a3 =0x%x\n",
                   (unsigned)f->a0, (unsigned)f->a1, (unsigned)f->a2, (unsigned)f->a3);
    esp_rom_printf("*** a4 =0x%x a5 =0x%x a6 =0x%x a7 =0x%x\n",
                   (unsigned)f->a4, (unsigned)f->a5, (unsigned)f->a6, (unsigned)f->a7);
    esp_rom_printf("*** a8 =0x%x a9 =0x%x a10=0x%x a11=0x%x\n",
                   (unsigned)f->a8, (unsigned)f->a9, (unsigned)f->a10, (unsigned)f->a11);
    esp_rom_printf("*** a12=0x%x a13=0x%x a14=0x%x a15=0x%x\n",
                   (unsigned)f->a12, (unsigned)f->a13, (unsigned)f->a14, (unsigned)f->a15);
    // Raw stack words near SP — code-looking values (0x4000../0x4037../0x42....)
    // can be addr2line'd to reconstruct the interrupt call chain.
    const uint32_t* sp = (const uint32_t*)f->a1;
    for (int i = 0; i < 32; i += 4) {
        esp_rom_printf("*** sp+%02d: 0x%x 0x%x 0x%x 0x%x\n", i * 4,
                       (unsigned)sp[i], (unsigned)sp[i+1], (unsigned)sp[i+2], (unsigned)sp[i+3]);
    }
    esp_rom_printf("*** spinning; interrupt watchdog will reset shortly\n");
    // MUST NOT return: a returning handler retries the faulting instruction,
    // which would just re-fault forever. Spin until the watchdog resets.
    while (1) { }
}

// Exception handlers are PER-CORE. The module runs on Core 0, but DMA/interrupt
// faults can land on Core 1 (radio SPI, etc.) — those escape a Core-0-only
// handler and hit the deadlocking default panic handler (silent TG1WDT). So we
// install on BOTH cores. Saved originals are restored on clean module exit.
static xt_exc_handler s_saved_exc[2][ELF_FAULT_NCAUSES];

static void elf_install_handlers(int core) {
    for (size_t i = 0; i < ELF_FAULT_NCAUSES; i++)
        s_saved_exc[core][i] = xt_set_exception_handler(kElfFaultCauses[i], elf_exception_handler);
}
static void elf_restore_handlers(int core) {
    for (size_t i = 0; i < ELF_FAULT_NCAUSES; i++)
        xt_set_exception_handler(kElfFaultCauses[i], s_saved_exc[core][i]);
}

// One-shot helper task to (un)install handlers on Core 1 (handlers install on
// the core that calls xt_set_exception_handler).
struct elf_c1_op { bool install; SemaphoreHandle_t done; };
static void elf_c1_op_task(void* arg) {
    elf_c1_op* op = (elf_c1_op*)arg;
    if (op->install) elf_install_handlers(1); else elf_restore_handlers(1);
    xSemaphoreGive(op->done);
    vTaskDelete(NULL);
}
static void elf_set_core1_handlers(bool install) {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) return;
    elf_c1_op op = { install, done };
    TaskHandle_t t = nullptr;
    bool ok = (xTaskCreatePinnedToCore(elf_c1_op_task, "elf_c1op", 2560, &op, 10, &t, 1) == pdPASS);
    if (ok) xSemaphoreTake(done, portMAX_DELAY);
    SLog.printf("[elf_host] Core1 fault handlers %s (%s)\n",
                  install ? "installed" : "restored", ok ? "ok" : "TASK-CREATE-FAILED");
    vSemaphoreDelete(done);
}

static void elf_run_task(void* param) {
    elf_run_ctx* ctx = (elf_run_ctx*)param;

    // Report current SP so a fault address can be compared against the stack range.
    uint32_t sp_top = (uint32_t)__builtin_frame_address(0);
    SLog.printf("[elf_host] elf_run task started, SP~0x%x\n", sp_top);

    // Intercept module faults on Core 0 (Core 1 is set up separately by caller).
    elf_install_handlers(0);

    ctx->result = elf_run(ctx->mod, ctx->argc, ctx->argv);

    // Clean return — restore the normal handlers.
    elf_restore_handlers(0);

    // High-water mark tells us how close we came to overflowing (for tuning).
    SLog.printf("[elf_host] module task finished (result=%d), min stack free: %u bytes\n",
                  ctx->result, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// _launch_elf(path [, arg1, arg2, ...])  — DEFERRED launch.
//
// The binding is called from a Lua game, so we cannot tear Lua down here (Lua is
// on the C stack). It deep-copies the args — the Lua strings will NOT survive the
// lua_close — sets s_elf_pending, and returns. The main loop (Core 0) then sees
// the flag, calls luaTearDown() (freeing the whole fragmented Lua heap so the
// module gets a clean contiguous PSRAM block), runs the module via
// elf_host_run_pending(), and recreates Lua + the launcher. Because Lua is gone
// before the module runs, _launch_elf returns no meaningful result — the user
// lands on the launcher home when the module exits.
// ---------------------------------------------------------------------------
#define ELF_MAX_ARGS 16
#define ELF_ARG_MAX  256
static volatile bool s_elf_pending = false;
static volatile bool s_elf_running = false;
static int  s_elf_argc = 0;
static char s_elf_args[ELF_MAX_ARGS][ELF_ARG_MAX];

static int lua_launch_elf(lua_State* L) {
    if (s_elf_pending || s_elf_running) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "launch already in progress");
        return 2;
    }
    int nargs = lua_gettop(L);
    if (nargs < 1) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "no ELF path");
        return 2;
    }
    if (nargs > ELF_MAX_ARGS) nargs = ELF_MAX_ARGS;
    s_elf_argc = nargs;
    for (int i = 0; i < nargs; i++) {
        const char* a = lua_tostring(L, i + 1);
        strlcpy(s_elf_args[i], a ? a : "", ELF_ARG_MAX);
    }
    s_elf_pending = true;
    lua_pushboolean(L, 1);   // queued; the real load happens after teardown
    lua_pushinteger(L, 0);
    return 2;
}

// Called from loop() (Core 0): true if a launch was requested; marks it running
// so a second tap is ignored until the module exits.
bool elf_host_pending_take(void) {
    if (!s_elf_pending) return false;
    s_elf_pending = false;
    s_elf_running = true;
    return true;
}

// Run the stashed module to completion. MUST be called only after luaTearDown()
// — Lua is down and this never touches lua_State. The caller recreates Lua after
// it returns. Returns the module result (negative on load/spawn failure).
int elf_host_run_pending(void) {
    const char* path = s_elf_args[0];
    int argc = s_elf_argc;
    char* argv[ELF_MAX_ARGS + 1];
    for (int i = 0; i < argc; i++) argv[i] = s_elf_args[i];
    argv[argc] = NULL;

    SLog.printf("[elf_host] loading %s\n", path);
    SLog.printf("[elf_host] PSRAM free: %u, largest block: %u\n",
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    // Read ELF file from SD
    uint32_t elf_size = 0;
    void* elf_data = host_read_file(path, &elf_size);
    if (!elf_data) {
        SLog.printf("[elf_host] failed to read ELF file: %s\n", path);
        s_elf_running = false;
        return -3;
    }
    SLog.printf("[elf_host] read %u bytes\n", elf_size);

    // Suspend LVGL, mesh task, BLE companion, and watchdog for module execution.
    lvgl_ticker.detach();
    // Widen the task watchdog timeout so long module init doesn't trip it.
    // Also set panic=false so even if it triggers, it just warns instead of rebooting.
    esp_task_wdt_init(120, false);
    // Disable the interrupt watchdog (TG1 MWDT). The first 3D frame from PSRAM
    // with cold caches can exceed the default 300ms timeout.
    {
        uint32_t before = REG_READ(TIMG_WDTCONFIG0_REG(1));
        REG_WRITE(TIMG_WDTWPROTECT_REG(1), 0x50D83AA1);   // unlock write-protect
        REG_WRITE(TIMG_WDTCONFIG0_REG(1), before & ~(1U << 31));  // clear EN bit 31
        uint32_t after = REG_READ(TIMG_WDTCONFIG0_REG(1));
        REG_WRITE(TIMG_WDTWPROTECT_REG(1), 0);            // re-lock
        SLog.printf("[elf_host] TG1 WDT: before=0x%08x after=0x%08x\n", before, after);
    }
    // Sound stays active — module audio is routed through the firmware's mixer
    // via host_audio_push() → sound_extern_push().
    elf_set_core1_handlers(true); // also catch module faults that land on Core 1
    host_clear_screen();

    // Reset input state and load keymap
    memset(prev_key_state, 0, INPUT_STATE_SIZE);
    kq_head = kq_tail = 0;
    esc_held = false;
    s_trk_raw_mode = false; // modules opt in via host_trackball_read()

    // Check for -keymap argument; default is passthrough (raw key codes).
    // Modules that need translated keycodes (e.g. Doom) pass their own
    // keymap string from their Lua launcher.
    const char* keymap_str = NULL;
    const char* trkball_str = NULL;
    for (int i = 0; i < argc - 1; i++) {
        if (strcmp(argv[i], "-keymap") == 0) {
            keymap_str = argv[i + 1];
        } else if (strcmp(argv[i], "-trkball") == 0) {
            trkball_str = argv[i + 1];
        }
    }
    if (keymap_str && strcmp(keymap_str, "passthrough") == 0) {
        keymap_passthrough = true;
        memset(keymap_table, 0, sizeof(keymap_table));
        SLog.println("[elf_host] keymap: passthrough (raw key codes)");
    } else if (keymap_str) {
        keymap_passthrough = false;
        parse_keymap_arg(keymap_str);
        SLog.printf("[elf_host] keymap: custom (%d chars)\n", (int)strlen(keymap_str));
    } else {
        // No -keymap argument: default to passthrough
        keymap_passthrough = true;
        memset(keymap_table, 0, sizeof(keymap_table));
        SLog.println("[elf_host] keymap: passthrough (default)");
    }

    // Parse trackball momentum settings: "enabled,impulse*10,friction*100,threshold*10"
    // e.g. "1,15,82,4" → enabled=true, impulse=1.5, friction=0.82, threshold=0.4
    trk_vel_x = trk_vel_y = 0;
    if (trkball_str) {
        int en = 1, imp = 15, fri = 82, thr = 4;
        sscanf(trkball_str, "%d,%d,%d,%d", &en, &imp, &fri, &thr);
        trk_momentum = (en != 0);
        trk_impulse  = imp / 10.0f;
        trk_friction = fri / 100.0f;
        trk_thresh   = thr / 10.0f;
        SLog.printf("[elf_host] trackball: momentum=%d impulse=%.1f friction=%.2f thresh=%.1f\n",
                      trk_momentum, trk_impulse, trk_friction, trk_thresh);
    } else {
        // Defaults
        trk_momentum = true;
        trk_impulse  = 1.5f;
        trk_friction = 0.82f;
        trk_thresh   = 0.4f;
    }

    // Start tracking module PSRAM allocations so we can free them on exit
    s_mod_alloc_count = 0;
    s_mod_tracking = true;

    // Load and relocate
    elf_module_t* mod = elf_load(elf_data, elf_size, host_exports);
    heap_caps_free(elf_data); // raw ELF data no longer needed
    elf_data = NULL;

    // Dump mesh object pointer region to detect corruption
    extern PunkMesh* the_mesh;
    SLog.printf("[elf_host] the_mesh=%p, first 16 bytes:", the_mesh);
    if (the_mesh) {
        uint8_t* p = (uint8_t*)the_mesh;
        for (int i = 0; i < 16; i++) SLog.printf(" %02x", p[i]);
    }
    SLog.println();

    int result = -1;
    if (mod) {
        SLog.printf("[elf_host] heap OK before run: %s\n",
                      heap_caps_check_integrity(MALLOC_CAP_SPIRAM, false) ? "yes" : "NO!");
        SLog.printf("[elf_host] the_mesh at: 0x%08x\n", (uint32_t)the_mesh);

        // Start the Core 1 keyboard sampler so input stays responsive even
        // while the module pins Core 0 (see elf_input_task_body). Stopped
        // below before this call returns and loopTask resumes its own scan.
        elf_input_start();

        // Run the module on a dedicated large-stack task pinned to Core 0
        // (the UI core; LVGL is idle and Lua is torn down, so Core 0 is
        // free). The mesh task keeps running on Core 1 the whole time —
        // messages still arrive, persist, and raise the C-side notification
        // alert (notify.cpp). This loopTask blocks until the module returns —
        // IDLE0 still runs between the module's per-frame yields, feeding the
        // watchdog.
        // Try progressively smaller stacks until one fits in available RAM.
        SemaphoreHandle_t done = xSemaphoreCreateBinary();
        elf_run_ctx ctx = { mod, argc, argv, -1, done };
        TaskHandle_t task = nullptr;
        BaseType_t ok = pdFAIL;
        uint32_t stack_used = 0;
        if (done) {
            for (uint32_t candidate : ELF_TASK_STACK_CANDIDATES) {
                ok = xTaskCreatePinnedToCore(
                    elf_run_task, "elf_run", candidate,
                    &ctx, 1 /* priority == loopTask */, &task, 0 /* Core 0 */);
                if (ok == pdPASS) {
                    stack_used = candidate;
                    SLog.printf("[elf_host] task created with %uKB stack\n", candidate / 1024);
                    break;
                }
                SLog.printf("[elf_host] %uKB stack failed, trying smaller...\n", candidate / 1024);
            }
        }
        if (ok == pdPASS) {
            xSemaphoreTake(done, portMAX_DELAY); // wait for module to return
            result = ctx.result;
            SLog.printf("[elf_host] module returned %d\n", result);
        } else {
            result = -2; // distinct from -1 (module ran but exit() called)
            SLog.printf("[elf_host] FAILED to create module task — largest internal block: %u bytes\n",
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
        if (done) vSemaphoreDelete(done);
        // Stop the Core 1 keyboard sampler and wait for it to exit before any
        // further cleanup, so its I2C reads can't overlap loopTask's once the
        // firmware's own keyboard scan resumes.
        elf_input_stop();
        // The pull callback (and the Audio state it reads) lives in module
        // memory; unregister before any of it is freed. Blocks until the
        // mixer is outside the callback. The module normally does this in
        // its own cleanup, but the exit()-longjmp path skips that.
        sound_extern_set_pull(NULL, 0);
        blit_drain();  // an async push may still be reading module BSS
        elf_unload(mod);
    } else {
        SLog.println("[elf_host] elf_load failed");
    }

    // Close any file descriptors the module left open
    mod_close_tracked_files();

    // Free any PSRAM the module leaked on exit()
    mod_free_tracked();

    // Free the DMA bounce buffer used during this session
    if (s_io_bounce) {
        heap_caps_free(s_io_bounce);
        s_io_bounce = nullptr;
    }

    // Reset audio rate tracking so the next module's first push sets it fresh
    s_audio_last_rate = 0;

    SLog.printf("[elf_host] PSRAM after cleanup: %u free, largest block: %u\n",
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    // Resume LVGL and watchdog
    host_clear_screen();
    wake_activity();             // reset inactivity timer + restore backlights
    elf_set_core1_handlers(false); // restore Core 1 exception handlers
    sound_extern_flush();        // drain any leftover module audio
    // Re-enable interrupt watchdog
    REG_WRITE(TIMG_WDTWPROTECT_REG(1), 0x50D83AA1);
    REG_SET_BIT(TIMG_WDTCONFIG0_REG(1), (1U << 31));
    REG_WRITE(TIMG_WDTWPROTECT_REG(1), 0);
    esp_task_wdt_init(5, true);  // restore normal watchdog (5s timeout, panic on trigger)
    lvgl_ticker.attach_ms(5, []() { lv_tick_inc(5); });
    // Force full redraw — LVGL doesn't know the display was wiped
    lv_obj_invalidate(lv_screen_active());

    SLog.printf("[elf_host] module session done (loaded=%d result=%d)\n",
                mod != NULL, result);
    s_elf_running = false;
    return result;
}

void elf_host_register_lua(lua_State* L) {
    lua_register(L, "_launch_elf", lua_launch_elf);
}
