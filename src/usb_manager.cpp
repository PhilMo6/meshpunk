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

// ── PCM sink: rate-converting ring between sound.cpp and the ISO stream ─────
// Producers: sound.cpp mixer tap (core 1) and Audio-lib file tap (core 0) —
// never simultaneous (the mixer task idles while a file plays), but on
// different cores, so the ring is guarded by a portMUX spinlock. Samples are
// resampled to the dongle rate at push time and stored stereo. Consumer: the
// ISO refill callback in usb_task. Ring lives in PSRAM.

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
    bool have_ac = false, have_as = false;
    int  best_score = -1;

    bool     cur_as = false;
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
            bool is_iso = USB_EP_DESC_GET_XFERTYPE(e) == USB_TRANSFER_TYPE_ISOCHRONOUS;
            bool is_out = USB_EP_DESC_GET_EP_DIR(e) == 0;
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
        if (s_info.kind == UKIND_HUB)
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

// ── Host task ────────────────────────────────────────────────────────────────

static void usb_task(void*) {
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
        // caller wakes us promptly via usb_host_client_unblock().
        if (s_flash_pause_req && !s_flash_paused) {
            for (int i = 0; i < 100 && s_xf_busy > 0; i++) {
                uint32_t f = 0;
                usb_host_lib_handle_events(0, &f);
                usb_host_client_handle_events(s_client, pdMS_TO_TICKS(2));
            }
            s_flash_paused = true;
        } else if (!s_flash_pause_req && s_flash_paused) {
            if (s_stream_on) resume_iso();
            s_flash_paused = false;
        }

        // Stack margin watch (StackType_t is 1 byte on ESP-IDF, so the high
        // water mark is already in bytes). Tells us the real headroom before
        // we consider trimming the 8KB stack again.
        static uint32_t s_last_hwm = 0;
        uint32_t now_hwm = millis();
        if (now_hwm - s_last_hwm > 5000) {
            s_last_hwm = now_hwm;
            ulog("stack free-min %u B, int heap %uK",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
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
            if (s_dev) { usb_host_device_close(s_client, s_dev); s_dev = NULL; }
            s_prof.valid = false;
            memset(&s_info, 0, sizeof(s_info));
        }

        // Streaming should run whenever an audio device is present and either
        // routing is enabled or the debug tone is on. Don't START while a flash
        // guard is active (a fresh stream mid-flash-write would crash); the
        // guard releases in a few ms and streaming starts on the next pass.
        bool want = s_prof.valid && (s_route_pref || s_tone_want);
        if (want && !s_stream_on && !s_flash_pause_req) audio_stream_start();
        else if (!want && s_stream_on)                  audio_stream_stop(true);

        if (s_tone_want && s_stream_on) tone_feed();
    }

    // Teardown in the order usb_host.h requires.
    if (s_stream_on) audio_stream_stop(true);
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
    s_flash_mux  = xSemaphoreCreateMutex();
}

// ── Flash-write guard (see header) ───────────────────────────────────────────

void usb_flash_guard_begin() {
    if (!s_flash_mux) return;                 // pre-init: USB can't be up yet
    xSemaphoreTake(s_flash_mux, portMAX_DELAY);   // serialize; held until end()
    if (!s_running) return;                   // no usb_task — nothing to coordinate
    // Always coordinate with the task when it's alive (even if not streaming
    // yet): the pause_req also blocks a stream from STARTING mid-write.
    s_flash_pause_req = true;
    if (s_client) usb_host_client_unblock(s_client);   // wake the usb_task now
    // Wait for it to drain + park the ISO stream (bounded; drain is ~<=30ms).
    // Also unblocks early if the task is tearing down (s_running clears).
    for (int i = 0; i < 300 && !s_flash_paused && s_running; i++)
        vTaskDelay(pdMS_TO_TICKS(1));
}

void usb_flash_guard_end() {
    if (!s_flash_mux) return;
    if (s_flash_pause_req) {
        s_flash_pause_req = false;
        if (s_running && s_client) usb_host_client_unblock(s_client);
    }
    xSemaphoreGive(s_flash_mux);
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
