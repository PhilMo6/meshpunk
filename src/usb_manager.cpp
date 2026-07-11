// USB-OTG host manager — permanent Meshpunk USB subsystem.
//
// Host stack lifecycle + device enumeration/classification, plus the UAC 1.0
// audio-out driver that streams the device mixer to a USB DAC dongle. Grew
// out of the Phase-B enumeration/tone prototype; all the hardware-validated
// quirks it discovered live on here (see memory: usb-host-experiments):
//   - install the host stack from a task pinned to CORE 1 (esp_intr_alloc is
//     calling-core-local and core 0's slots are full), intr_flags = 0;
//   - the internal PHY select is an RTC-domain register that SURVIVES warm
//     resets — restore it to Serial-JTAG on teardown and on boot, or USB
//     serial stays dead (Windows Code 43) until a full power cycle;
//   - patch the CACHED config descriptor before claiming an ISO endpoint:
//     clamp wMaxPacketSize > 192 (DWC balanced-FIFO periodic-OUT limit in the
//     prebuilt IDF 4.4 libs) and bInterval 0 -> 1, else the claim returns
//     ESP_ERR_NOT_SUPPORTED; cap the sample rate at 48 kHz for the same
//     reason;
//   - EP0 control-transfer completions route through the LIBRARY event
//     handler first, so every wait loop must pump BOTH lib and client
//     handlers or SET_INTERFACE/SET_CUR silently time out.
//
// Everything (install, enumeration, descriptor parsing, ISO refill) runs in
// the single usb_task on core 1. Started manually from Tools/USB; host mode
// takes the USB pins from Serial-JTAG until Stop.

#include "usb_manager.h"

#include <Arduino.h>
#include <math.h>
#include <usb/usb_host.h>
#include <esp_heap_caps.h>
#include <soc/rtc_cntl_struct.h>
#include <soc/usb_serial_jtag_struct.h>
#include "sound.h"                  // system volume/mute applied to USB output
#include "elf_host.h"               // elf_input_active/inject — keys to modules
#include "usb_fs.h"                 // FatFs mount/unmount for the MSC drive

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

// ── Log line ring (usb task -> Lua poll; serial is dead in host mode) ───────

#define ULOG_LINES    48
#define ULOG_LINE_LEN 96

static char     s_log[ULOG_LINES][ULOG_LINE_LEN];
static uint8_t  s_log_head = 0;
static uint8_t  s_log_tail = 0;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;

static void ulog(const char* fmt, ...) {
    char line[ULOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    portENTER_CRITICAL(&s_log_mux);
    memcpy(s_log[s_log_head], line, ULOG_LINE_LEN);
    s_log_head = (s_log_head + 1) % ULOG_LINES;
    if (s_log_head == s_log_tail) s_log_tail = (s_log_tail + 1) % ULOG_LINES;
    portEXIT_CRITICAL(&s_log_mux);
}

// Public wrapper so sibling modules (usb_fs.cpp) can reach the log ring —
// serial is dead in host mode, this ring is the only visible channel.
void usb_ulog(const char* fmt, ...) {
    char line[ULOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ulog("%s", line);
}

static bool ulog_pop(char* out) {
    bool got = false;
    portENTER_CRITICAL(&s_log_mux);
    if (s_log_tail != s_log_head) {
        memcpy(out, s_log[s_log_tail], ULOG_LINE_LEN);
        s_log_tail = (s_log_tail + 1) % ULOG_LINES;
        got = true;
    }
    portEXIT_CRITICAL(&s_log_mux);
    return got;
}

// ── Host + device state ──────────────────────────────────────────────────────

static volatile bool            s_running    = false;
static volatile bool            s_stop_req   = false;
static usb_host_client_handle_t s_client     = NULL;
static usb_device_handle_t      s_dev        = NULL;
static volatile uint8_t         s_new_addr   = 0;
static volatile bool            s_dev_gone   = false;

static void (*s_prefs_save)()   = nullptr;

enum UsbKind { UKIND_NONE, UKIND_AUDIO, UKIND_HID, UKIND_MSC, UKIND_CDC, UKIND_HUB, UKIND_OTHER };

struct UsbDeviceInfo {
    bool     connected;
    uint16_t vid, pid;
    char     product[32];
    UsbKind  kind;
    bool     has_kbd;   // boot-keyboard interface present (drivable)
    bool     has_msc;   // BOT mass-storage interface present (drivable)
    uint32_t rate;      // AUDIO: chosen sample rate
    uint8_t  bits;      // AUDIO: bit depth
};
static UsbDeviceInfo s_info;   // zero-initialized

static const char* kind_name(UsbKind k) {
    switch (k) {
        case UKIND_AUDIO: return "audio";
        case UKIND_HID:   return "HID";
        case UKIND_MSC:   return "storage";
        case UKIND_CDC:   return "serial";
        case UKIND_HUB:   return "hub";
        case UKIND_OTHER: return "other";
        default:          return "none";
    }
}

// ── UAC streaming profile (filled during enumeration) ───────────────────────

#define MAX_RATES 8

struct AudioProfile {
    bool     valid;
    bool     continuous;          // bSamFreqType==0: rates[0..1] = min..max
    uint8_t  ac_ifnum;
    uint8_t  ifnum, alt;          // AudioStreaming alt setting to select
    uint8_t  ep;                  // its ISO OUT endpoint address
    uint16_t mps;
    uint8_t  binterval;
    const usb_ep_desc_t* ep_desc; // points into the cached config descriptor
    uint8_t  channels, subsize, bits;
    int      nrates;
    uint32_t rates[MAX_RATES];
    uint32_t rate;                // chosen sample rate
};
static AudioProfile s_prof;

// ── HID boot-keyboard profile + state ────────────────────────────────────────

struct HidKbdProfile {
    bool     valid;
    uint8_t  ifnum;
    uint8_t  ep;          // interrupt-IN endpoint address (0x8x)
    uint16_t mps;
    uint8_t  binterval;
};
static HidKbdProfile s_kbd_prof;

static usb_transfer_t* s_hid_xf   = nullptr;
static volatile int    s_hid_busy = 0;      // in-flight interrupt transfers (0/1)
static volatile bool   s_hid_on   = false;  // interface claimed + transfer armed
static uint8_t         s_hid_prev[8];       // last boot report (edge diffs)

// UI handoff (usb_task core 1 → keyboard_read_cb core 0): absolute held-char
// image rebuilt from every boot report — a report lists ALL held keys, so
// stale state is structurally impossible (incl. across ELF game runs, where
// nobody reads it until loopTask resumes).
static bool         s_usb_held[128];
static volatile int s_usb_nheld = 0;
static portMUX_TYPE s_kbd_mux = portMUX_INITIALIZER_UNLOCKED;

// UI arrow-nav repeat: held USB arrows re-fire the trackball nav counters
// (the existing ISR-style channel keyboard_read_cb drains and rate-limits).
static uint8_t  s_arrow_state = 0;          // bit0..3 = up/down/left/right held
static uint32_t s_arrow_t0 = 0, s_arrow_last = 0;
#define HID_ARROW_DELAY_MS  400
#define HID_ARROW_REPEAT_MS 150
extern volatile int trackball_up, trackball_down, trackball_left, trackball_right;

// Lock-key state — HOST-tracked (the keyboard only reports the keypress; the
// host owns the state and drives the LEDs). NumLock defaults ON at connect
// (keypad types digits; OFF = nav cluster, UI only). The LED report is sent
// from the usb_task LOOP, never from the parse callback (ctrl_req pumps the
// client queue and must not run inside a transfer completion).
static bool          s_caps_lock   = false;
static bool          s_num_lock    = true;
static volatile bool s_led_pending = false;

// HID usage → ASCII, US QWERTY boot layout (usages 0x04-0x38). Enter/Backspace
// use the matrix pipeline's 0x0D/0x08 codes; Esc 0x1B, Tab 0x09. Usage 0x32
// (non-US #~) unmapped. F-keys and arrows are handled outside the tables.
static const uint8_t kUsage2Char[0x39] = {
    0, 0, 0, 0,
    'a','b','c','d','e','f','g','h','i','j','k','l','m',        // 0x04-0x10
    'n','o','p','q','r','s','t','u','v','w','x','y','z',        // 0x11-0x1D
    '1','2','3','4','5','6','7','8','9','0',                    // 0x1E-0x27
    0x0D, 0x1B, 0x08, 0x09, ' ',                                // 0x28-0x2C
    '-','=','[',']','\\', 0, ';','\'','`',',','.','/',          // 0x2D-0x38
};
static const uint8_t kUsage2CharShift[0x39] = {
    0, 0, 0, 0,
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    0x0D, 0x1B, 0x08, 0x09, ' ',
    '_','+','{','}','|', 0, ':','"','~','<','>','?',
};

// ── MSC (Bulk-Only Transport) profile + state ────────────────────────────────
// Thumb-drive class driver: SCSI over a bulk IN/OUT endpoint pair. SCSI
// transactions are serialized by s_msc_mutex and may originate on ANY task
// (Lua file I/O on core 0, sound_task, ...); completions always fire in
// usb_task's event pump. Waiters off usb_task block on s_msc_done; callers
// ON usb_task (the mount-time reads) self-pump instead — see msc_wait_done().
//
// FLASH-GUARD EXEMPT by design: bulk has no isochronous deadline, so a
// transfer in flight across a flash cache-stall just has its completion IRQ
// latched until the cores resume (same passive-pending rationale as the HID
// guard-begin fallback). Keeping MSC entirely off s_flash_mux also lets a
// guard HOLDER do USB-drive I/O mid-guard (e.g. _fs_copy U:→L: wraps the
// whole op in UsbFlashGuardIf) without any deadlock cycle.

struct MscProfile {
    bool     valid;
    uint8_t  ifnum;
    uint8_t  ep_in, ep_out;       // bulk endpoints (0x8x / 0x0x)
    uint16_t mps_in, mps_out;
};
static MscProfile s_msc_prof;

#define MSC_CHUNK_BYTES   16384       // data-phase ceiling (32 × 512B sectors)
#define MSC_IO_TIMEOUT_MS 5000        // per-phase completion timeout

static usb_transfer_t*   s_msc_xf        = nullptr; // one reusable bulk transfer
static volatile bool     s_msc_on        = false;   // claimed + unit ready
static SemaphoreHandle_t s_msc_mutex     = nullptr; // serializes transactions
static SemaphoreHandle_t s_msc_done      = nullptr; // binary; given by msc_xfer_cb
static volatile bool     s_msc_done_flag = false;   // usb_task self-pump variant
static TaskHandle_t      s_usb_task_handle = nullptr;
static uint32_t          s_msc_tag       = 1;       // CBW tag counter
static uint32_t          s_msc_sectors   = 0;       // capacity (last LBA + 1)
static uint32_t          s_msc_ssize     = 0;       // logical sector size

// ── PCM sink: rate-converting ring between sound.cpp and the ISO stream ─────
// Producers: sound.cpp's sound_task (core 1) — the mixer tap while tones or
// module audio play, or the staged file-playout tap (play_staged) while a
// file decodes. Same task, so never simultaneous. Consumer: the ISO refill
// callback in usb_task (core 1, prio 4 — preempts the producer), hence the
// portMUX spinlock. Samples are resampled to the dongle rate at push time and
// stored stereo. Ring lives in PSRAM.

#define SINK_FRAMES 8192                 // power of two; ~170 ms at 48 kHz
#define SINK_MASK   (SINK_FRAMES - 1)

static int16_t*          s_ring = nullptr;   // [SINK_FRAMES*2] stereo, PSRAM
static volatile uint32_t s_ring_head = 0;    // producer writes
static volatile uint32_t s_ring_tail = 0;    // consumer reads
static portMUX_TYPE      s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

// Linear resampler with async rate conversion (producer side). The clock that
// drives production (speaker I2S @44.1k for the mixer, or the file's rate) and
// the dongle clock that drives consumption are independent crystals, so a
// FIXED resample ratio slowly drifts the ring to empty (crackle) or full
// (drops) — this was the "only real audio crackles" bug; the debug tone
// avoided it by being fill-targeted at the source. The step is nudged each
// push toward holding the ring near USB_TARGET_MS, which tracks the dongle's
// true rate — the same closed-loop idea, applied to the mixer/file feed.
#define RS_FRAC       16
#define USB_TARGET_MS 50             // ring fill setpoint (also the prime depth)
static uint32_t s_rs_base;                   // nominal (src/dst) in .16 fixed point
static uint32_t s_rs_step;                   // base + fill-level correction
static uint32_t s_rs_frac;                   // .16 accumulator
static int16_t  s_rs_prevL, s_rs_prevR;
static int      s_rs_src = 0;                // current source rate

// Streaming/routing control.
static volatile bool s_stream_on   = false;  // ISO transfers in flight
static volatile bool s_route_pref  = true;   // usb_audio pref
static volatile bool s_speaker_pref = false; // usb_speaker pref (keep speaker on)
static volatile bool s_tone_want   = false;  // debug tone

// ── ISO transfer pool ───────────────────────────────────────────────────────

#define ISO_XFERS 3
#define ISO_PKTS  10

static usb_transfer_t* s_xf[ISO_XFERS];
static volatile int    s_xf_busy = 0;
static uint32_t        s_iso_frac = 0;       // fractional samples/ms accumulator

// Flash-write guard: a caller about to write internal flash sets pause_req; the
// usb_task drains in-flight ISO transfers and sets paused; the caller then does
// the write (cache stall now harmless — nothing in flight) and clears pause_req
// to resume. Serialized by s_flash_mux (held across the whole write).
static SemaphoreHandle_t s_flash_mux      = nullptr;
static volatile bool     s_flash_pause_req = false;
static volatile bool     s_flash_paused    = false;

// Debug tone generator (dongle-rate sine into the same sink).
#define TONE_HZ  440.0f
#define TONE_AMP 5000.0f
static float s_tone_phase = 0;

// ── PHY restore (RTC-domain; survives warm reset) ───────────────────────────
// Hand-inlined usb_phy_ll_int_jtag_enable() — hal/usb_phy_ll.h can't be
// included from C++ (an unrelated inline copies a volatile struct).
static void restore_serial_jtag_phy() {
    USB_SERIAL_JTAG.conf0.phy_sel           = 0;
    USB_SERIAL_JTAG.conf0.pad_pull_override = 0;
    USB_SERIAL_JTAG.conf0.dp_pullup         = 1;
    USB_SERIAL_JTAG.conf0.usb_pad_enable    = 1;
    RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 1;
    RTCCNTL.usb_conf.sw_usb_phy_sel    = 0;
}

// ── Sink primitives ──────────────────────────────────────────────────────────

static int sink_fill() {
    return (int)((s_ring_head - s_ring_tail) & SINK_MASK);
}

// Append stereo frames; drops on overflow. Short critical section.
static void sink_write(const int16_t* stereo, int nframes) {
    if (!s_ring) return;
    portENTER_CRITICAL(&s_ring_mux);
    uint32_t head = s_ring_head, tail = s_ring_tail;
    for (int i = 0; i < nframes; i++) {
        uint32_t next = (head + 1) & SINK_MASK;
        if (next == tail) break;               // full — drop the rest
        s_ring[head * 2]     = stereo[i * 2];
        s_ring[head * 2 + 1] = stereo[i * 2 + 1];
        head = next;
    }
    s_ring_head = head;
    portEXIT_CRITICAL(&s_ring_mux);
}

// Pop up to nframes; zero-fills the remainder on underrun. Returns real frames.
static int sink_read(int16_t* out, int nframes) {
    int got = 0;
    if (s_ring) {
        portENTER_CRITICAL(&s_ring_mux);
        uint32_t head = s_ring_head, tail = s_ring_tail;
        for (; got < nframes; got++) {
            if (tail == head) break;
            out[got * 2]     = s_ring[tail * 2];
            out[got * 2 + 1] = s_ring[tail * 2 + 1];
            tail = (tail + 1) & SINK_MASK;
        }
        s_ring_tail = tail;
        portEXIT_CRITICAL(&s_ring_mux);
    }
    for (int i = got; i < nframes; i++) { out[i * 2] = 0; out[i * 2 + 1] = 0; }
    return got;
}

// ── Public sink API (called from sound.cpp) ─────────────────────────────────

bool usb_audio_active() {
    // Not while the debug tone is running: tone_feed() owns the ring then, and
    // letting the mixer also push (silence/audio) would interleave two
    // producers into the same ring and click. The tone is an exclusive test.
    return s_stream_on && s_route_pref && s_prof.valid && !s_tone_want;
}

bool usb_audio_push(const int16_t* pcm, int frames, int src_rate, int channels) {
    if (!usb_audio_active() || !s_ring || frames <= 0) return false;
    if (src_rate <= 0) src_rate = 44100;

    // (Re)compute the nominal ratio when the source rate changes.
    if (src_rate != s_rs_src) {
        s_rs_src  = src_rate;
        s_rs_base = (uint32_t)(((uint64_t)src_rate << RS_FRAC) / s_prof.rate);
        if (s_rs_base == 0) s_rs_base = 1;
        s_rs_frac = 0;
    }

    // Async rate correction: err>0 (ring too full) -> larger step -> fewer
    // output frames -> drain; err<0 (too empty) -> smaller step -> more
    // frames -> fill. Gain saturates the ~2% clamp for large errors (fast
    // recovery) and is gentle near the setpoint (inaudible in steady state).
    int target = (int)s_prof.rate * USB_TARGET_MS / 1000;
    int err    = sink_fill() - target;
    int32_t maxc = (int32_t)(s_rs_base / 50);                 // ~2% of ratio
    int32_t corr = target ? (int32_t)(((int64_t)s_rs_base * err) / (target * 16)) : 0;
    if (corr >  maxc) corr =  maxc;
    if (corr < -maxc) corr = -maxc;
    int32_t step = (int32_t)s_rs_base + corr;
    s_rs_step = (uint32_t)(step < 1 ? 1 : step);

    // Resample src_rate -> dongle rate, emit stereo into a small batch buffer
    // flushed to the ring (bounds the critical section for slow-rate sources
    // that expand into many output frames).
    int16_t batch[128 * 2];
    int bn = 0;
    for (int i = 0; i < frames; i++) {
        int16_t inL, inR;
        if (channels >= 2) { inL = pcm[i * 2]; inR = pcm[i * 2 + 1]; }
        else               { inL = inR = pcm[i]; }
        while (s_rs_frac < (1u << RS_FRAC)) {
            int32_t oL = s_rs_prevL +
                (int32_t)(((int64_t)(inL - s_rs_prevL) * s_rs_frac) >> RS_FRAC);
            int32_t oR = s_rs_prevR +
                (int32_t)(((int64_t)(inR - s_rs_prevR) * s_rs_frac) >> RS_FRAC);
            batch[bn * 2]     = (int16_t)oL;
            batch[bn * 2 + 1] = (int16_t)oR;
            if (++bn == 128) { sink_write(batch, 128); bn = 0; }
            s_rs_frac += s_rs_step;
        }
        s_rs_frac -= (1u << RS_FRAC);
        s_rs_prevL = inL;
        s_rs_prevR = inR;
    }
    if (bn) sink_write(batch, bn);

    return !s_speaker_pref;   // caller silences its own I2S when speaker off
}

// ── Enumeration event + helpers ──────────────────────────────────────────────

static volatile bool s_ctrl_done = false;
static void ctrl_cb(usb_transfer_t*) { s_ctrl_done = true; }

static void client_event_cb(const usb_host_client_event_msg_t* msg, void*) {
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)      s_new_addr = msg->new_dev.address;
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) s_dev_gone = true;
}

