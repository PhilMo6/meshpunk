// USB class driver: UAC 1.0 audio out (DAC dongles) — CORE-PRIVILEGED.
//
// Audio is welded to the firmware on both ends (sound.cpp taps feed the PCM
// sink; ISO packet descriptors + the cached-descriptor MPS patch need raw
// host access), so unlike the HID/MSC drivers this file uses usb_core_int.h
// internals directly. It still attaches through the same UsbDriverDesc
// lifecycle as every other driver — probe/want/start/stop/busy/resume/tick —
// so the registry loop treats it uniformly.
//
// Hardware-validated quirks preserved verbatim from the monolith:
//   - patch the CACHED config descriptor before claiming an ISO endpoint:
//     clamp wMaxPacketSize > 192 (DWC balanced-FIFO periodic-OUT limit in the
//     prebuilt IDF 4.4 libs) and bInterval 0 -> 1, else the claim returns
//     ESP_ERR_NOT_SUPPORTED; cap the sample rate at 48 kHz for the same
//     reason;
//   - ISO transfers park around flash writes by DECLINING resubmit (the
//     callback sees flash_pause_req), never by halt/flush.

#include "../usb_manager.h"
#include "usb_core_int.h"

#include <Arduino.h>
#include <math.h>
#include <esp_heap_caps.h>
#include "../sound.h"               // system volume/mute applied at playout

#define ulog usbcore_log

// ── UAC streaming profile (filled by probe) ─────────────────────────────────

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
// Producers: sound.cpp's sound_task (core 1). Consumer: the ISO refill
// callback in usb_task (core 1, prio 4 — preempts the producer), hence the
// portMUX spinlock. Samples are resampled to the dongle rate at push time and
// stored stereo. Ring lives in PSRAM.

#define SINK_FRAMES 8192                 // power of two; ~170 ms at 48 kHz
#define SINK_MASK   (SINK_FRAMES - 1)

static int16_t*          s_ring = nullptr;   // [SINK_FRAMES*2] stereo, PSRAM
static volatile uint32_t s_ring_head = 0;    // producer writes
static volatile uint32_t s_ring_tail = 0;    // consumer reads
static portMUX_TYPE      s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

// Linear resampler with async rate conversion (producer side). The producer
// clock (speaker I2S / file rate) and the dongle clock are independent
// crystals, so a FIXED ratio drifts the ring empty (crackle) or full (drops).
// The step is nudged each push toward holding the ring near USB_TARGET_MS.
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

// Debug tone generator (dongle-rate sine into the same sink).
#define TONE_HZ  440.0f
#define TONE_AMP 5000.0f
static float s_tone_phase = 0;

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
        (void)bytes;

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
    if (s_stream_on && !usbcore_flash_pause_req()
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
        usb_host_client_handle_events(usbcore_client(), pdMS_TO_TICKS(10));
    }
    for (int i = 0; i < ISO_XFERS; i++) {
        if (s_xf[i]) { usb_host_transfer_free(s_xf[i]); s_xf[i] = NULL; }
    }
    s_xf_busy = 0;
    if (usbcore_dev()) {
        if (dev_present)
            usbcore_ctrl_req(0x01, 11 /*SET_INTERFACE*/, 0, s_prof.ifnum, NULL, 0);
        usb_host_interface_release(usbcore_client(), usbcore_dev(), s_prof.ifnum);
    }
    ulog("Audio streaming stopped.");
}

// Returns true when the stream is up (registry `running`); false = retry on
// a later pass (the monolith's audio never invalidated its profile on a
// transient claim/alloc failure, so neither do we).
static bool audio_stream_start() {
    if (!usbcore_dev() || !s_prof.valid) return false;
    int ch = s_prof.channels ? s_prof.channels : 2;
    uint32_t bpms = ((s_prof.rate + 999) / 1000) * ch * 2;
    if (bpms > s_prof.mps) {
        ulog("stream: %u B/ms exceeds ep mps %u", (unsigned)bpms, s_prof.mps);
        return false;
    }

    esp_err_t err = usb_host_interface_claim(usbcore_client(), usbcore_dev(),
                                             s_prof.ifnum, s_prof.alt);
    if (err != ESP_OK) {
        ulog("claim IF%u alt%u failed: %s", s_prof.ifnum, s_prof.alt, esp_err_to_name(err));
        return false;
    }
    if (!usbcore_ctrl_req(0x01, 11 /*SET_INTERFACE*/, s_prof.alt, s_prof.ifnum, NULL, 0))
        ulog("SET_INTERFACE failed (continuing)");
    uint8_t r3[3] = { (uint8_t)s_prof.rate, (uint8_t)(s_prof.rate >> 8), (uint8_t)(s_prof.rate >> 16) };
    if (!usbcore_ctrl_req(0x22, 0x01 /*SET_CUR*/, 0x0100 /*SAMPLING_FREQ*/, s_prof.ep, r3, 3))
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
        s_xf[i]->device_handle    = usbcore_dev();
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
        usb_host_interface_release(usbcore_client(), usbcore_dev(), s_prof.ifnum);
        return false;
    }
    s_xf_busy = made;
    s_stream_on = true;
    ulog("Audio streaming @ %u Hz, %dch (%d xfers)", (unsigned)s_prof.rate, ch, made);
    return true;
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