static void str_desc_ascii(const usb_str_desc_t* sd, char* out, int out_len) {
    int n = 0;
    if (sd) {
        int chars = (sd->bLength - 2) / 2;
        for (int i = 0; i < chars && n < out_len - 1; i++) {
            uint16_t c = sd->wData[i];
            out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    }
    out[n] = 0;
}

static const char* class_name(uint8_t cls) {
    switch (cls) {
        case USB_CLASS_PER_INTERFACE: return "per-interface";
        case USB_CLASS_AUDIO:         return "AUDIO";
        case USB_CLASS_COMM:          return "comm";
        case USB_CLASS_HID:           return "HID";
        case USB_CLASS_MASS_STORAGE:  return "mass storage";
        case USB_CLASS_HUB:           return "hub";
        case USB_CLASS_CDC_DATA:      return "cdc data";
        case USB_CLASS_VENDOR_SPEC:   return "vendor";
        default:                      return "other";
    }
}

static UsbKind class_to_kind(uint8_t cls) {
    switch (cls) {
        case USB_CLASS_AUDIO:        return UKIND_AUDIO;
        case USB_CLASS_HID:          return UKIND_HID;
        case USB_CLASS_MASS_STORAGE: return UKIND_MSC;
        case USB_CLASS_COMM:
        case USB_CLASS_CDC_DATA:     return UKIND_CDC;
        case USB_CLASS_HUB:          return UKIND_HUB;
        default:                     return UKIND_OTHER;
    }
}

// Open the device, log its descriptors, classify it, and (for audio) build the
// streaming profile with the DWC descriptor patches. Keeps the device open so
// DEV_GONE fires on unplug.
static void describe_device(uint8_t addr) {
    ulog("Device connected, addr %u — opening...", addr);

    esp_err_t err = usb_host_device_open(s_client, addr, &s_dev);
    if (err != ESP_OK) { ulog("open failed: %s", esp_err_to_name(err)); s_dev = NULL; return; }

    memset(&s_info, 0, sizeof(s_info));
    s_info.connected = true;

    usb_device_info_t info;
    if (usb_host_device_info(s_dev, &info) == ESP_OK) {
        char mfg[32];
        str_desc_ascii(info.str_desc_manufacturer, mfg, sizeof(mfg));
        str_desc_ascii(info.str_desc_product, s_info.product, sizeof(s_info.product));
        ulog("speed: %s", info.speed == USB_SPEED_FULL ? "full (12M)" : "low (1.5M)");
        if (mfg[0])            ulog("mfg:  %s", mfg);
        if (s_info.product[0]) ulog("prod: %s", s_info.product);
    }

    const usb_device_desc_t* dd = NULL;
    if (usb_host_get_device_descriptor(s_dev, &dd) == ESP_OK && dd) {
        s_info.vid = dd->idVendor;
        s_info.pid = dd->idProduct;
        ulog("VID %04X  PID %04X  dev class %02X", dd->idVendor, dd->idProduct, dd->bDeviceClass);
        if (dd->bDeviceClass && dd->bDeviceClass != USB_CLASS_PER_INTERFACE)
            s_info.kind = class_to_kind(dd->bDeviceClass);
    }

    const usb_config_desc_t* cfg = NULL;
    err = usb_host_get_active_config_descriptor(s_dev, &cfg);
    if (err != ESP_OK || !cfg) { ulog("config desc failed: %s", esp_err_to_name(err)); return; }

    // Walk the config: classify by first data-carrying interface, and build
    // the audio profile (best ISO OUT alt: fits 192B FIFO +2, 16-bit +1).
    memset(&s_prof, 0, sizeof(s_prof));
    memset(&s_kbd_prof, 0, sizeof(s_kbd_prof));
    memset(&s_msc_prof, 0, sizeof(s_msc_prof));
    bool have_ac = false, have_as = false;
    int  best_score = -1;

    bool     cur_as = false, cur_hid_kbd = false, cur_msc = false;
    uint8_t  cur_if = 0, cur_alt = 0, cur_ch = 0, cur_sub = 0, cur_bits = 0;
    bool     cur_cont = false;
    int      cur_nrates = 0;
    uint32_t cur_rates[MAX_RATES] = {};

    const usb_standard_desc_t* d = (const usb_standard_desc_t*)cfg;
    int offset = 0;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != NULL) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t* i = (const usb_intf_desc_t*)d;
            ulog("IF %u alt %u: class %02X/%02X (%s) eps %u",
                 i->bInterfaceNumber, i->bAlternateSetting,
                 i->bInterfaceClass, i->bInterfaceSubClass,
                 class_name(i->bInterfaceClass), i->bNumEndpoints);
            cur_as = false;
            cur_if = i->bInterfaceNumber;
            cur_alt = i->bAlternateSetting;
            cur_ch = cur_sub = cur_bits = 0;
            cur_cont = false;
            cur_nrates = 0;
            // First non-audio interface with a concrete class sets the kind;
            // audio is decided by the profile below.
            if (s_info.kind == UKIND_NONE && i->bInterfaceClass != USB_CLASS_PER_INTERFACE)
                s_info.kind = class_to_kind(i->bInterfaceClass);
            if (i->bInterfaceClass == USB_CLASS_AUDIO) {
                if (i->bInterfaceSubClass == 0x01) { have_ac = true; s_prof.ac_ifnum = cur_if; }
                if (i->bInterfaceSubClass == 0x02) { have_as = true; cur_as = true; }
            }
            // HID boot-protocol keyboard (subclass 1 = boot, protocol 1 = kbd).
            cur_hid_kbd = (i->bInterfaceClass == USB_CLASS_HID &&
                           i->bInterfaceSubClass == 1 &&
                           i->bInterfaceProtocol == 1 &&
                           i->bAlternateSetting == 0);
            // MSC thumb drive (subclass 6 = SCSI transparent, proto 0x50 = BOT).
            // First matching interface wins; ignore any later MSC interface.
            cur_msc = (i->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
                       i->bInterfaceSubClass == 0x06 &&
                       i->bInterfaceProtocol == 0x50 &&
                       i->bAlternateSetting == 0 &&
                       !s_msc_prof.valid);
            if (cur_msc) s_msc_prof.ifnum = cur_if;
        } else if (cur_as && d->bDescriptorType == 0x24) {
            // Class-specific AS interface; subtype 0x02 = FORMAT_TYPE (UAC 1.0
            // Type I): [4]=channels [5]=subframe [6]=bits [7]=bSamFreqType
            // (0=continuous), then 3-byte LE sample-rate triplets.
            const uint8_t* b = (const uint8_t*)d;
            if (b[2] == 0x02 && b[0] >= 8) {
                cur_ch = b[4]; cur_sub = b[5]; cur_bits = b[6];
                uint8_t nf = b[7];
                cur_nrates = 0;
                cur_cont = (nf == 0);
                int triplets = cur_cont ? 2 : nf;
                for (int k = 0; k < triplets && cur_nrates < MAX_RATES; k++) {
                    if (8 + 3 * k + 2 >= b[0]) break;
                    cur_rates[cur_nrates++] = (uint32_t)b[8 + 3 * k]
                                            | ((uint32_t)b[9 + 3 * k] << 8)
                                            | ((uint32_t)b[10 + 3 * k] << 16);
                }
            }
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t* e = (const usb_ep_desc_t*)d;
            bool is_iso  = USB_EP_DESC_GET_XFERTYPE(e) == USB_TRANSFER_TYPE_ISOCHRONOUS;
            bool is_int  = USB_EP_DESC_GET_XFERTYPE(e) == USB_TRANSFER_TYPE_INTR;
            bool is_bulk = USB_EP_DESC_GET_XFERTYPE(e) == USB_TRANSFER_TYPE_BULK;
            bool is_out  = USB_EP_DESC_GET_EP_DIR(e) == 0;
            if (is_bulk && cur_msc) {
                if (is_out && !s_msc_prof.ep_out) {
                    s_msc_prof.ep_out = e->bEndpointAddress;
                    s_msc_prof.mps_out = USB_EP_DESC_GET_MPS(e);
                } else if (!is_out && !s_msc_prof.ep_in) {
                    s_msc_prof.ep_in = e->bEndpointAddress;
                    s_msc_prof.mps_in = USB_EP_DESC_GET_MPS(e);
                }
                if (s_msc_prof.ep_in && s_msc_prof.ep_out && !s_msc_prof.valid) {
                    s_msc_prof.valid = true;
                    ulog("  msc: IF %u in %02X/%u out %02X/%u",
                         s_msc_prof.ifnum, s_msc_prof.ep_in, s_msc_prof.mps_in,
                         s_msc_prof.ep_out, s_msc_prof.mps_out);
                }
            }
            if (is_int && !is_out && cur_hid_kbd && !s_kbd_prof.valid
                       && USB_EP_DESC_GET_MPS(e) >= 8) {   // boot report = 8B
                s_kbd_prof.valid     = true;
                s_kbd_prof.ifnum     = cur_if;
                s_kbd_prof.ep        = e->bEndpointAddress;
                s_kbd_prof.mps       = USB_EP_DESC_GET_MPS(e);
                s_kbd_prof.binterval = e->bInterval;
                ulog("  boot kbd: IF %u ep %02X mps %u intv %u",
                     cur_if, s_kbd_prof.ep, s_kbd_prof.mps, s_kbd_prof.binterval);
            }
            if (is_iso && is_out && cur_as) {
                ulog("  cand alt%u: mps %u intv %u, %uch %u-bit",
                     cur_alt, USB_EP_DESC_GET_MPS(e), e->bInterval, cur_ch, cur_bits);
                int score = (USB_EP_DESC_GET_MPS(e) <= 192 ? 2 : 0) + (cur_sub == 2 ? 1 : 0);
                if (score > best_score) {
                    best_score        = score;
                    s_prof.valid      = true;
                    s_prof.continuous = cur_cont;
                    s_prof.ifnum      = cur_if;
                    s_prof.alt        = cur_alt;
                    s_prof.ep         = e->bEndpointAddress;
                    s_prof.mps        = USB_EP_DESC_GET_MPS(e);
                    s_prof.binterval  = e->bInterval;
                    s_prof.ep_desc    = e;
                    s_prof.channels   = cur_ch;
                    s_prof.subsize    = cur_sub;
                    s_prof.bits       = cur_bits;
                    s_prof.nrates     = cur_nrates;
                    memcpy(s_prof.rates, cur_rates, sizeof(cur_rates));
                }
            }
        }
    }

    s_info.has_kbd = s_kbd_prof.valid;
    s_info.has_msc = s_msc_prof.valid;

    if (have_ac && s_prof.valid) {
        s_info.kind = UKIND_AUDIO;
        // Rate: 44100 if listed (mixer-native), else 48000, else first; cap at
        // 48k (>48k needs >192B/ms packets, over the FIFO limit).
        if (s_prof.continuous) {
            uint32_t lo = s_prof.rates[0], hi = s_prof.rates[1];
            s_prof.rate = (44100 >= lo && 44100 <= hi) ? 44100 : lo;
        } else {
            s_prof.rate = s_prof.nrates ? s_prof.rates[0] : 48000;
            for (int k = 0; k < s_prof.nrates; k++)
                if (s_prof.rates[k] == 44100) { s_prof.rate = 44100; break; }
            if (s_prof.rate != 44100)
                for (int k = 0; k < s_prof.nrates; k++)
                    if (s_prof.rates[k] == 48000) { s_prof.rate = 48000; break; }
            if (s_prof.rate > 48000) {
                uint32_t fit = 0;
                for (int k = 0; k < s_prof.nrates; k++)
                    if (s_prof.rates[k] <= 48000 && s_prof.rates[k] > fit) fit = s_prof.rates[k];
                if (fit) s_prof.rate = fit;
            }
        }

        // Malformed descriptors can advertise a 0 (or junk) sample rate, and
        // the rate feeds divisions in the resampler and the packet-per-ms
        // math — reject the profile instead of crashing on the first push.
        // (The device stays listed; it's just never streamed to.)
        if (s_prof.rate < 8000) {
            ulog("bad sample rate %u — audio profile rejected", (unsigned)s_prof.rate);
            s_prof.valid = false;
        }

        // Patch the cached descriptor so the HCD accepts the claim (host RAM
        // only; the device never sees it).
        usb_ep_desc_t* epw = (usb_ep_desc_t*)s_prof.ep_desc;
        if (s_prof.mps > 192) {
            ulog("patching ep mps %u -> 192 (DWC FIFO limit)", s_prof.mps);
            epw->wMaxPacketSize = 192; s_prof.mps = 192;
        }
        if (s_prof.binterval == 0) {
            ulog("patching ep bInterval 0 -> 1");
            epw->bInterval = 1; s_prof.binterval = 1;
        }

        s_info.rate = s_prof.rate;
        s_info.bits = s_prof.bits;
        ulog("AS: IF %u alt %u ep %02X mps %u, %uch %u-bit",
             s_prof.ifnum, s_prof.alt, s_prof.ep, s_prof.mps, s_prof.channels, s_prof.bits);
        ulog("rate: %u Hz%s", (unsigned)s_prof.rate, s_prof.continuous ? " (continuous)" : "");
        if (s_prof.subsize != 2)
            ulog("WARNING: no 16-bit alt (subframe %u)", s_prof.subsize);
        ulog(">>> Audio device ready%s.", s_route_pref ? " — routing" : " (routing off)");
    } else {
        s_prof.valid = false;
        if (s_msc_prof.valid)
            ulog(">>> Storage detected — mounting.");
        else if (s_kbd_prof.valid)
            ulog(">>> Keyboard detected — driver starting.");
        else if (s_info.kind == UKIND_HUB)
            ulog(">>> Hub — unsupported until IDF5 (single device only).");
        else
            ulog(">>> Device kind: %s (no driver yet).", kind_name(s_info.kind));
    }
}

// ── Control transfers (usb_task context only) ───────────────────────────────

static bool ctrl_req(uint8_t bmReqType, uint8_t bReq, uint16_t wValue,
                     uint16_t wIndex, const uint8_t* data, uint16_t wLength) {
    usb_transfer_t* t = NULL;
    if (usb_host_transfer_alloc(8 + wLength, 0, &t) != ESP_OK) return false;
    uint8_t* b = t->data_buffer;
    b[0] = bmReqType; b[1] = bReq;
    b[2] = wValue & 0xFF;  b[3] = wValue >> 8;
    b[4] = wIndex & 0xFF;  b[5] = wIndex >> 8;
    b[6] = wLength & 0xFF; b[7] = wLength >> 8;
    if (data && wLength) memcpy(b + 8, data, wLength);
    t->num_bytes        = 8 + wLength;
    t->device_handle    = s_dev;
    t->bEndpointAddress = 0;
    t->callback         = ctrl_cb;
    t->context          = NULL;
    s_ctrl_done = false;
    if (usb_host_transfer_submit_control(s_client, t) != ESP_OK) {
        usb_host_transfer_free(t);
        return false;
    }
    // Pump BOTH handlers — EP0 completes on the default pipe (library-owned).
    for (int i = 0; i < 100 && !s_ctrl_done; i++) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(0, &flags);
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
    }
    bool ok = s_ctrl_done && t->status == USB_TRANSFER_STATUS_COMPLETED;
    if (!ok)
        ulog("ctrl %02X/%u: %s", bmReqType, bReq,
             !s_ctrl_done ? "timeout"
             : t->status == USB_TRANSFER_STATUS_STALL ? "STALL" : "error");
    usb_host_transfer_free(t);
    return ok;
}