// ── Probe (usb_task; walks the cached config descriptor) ────────────────────
// Classify the audio function, pick the best ISO-OUT alt (fits-192B-FIFO +2,
// 16-bit +1), choose the rate (44.1k preferred, capped 48k), and patch the
// cached endpoint descriptor for the DWC FIFO limit — all exactly as the
// monolith's describe_device audio arm did.

static bool aud_probe(const UsbHostApi* api) {
    memset(&s_prof, 0, sizeof(s_prof));
    s_stream_on = false;
    s_xf_busy   = 0;
    for (int i = 0; i < ISO_XFERS; i++) s_xf[i] = NULL;

    const usb_config_desc_t* cfg = (const usb_config_desc_t*)api->config_desc();
    if (!cfg) return false;

    bool have_ac = false;
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
            cur_as = false;
            cur_if = i->bInterfaceNumber;
            cur_alt = i->bAlternateSetting;
            cur_ch = cur_sub = cur_bits = 0;
            cur_cont = false;
            cur_nrates = 0;
            if (i->bInterfaceClass == USB_CLASS_AUDIO) {
                if (i->bInterfaceSubClass == 0x01) { have_ac = true; s_prof.ac_ifnum = cur_if; }
                if (i->bInterfaceSubClass == 0x02) { cur_as = true; }
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
                    s_prof.bits      = cur_bits;
                    s_prof.nrates     = cur_nrates;
                    memcpy(s_prof.rates, cur_rates, sizeof(cur_rates));
                }
            }
        }
    }

    if (!(have_ac && s_prof.valid)) {
        s_prof.valid = false;
        return false;
    }

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

    // Malformed descriptors can advertise a 0 (or junk) sample rate, and the
    // rate feeds divisions in the resampler and the packet-per-ms math —
    // reject the profile instead of crashing on the first push.
    if (s_prof.rate < 8000) {
        ulog("bad sample rate %u — audio profile rejected", (unsigned)s_prof.rate);
        s_prof.valid = false;
        return false;
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

    usbcore_note_audio(s_prof.rate, s_prof.bits);
    ulog("AS: IF %u alt %u ep %02X mps %u, %uch %u-bit",
         s_prof.ifnum, s_prof.alt, s_prof.ep, s_prof.mps, s_prof.channels, s_prof.bits);
    ulog("rate: %u Hz%s", (unsigned)s_prof.rate, s_prof.continuous ? " (continuous)" : "");
    if (s_prof.subsize != 2)
        ulog("WARNING: no 16-bit alt (subframe %u)", s_prof.subsize);
    ulog(">>> Audio device ready%s.", s_route_pref ? " — routing" : " (routing off)");
    return true;
}

// ── Descriptor hooks ─────────────────────────────────────────────────────────

static bool aud_want(void) {
    return s_prof.valid && (s_route_pref || s_tone_want);
}

static bool aud_start(const UsbHostApi*)               { return audio_stream_start(); }
static void aud_stop(const UsbHostApi*, bool present)  { audio_stream_stop(present); }
static int  aud_busy(void)                             { return s_xf_busy; }

static void aud_resume(void) {
    if (s_stream_on) resume_iso();
}

static void aud_tick(const UsbHostApi*) {
    if (s_tone_want && s_stream_on) tone_feed();
}

static void aud_status(char* out, uint32_t n) {
    snprintf(out, n, "%uHz/%ubit", (unsigned)s_prof.rate, s_prof.bits);
}

static const UsbDriverDesc s_aud_desc = {
    USB_DRIVER_ABI_VERSION,
    "audio",
    &aud_probe,
    &aud_want,
    &aud_start,
    &aud_stop,
    &aud_busy,
    NULL,           // park: ISO drains by declining resubmit in iso_xfer_cb
    &aud_resume,
    &aud_tick,
    &aud_status,
};

const UsbDriverDesc* usbaud_desc(void) { return &s_aud_desc; }

// ── Core-internal hooks (usb_core_int.h) ────────────────────────────────────

bool usbaud_session_reset(void) {
    if (!s_ring) {
        s_ring = (int16_t*)heap_caps_malloc(SINK_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!s_ring) return false;
    }
    s_ring_head = s_ring_tail = 0;
    s_tone_want = false;
    return true;
}

bool usbaud_tone_toggle(void) {
    s_tone_want = !s_tone_want;
    return s_tone_want;
}

int usbaud_iso_busy(void) { return s_xf_busy; }

// ── Prefs (public; persisted via firmware_prefs in main.cpp) ────────────────

bool usb_audio_pref_get() { return s_route_pref; }
void usb_audio_pref_set(bool on) { s_route_pref = on; }
bool usb_speaker_pref_get() { return s_speaker_pref; }
void usb_speaker_pref_set(bool on) { s_speaker_pref = on; }