// ── ISO streaming (usb_task context) ────────────────────────────────────────

// Fill one ISO transfer from the sink: ISO_PKTS packets of ceil(rate/1000)
// frames each, packed to the dongle's channel count. Underruns emit silence.
static void fill_xfer(usb_transfer_t* t) {
    uint8_t* buf = t->data_buffer;
    int ch = s_prof.channels ? s_prof.channels : 2;
    int total = 0;
    // Apply the system volume/mute here so the USB output tracks the slider —
    // producers push full-scale PCM into the ring; the volume lands at playout
    // (no ring-depth lag, instant mute). .16 fixed point, avoids per-sample div.
    int32_t vscale = sound_get_muted() ? 0
                   : ((int32_t)sound_get_volume() << 16) / 21;
    for (int p = 0; p < ISO_PKTS; p++) {
        s_iso_frac += s_prof.rate;
        int n = s_iso_frac / 1000;
        s_iso_frac %= 1000;
        int bytes = n * ch * 2;
        if (total + bytes > (int)t->data_buffer_size) { n = 0; bytes = 0; }

        int16_t frames[64 * 2];                 // n <= 48 (48k) + slack
        if (n > 64) n = 64;
        sink_read(frames, n);                   // zero-fills on underrun

        int16_t* o = (int16_t*)(buf + total);
        for (int k = 0; k < n; k++) {
            int32_t l = ((int32_t)frames[k * 2]     * vscale) >> 16;
            int32_t r = ((int32_t)frames[k * 2 + 1] * vscale) >> 16;
            if (ch == 1) *o++ = (int16_t)((l + r) >> 1);
            else { *o++ = (int16_t)l; *o++ = (int16_t)r; }
        }
        t->isoc_packet_desc[p].num_bytes = n * ch * 2;
        total += n * ch * 2;
    }
    t->num_bytes = total;
}

static void iso_xfer_cb(usb_transfer_t* t) {
    // Stop resubmitting while a flash write is pending so the transfers drain
    // to zero in-flight before the cache stall.
    if (s_stream_on && !s_flash_pause_req
                    && t->status != USB_TRANSFER_STATUS_NO_DEVICE
                    && t->status != USB_TRANSFER_STATUS_CANCELED) {
        fill_xfer(t);
        if (usb_host_transfer_submit(t) == ESP_OK) return;
        ulog("iso: resubmit failed");
    }
    s_xf_busy--;
}

// Re-arm the ISO transfers after a flash-guard pause (transfer objects were
// kept; they just stopped being resubmitted). Re-primes the ring first.
static void resume_iso() {
    s_iso_frac = 0;
    int made = 0;
    for (int i = 0; i < ISO_XFERS; i++) {
        if (!s_xf[i]) continue;
        fill_xfer(s_xf[i]);
        if (usb_host_transfer_submit(s_xf[i]) == ESP_OK) made++;
    }
    s_xf_busy = made;
}

static void audio_stream_stop(bool dev_present) {
    s_stream_on = false;
    for (int i = 0; i < 100 && s_xf_busy > 0; i++) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(0, &flags);
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
    }
    for (int i = 0; i < ISO_XFERS; i++) {
        if (s_xf[i]) { usb_host_transfer_free(s_xf[i]); s_xf[i] = NULL; }
    }
    s_xf_busy = 0;
    if (s_dev) {
        if (dev_present) ctrl_req(0x01, 11 /*SET_INTERFACE*/, 0, s_prof.ifnum, NULL, 0);
        usb_host_interface_release(s_client, s_dev, s_prof.ifnum);
    }
    ulog("Audio streaming stopped.");
}

static void audio_stream_start() {
    if (!s_dev || !s_prof.valid) return;
    int ch = s_prof.channels ? s_prof.channels : 2;
    uint32_t bpms = ((s_prof.rate + 999) / 1000) * ch * 2;
    if (bpms > s_prof.mps) {
        ulog("stream: %u B/ms exceeds ep mps %u", (unsigned)bpms, s_prof.mps);
        return;
    }

    esp_err_t err = usb_host_interface_claim(s_client, s_dev, s_prof.ifnum, s_prof.alt);
    if (err != ESP_OK) {
        ulog("claim IF%u alt%u failed: %s", s_prof.ifnum, s_prof.alt, esp_err_to_name(err));
        return;
    }
    if (!ctrl_req(0x01, 11 /*SET_INTERFACE*/, s_prof.alt, s_prof.ifnum, NULL, 0))
        ulog("SET_INTERFACE failed (continuing)");
    uint8_t r3[3] = { (uint8_t)s_prof.rate, (uint8_t)(s_prof.rate >> 8), (uint8_t)(s_prof.rate >> 16) };
    if (!ctrl_req(0x22, 0x01 /*SET_CUR*/, 0x0100 /*SAMPLING_FREQ*/, s_prof.ep, r3, 3))
        ulog("SET_CUR rate failed (dongle may be fixed-rate)");

    // Reset sink + resampler so a fresh stream starts clean.
    s_ring_head = s_ring_tail = 0;
    s_rs_src = 0; s_rs_frac = 0; s_rs_prevL = s_rs_prevR = 0;
    s_iso_frac = 0;

    // Prime the ring to the async-rate setpoint so production starts centered
    // (the ASRC in usb_audio_push then holds it there). This is the playback
    // latency; also absorbs the bursty producer/consumer phase mismatch.
    {
        int16_t z[128 * 2] = {};
        int prime = (int)s_prof.rate * USB_TARGET_MS / 1000;
        while (prime > 0) { int n = prime > 128 ? 128 : prime; sink_write(z, n); prime -= n; }
    }

    int made = 0;
    for (int i = 0; i < ISO_XFERS; i++) {
        if (usb_host_transfer_alloc(ISO_PKTS * s_prof.mps, ISO_PKTS, &s_xf[i]) != ESP_OK) break;
        s_xf[i]->device_handle    = s_dev;
        s_xf[i]->bEndpointAddress = s_prof.ep;
        s_xf[i]->callback         = iso_xfer_cb;
        s_xf[i]->context          = NULL;
        fill_xfer(s_xf[i]);                     // starts as silence
        if (usb_host_transfer_submit(s_xf[i]) != ESP_OK) {
            usb_host_transfer_free(s_xf[i]); s_xf[i] = NULL; break;
        }
        made++;
    }
    if (!made) {
        ulog("stream: transfer alloc/submit failed");
        usb_host_interface_release(s_client, s_dev, s_prof.ifnum);
        return;
    }
    s_xf_busy = made;
    s_stream_on = true;
    ulog("Audio streaming @ %u Hz, %dch (%d xfers)", (unsigned)s_prof.rate, ch, made);
}

// Top the ring up with 440Hz sine while the debug tone is on (dongle rate).
static void tone_feed() {
    int target = (int)s_prof.rate / 20;         // ~50 ms
    while (sink_fill() < target) {
        int16_t batch[128 * 2];
        int n = target - sink_fill();
        if (n > 128) n = 128;
        for (int k = 0; k < n; k++) {
            int16_t v = (int16_t)(sinf(s_tone_phase) * TONE_AMP);
            s_tone_phase += 2.0f * (float)M_PI * TONE_HZ / (float)s_prof.rate;
            if (s_tone_phase > 2.0f * (float)M_PI) s_tone_phase -= 2.0f * (float)M_PI;
            batch[k * 2] = batch[k * 2 + 1] = v;
        }
        sink_write(batch, n);
    }
}

// ── HID boot-keyboard driver (usb_task context) ─────────────────────────────

// Translate one usage for the module pipeline: unshifted base chars (modules
// get separate modifier pseudo-key edges, matching the matrix convention).
// PURE function of the usage — deliberately ignores NumLock/Caps so a release
// always emits the same code as its press (no stuck keys if a lock toggles
// mid-hold); keypad is always digits for modules. 0 = not mapped.
//
// Pseudo-code allocation (kq/keymap space; 0x80-0x85 + 0xB0-0xB9 pre-date USB):
//   0x80 shift  0x81-0x84 up/down/left/right  0x85 click (trackball)
//   0x86 Home   0x87 End   0x88 PgUp   0x89 PgDn   0x8A Insert
//   0x8B Ctrl   0x8C Alt   0x8D GUI    0x8E CapsLock   0x8F NumLock
//   0x90-0x9F RESERVED — PC-XT launcher binding outputs live at 0x91-0x99
//   0xA0 PrtSc  0xA1 ScrLk 0xA2 Pause  0xA3 Menu
//   0xB0-0xB9 F1-F10   0xBA F11   0xBB F12   0x7F Delete (ASCII DEL)
static uint8_t usage_to_game(uint8_t u) {
    if (u < 0x39) return kUsage2Char[u];
    if (u >= 0x3A && u <= 0x43) return (uint8_t)(0xB0 + (u - 0x3A));  // F1-F10
    if (u >= 0x59 && u <= 0x61) return (uint8_t)('1' + (u - 0x59));   // KP1-KP9
    switch (u) {
        case 0x39: return 0x8E;   // CapsLock (state/LED handled host-side too)
        case 0x44: return 0xBA;   // F11
        case 0x45: return 0xBB;   // F12
        case 0x46: return 0xA0;   // PrintScreen
        case 0x47: return 0xA1;   // ScrollLock
        case 0x48: return 0xA2;   // Pause
        case 0x49: return 0x8A;   // Insert
        case 0x4A: return 0x86;   // Home
        case 0x4B: return 0x88;   // PgUp
        case 0x4C: return 0x7F;   // Delete
        case 0x4D: return 0x87;   // End
        case 0x4E: return 0x89;   // PgDn
        case 0x4F: return 0x84;   // right
        case 0x50: return 0x83;   // left
        case 0x51: return 0x82;   // down
        case 0x52: return 0x81;   // up
        case 0x53: return 0x8F;   // NumLock
        case 0x54: return '/';    // keypad ─ always the digit/operator layer
        case 0x55: return '*';
        case 0x56: return '-';
        case 0x57: return '+';
        case 0x58: return 0x0D;   // KP Enter
        case 0x62: return '0';
        case 0x63: return '.';
        case 0x65: return 0xA3;   // Menu
    }
    return 0;
}

// Translate one usage for the UI pipeline (kb_key_state is 128 ASCII slots +
// the low LVGL control codes). Shift picks the shifted table; Caps flips
// letter case only (shift XOR caps, standard behavior); NumLock picks the
// keypad layer. Home/End/Delete land on LV_KEY_HOME/END/DEL (2/3/0x7F), which
// pass through the main.cpp merge untouched. Arrows (incl. keypad arrows with
// NumLock off) are NOT chars — they ride the trackball counters. F-keys and
// the remaining pseudo-coded keys don't fit under 128 and stay module-only.
static uint8_t usage_to_ui(uint8_t u, bool shift) {
    if (u >= 0x04 && u <= 0x1D)                           // letters
        return (shift ^ s_caps_lock) ? kUsage2CharShift[u] : kUsage2Char[u];
    if (u < 0x39) return shift ? kUsage2CharShift[u] : kUsage2Char[u];
    switch (u) {
        case 0x4A: return 2;      // Home   -> LV_KEY_HOME
        case 0x4C: return 0x7F;   // Delete -> LV_KEY_DEL (textarea fwd-delete)
        case 0x4D: return 3;      // End    -> LV_KEY_END
        case 0x58: return 0x0D;   // KP Enter
    }
    if (s_num_lock) {                                     // keypad: digits
        if (u >= 0x59 && u <= 0x61) return (uint8_t)('1' + (u - 0x59));
        switch (u) {
            case 0x54: return '/';
            case 0x55: return '*';
            case 0x56: return '-';
            case 0x57: return '+';
            case 0x62: return '0';
            case 0x63: return '.';
        }
    } else {                                              // keypad: nav layer
        switch (u) {
            case 0x5F: return 2;      // KP7 Home
            case 0x59: return 3;      // KP1 End
            case 0x63: return 0x7F;   // KP. Delete
        }
    }
    return 0;
}

// Modifier byte (report[0]) → module pseudo-key edges, L|R combined like the
// matrix's single shift. The UI consumes shift/caps only (via usage_to_ui).
static const struct { uint8_t mask; uint8_t code; } kModGame[] = {
    { 0x22, 0x80 },   // shift
    { 0x11, 0x8B },   // ctrl
    { 0x44, 0x8C },   // alt
    { 0x88, 0x8D },   // gui
};
#define MODGAME_N (sizeof(kModGame) / sizeof(kModGame[0]))

// Parse one 8-byte boot report: [0]=modifiers [1]=reserved [2..7]=usages.
// Reports are ABSOLUTE (all currently-held keys), so both consumers are
// rebuilt/diffed from scratch each time — nothing can stick.
static void hid_parse_report(const uint8_t* r) {
    // Phantom rollover (>6 keys): all usage slots 0x01 — keep previous state.
    bool phantom = true;
    for (int i = 2; i < 8; i++) if (r[i] != 0x01) { phantom = false; break; }
    if (phantom) return;

    bool game  = elf_input_active();
    bool shift = (r[0] & 0x22) != 0;            // L/R shift modifier bits

    // Lock keys toggle host state on press edge; the LED report is deferred
    // to the usb_task loop (s_led_pending) — see the state block comment.
    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (u != 0x39 && u != 0x53) continue;
        bool was = false;
        for (int j = 2; j < 8; j++) if (s_hid_prev[j] == u) { was = true; break; }
        if (!was) {
            if (u == 0x39) s_caps_lock = !s_caps_lock;
            else           s_num_lock  = !s_num_lock;
            s_led_pending = true;
        }
    }

    // UI image: shift/caps/numlock applied at parse time.
    bool held[128] = {};
    int  nheld = 0;
    for (int i = 2; i < 8; i++) {
        uint8_t ch = usage_to_ui(r[i], shift);
        if (ch && ch < 128 && !held[ch]) { held[ch] = true; nheld++; }
    }
    portENTER_CRITICAL(&s_kbd_mux);
    memcpy(s_usb_held, held, sizeof(s_usb_held));
    s_usb_nheld = nheld;
    portEXIT_CRITICAL(&s_kbd_mux);

    // Module edges: usage-set diff vs the previous report, unshifted codes.
    if (game) {
        for (int i = 2; i < 8; i++) {           // releases: in prev, gone now
            uint8_t u = s_hid_prev[i];
            if (!u) continue;
            bool still = false;
            for (int j = 2; j < 8; j++) if (r[j] == u) { still = true; break; }
            if (!still) { uint8_t c = usage_to_game(u); if (c) elf_input_inject(c, 0); }
        }
        for (int i = 2; i < 8; i++) {           // presses: new this report
            uint8_t u = r[i];
            if (!u) continue;
            bool was = false;
            for (int j = 2; j < 8; j++) if (s_hid_prev[j] == u) { was = true; break; }
            if (!was) { uint8_t c = usage_to_game(u); if (c) elf_input_inject(c, 1); }
        }
        for (size_t m = 0; m < MODGAME_N; m++) {   // modifier edges (shift/ctrl/alt/gui)
            bool now = (r[0] & kModGame[m].mask) != 0;
            bool was = (s_hid_prev[0] & kModGame[m].mask) != 0;
            if (now != was) elf_input_inject(kModGame[m].code, now ? 1 : 0);
        }
    }

    // UI arrow nav via the trackball counters (rising edges; hid_arrow_tick
    // repeats). Keypad 8/2/4/6 join in when NumLock is off. Suppressed
    // in-game — there arrows are 0x81-0x84 edges above, and counter
    // increments would fight poll_input's momentum integration.
    uint8_t arrows = 0;
    for (int i = 2; i < 8; i++) {
        switch (r[i]) {
            case 0x52: arrows |= 1; break;      // up
            case 0x51: arrows |= 2; break;      // down
            case 0x50: arrows |= 4; break;      // left
            case 0x4F: arrows |= 8; break;      // right
            case 0x60: if (!s_num_lock) arrows |= 1; break;   // KP8
            case 0x5A: if (!s_num_lock) arrows |= 2; break;   // KP2
            case 0x5C: if (!s_num_lock) arrows |= 4; break;   // KP4
            case 0x5E: if (!s_num_lock) arrows |= 8; break;   // KP6
        }
    }
    if (!game) {
        uint8_t newly = arrows & (uint8_t)~s_arrow_state;
        if (newly & 1) trackball_up++;
        if (newly & 2) trackball_down++;
        if (newly & 4) trackball_left++;
        if (newly & 8) trackball_right++;
        if (newly) { s_arrow_t0 = millis(); s_arrow_last = s_arrow_t0; }
        s_arrow_state = arrows;
    } else {
        s_arrow_state = 0;
    }

    memcpy(s_hid_prev, r, 8);
}

// Held-arrow nav repeat (UI only). Called once per usb_task loop pass.
static void hid_arrow_tick() {
    if (!s_arrow_state) return;
    uint32_t now = millis();
    if (now - s_arrow_t0 < HID_ARROW_DELAY_MS) return;
    if (now - s_arrow_last < HID_ARROW_REPEAT_MS) return;
    s_arrow_last = now;
    if (s_arrow_state & 1) trackball_up++;
    if (s_arrow_state & 2) trackball_down++;
    if (s_arrow_state & 4) trackball_left++;
    if (s_arrow_state & 8) trackball_right++;
}

// Release everything (unplug/stop): clear the UI image and, mid-game, inject
// release edges for whatever the last report showed held — no stuck keys.
static void hid_all_keys_up() {
    portENTER_CRITICAL(&s_kbd_mux);
    memset(s_usb_held, 0, sizeof(s_usb_held));
    s_usb_nheld = 0;
    portEXIT_CRITICAL(&s_kbd_mux);
    if (elf_input_active()) {
        for (int i = 2; i < 8; i++) {
            uint8_t c = usage_to_game(s_hid_prev[i]);
            if (c) elf_input_inject(c, 0);
        }
        for (size_t m = 0; m < MODGAME_N; m++)
            if (s_hid_prev[0] & kModGame[m].mask)
                elf_input_inject(kModGame[m].code, 0);
    }
    s_arrow_state = 0;
    memset(s_hid_prev, 0, sizeof(s_hid_prev));
}

static void hid_xfer_cb(usb_transfer_t* t) {
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes >= 8)
        hid_parse_report(t->data_buffer);
    // Same drain discipline as iso_xfer_cb: no resubmit while a flash write
    // is pending or once the device/driver is gone.
    if (s_hid_on && !s_flash_pause_req
                 && t->status != USB_TRANSFER_STATUS_NO_DEVICE
                 && t->status != USB_TRANSFER_STATUS_CANCELED) {
        t->num_bytes = s_kbd_prof.mps;
        if (usb_host_transfer_submit(t) == ESP_OK) return;
        ulog("hid: resubmit failed");
    }
    s_hid_busy--;
}

static void hid_kbd_start() {
    if (!s_dev || !s_kbd_prof.valid || s_hid_on) return;
    esp_err_t err = usb_host_interface_claim(s_client, s_dev, s_kbd_prof.ifnum, 0);
    if (err != ESP_OK) {
        ulog("kbd claim IF%u failed: %s", s_kbd_prof.ifnum, esp_err_to_name(err));
        s_kbd_prof.valid = false;               // don't retry-spin
        return;
    }
    // Boot protocol + report-on-change (idle 0). Both non-fatal on STALL:
    // reports are absolute snapshots, so periodic idle re-sends of an
    // unchanged report just produce zero diffs.
    if (!ctrl_req(0x21, 0x0B /*SET_PROTOCOL*/, 0 /*boot*/, s_kbd_prof.ifnum, NULL, 0))
        ulog("SET_PROTOCOL boot failed (continuing)");
    if (!ctrl_req(0x21, 0x0A /*SET_IDLE*/, 0, s_kbd_prof.ifnum, NULL, 0))
        ulog("SET_IDLE failed (continuing)");

    memset(s_hid_prev, 0, sizeof(s_hid_prev));
    if (usb_host_transfer_alloc(s_kbd_prof.mps, 0, &s_hid_xf) != ESP_OK) {
        ulog("kbd transfer alloc failed");
        usb_host_interface_release(s_client, s_dev, s_kbd_prof.ifnum);
        s_kbd_prof.valid = false;
        return;
    }
    s_hid_xf->device_handle    = s_dev;
    s_hid_xf->bEndpointAddress = s_kbd_prof.ep;
    s_hid_xf->callback         = hid_xfer_cb;
    s_hid_xf->context          = NULL;
    s_hid_xf->num_bytes        = s_kbd_prof.mps;   // IN: multiple of mps
    if (usb_host_transfer_submit(s_hid_xf) != ESP_OK) {
        ulog("kbd transfer submit failed");
        usb_host_transfer_free(s_hid_xf); s_hid_xf = nullptr;
        usb_host_interface_release(s_client, s_dev, s_kbd_prof.ifnum);
        s_kbd_prof.valid = false;
        return;
    }
    s_hid_busy = 1;
    s_hid_on   = true;
    // Fresh keyboard = fresh lock state; push the initial LEDs (NumLock on)
    // from the loop on its next pass.
    s_caps_lock = false;
    s_num_lock  = true;
    s_led_pending = true;
    ulog(">>> Keyboard ready.");
}

// Push the lock-key LED state (boot output report: bit0 Num, bit1 Caps).
// Loop context only — ctrl_req self-pumps and must not run inside callbacks.
static void hid_led_update() {
    s_led_pending = false;
    uint8_t led = (uint8_t)((s_num_lock ? 1 : 0) | (s_caps_lock ? 2 : 0));
    if (!ctrl_req(0x21, 0x09 /*SET_REPORT*/, 0x0200 /*Output, report 0*/,
                  s_kbd_prof.ifnum, &led, 1))
        ulog("kbd LED report failed (continuing)");
}

static void hid_kbd_stop(bool dev_present) {
    s_hid_on = false;
    if (s_hid_busy > 0 && dev_present && s_dev) {
        // A quiet keyboard's IN transfer stays pending forever — force it to
        // complete (CANCELED) instead of waiting for a keypress.
        usb_host_endpoint_halt(s_dev, s_kbd_prof.ep);
        usb_host_endpoint_flush(s_dev, s_kbd_prof.ep);
    }
    for (int i = 0; i < 100 && s_hid_busy > 0; i++) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(0, &flags);
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
    }
    s_hid_busy = 0;
    if (s_hid_xf) { usb_host_transfer_free(s_hid_xf); s_hid_xf = nullptr; }
    if (s_dev) usb_host_interface_release(s_client, s_dev, s_kbd_prof.ifnum);
    hid_all_keys_up();
    ulog("Keyboard stopped.");
}

// Flash-guard park/resume. Unlike ISO (which drains by declining resubmit
// within a few ms), an idle interrupt-IN never completes on its own — halt +
// flush force a CANCELED completion so the caller's pump loop can drain
// s_hid_busy to 0 quickly. Resume clears the halt on BOTH sides (device via
// CLEAR_FEATURE, host via endpoint_clear) so the data toggles stay in sync,
// then re-arms the kept transfer object.
static void hid_flash_park() {
    if (!s_dev) return;
    usb_host_endpoint_halt(s_dev, s_kbd_prof.ep);
    usb_host_endpoint_flush(s_dev, s_kbd_prof.ep);
}

static void hid_resume() {
    if (!s_dev || !s_hid_xf) return;
    if (s_hid_busy > 0) return;   // park drain timed out: still in flight

    ctrl_req(0x02, 0x01 /*CLEAR_FEATURE(ENDPOINT_HALT)*/, 0, s_kbd_prof.ep, NULL, 0);
    usb_host_endpoint_clear(s_dev, s_kbd_prof.ep);
    s_hid_xf->num_bytes = s_kbd_prof.mps;
    if (usb_host_transfer_submit(s_hid_xf) == ESP_OK) s_hid_busy = 1;
    else ulog("hid: resume resubmit failed");
}

// ── MSC Bulk-Only Transport driver ───────────────────────────────────────────
// SCSI over bulk IN/OUT (see the state block up top for the task/locking
// model). Two transfer objects: s_msc_xf_cmd (64B, CBW out + CSW in) and
// s_msc_xf (MSC_CHUNK_BYTES, data phases) — separate so an OUT payload never
// collides with the CBW bytes. msc_scsi() callers hold s_msc_mutex, except
// the pre-mount init sequence in msc_start (no foreign access can exist
// before the volume registers).

static usb_transfer_t* s_msc_xf_cmd = nullptr;

// Completion callback for every MSC transfer (bulk and MSC-owned control):
// flag feeds the usb_task self-pump path, semaphore wakes foreign waiters.
static void msc_xfer_cb(usb_transfer_t*) {
    s_msc_done_flag = true;
    xSemaphoreGive(s_msc_done);
}

// Dual-context completion wait. On usb_task the pump isn't running (we ARE
// usb_task), so pump events until the flag; other tasks block on the
// semaphore that usb_task's pump gives.
static bool msc_wait_done(uint32_t timeout_ms) {
    if (xTaskGetCurrentTaskHandle() == s_usb_task_handle) {
        uint32_t t0 = millis();
        while (!s_msc_done_flag) {
            uint32_t flags = 0;
            usb_host_lib_handle_events(0, &flags);
            usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
            if (millis() - t0 > timeout_ms) return false;
        }
        return true;
    }
    return xSemaphoreTake(s_msc_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

// One bulk phase on transfer `t`. `len` is the wire length (IN lengths must
// already be MPS-rounded). Returns the final transfer status.
static usb_transfer_status_t msc_bulk(usb_transfer_t* t, uint8_t ep, int len) {
    t->device_handle    = s_dev;
    t->bEndpointAddress = ep;
    t->num_bytes        = len;
    t->callback         = msc_xfer_cb;
    t->context          = NULL;
    s_msc_done_flag = false;
    xSemaphoreTake(s_msc_done, 0);              // drain a stale give
    if (usb_host_transfer_submit(t) != ESP_OK)
        return USB_TRANSFER_STATUS_ERROR;
    if (!msc_wait_done(MSC_IO_TIMEOUT_MS)) {
        // Still in flight past the timeout — force a completion so the
        // transfer object is safely reusable before we return.
        usb_host_endpoint_halt(s_dev, ep);
        usb_host_endpoint_flush(s_dev, ep);
        msc_wait_done(1000);
        usb_host_endpoint_clear(s_dev, ep);
        return USB_TRANSFER_STATUS_TIMED_OUT;
    }
    return t->status;
}

// MSC-context control request, callable from ANY task (unlike ctrl_req,
// which self-pumps and is usb_task-only). Rides the same dual-context wait.
// IN payloads (bmReqType bit7) are copied into `data`.
static bool msc_ctrl(uint8_t bmReqType, uint8_t bReq, uint16_t wValue,
                     uint16_t wIndex, uint8_t* data, uint16_t wLength) {
    usb_transfer_t* t = NULL;
    if (usb_host_transfer_alloc(8 + wLength, 0, &t) != ESP_OK) return false;
    uint8_t* b = t->data_buffer;
    b[0] = bmReqType; b[1] = bReq;
    b[2] = wValue & 0xFF;  b[3] = wValue >> 8;
    b[4] = wIndex & 0xFF;  b[5] = wIndex >> 8;
    b[6] = wLength & 0xFF; b[7] = wLength >> 8;
    if (data && wLength && !(bmReqType & 0x80)) memcpy(b + 8, data, wLength);
    t->num_bytes        = 8 + wLength;
    t->device_handle    = s_dev;
    t->bEndpointAddress = 0;
    t->callback         = msc_xfer_cb;
    t->context          = NULL;
    s_msc_done_flag = false;
    xSemaphoreTake(s_msc_done, 0);
    bool ok = false;
    if (usb_host_transfer_submit_control(s_client, t) == ESP_OK) {
        ok = msc_wait_done(MSC_IO_TIMEOUT_MS)
          && t->status == USB_TRANSFER_STATUS_COMPLETED;
        if (ok && data && wLength && (bmReqType & 0x80)) memcpy(data, b + 8, wLength);
    }
    usb_host_transfer_free(t);   // same freed-after-timeout exposure as ctrl_req
    return ok;
}

// Clear a bulk-endpoint stall: host side (halt+flush+clear resets the data
// toggle) and wire side (CLEAR_FEATURE so the device resets its toggle too).
static void msc_clear_stall(uint8_t ep) {
    usb_host_endpoint_halt(s_dev, ep);
    usb_host_endpoint_flush(s_dev, ep);
    msc_ctrl(0x02, 0x01 /*CLEAR_FEATURE(ENDPOINT_HALT)*/, 0, ep, NULL, 0);
    usb_host_endpoint_clear(s_dev, ep);
}

// BOT §5.3.4 reset recovery: Bulk-Only Mass Storage Reset + both stalls.
static void msc_bot_reset() {
    if (!s_dev) return;
    ulog("msc: BOT reset");
    msc_ctrl(0x21, 0xFF /*Bulk-Only Reset*/, 0, s_msc_prof.ifnum, NULL, 0);
    msc_clear_stall(s_msc_prof.ep_in);
    msc_clear_stall(s_msc_prof.ep_out);
}

// One full SCSI transaction: CBW → optional data phase (≤ MSC_CHUNK_BYTES via
// s_msc_xf->data_buffer; OUT payloads pre-loaded by the caller, IN payloads
// read out by the caller) → CSW. True only on CSW GOOD with a clean data
// phase. `data_in_len` receives the actual IN byte count when non-NULL.
static bool msc_scsi(const uint8_t* cdb, uint8_t cdb_len,
                     bool dir_in, uint32_t data_len, uint32_t* data_in_len) {
    if (!s_dev || !s_msc_xf || !s_msc_xf_cmd) return false;

    // CBW (31 bytes)
    uint8_t* b = s_msc_xf_cmd->data_buffer;
    uint32_t tag = s_msc_tag++;
    memset(b, 0, 31);
    b[0]='U'; b[1]='S'; b[2]='B'; b[3]='C';
    b[4] = (uint8_t)tag; b[5] = (uint8_t)(tag>>8);
    b[6] = (uint8_t)(tag>>16); b[7] = (uint8_t)(tag>>24);
    b[8]  = (uint8_t)data_len;        b[9]  = (uint8_t)(data_len>>8);
    b[10] = (uint8_t)(data_len>>16);  b[11] = (uint8_t)(data_len>>24);
    b[12] = dir_in ? 0x80 : 0x00;
    b[13] = 0;                        // LUN 0
    b[14] = cdb_len;
    memcpy(b + 15, cdb, cdb_len);
    if (msc_bulk(s_msc_xf_cmd, s_msc_prof.ep_out, 31) != USB_TRANSFER_STATUS_COMPLETED) {
        msc_bot_reset();
        return false;
    }

    // Data phase
    uint32_t in_got  = 0;
    bool     data_ok = true;
    if (data_len) {
        uint8_t ep = dir_in ? s_msc_prof.ep_in : s_msc_prof.ep_out;
        int wire = (int)data_len;
        if (dir_in) {                 // IN wire length must be MPS-multiple
            int mps = s_msc_prof.mps_in ? s_msc_prof.mps_in : 64;
            wire = ((wire + mps - 1) / mps) * mps;
        }
        usb_transfer_status_t st = msc_bulk(s_msc_xf, ep, wire);
        if (st == USB_TRANSFER_STATUS_COMPLETED) {
            in_got = s_msc_xf->actual_num_bytes;
        } else if (st == USB_TRANSFER_STATUS_STALL) {
            // Device flags the error in the data phase; clear and read the
            // CSW, which carries the real status.
            msc_clear_stall(ep);
            data_ok = false;
        } else {
            msc_bot_reset();
            return false;
        }
    }

    // CSW (13 bytes; submit MPS-sized, completes short)
    int csw_wire = s_msc_prof.mps_in ? s_msc_prof.mps_in : 64;
    usb_transfer_status_t st = msc_bulk(s_msc_xf_cmd, s_msc_prof.ep_in, csw_wire);
    if (st == USB_TRANSFER_STATUS_STALL) {        // one retry per BOT spec
        msc_clear_stall(s_msc_prof.ep_in);
        st = msc_bulk(s_msc_xf_cmd, s_msc_prof.ep_in, csw_wire);
    }
    uint8_t* c = s_msc_xf_cmd->data_buffer;
    uint32_t rtag = (uint32_t)c[4] | ((uint32_t)c[5]<<8)
                  | ((uint32_t)c[6]<<16) | ((uint32_t)c[7]<<24);
    if (st != USB_TRANSFER_STATUS_COMPLETED ||
        s_msc_xf_cmd->actual_num_bytes < 13 ||
        c[0]!='U' || c[1]!='S' || c[2]!='B' || c[3]!='S' || rtag != tag) {
        msc_bot_reset();
        return false;
    }
    if (c[12] == 2) { msc_bot_reset(); return false; }   // phase error
    if (data_in_len) *data_in_len = in_got;
    return data_ok && c[12] == 0;                        // 0 = GOOD, 1 = failed
}

// REQUEST SENSE: fetch + log why the last command failed (clears the unit's
// pending sense). Caller holds the same context as the failed command.
static void scsi_request_sense() {
    uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint32_t got = 0;
    if (msc_scsi(cdb, 6, true, 18, &got) && got >= 14) {
        uint8_t* d = s_msc_xf->data_buffer;
        ulog("msc: sense key %X asc %02X/%02X", d[2] & 0x0F, d[12], d[13]);
    }
}

// ── Public sector API (any task; see usb_manager.h) ─────────────────────────

bool     usb_msc_ready()        { return s_msc_on; }
uint32_t usb_msc_sector_count() { return s_msc_sectors; }
uint32_t usb_msc_sector_size()  { return s_msc_ssize; }

bool usb_msc_read(uint32_t lba, uint32_t count, uint8_t* buf) {
    if (!s_msc_on || !s_msc_ssize) return false;
    if (xSemaphoreTake(s_msc_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) return false;
    uint32_t per = MSC_CHUNK_BYTES / s_msc_ssize;
    bool ok = true;
    while (count && ok) {
        uint32_t n = count > per ? per : count;
        uint32_t len = n * s_msc_ssize;
        uint8_t cdb[10] = { 0x28, 0,
            (uint8_t)(lba>>24), (uint8_t)(lba>>16), (uint8_t)(lba>>8), (uint8_t)lba,
            0, (uint8_t)(n>>8), (uint8_t)n, 0 };
        uint32_t got = 0;
        ok = s_msc_on && msc_scsi(cdb, 10, true, len, &got) && got >= len;
        if (ok) {
            memcpy(buf, s_msc_xf->data_buffer, len);
            buf += len; lba += n; count -= n;
        } else if (s_msc_on) {
            scsi_request_sense();
        }
    }
    xSemaphoreGive(s_msc_mutex);
    return ok;
}

bool usb_msc_write(uint32_t lba, uint32_t count, const uint8_t* buf) {
    if (!s_msc_on || !s_msc_ssize) return false;
    if (xSemaphoreTake(s_msc_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) return false;
    uint32_t per = MSC_CHUNK_BYTES / s_msc_ssize;
    bool ok = true;
    while (count && ok) {
        uint32_t n = count > per ? per : count;
        uint32_t len = n * s_msc_ssize;
        if (!s_msc_on || !s_msc_xf) { ok = false; break; }
        memcpy(s_msc_xf->data_buffer, buf, len);
        uint8_t cdb[10] = { 0x2A, 0,
            (uint8_t)(lba>>24), (uint8_t)(lba>>16), (uint8_t)(lba>>8), (uint8_t)lba,
            0, (uint8_t)(n>>8), (uint8_t)n, 0 };
        ok = msc_scsi(cdb, 10, false, len, NULL);
        if (ok) { buf += len; lba += n; count -= n; }
        else if (s_msc_on) scsi_request_sense();
    }
    xSemaphoreGive(s_msc_mutex);
    return ok;
}

bool usb_msc_sync() {
    if (!s_msc_on) return false;
    if (xSemaphoreTake(s_msc_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) return false;
    uint8_t cdb[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    bool ok = msc_scsi(cdb, 10, false, 0, NULL);
    if (!ok && s_msc_on) scsi_request_sense();
    xSemaphoreGive(s_msc_mutex);
    return true;   // tolerated: many sticks stub SYNCHRONIZE CACHE
}

// ── MSC lifecycle (usb_task context) ────────────────────────────────────────

static void msc_abort_start() {
    if (s_msc_xf)     { usb_host_transfer_free(s_msc_xf);     s_msc_xf = nullptr; }
    if (s_msc_xf_cmd) { usb_host_transfer_free(s_msc_xf_cmd); s_msc_xf_cmd = nullptr; }
    if (s_dev) usb_host_interface_release(s_client, s_dev, s_msc_prof.ifnum);
    s_msc_prof.valid = false;      // don't retry-spin
}

static void msc_start() {
    if (!s_dev || !s_msc_prof.valid || s_msc_on) return;
    esp_err_t err = usb_host_interface_claim(s_client, s_dev, s_msc_prof.ifnum, 0);
    if (err != ESP_OK) {
        ulog("msc claim IF%u failed: %s", s_msc_prof.ifnum, esp_err_to_name(err));
        s_msc_prof.valid = false;
        return;
    }
    if (usb_host_transfer_alloc(MSC_CHUNK_BYTES, 0, &s_msc_xf) != ESP_OK ||
        usb_host_transfer_alloc(64, 0, &s_msc_xf_cmd) != ESP_OK) {
        ulog("msc transfer alloc failed (%dK internal)", MSC_CHUNK_BYTES / 1024);
        msc_abort_start();
        return;
    }

    // Get Max LUN — informational, we drive LUN 0 only; STALL means "1 LUN".
    uint8_t maxlun = 0;
    if (msc_ctrl(0xA1, 0xFE, 0, s_msc_prof.ifnum, &maxlun, 1) && maxlun > 0)
        ulog("msc: %u LUNs (using 0)", maxlun + 1);

    // INQUIRY (informational: vendor/product)
    {
        uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
        uint32_t got = 0;
        if (msc_scsi(cdb, 6, true, 36, &got) && got >= 32) {
            char v[9] = {0}, p[17] = {0};
            memcpy(v, s_msc_xf->data_buffer + 8, 8);
            memcpy(p, s_msc_xf->data_buffer + 16, 16);
            ulog("msc: %s %s", v, p);
        }
    }

    // TEST UNIT READY — sticks spin up / raise a unit-attention that a
    // REQUEST SENSE clears; retry with backoff.
    bool ready = false;
    for (int i = 0; i < 10 && !ready; i++) {
        uint8_t cdb[6] = { 0, 0, 0, 0, 0, 0 };
        ready = msc_scsi(cdb, 6, false, 0, NULL);
        if (!ready) { scsi_request_sense(); vTaskDelay(pdMS_TO_TICKS(500)); }
    }
    if (!ready) { ulog("msc: unit never became ready"); msc_abort_start(); return; }

    // READ CAPACITY(10)
    {
        uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        uint32_t got = 0;
        if (!msc_scsi(cdb, 10, true, 8, &got) || got < 8) {
            ulog("msc: read capacity failed"); msc_abort_start(); return;
        }
        uint8_t* d = s_msc_xf->data_buffer;
        uint32_t last = ((uint32_t)d[0]<<24) | ((uint32_t)d[1]<<16)
                      | ((uint32_t)d[2]<<8)  | d[3];
        uint32_t ss   = ((uint32_t)d[4]<<24) | ((uint32_t)d[5]<<16)
                      | ((uint32_t)d[6]<<8)  | d[7];
        if (last == 0xFFFFFFFF) { ulog("msc: >2TB unsupported"); msc_abort_start(); return; }
        if (ss < 512 || ss > 4096) { ulog("msc: sector size %u unsupported", (unsigned)ss); msc_abort_start(); return; }
        s_msc_sectors = last + 1;
        s_msc_ssize   = ss;
    }

    s_msc_on = true;
    ulog(">>> Storage ready: %u MB (%uB sectors)",
         (unsigned)(((uint64_t)s_msc_sectors * s_msc_ssize) / (1024 * 1024)),
         (unsigned)s_msc_ssize);

    // FatFs mount (usb_fs.cpp) — its sector reads land back in usb_msc_read
    // on THIS task and self-pump. Failure (exFAT/unformatted) is logged
    // there; the device stays listed either way.
    usb_fs_mount();
}

static void msc_stop(bool dev_present) {
    (void)dev_present;
    if (!s_msc_xf && !s_msc_xf_cmd && !s_msc_on) return;
    s_msc_on = false;                 // new transactions refuse from here on
    // Force any in-flight phase to complete so its owner fails fast.
    if (s_dev && s_msc_prof.ep_in) {
        usb_host_endpoint_halt(s_dev, s_msc_prof.ep_in);
        usb_host_endpoint_flush(s_dev, s_msc_prof.ep_in);
        usb_host_endpoint_halt(s_dev, s_msc_prof.ep_out);
        usb_host_endpoint_flush(s_dev, s_msc_prof.ep_out);
    }
    // Acquire the transaction mutex while PUMPING (the owner's completion and
    // failure path need usb_task's event handling to keep flowing). Hold it
    // across the teardown so no late caller touches freed transfers.
    bool have_mutex = false;
    for (int i = 0; i < 600 && !have_mutex; i++) {
        have_mutex = (xSemaphoreTake(s_msc_mutex, 0) == pdTRUE);
        if (!have_mutex) {
            uint32_t f = 0;
            usb_host_lib_handle_events(0, &f);
            usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
        }
    }
    usb_fs_unmount();
    if (have_mutex) {
        if (s_msc_xf)     { usb_host_transfer_free(s_msc_xf);     s_msc_xf = nullptr; }
        if (s_msc_xf_cmd) { usb_host_transfer_free(s_msc_xf_cmd); s_msc_xf_cmd = nullptr; }
    } else {
        // Pathological: a transaction owner is still wedged after ~6s of
        // pumping. Leak the transfer objects (16KB internal) rather than
        // free them under a task that may still dereference them.
        ulog("msc: teardown timeout — leaking transfers");
        s_msc_xf = nullptr;
        s_msc_xf_cmd = nullptr;
    }
    if (s_dev) usb_host_interface_release(s_client, s_dev, s_msc_prof.ifnum);
    s_msc_sectors = 0;
    s_msc_ssize   = 0;
    if (have_mutex) xSemaphoreGive(s_msc_mutex);
    ulog("Storage unmounted.");
}

// ── Host task ────────────────────────────────────────────────────────────────

static void usb_task(void*) {
    s_usb_task_handle = xTaskGetCurrentTaskHandle();   // msc_wait_done context test
    // Install from THIS task so esp_intr_alloc binds a core-1 interrupt slot.
    usb_host_config_t host_cfg = {};
    host_cfg.skip_phy_setup = false;
    host_cfg.intr_flags     = 0;
    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        ulog("usb_host_install failed: %s", esp_err_to_name(err));
        s_running = false; vTaskDelete(NULL); return;
    }

    usb_host_client_config_t client_cfg = {};
    client_cfg.is_synchronous              = false;
    client_cfg.max_num_event_msg           = 5;
    client_cfg.async.client_event_callback = client_event_cb;
    client_cfg.async.callback_arg          = NULL;
    err = usb_host_client_register(&client_cfg, &s_client);
    if (err != ESP_OK) {
        ulog("client_register failed: %s", esp_err_to_name(err));
        usb_host_uninstall(); s_client = NULL; s_running = false; vTaskDelete(NULL); return;
    }

    ulog("Host running — plug in a powered dongle.");

    while (!s_stop_req) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(0, &flags);
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(50));

        // Flash-write guard: drain in-flight ISO so a caller's flash write
        // (cache stall) can't strand a transfer, then idle until released. The
        // caller wakes us promptly via usb_host_client_unblock(). The keyboard
        // endpoint is parked too (halt+flush — an idle IN never completes on
        // its own) so its transfer also drains before the stall.
        if (s_flash_pause_req && !s_flash_paused) {
            if (s_hid_on && s_hid_busy > 0) hid_flash_park();
            for (int i = 0; i < 100 && (s_xf_busy > 0 || s_hid_busy > 0); i++) {
                uint32_t f = 0;
                usb_host_lib_handle_events(0, &f);
                usb_host_client_handle_events(s_client, pdMS_TO_TICKS(2));
            }
            s_flash_paused = true;
        } else if (!s_flash_pause_req && s_flash_paused) {
            if (s_stream_on) resume_iso();
            if (s_hid_on) hid_resume();
            s_flash_paused = false;
        }

        if (s_new_addr) {
            uint8_t addr = s_new_addr;
            s_new_addr = 0;
            if (!s_dev) describe_device(addr);
        }
        if (s_dev_gone) {
            s_dev_gone = false;
            ulog("Device disconnected.");
            if (s_stream_on) audio_stream_stop(false);
            if (s_hid_on) hid_kbd_stop(false);
            if (s_msc_on || s_msc_xf) msc_stop(false);
            if (s_dev) { usb_host_device_close(s_client, s_dev); s_dev = NULL; }
            s_prof.valid = false;
            s_kbd_prof.valid = false;
            s_msc_prof.valid = false;
            memset(&s_info, 0, sizeof(s_info));
        }

        // Streaming should run whenever an audio device is present and either
        // routing is enabled or the debug tone is on. Don't START while a flash
        // guard is active (a fresh stream mid-flash-write would crash); the
        // guard releases in a few ms and streaming starts on the next pass.
        bool want = s_prof.valid && (s_route_pref || s_tone_want);
        if (want && !s_stream_on && !s_flash_pause_req) audio_stream_start();
        else if (!want && s_stream_on)                  audio_stream_stop(true);

        // Keyboard: claim + arm as soon as a boot-keyboard profile exists
        // (same no-start-mid-guard rule as audio; no pref — plugging a
        // keyboard into a manually-started host is deliberate).
        if (s_kbd_prof.valid && !s_hid_on && !s_flash_pause_req) hid_kbd_start();
        if (s_hid_on) hid_arrow_tick();
        if (s_hid_on && s_led_pending && !s_flash_pause_req) hid_led_update();

        // Storage: claim + SCSI init + FatFs mount when a BOT profile exists.
        // Blocks usb_task for the init (TUR retries on a slow stick) — fine:
        // single-device host, so nothing else is attached to service.
        if (s_msc_prof.valid && !s_msc_on && !s_flash_pause_req) msc_start();

        if (s_tone_want && s_stream_on) tone_feed();
    }

    // Teardown in the order usb_host.h requires.
    if (s_stream_on) audio_stream_stop(true);
    if (s_hid_on) hid_kbd_stop(true);
    if (s_msc_on || s_msc_xf) msc_stop(true);
    if (s_dev) { usb_host_device_close(s_client, s_dev); s_dev = NULL; }
    usb_host_client_deregister(s_client);
    s_client = NULL;
    usb_host_device_free_all();
    for (int i = 0; i < 40; i++) {
        uint32_t f = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(50), &f);
        if (f & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) break;
    }
    esp_err_t uerr = usb_host_uninstall();
    restore_serial_jtag_phy();
    memset(&s_info, 0, sizeof(s_info));
    ulog("Host stopped (%s). USB serial restored", uerr == ESP_OK ? "clean" : esp_err_to_name(uerr));
    ulog("(replug the PC cable if it doesn't show).");

    s_running = false;
    vTaskDelete(NULL);
}

// ── Lifecycle / prefs ────────────────────────────────────────────────────────

void usb_manager_init(void (*prefs_save_fn)()) {
    s_prefs_save = prefs_save_fn;
    // Recursive: guarded call chains nest (e.g. a guarded prefs save calling
    // meshpunk_open, which guards its own LittleFS truncate). Only the
    // OUTERMOST level pauses/resumes the stream — see s_flash_guard_depth.
    s_flash_mux  = xSemaphoreCreateRecursiveMutex();
    s_msc_mutex  = xSemaphoreCreateMutex();          // priority inheritance
    s_msc_done   = xSemaphoreCreateBinary();
}

// ── Flash-write guard (see header) ───────────────────────────────────────────

// Nesting depth of the current holder. Mutated only while s_flash_mux is held,
// so no extra locking needed.
static int s_flash_guard_depth = 0;

void usb_flash_guard_begin() {
    if (!s_flash_mux) return;                 // pre-init: USB can't be up yet
    xSemaphoreTakeRecursive(s_flash_mux, portMAX_DELAY);   // held until end()
    if (++s_flash_guard_depth > 1) return;    // nested: outer level already paused
    if (!s_running) return;                   // no usb_task — nothing to coordinate
    // Always coordinate with the task when it's alive (even if not streaming
    // yet): the pause_req also blocks a stream from STARTING mid-write.
    s_flash_pause_req = true;
    if (s_client) usb_host_client_unblock(s_client);   // wake the usb_task now
    // Wait for it to drain + park the ISO stream (bounded; drain is ~<=30ms).
    // Also unblocks early if the task is tearing down (s_running clears).
    for (int i = 0; i < 300 && !s_flash_paused && s_running; i++)
        vTaskDelay(pdMS_TO_TICKS(1));
    // The usb_task can sit in a >300ms control-transfer wait during
    // enumeration and miss the park request. The ISO callbacks still see
    // pause_req and stop resubmitting, so wait (bounded) for the in-flight
    // count itself before letting the caller stall the flash cache.
    if (s_running && !s_flash_paused) {
        for (int i = 0; i < 1700 && s_xf_busy > 0 && s_running; i++)
            vTaskDelay(pdMS_TO_TICKS(1));
        if (s_xf_busy > 0)
            ulog("flash guard: ISO drain timed out — write proceeding");
    }
}

void usb_flash_guard_end() {
    if (!s_flash_mux) return;
    if (s_flash_guard_depth > 0 && --s_flash_guard_depth == 0) {
        if (s_flash_pause_req) {
            s_flash_pause_req = false;
            if (s_running && s_client) usb_host_client_unblock(s_client);
        }
    }
    xSemaphoreGiveRecursive(s_flash_mux);
}

bool usb_manager_start() {
    if (s_running) return true;

    if (!s_ring) {
        s_ring = (int16_t*)heap_caps_malloc(SINK_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!s_ring) { ulog("sink alloc failed (no PSRAM)"); return false; }
    }
    s_ring_head = s_ring_tail = 0;

    ulog("Switching USB port to HOST mode.");
    ulog("(USB serial is OFF until Stop or reboot)");

    s_stop_req = false; s_new_addr = 0; s_dev_gone = false;
    s_tone_want = false; s_client = NULL; s_running = true;
    s_kbd_prof.valid = false; s_hid_on = false; s_hid_busy = 0;
    s_msc_prof.valid = false; s_msc_on = false;
    // 8KB stack. A 6KB trim caused crashes ("works then dies after a short
    // time") — the USB host library's enumeration/transfer callbacks run on
    // THIS stack and go deeper than 6KB. Keep the known-good 8KB; the periodic
    // high-water log below tells us the real margin before trimming again.
    if (xTaskCreatePinnedToCore(usb_task, "usb_mgr", 8192, NULL, 4, NULL, 1) != pdPASS) {
        ulog("task create failed");
        s_running = false;
        return false;
    }
    return true;
}

void usb_manager_stop() { if (s_running) s_stop_req = true; }
bool usb_manager_running() { return s_running; }

bool usb_audio_pref_get() { return s_route_pref; }
void usb_audio_pref_set(bool on) { s_route_pref = on; }
bool usb_speaker_pref_get() { return s_speaker_pref; }
void usb_speaker_pref_set(bool on) { s_speaker_pref = on; }

// Pipeline-A consumer (keyboard_read_cb, core 0): copy the USB-held key set.
bool usb_kbd_snapshot(bool out[128]) {
    if (s_usb_nheld == 0) return false;       // fast path: nothing held
    portENTER_CRITICAL(&s_kbd_mux);
    memcpy(out, s_usb_held, sizeof(s_usb_held));
    portEXIT_CRITICAL(&s_kbd_mux);
    return true;
}

// ── Lua bridge ───────────────────────────────────────────────────────────────

void usb_manager_register_lua(lua_State* L) {
    // Boot self-heal: a reset while host mode was active leaves the RTC PHY
    // select on OTG (USB serial dead). Guard so re-registration after an ELF
    // Lua re-init doesn't stomp an active session.
    if (!s_running) restore_serial_jtag_phy();

    lua_register(L, "_usb_start",   [](lua_State* L) -> int { lua_pushboolean(L, usb_manager_start());   return 1; });
    lua_register(L, "_usb_stop",    [](lua_State* L) -> int { usb_manager_stop();                        return 0; });
    lua_register(L, "_usb_running", [](lua_State* L) -> int { lua_pushboolean(L, usb_manager_running()); return 1; });

    // nil, or a table describing the connected device for the UI.
    lua_register(L, "_usb_device", [](lua_State* L) -> int {
        if (!s_info.connected) { lua_pushnil(L); return 1; }
        lua_newtable(L);
        lua_pushinteger(L, s_info.vid);          lua_setfield(L, -2, "vid");
        lua_pushinteger(L, s_info.pid);          lua_setfield(L, -2, "pid");
        lua_pushstring(L, s_info.product);       lua_setfield(L, -2, "product");
        lua_pushstring(L, kind_name(s_info.kind)); lua_setfield(L, -2, "kind");
        lua_pushboolean(L, s_info.has_kbd);      lua_setfield(L, -2, "kbd");
        lua_pushboolean(L, usb_fs_mounted());    lua_setfield(L, -2, "msc");
        lua_pushnumber(L, s_msc_on ? ((double)s_msc_sectors * s_msc_ssize) / (1024.0 * 1024.0) : 0);
        lua_setfield(L, -2, "msc_mb");
        lua_pushinteger(L, s_info.rate);         lua_setfield(L, -2, "rate");
        lua_pushinteger(L, s_info.bits);         lua_setfield(L, -2, "bits");
        lua_pushboolean(L, usb_audio_active());  lua_setfield(L, -2, "streaming");
        return 1;
    });

    lua_register(L, "_usb_audio_get", [](lua_State* L) -> int { lua_pushboolean(L, s_route_pref); return 1; });
    lua_register(L, "_usb_audio_set", [](lua_State* L) -> int {
        s_route_pref = lua_toboolean(L, 1);
        if (s_prefs_save) s_prefs_save();
        lua_pushboolean(L, s_route_pref);
        return 1;
    });
    lua_register(L, "_usb_speaker_get", [](lua_State* L) -> int { lua_pushboolean(L, s_speaker_pref); return 1; });
    lua_register(L, "_usb_speaker_set", [](lua_State* L) -> int {
        s_speaker_pref = lua_toboolean(L, 1);
        if (s_prefs_save) s_prefs_save();
        lua_pushboolean(L, s_speaker_pref);
        return 1;
    });

    // Debug tone toggle.
    lua_register(L, "_usb_tone", [](lua_State* L) -> int {
        s_tone_want = !s_tone_want;
        lua_pushboolean(L, s_tone_want);
        return 1;
    });

    // One log line per call, nil when drained.
    lua_register(L, "_usb_poll", [](lua_State* L) -> int {
        char line[ULOG_LINE_LEN];
        if (ulog_pop(line)) lua_pushstring(L, line);
        else                lua_pushnil(L);
        return 1;
    });
}
