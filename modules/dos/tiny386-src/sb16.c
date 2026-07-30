/*
 * QEMU Soundblaster 16 emulation
 *
 * Copyright (c) 2003-2005 Vassili Karpov (malc)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "sb16.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "i8257.h"

#ifdef BUILD_ESP32
void *pcmalloc(long size);
#define TMPBUF_LEN 512
#else
#define pcmalloc malloc
/* MESHPUNK: overridable. Upstream picks 4096 for desktop and 512 under
   BUILD_ESP32, which we deliberately do not define -- so we were getting a 4KB
   stack frame per write_audio() call on a 64KB module stack, to move ~40 bytes.
   With the read now clamped to ring space this never needs to be large. */
#ifndef TMPBUF_LEN
#define TMPBUF_LEN 4096
#endif
#endif

#ifdef __wasm__
#define AUDIO_BUF_LEN 8192
#define AUDIO_BUF_LIMIT 4096
#define AUDIO_BUF_LIMIT_HIGH 8192
#define AUDIO_BUF_LIMIT_LOW 2048
#else
#define AUDIO_BUF_LEN 4096
/* MESHPUNK: these cap how much guest audio we queue ahead, so they ARE the
   latency between the game writing a sample and the panel-side hearing it.
   Upstream's desktop values buy safety we do not need: the ring is refilled
   from our own emulation loop every ~2.5ms, and the longest gap between refills
   is one vga_refresh (~15ms, measured -- it was ~30ms before direct-to-panel
   rendering removed the blit pass).
   LOW covers 8-bit / sub-22kHz, which is every DOS game: at 15800 B/s, 2048
   bytes was 130ms of lag -- and 260ms under a 1/2 rate divider, since the same
   buffer drains half as fast. 512 was tried and clicked: 32ms leaves only ~2x
   the worst refill gap, and normal jitter ate it. 1024 (65ms, ~4x margin) was
   clean, and still cuts the original latency fourfold.
   HIGH is left alone: 16-bit 44.1k stereo runs 176400 B/s, where 4096 bytes is
   only 23ms and shrinking it really would underrun.
   If this is too tight the [aud] counter says so directly -- starved/empty
   climb off zero. */
#define AUDIO_BUF_LIMIT 2048
#define AUDIO_BUF_LIMIT_HIGH 4096
#define AUDIO_BUF_LIMIT_LOW 1024
#endif

/* MESHPUNK: seam crossfade length for DMA concealment, in input bytes.
   256 (one full block, ~16ms). 32 was tried first and still clicked: a seam
   joins two different points in the melody, and a 2ms blend does not even
   cover half a period of the bass tones -- the ear still hears a step. At a
   full block the join is a quick morph instead. When misses chain (every
   block during the tune), each fade completes exactly as the next arms, so
   the stream stays continuously blended with no hard edges anywhere. */
#define MESHPUNK_CONCEAL_FADE 256

/* MESHPUNK: trailing-window re-read cap, in input bytes. The window never
   exceeds one block (see SB_read_DMA), so this covers the whole current block
   at the block sizes 8-bit DOS games use (MS Pac-Man: 256); it also bounds the
   stack buffer the re-read copies through. */
#define MESHPUNK_REREAD_MAX 512

#ifdef SB16_LOG
#define dolog(...) fprintf(stderr, "sb16: " __VA_ARGS__)
#define qemu_log_mask(_, ...) fprintf(stderr, "sb16: " __VA_ARGS__)
#else
#define dolog(...)
#define qemu_log_mask(_, ...)
#endif

/* #define DEBUG */
/* #define DEBUG_SB16_MOST */

#ifdef DEBUG
#define ldebug(...) dolog (__VA_ARGS__)
#else
#define ldebug(...)
#endif

typedef enum {
    AUDIO_FORMAT_U8,
    AUDIO_FORMAT_S8,
    AUDIO_FORMAT_U16,
    AUDIO_FORMAT_S16,
} AudioFormat;

static const char e3[] = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";

struct SB16State {
//    QEMUSoundCard card;
    void *pic;
    void (*set_irq)(void *pic, int irq, int level);
    uint32_t irq;
    uint32_t dma;
    uint32_t hdma;
    uint32_t port;
    uint32_t ver;
    IsaDma *isa_dma;
    IsaDma *isa_hdma;

    int in_index;
    int out_data_len;
    int fmt_stereo;
    int fmt_signed;
    int fmt_bits;
    AudioFormat fmt;
    int dma_auto;
    int block_size;
    int fifo;
    int freq;
    int time_const;
    int speaker;
    int needed_bytes;
    int cmd;
    int use_hdma;
    int highspeed;
    int can_write;

    int v2x6;

    uint8_t csp_param;
    uint8_t csp_value;
    uint8_t csp_mode;
    uint8_t csp_regs[256];
    uint8_t csp_index;
    uint8_t csp_reg83[4];
    int csp_reg83r;
    int csp_reg83w;

    uint8_t in2_data[10];
    uint8_t out_data[50];
    uint8_t test_reg;
    uint8_t last_read_byte;
    int nzero;

    int left_till_irq;

    int dma_running;
    /* MESHPUNK: see the priming gate in sb16_audio_callback. */
    int priming;
    /* MESHPUNK: DMA concealment state -- see SB_read_DMA. */
    int conceal;        /* nonzero while reading un-refilled (stale) data */
    int fade;           /* bytes of seam crossfade still to apply */
    uint8_t fade_from;  /* seam anchor: last byte before the seam */
    uint8_t last_byte;  /* last byte delivered to the ring */
    /* MESHPUNK: trailing-window re-read state -- see SB_read_DMA. */
    int late_writer;    /* guest observed refilling a half AFTER it was read */
    unsigned int reread_floor;  /* audio_q watermark: never patch below it */
    /* MESHPUNK: resampler phase carried ACROSS mixer calls -- see the note in
       resample_u8m. [0]=os [1]=is the pair the phase belongs to (0 = invalid),
       [2]=uc [3]=dc. */
    int rs_phase[4];
    int bytes_per_second;
    int align;
    int audio_free;
    uint8_t audio_buf[AUDIO_BUF_LEN];
    unsigned int audio_p, audio_q;
    void *voice;
    int active_out;

//    QEMUTimer *aux_ts;
    /* mixer state */
    int mixer_nreg;
    uint8_t mixer_regs[256];

    uint8_t e2_valadd;
    uint8_t e2_valxor;
};

static void AUD_set_active_out (SB16State *s, int i)
{
    s->active_out = i;
}

static void set_audio(void *s, int format, int freq, int nchan)
{
    dolog("audio fmt %d freq %d chan %d\n", format, freq, nchan);
}

static int magic_of_irq (int irq)
{
    switch (irq) {
    case 5:
        return 2;
    case 7:
        return 4;
    case 9:
        return 1;
    case 10:
        return 8;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "bad irq %d\n", irq);
        return 2;
    }
}

static int irq_of_magic (int magic)
{
    switch (magic) {
    case 1:
        return 9;
    case 2:
        return 5;
    case 4:
        return 7;
    case 8:
        return 10;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "bad irq magic %d\n", magic);
        return -1;
    }
}

/* MESHPUNK: SB digital mode, launcher "SB digital" (module arg -sbdigi).
   1 = ON, normal card.
   3 = NO DMA: DSP and interrupts work, but SB DMA transfers never progress
       -- the wrong-DMA-jumper condition. A game's DMA verification (a tiny
       test transfer with a completion timeout) fails deterministically,
       while the DSP handshake and the IRQ test both pass. Games with a
       distinct "SB present but DMA broken" branch keep music and disable
       digitised playback only.
   2 = NO IRQ: the DSP answers everything normally but the card's interrupt
       line never raises -- the classic wrong-IRQ-jumper condition on real
       hardware. Era games probe for exactly this (DSP command 0xF2 or a
       short DMA transfer, then wait for the IRQ) and fall back gracefully:
       "SB found, interrupt not responding, digitised sound disabled" --
       while FM music (Adlib, a separate chip) keeps working. Detection
       completes FAST because the DSP handshake succeeds; only the IRQ test
       fails, and it fails deterministically. The card-internal interrupt
       status (mixer_regs[0x82]) still sets, as on real hardware -- the card
       thinks it interrupted; the line is dead.
   0 = OFF: the DSP ports float (writes ignored, reads 0xFF) so the reset/
       detect handshake itself fails. Motive for all reduced modes:
       per-sample digitised players (direct DAC and friends) demand
       interrupt rates several times this emulator's whole CPU budget, so
       the guest starves whenever a sample plays. */
int meshpunk_sb_digital = 1;

static void sb_raise_irq(SB16State *s)
{
    if (meshpunk_sb_digital != 2)
        s->set_irq(s->pic, s->irq, 1);
}

#if 0
static void log_dsp (SB16State *dsp)
{
    ldebug ("%s:%s:%d:%s:dmasize=%d:freq=%d:const=%d:speaker=%d\n",
            dsp->fmt_stereo ? "Stereo" : "Mono",
            dsp->fmt_signed ? "Signed" : "Unsigned",
            dsp->fmt_bits,
            dsp->dma_auto ? "Auto" : "Single",
            dsp->block_size,
            dsp->freq,
            dsp->time_const,
            dsp->speaker);
}
#endif

static void speaker (SB16State *s, int on)
{
    s->speaker = on;
    /* AUD_enable (s->voice, on); */
}

static void control (SB16State *s, int hold)
{
    int dma = s->use_hdma ? s->hdma : s->dma;
    IsaDma *isa_dma = s->use_hdma ? s->isa_hdma : s->isa_dma;
    s->dma_running = hold;

    ldebug ("hold %d high %d dma %d\n", hold, s->use_hdma, dma);

    if (hold) {
        i8257_dma_hold_DREQ(isa_dma, dma);
        s->audio_p = s->audio_q;   /* MESHPUNK: a new stream never inherits
                                      stale ring bytes. Short one-shots the
                                      priming gate held back would otherwise
                                      accumulate until the ring limit makes
                                      write_audio accept nothing -- which
                                      stalls the next transfer's DMA forever
                                      and its completion IRQ never fires. */
        s->priming = 1;   /* MESHPUNK: refill before emitting anything */
        s->conceal = 0;   /* MESHPUNK: fresh stream, nothing to conceal */
        s->fade = 0;
        s->late_writer = 0;          /* MESHPUNK: re-detect per stream */
        s->reread_floor = s->audio_q;
        s->rs_phase[0] = 0;   /* MESHPUNK: new stream, resampler phase restarts */
        AUD_set_active_out (s->voice, 1);
    }
    else {
        i8257_dma_release_DREQ(isa_dma, dma);
        AUD_set_active_out (s->voice, 0);
    }
}

#if 0
static void aux_timer (void *opaque)
{
    SB16State *s = opaque;
    s->can_write = 1;
    s->set_irq(s->pic, s->irq, 1);
}
#endif

#define DMA8_AUTO 1
#define DMA8_HIGH 2

static void continue_dma8 (SB16State *s)
{
    if (s->freq > 0) {
        set_audio(s, s->fmt, s->freq, 1 << s->fmt_stereo);
        s->voice = s;
    }

    control (s, 1);
}

static void dma_cmd8 (SB16State *s, int mask, int dma_len)
{
    s->fmt = AUDIO_FORMAT_U8;
    s->use_hdma = 0;
    s->fmt_bits = 8;
    s->fmt_signed = 0;
    s->fmt_stereo = (s->mixer_regs[0x0e] & 2) != 0;
    if (-1 == s->time_const) {
        if (s->freq <= 0)
            s->freq = 11025;
    }
    else {
        int tmp = (256 - s->time_const);
        s->freq = (1000000 + (tmp / 2)) / tmp;
    }

    if (dma_len != -1) {
        s->block_size = dma_len << s->fmt_stereo;
    }
    else {
        /* This is apparently the only way to make both Act1/PL
           and SecondReality/FC work

           Act1 sets block size via command 0x48 and it's an odd number
           SR does the same with even number
           Both use stereo, and Creatives own documentation states that
           0x48 sets block size in bytes less one.. go figure */
        s->block_size &= ~s->fmt_stereo;
    }

    s->freq >>= s->fmt_stereo;
    s->left_till_irq = s->block_size;
    s->bytes_per_second = (s->freq << s->fmt_stereo);
    /* s->highspeed = (mask & DMA8_HIGH) != 0; */
    s->dma_auto = (mask & DMA8_AUTO) != 0;
    s->align = (1 << s->fmt_stereo) - 1;

    if (s->block_size & s->align) {
        qemu_log_mask(LOG_GUEST_ERROR, "warning: misaligned block size %d,"
                      " alignment %d\n", s->block_size, s->align + 1);
    }

    ldebug ("freq %d, stereo %d, sign %d, bits %d, "
            "dma %d, auto %d, fifo %d, high %d\n",
            s->freq, s->fmt_stereo, s->fmt_signed, s->fmt_bits,
            s->block_size, s->dma_auto, s->fifo, s->highspeed);

    continue_dma8 (s);
    speaker (s, 1);
}

static void dma_cmd (SB16State *s, uint8_t cmd, uint8_t d0, int dma_len)
{
    s->use_hdma = cmd < 0xc0;
    s->fifo = (cmd >> 1) & 1;
    s->dma_auto = (cmd >> 2) & 1;
    s->fmt_signed = (d0 >> 4) & 1;
    s->fmt_stereo = (d0 >> 5) & 1;

    switch (cmd >> 4) {
    case 11:
        s->fmt_bits = 16;
        break;

    case 12:
        s->fmt_bits = 8;
        break;
    }

    if (-1 != s->time_const) {
#if 1
        int tmp = 256 - s->time_const;
        s->freq = (1000000 + (tmp / 2)) / tmp;
#else
        /* s->freq = 1000000 / ((255 - s->time_const) << s->fmt_stereo); */
        s->freq = 1000000 / ((255 - s->time_const));
#endif
        s->time_const = -1;
    }

    s->block_size = dma_len + 1;
    s->block_size <<= (s->fmt_bits == 16);
    if (!s->dma_auto) {
        /* It is clear that for DOOM and auto-init this value
           shouldn't take stereo into account, while Miles Sound Systems
           setsound.exe with single transfer mode wouldn't work without it
           wonders of SB16 yet again */
        s->block_size <<= s->fmt_stereo;
    }

    ldebug ("freq %d, stereo %d, sign %d, bits %d, "
            "dma %d, auto %d, fifo %d, high %d\n",
            s->freq, s->fmt_stereo, s->fmt_signed, s->fmt_bits,
            s->block_size, s->dma_auto, s->fifo, s->highspeed);

    if (16 == s->fmt_bits) {
        if (s->fmt_signed) {
            s->fmt = AUDIO_FORMAT_S16;
        }
        else {
            s->fmt = AUDIO_FORMAT_U16;
        }
    }
    else {
        if (s->fmt_signed) {
            s->fmt = AUDIO_FORMAT_S8;
        }
        else {
            s->fmt = AUDIO_FORMAT_U8;
        }
    }

    s->left_till_irq = s->block_size;

    s->bytes_per_second = (s->freq << s->fmt_stereo) << (s->fmt_bits == 16);
    s->highspeed = 0;
    s->align = (1 << (s->fmt_stereo + (s->fmt_bits == 16))) - 1;
    if (s->block_size & s->align) {
        qemu_log_mask(LOG_GUEST_ERROR, "warning: misaligned block size %d,"
                      " alignment %d\n", s->block_size, s->align + 1);
    }

    if (s->freq) {
        set_audio(s, s->fmt, s->freq, 1 << s->fmt_stereo);
        s->voice = s;
    }

    control (s, 1);
    speaker (s, 1);
}

static inline void dsp_out_data (SB16State *s, uint8_t val)
{
    ldebug ("outdata %#x\n", val);
    if ((size_t) s->out_data_len < sizeof (s->out_data)) {
        s->out_data[s->out_data_len++] = val;
    }
}

static inline uint8_t dsp_get_data (SB16State *s)
{
    if (s->in_index) {
        return s->in2_data[--s->in_index];
    }
    else {
        dolog ("buffer underflow\n");
        return 0;
    }
}

static void command (SB16State *s, uint8_t cmd)
{
    ldebug ("command %#x\n", cmd);

    if (cmd > 0xaf && cmd < 0xd0) {
        if (cmd & 8) {
            qemu_log_mask(LOG_UNIMP, "ADC not yet supported (command %#x)\n",
                          cmd);
        }

        switch (cmd >> 4) {
        case 11:
        case 12:
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "%#x wrong bits\n", cmd);
        }
        s->needed_bytes = 3;
    }
    else {
        s->needed_bytes = 0;

        switch (cmd) {
        case 0x03:
            dsp_out_data (s, 0x10); /* s->csp_param); */
            goto warn;

        case 0x04:
            s->needed_bytes = 1;
            goto warn;

        case 0x05:
            s->needed_bytes = 2;
            goto warn;

        case 0x08:
            /* __asm__ ("int3"); */
            goto warn;

        case 0x0e:
            s->needed_bytes = 2;
            goto warn;

        case 0x09:
            dsp_out_data (s, 0xf8);
            goto warn;

        case 0x0f:
            s->needed_bytes = 1;
            goto warn;

        case 0x10:
            s->needed_bytes = 1;
            goto warn;

        case 0x14:
            s->needed_bytes = 2;
            s->block_size = 0;
            break;

        case 0x1c:              /* Auto-Initialize DMA DAC, 8-bit */
            dma_cmd8 (s, DMA8_AUTO, -1);
            break;

        case 0x20:              /* Direct ADC, Juice/PL */
            dsp_out_data (s, 0xff);
            goto warn;

        case 0x35:
            qemu_log_mask(LOG_UNIMP, "0x35 - MIDI command not implemented\n");
            break;

        case 0x40:
            s->freq = -1;
            s->time_const = -1;
            s->needed_bytes = 1;
            break;

        case 0x41:
            s->freq = -1;
            s->time_const = -1;
            s->needed_bytes = 2;
            break;

        case 0x42:
            s->freq = -1;
            s->time_const = -1;
            s->needed_bytes = 2;
            goto warn;

        case 0x45:
            dsp_out_data (s, 0xaa);
            goto warn;

        case 0x47:                /* Continue Auto-Initialize DMA 16bit */
            break;

        case 0x48:
            s->needed_bytes = 2;
            break;

        case 0x74:
            s->needed_bytes = 2; /* DMA DAC, 4-bit ADPCM */
            qemu_log_mask(LOG_UNIMP, "0x75 - DMA DAC, 4-bit ADPCM not"
                          " implemented\n");
            break;

        case 0x75:              /* DMA DAC, 4-bit ADPCM Reference */
            s->needed_bytes = 2;
            qemu_log_mask(LOG_UNIMP, "0x74 - DMA DAC, 4-bit ADPCM Reference not"
                          " implemented\n");
            break;

        case 0x76:              /* DMA DAC, 2.6-bit ADPCM */
            s->needed_bytes = 2;
            qemu_log_mask(LOG_UNIMP, "0x74 - DMA DAC, 2.6-bit ADPCM not"
                          " implemented\n");
            break;

        case 0x77:              /* DMA DAC, 2.6-bit ADPCM Reference */
            s->needed_bytes = 2;
            qemu_log_mask(LOG_UNIMP, "0x74 - DMA DAC, 2.6-bit ADPCM Reference"
                          " not implemented\n");
            break;

        case 0x7d:
            qemu_log_mask(LOG_UNIMP, "0x7d - Autio-Initialize DMA DAC, 4-bit"
                          " ADPCM Reference\n");
            qemu_log_mask(LOG_UNIMP, "not implemented\n");
            break;

        case 0x7f:
            qemu_log_mask(LOG_UNIMP, "0x7d - Autio-Initialize DMA DAC, 2.6-bit"
                          " ADPCM Reference\n");
            qemu_log_mask(LOG_UNIMP, "not implemented\n");
            break;

        case 0x80:
            s->needed_bytes = 2;
            break;

        case 0x90:
        case 0x91:
            dma_cmd8 (s, ((cmd & 1) == 0) | DMA8_HIGH, -1);
            break;

        case 0xd0:              /* halt DMA operation. 8bit */
            control (s, 0);
            break;

        case 0xd1:              /* speaker on */
            speaker (s, 1);
            break;

        case 0xd3:              /* speaker off */
            speaker (s, 0);
            break;

        case 0xd4:              /* continue DMA operation. 8bit */
            /* KQ6 (or maybe Sierras audblst.drv in general) resets
               the frequency between halt/continue */
            continue_dma8 (s);
            break;

        case 0xd5:              /* halt DMA operation. 16bit */
            control (s, 0);
            break;

        case 0xd6:              /* continue DMA operation. 16bit */
            control (s, 1);
            break;

        case 0xd9:              /* exit auto-init DMA after this block. 16bit */
            s->dma_auto = 0;
            break;

        case 0xda:              /* exit auto-init DMA after this block. 8bit */
            s->dma_auto = 0;
            break;

        case 0xe0:              /* DSP identification */
            s->needed_bytes = 1;
            break;

        case 0xe1:
            dsp_out_data (s, s->ver & 0xff);
            dsp_out_data (s, s->ver >> 8);
            break;

        case 0xe2:
            s->needed_bytes = 1;
            goto warn;

        case 0xe3:
            {
                int i;
                for (i = sizeof (e3) - 1; i >= 0; --i)
                    dsp_out_data (s, e3[i]);
            }
            break;

        case 0xe4:              /* write test reg */
            s->needed_bytes = 1;
            break;

        case 0xe7:
            qemu_log_mask(LOG_UNIMP, "Attempt to probe for ESS (0xe7)?\n");
            break;

        case 0xe8:              /* read test reg */
            dsp_out_data (s, s->test_reg);
            break;

        case 0xf2:
        case 0xf3:
            dsp_out_data (s, 0xaa);
            s->mixer_regs[0x82] |= (cmd == 0xf2) ? 1 : 2;
            sb_raise_irq(s);   /* MESHPUNK: the IRQ-test command */
            break;

        case 0xf9:
            s->needed_bytes = 1;
            goto warn;

        case 0xfa:
            dsp_out_data (s, 0);
            goto warn;

        case 0xfc:              /* FIXME */
        case 0xf8:
            dsp_out_data (s, 0);
            goto warn;

        default:
            qemu_log_mask(LOG_UNIMP, "Unrecognized command %#x\n", cmd);
            break;
        }
    }

    if (!s->needed_bytes) {
        ldebug ("\n");
    }

 exit:
    if (!s->needed_bytes) {
        s->cmd = -1;
    }
    else {
        s->cmd = cmd;
    }
    return;

 warn:
    qemu_log_mask(LOG_UNIMP, "warning: command %#x,%d is not truly understood"
                  " yet\n", cmd, s->needed_bytes);
    goto exit;

}

static uint16_t dsp_get_lohi (SB16State *s)
{
    uint8_t hi = dsp_get_data (s);
    uint8_t lo = dsp_get_data (s);
    return (hi << 8) | lo;
}

static uint16_t dsp_get_hilo (SB16State *s)
{
    uint8_t lo = dsp_get_data (s);
    uint8_t hi = dsp_get_data (s);
    return (hi << 8) | lo;
}

#define NANOSECONDS_PER_SECOND 1000000000LL
static inline uint64_t muldiv64(uint64_t a, uint32_t b, uint32_t c)
{
    union {
        uint64_t ll;
        struct {
//#ifdef HOST_WORDS_BIGENDIAN
//            uint32_t high, low;
//#else
            uint32_t low, high;
//#endif
        } l;
    } u, res;
    uint64_t rl, rh;

    u.ll = a;
    rl = (uint64_t)u.l.low * (uint64_t)b;
    rh = (uint64_t)u.l.high * (uint64_t)b;
    rh += (rl >> 32);
    res.l.high = rh / c;
    res.l.low = (((rh % c) << 32) + (rl & 0xffffffff)) / c;
    return res.ll;
}

static void complete (SB16State *s)
{
    int d0, d1, d2;
    ldebug ("complete command %#x, in_index %d, needed_bytes %d\n",
            s->cmd, s->in_index, s->needed_bytes);

    if (s->cmd > 0xaf && s->cmd < 0xd0) {
        d2 = dsp_get_data (s);
        d1 = dsp_get_data (s);
        d0 = dsp_get_data (s);

        if (s->cmd & 8) {
            dolog ("ADC params cmd = %#x d0 = %d, d1 = %d, d2 = %d\n",
                   s->cmd, d0, d1, d2);
        }
        else {
            ldebug ("cmd = %#x d0 = %d, d1 = %d, d2 = %d\n",
                    s->cmd, d0, d1, d2);
            dma_cmd (s, s->cmd, d0, d1 + (d2 << 8));
        }
    }
    else {
        switch (s->cmd) {
        case 0x04:
            s->csp_mode = dsp_get_data (s);
            s->csp_reg83r = 0;
            s->csp_reg83w = 0;
            ldebug ("CSP command 0x04: mode=%#x\n", s->csp_mode);
            break;

        case 0x05:
            s->csp_param = dsp_get_data (s);
            s->csp_value = dsp_get_data (s);
            ldebug ("CSP command 0x05: param=%#x value=%#x\n",
                    s->csp_param,
                    s->csp_value);
            break;

        case 0x0e:
            d0 = dsp_get_data (s);
            d1 = dsp_get_data (s);
            ldebug ("write CSP register %d <- %#x\n", d1, d0);
            if (d1 == 0x83) {
                ldebug ("0x83[%d] <- %#x\n", s->csp_reg83r, d0);
                s->csp_reg83[s->csp_reg83r % 4] = d0;
                s->csp_reg83r += 1;
            }
            else {
                s->csp_regs[d1] = d0;
            }
            break;

        case 0x0f:
            d0 = dsp_get_data (s);
            ldebug ("read CSP register %#x -> %#x, mode=%#x\n",
                    d0, s->csp_regs[d0], s->csp_mode);
            if (d0 == 0x83) {
                ldebug ("0x83[%d] -> %#x\n",
                        s->csp_reg83w,
                        s->csp_reg83[s->csp_reg83w % 4]);
                dsp_out_data (s, s->csp_reg83[s->csp_reg83w % 4]);
                s->csp_reg83w += 1;
            }
            else {
                dsp_out_data (s, s->csp_regs[d0]);
            }
            break;

        case 0x10:
            d0 = dsp_get_data (s);
            dolog ("cmd 0x10 d0=%#x\n", d0);
            break;

        case 0x14:
            dma_cmd8 (s, 0, dsp_get_lohi (s) + 1);
            break;

        case 0x40:
            s->time_const = dsp_get_data (s);
            ldebug ("set time const %d\n", s->time_const);
            break;

        case 0x41:
        case 0x42:
            /*
             * 0x41 is documented as setting the output sample rate,
             * and 0x42 the input sample rate, but in fact SB16 hardware
             * seems to have only a single sample rate under the hood,
             * and FT2 sets output freq with this (go figure).  Compare:
             * http://homepages.cae.wisc.edu/~brodskye/sb16doc/sb16doc.html#SamplingRate
             */
            s->freq = dsp_get_hilo (s);
            ldebug ("set freq %d\n", s->freq);
            break;

        case 0x48:
            s->block_size = dsp_get_lohi (s) + 1;
            ldebug ("set dma block len %d\n", s->block_size);
            break;

        case 0x74:
        case 0x75:
        case 0x76:
        case 0x77:
            /* ADPCM stuff, ignore */
            break;

        case 0x80:
            {
                int freq, samples, bytes;
                int64_t ticks;

                freq = s->freq > 0 ? s->freq : 11025;
                samples = dsp_get_lohi (s) + 1;
                bytes = samples << s->fmt_stereo << (s->fmt_bits == 16);
                ticks = muldiv64(bytes, NANOSECONDS_PER_SECOND, freq);
                if (ticks < NANOSECONDS_PER_SECOND / 1024) {
                    sb_raise_irq(s);
                } else {
                    dolog("TODO: aux_ts\n");
                }
//                else {
//                    if (s->aux_ts) {
//                        timer_mod (
//                            s->aux_ts,
//                            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ticks
//                            );
//                    }
//                }
                ldebug ("mix silence %d %d %" PRId64 "\n", samples, bytes, ticks);
            }
            break;

        case 0xe0:
            d0 = dsp_get_data (s);
            s->out_data_len = 0;
            ldebug ("E0 data = %#x\n", d0);
            dsp_out_data (s, ~d0);
            break;

        case 0xe2:
            d0 = dsp_get_data (s);
            s->e2_valadd += ((uint8_t) d0) ^ s->e2_valxor;
            s->e2_valxor = (s->e2_valxor >> 2) | (s->e2_valxor << 6);
            i8257_dma_write_memory(s->isa_dma, s->dma, &(s->e2_valadd), 0, 1);
            break;

        case 0xe4:
            s->test_reg = dsp_get_data (s);
            break;

        case 0xf9:
            d0 = dsp_get_data (s);
            ldebug ("command 0xf9 with %#x\n", d0);
            switch (d0) {
            case 0x0e:
                dsp_out_data (s, 0xff);
                break;

            case 0x0f:
                dsp_out_data (s, 0x07);
                break;

            case 0x37:
                dsp_out_data (s, 0x38);
                break;

            default:
                dsp_out_data (s, 0x00);
                break;
            }
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "complete: unrecognized command %#x\n",
                          s->cmd);
            return;
        }
    }

    ldebug ("\n");
    s->cmd = -1;
}

static void legacy_reset (SB16State *s)
{
    s->freq = 11025;
    s->fmt_signed = 0;
    s->fmt_bits = 8;
    s->fmt_stereo = 0;
    set_audio(s, AUDIO_FORMAT_U8, s->freq, 1);
    s->voice = s;

    /* Not sure about that... */
    /* AUD_set_active_out (s->voice, 1); */
}

static void reset (SB16State *s)
{
    s->set_irq(s->pic, s->irq, 0);
    if (s->dma_auto) {
        sb_raise_irq(s);
        s->set_irq(s->pic, s->irq, 0);
    }

    s->mixer_regs[0x82] = 0;
    s->dma_auto = 0;
    s->in_index = 0;
    s->out_data_len = 0;
    s->left_till_irq = 0;
    s->needed_bytes = 0;
    s->block_size = -1;
    s->nzero = 0;
    s->highspeed = 0;
    s->v2x6 = 0;
    s->cmd = -1;

    s->e2_valadd = 0xaa;
    s->e2_valxor = 0x96;

    dsp_out_data (s, 0xaa);
    speaker (s, 0);
    control (s, 0);
    legacy_reset (s);
}

void sb16_dsp_write(void *opaque, uint32_t nport, uint32_t val)
{
    SB16State *s = opaque;
    int iport;

    if (!meshpunk_sb_digital)
        return;

    iport = nport - s->port;

    ldebug ("write %#x <- %#x\n", nport, val);
    switch (iport) {
    case 0x06:
        switch (val) {
        case 0x00:
            if (s->v2x6 == 1) {
                reset (s);
            }
            s->v2x6 = 0;
            break;

        case 0x01:
        case 0x03:              /* FreeBSD kludge */
            s->v2x6 = 1;
            break;

        case 0xc6:
            s->v2x6 = 0;        /* Prince of Persia, csp.sys, diagnose.exe */
            break;

        case 0xb8:              /* Panic */
            reset (s);
            break;

        case 0x39:
            dsp_out_data (s, 0x38);
            reset (s);
            s->v2x6 = 0x39;
            break;

        default:
            s->v2x6 = val;
            break;
        }
        break;

    case 0x0c:                  /* write data or command | write status */
/*         if (s->highspeed) */
/*             break; */

        if (s->needed_bytes == 0) {
            command (s, val);
#if 0
            if (0 == s->needed_bytes) {
                log_dsp (s);
            }
#endif
        }
        else {
            if (s->in_index == sizeof (s->in2_data)) {
                dolog ("in data overrun\n");
            }
            else {
                s->in2_data[s->in_index++] = val;
                if (s->in_index == s->needed_bytes) {
                    s->needed_bytes = 0;
                    complete (s);
#if 0
                    log_dsp (s);
#endif
                }
            }
        }
        break;

    default:
        ldebug ("(nport=%#x, val=%#x)\n", nport, val);
        break;
    }
}

uint32_t sb16_dsp_read(void *opaque, uint32_t nport)
{
    SB16State *s = opaque;
    int iport, retval, ack = 0;

    if (!meshpunk_sb_digital)
        return 0xff;   /* MESHPUNK: floating bus -- see the kill switch above */

    iport = nport - s->port;

    switch (iport) {
    case 0x06:                  /* reset */
        retval = 0xff;
        break;

    case 0x0a:                  /* read data */
        if (s->out_data_len) {
            retval = s->out_data[--s->out_data_len];
            s->last_read_byte = retval;
        }
        else {
            if (s->cmd != -1) {
                dolog ("empty output buffer for command %#x\n",
                       s->cmd);
            }
            retval = s->last_read_byte;
            /* goto error; */
        }
        break;

    case 0x0c:                  /* 0 can write */
        retval = s->can_write ? 0 : 0x80;
        break;

    case 0x0d:                  /* timer interrupt clear */
        /* dolog ("timer interrupt clear\n"); */
        retval = 0;
        break;

    case 0x0e:                  /* data available status | irq 8 ack */
        retval = (!s->out_data_len || s->highspeed) ? 0 : 0x80;
        if (s->mixer_regs[0x82] & 1) {
            ack = 1;
            s->mixer_regs[0x82] &= ~1;
            s->set_irq(s->pic, s->irq, 0);
        }
        break;

    case 0x0f:                  /* irq 16 ack */
        retval = 0xff;
        if (s->mixer_regs[0x82] & 2) {
            ack = 1;
            s->mixer_regs[0x82] &= ~2;
            s->set_irq(s->pic, s->irq, 0);
        }
        break;

    default:
        goto error;
    }

    if (!ack) {
        ldebug ("read %#x -> %#x\n", nport, retval);
    }

    return retval;

 error:
    dolog ("warning: dsp_read %#x error\n", nport);
    return 0xff;
}

static void reset_mixer (SB16State *s)
{
    int i;

    memset (s->mixer_regs, 0xff, 0x7f);
    memset (s->mixer_regs + 0x83, 0xff, sizeof (s->mixer_regs) - 0x83);

    s->mixer_regs[0x02] = 4;    /* master volume 3bits */
    s->mixer_regs[0x06] = 4;    /* MIDI volume 3bits */
    s->mixer_regs[0x08] = 0;    /* CD volume 3bits */
    s->mixer_regs[0x0a] = 0;    /* voice volume 2bits */

    /* d5=input filt, d3=lowpass filt, d1,d2=input source */
    s->mixer_regs[0x0c] = 0;

    /* d5=output filt, d1=stereo switch */
    s->mixer_regs[0x0e] = 0;

    /* voice volume L d5,d7, R d1,d3 */
    s->mixer_regs[0x04] = (4 << 5) | (4 << 1);
    /* master ... */
    s->mixer_regs[0x22] = (4 << 5) | (4 << 1);
    /* MIDI ... */
    s->mixer_regs[0x26] = (4 << 5) | (4 << 1);

    for (i = 0x30; i < 0x48; i++) {
        s->mixer_regs[i] = 0x20;
    }
}

void sb16_mixer_write_indexb(void *opaque, uint32_t nport, uint32_t val)
{
    SB16State *s = opaque;
    (void) nport;
    s->mixer_nreg = val;
}

void sb16_mixer_write_datab(void *opaque, uint32_t nport, uint32_t val)
{
    SB16State *s = opaque;

    (void) nport;
    ldebug ("mixer_write [%#x] <- %#x\n", s->mixer_nreg, val);

    switch (s->mixer_nreg) {
    case 0x00:
        reset_mixer (s);
        break;

    case 0x80:
        {
            int irq = irq_of_magic (val);
            ldebug ("setting irq to %d (val=%#x)\n", irq, val);
            if (irq > 0) {
                s->irq = irq;
            }
        }
        break;

    case 0x81:
        {
            int dma, hdma;

            dma = __builtin_ctz (val & 0xf);
            hdma = __builtin_ctz (val & 0xf0);
            if (dma != s->dma || hdma != s->hdma) {
                qemu_log_mask(LOG_GUEST_ERROR, "attempt to change DMA 8bit"
                              " %d(%d), 16bit %d(%d) (val=%#x)\n", dma, s->dma,
                              hdma, s->hdma, val);
            }
#if 0
            s->dma = dma;
            s->hdma = hdma;
#endif
        }
        break;

    case 0x82:
        qemu_log_mask(LOG_GUEST_ERROR, "attempt to write into IRQ status"
                      " register (val=%#x)\n", val);
        return;

    default:
        if (s->mixer_nreg >= 0x80) {
            ldebug ("attempt to write mixer[%#x] <- %#x\n", s->mixer_nreg, val);
        }
        break;
    }

    s->mixer_regs[s->mixer_nreg] = val;
}

uint32_t sb16_mixer_read(void *opaque, uint32_t nport)
{
    SB16State *s = opaque;

    (void) nport;
#ifndef DEBUG_SB16_MOST
    if (s->mixer_nreg != 0x82) {
        ldebug ("mixer_read[%#x] -> %#x\n",
                s->mixer_nreg, s->mixer_regs[s->mixer_nreg]);
    }
#else
    ldebug ("mixer_read[%#x] -> %#x\n",
            s->mixer_nreg, s->mixer_regs[s->mixer_nreg]);
#endif
    return s->mixer_regs[s->mixer_nreg];
}

static int write_audio (SB16State *s, int nchan, int dma_pos,
                        int dma_len, int len)
{
    IsaDma *isa_dma = nchan == s->dma ? s->isa_dma : s->isa_hdma;

    int temp, net;
    uint8_t tmpbuf[TMPBUF_LEN];

    temp = len;
    net = 0;

    while (temp) {
        int left = dma_len - dma_pos;
        int copied;
        size_t to_copy;

        /* MESHPUNK: work out how much the ring can actually take BEFORE reading
           guest memory. Upstream read up to `temp` bytes out of PSRAM, then
           clamped to the free space and discarded the remainder -- measured at
           ~980 byte-reads to deliver 40 bytes, because the ring drains at the
           guest's sample rate while this asks for a full block every call. */
        unsigned int limit = AUDIO_BUF_LIMIT;
        if (s->freq >= 44100 && s->fmt >= 2)
            limit = AUDIO_BUF_LIMIT_HIGH;
        else if (s->freq < 22050 || s->fmt < 2)
            limit = AUDIO_BUF_LIMIT_LOW;
        unsigned int space = limit - (s->audio_q - s->audio_p);
        if (space > AUDIO_BUF_LEN)
            space = 0;              /* underflow: queue is over the limit */
        if (!space)
            break;                  /* old code read, dropped it, then broke */

        to_copy = temp;
        if (left < temp)
            to_copy = left;
        if (to_copy > space)
            to_copy = space;
        if (to_copy > sizeof (tmpbuf)) {
            to_copy = sizeof (tmpbuf);
        }

        copied = i8257_dma_read_memory(isa_dma, nchan, tmpbuf, dma_pos, to_copy);

        /* MESHPUNK: seam crossfade for concealment -- see SB_read_DMA. */
        if (s->fade > 0) {
            int k;
            for (k = 0; k < copied && s->fade > 0; k++, s->fade--)
                tmpbuf[k] = (uint8_t)(((int)s->fade_from * s->fade
                          + (int)tmpbuf[k] * (MESHPUNK_CONCEAL_FADE - s->fade))
                          / MESHPUNK_CONCEAL_FADE);
        }

        unsigned int len = copied;  /* already <= space by construction */
        if (len) {
            s->last_byte = tmpbuf[len - 1];   /* MESHPUNK: seam anchor */
            unsigned int q = s->audio_q % AUDIO_BUF_LEN;
            if (q + len < AUDIO_BUF_LEN) {
                memcpy(s->audio_buf + q, tmpbuf, len);
            } else {
                unsigned int r = AUDIO_BUF_LEN - q;
                memcpy(s->audio_buf + q, tmpbuf, r);
                memcpy(s->audio_buf, tmpbuf + r, len - r);
            }
            s->audio_q += len;
        }
        copied = len;

        temp -= copied;
        dma_pos = (dma_pos + copied) % dma_len;
        net += copied;

        if (!copied) {
            break;
        }
    }
    return net;
}

static int SB_read_DMA (void *opaque, int nchan, int dma_pos, int dma_len)
{
    SB16State *s = opaque;
    int till, copy, written, free;

    /* MESHPUNK: NO DMA mode -- the transfer never progresses, so a game's
       DMA verification times out. See meshpunk_sb_digital above. */
    if (meshpunk_sb_digital == 3)
        return dma_pos;

    if (s->block_size <= 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "invalid block size=%d nchan=%d"
                      " dma_pos=%d dma_len=%d\n", s->block_size, nchan,
                      dma_pos, dma_len);
        return dma_pos;
    }

    if (s->left_till_irq < 0) {
        s->left_till_irq = s->block_size;
    }

    if (s->voice) {
        free = s->audio_free & ~s->align;
        if ((free <= 0) || !dma_len) {
            return dma_pos;
        }
    }
    else {
        free = dma_len;
    }

    /* MESHPUNK: trailing-window re-read (stale-head repair). The guest's ISR
       finishes refilling a DMA half AFTER this pump has consumed the first
       bytes past the boundary -- byte-level capture forensics measured the
       refill landing 4-104 bytes of consumption late on EVERY block, so every
       block's head entered the ring one region stale (two splices per block =
       the audible static). The ring buffers up to ~1024 bytes ahead of
       playback, which leaves time to fix it: re-read the trailing DMA
       positions this pump already consumed and overwrite the matching ring
       bytes in place (the last `span` bytes appended came from the last
       `span` DMA positions -- both advance only and always together).
       Idempotent: once the refill has landed, later re-reads copy the same
       stable data.
       Window: the current block always; PLUS the previous block once the
       guest is a proven late-writer. A guest that refills each half only
       after entering it leaves the half it vacated untouched until the next
       wrap, so those positions stay valid for a second block period --
       doubling the repair deadline. The proof is the latch: a re-read that
       DIFFERS from what the ring holds is a refill landing after our first
       read, which is exactly the late-writer behaviour. Until then the
       window stays current-block-only, because an early-writer guest puts
       NEXT-pass data in the vacated half and patching that would splice
       future audio into the past.
       reread_floor fences everything a conceal episode replayed or a seam
       fade blended: those ring bytes are deliberate constructions and raw
       re-patching would re-open the seams they closed. The floor rides the
       write head while a fade drains. Clamped at the region wrap (two-chunk
       read), the play cursor, and MESHPUNK_REREAD_MAX. */
    if (s->dma_auto && s->fmt_bits == 8 && !s->conceal) {
        if (s->fade > 0) {
            s->reread_floor = s->audio_q;
        } else if (s->left_till_irq < s->block_size) {
            int span = s->block_size - s->left_till_irq;
            if (s->late_writer)
                span += s->block_size;
            if (span > dma_len)
                span = dma_len;
            if (span > MESHPUNK_REREAD_MAX)
                span = MESHPUNK_REREAD_MAX;
            unsigned int avail = s->audio_q - s->audio_p;
            unsigned int fenced = s->audio_q - s->reread_floor;
            if (fenced < avail)
                avail = fenced;
            if ((unsigned int)span > avail)
                span = (int)avail;
            if (span > 0) {
                uint8_t rbuf[MESHPUNK_REREAD_MAX];
                IsaDma *rdma = nchan == s->dma ? s->isa_dma : s->isa_hdma;
                int start = dma_pos - span;
                int got;
                if (start < 0) {
                    int pre = -start;   /* tail of the region, pre-wrap */
                    got = i8257_dma_read_memory(rdma, nchan, rbuf,
                                                dma_len - pre, pre);
                    if (got == pre && dma_pos > 0)
                        got += i8257_dma_read_memory(rdma, nchan, rbuf + pre,
                                                     0, dma_pos);
                } else {
                    got = i8257_dma_read_memory(rdma, nchan, rbuf, start, span);
                }
                if (got > 0) {
                    unsigned int rq = (s->audio_q - (unsigned int)span)
                                      % AUDIO_BUF_LEN;
                    unsigned int n1 = AUDIO_BUF_LEN - rq;
                    if (n1 > (unsigned int)got)
                        n1 = (unsigned int)got;
                    if (!s->late_writer && memcmp(s->audio_buf + rq, rbuf, n1))
                        s->late_writer = 1;
                    memcpy(s->audio_buf + rq, rbuf, n1);
                    if ((unsigned int)got > n1) {
                        if (!s->late_writer
                            && memcmp(s->audio_buf, rbuf + n1,
                                      (unsigned int)got - n1))
                            s->late_writer = 1;
                        memcpy(s->audio_buf, rbuf + n1,
                               (unsigned int)got - n1);
                    }
                    if (got == span)
                        s->last_byte = rbuf[got - 1];
                }
            }
        }
    }

    copy = free;
    till = s->left_till_irq;

#ifdef DEBUG_SB16_MOST
    dolog ("pos:%06d %d till:%d len:%d\n",
           dma_pos, free, till, dma_len);
#endif

    if (till <= copy) {
        if (s->dma_auto == 0) {
            copy = till;
        }
    }

    /* MESHPUNK concealment (v2 of the ack gate). While the last
       block-boundary IRQ is un-acked (guest acks via DSP 0x22E/0x22F), data
       past the next boundary has not been refilled by the ISR yet. v1 held
       the pump there and hw-stuttered: at this emulation speed the ack
       routinely lands 4-29ms after the raise, longer than the one-block
       window, so the mixer starved. Instead: deliver valid data exactly UP
       TO the boundary (so the seam falls on a call edge), then keep reading
       -- the stale bytes replay the previous region, the least-wrong filler
       for quasi-periodic game audio -- and crossfade both seams. The WAV
       captures showed the splice EDGES were the audible click, not the
       replayed content: 120/207 clicks were clean 2-block replays. Entry
       fade arms at the boundary crossing (raise block below); exit fade
       arms here when the ack is seen. 8-bit formats only: the fade blends
       raw bytes, which would mangle 16-bit samples. */
    if (s->conceal && dma_pos == 0
        && !(s->mixer_regs[0x82] & ((nchan & 4) ? 2 : 1))) {
        /* Exit ONLY at the region wrap, and only acked. Capture 3 proved that
           exiting at the ack alone still splices: the guest acks EARLY in its
           ISR and copies the refill in AFTER, so reads resumed on still-stale
           data and the refill landed mid-read (clicks smeared to just past
           the boundary phase). At the wrap the stream is naturally
           continuous -- the guest writes half A then half B in stream order,
           so stored-B-end -> stored-A-start is the true succession -- and the
           refill has had the whole half-region traversal to complete. The
           fade is insurance for the residual race, blending nearly-identical
           streams when all is well. */
        s->conceal = 0;
        s->fade = MESHPUNK_CONCEAL_FADE;
        s->fade_from = s->last_byte;
    }
    if (s->conceal) {
        /* land the possible exit on a call edge: never straddle the wrap */
        int to_wrap = dma_len - dma_pos;
        if (copy > to_wrap)
            copy = to_wrap;
    }
    if (s->dma_auto && s->fmt_bits == 8 && !s->conceal
        && (s->mixer_regs[0x82] & ((nchan & 4) ? 2 : 1))
        && copy > till) {
        int valid = till & ~s->align;
        if (valid > 0)
            copy = valid;   /* stop at the boundary; conceal from next call */
    }

    written = write_audio (s, nchan, dma_pos, dma_len, copy);
    dma_pos = (dma_pos + written) % dma_len;
    s->left_till_irq -= written;

    if (s->left_till_irq <= 0) {
        if (s->dma_auto && s->fmt_bits == 8 && !s->conceal
            && (s->mixer_regs[0x82] & ((nchan & 4) ? 2 : 1))) {
            /* MESHPUNK: crossed a boundary while the previous IRQ is still
               un-acked -- everything from here on is un-refilled. Conceal:
               keep reading (replays the previous region) and fade into it. */
            s->conceal = 1;
            s->fade = MESHPUNK_CONCEAL_FADE;
            s->fade_from = s->last_byte;
        }
        s->mixer_regs[0x82] |= (nchan & 4) ? 2 : 1;
        sb_raise_irq(s);
        if (s->dma_auto == 0) {
            control (s, 0);
            speaker (s, 0);
        }
    }

#ifdef DEBUG_SB16_MOST
    ldebug ("pos %5d free %5d size %5d till % 5d copy %5d written %5d size %5d\n",
            dma_pos, free, dma_len, s->left_till_irq, copy, written,
            s->block_size);
#endif

    while (s->left_till_irq <= 0) {
        s->left_till_irq = s->block_size + s->left_till_irq;
    }

    return dma_pos;
}

static void approx_frac(int *pn, int *pd)
{
    int limit = 8;
    int h0 = 0, h1 = 1;
    int k0 = 1, k1 = 0;

    int r = *pn, s = *pd;
    int a, h2, k2;

    while (s != 0) {
        a = r / s;
        int next_s = r % s;
        r = s;
        s = next_s;

        h2 = a * h1 + h0;
        k2 = a * k1 + k0;

        if (k2 > limit) {
            break;
        }

        h0 = h1; h1 = h2;
        k0 = k1; k1 = k2;
    }

    *pn = h1;
    *pd = k1;
}

/* MESHPUNK: the resamplers index the ring with `& (itlen - 1)` instead of
   `% itlen`, which was a real integer divide per OUTPUT SAMPLE (~51k/s) because
   itlen arrives as a runtime parameter the compiler cannot prove is a power of
   two. Every call site passes AUDIO_BUF_LEN or AUDIO_BUF_LEN/2; this pins that. */
_Static_assert((AUDIO_BUF_LEN & (AUDIO_BUF_LEN - 1)) == 0,
               "AUDIO_BUF_LEN must be a power of two: the resamplers mask with it");

/* MESHPUNK: approx_frac() is a continued-fraction loop full of integer
   divisions, and every resampler called it on EVERY mixer invocation (~400/s)
   to re-derive the same ratio. Both inputs -- 44100 and s->freq -- only change
   when the guest reprograms the DSP, so memoise the last pair. */
/* MESHPUNK: how many output slots the last resample actually filled. The
   loops stop on `i < ilen`, so a ring drained low (but not empty) leaves the
   tail unwritten -- see the tail-hold in sb16_audio_callback. */
static int rs_produced;

static int rs_key_n = -1, rs_key_d = -1, rs_val_n, rs_val_d;
static void approx_frac_cached(int *pn, int *pd)
{
    if (*pn == rs_key_n && *pd == rs_key_d) {
        *pn = rs_val_n;
        *pd = rs_val_d;
        return;
    }
    int kn = *pn, kd = *pd;
    approx_frac(pn, pd);
    rs_key_n = kn; rs_key_d = kd;
    rs_val_n = *pn; rs_val_d = *pd;
}

static int resample_s16m(int16_t *out, int olen, int os,
                         int16_t *in, int ip, int ilen, int itlen, int is)
{
    approx_frac_cached(&os, &is);
    int uc = os;
    int dc = is;
    int i = 0;
    int j = 0;
    while (i < ilen && j + 1 < olen) {
        dc--;
        if (dc == 0) {
            dc = is;
            out[j + 1] = out[j] = in[(ip + i) & (itlen - 1)];
            j += 2;
        }
        uc--;
        if (uc == 0) {
            uc = os;
            i++;
        }
    }
    rs_produced = j;
    return i;
}

static int resample_s16s(int16_t *out, int olen, int os,
                         int16_t *in, int ip, int ilen, int itlen, int is)
{
    approx_frac_cached(&os, &is);
    int uc = os;
    int dc = is;
    int i = 0;
    int j = 0;
    while (i + 1 < ilen && j + 1 < olen) {
        dc--;
        if (dc == 0) {
            dc = is;
            out[j] = in[(ip + i) & (itlen - 1)];
            out[j + 1] = in[(ip + i + 1) & (itlen - 1)];
            j += 2;
        }
        uc--;
        if (uc == 0) {
            uc = os;
            i += 2;
        }
    }
    rs_produced = j;
    return i;
}

static int resample_u16m(int16_t *out, int olen, int os,
                         int16_t *in, int ip, int ilen, int itlen, int is)
{
    approx_frac_cached(&os, &is);
    int uc = os;
    int dc = is;
    int i = 0;
    int j = 0;
    while (i < ilen && j + 1 < olen) {
        dc--;
        if (dc == 0) {
            dc = is;
            out[j + 1] = out[j] = in[(ip + i) & (itlen - 1)] - 32768;
            j += 2;
        }
        uc--;
        if (uc == 0) {
            uc = os;
            i++;
        }
    }
    rs_produced = j;
    return i;
}

static int resample_u16s(int16_t *out, int olen, int os,
                         int16_t *in, int ip, int ilen, int itlen, int is)
{
    approx_frac_cached(&os, &is);
    int uc = os;
    int dc = is;
    int i = 0;
    int j = 0;
    while (i + 1 < ilen && j + 1 < olen) {
        dc--;
        if (dc == 0) {
            dc = is;
            out[j] = in[(ip + i) & (itlen - 1)] - 32768;
            out[j + 1] = in[(ip + i + 1) & (itlen - 1)] - 32768;
            j += 2;
        }
        uc--;
        if (uc == 0) {
            uc = os;
            i += 2;
        }
    }
    rs_produced = j;
    return i;
}

/* MESHPUNK: `ph` carries the resampler phase across calls. Upstream restarts
   uc/dc from scratch every call and advances the ring position by WHOLE bytes,
   discarding the fractional position inside the current byte -- so every
   mixer slice (128 frames) re-emitted that fraction. The 0.25-rate capture
   showed it verbatim: a ~7-sample span played twice, values identical, at
   every steep passage (~0.5 byte at ratio 89/8). At full rate the repeat is
   ~2 samples wide -- the rate-invariant "static" heard since the first build.
   Carrying (uc, dc) resumes mid-group, so nothing is re-emitted. The phase is
   only valid for the (os, is) pair it was made with; a rate change or stream
   restart naturally resets it. */
static int resample_u8m(int *ph, int16_t *out, int olen, int os,
                        uint8_t *in, int ip, int ilen, int itlen, int is)
{
    approx_frac_cached(&os, &is);
    int uc, dc;
    if (ph && ph[0] == os && ph[1] == is) {
        uc = ph[2]; dc = ph[3];
    } else {
        uc = os; dc = is;
    }
    int i = 0;
    int j = 0;
    while (i < ilen && j + 1 < olen) {
        dc--;
        if (dc == 0) {
            dc = is;
            /* MESHPUNK: linear interpolation, not sample-and-hold. `uc` counts
               down from os across one input sample, so (os - uc) is the phase
               within it. Upstream emitted the same value for the whole span,
               which at 44100/15800 holds each sample 2-3 output slots -- and
               2-4x longer again when meshpunk_sb_rate_pct reduces the rate.
               Interpolating in the 16-bit output domain, not 8-bit: rounding
               back to 8 bits would quantise the result and gain nothing. */
            int a = ((int)in[(ip + i) & (itlen - 1)] - 128) << 8;
            int b = (i + 1 < ilen)
                  ? (((int)in[(ip + i + 1) & (itlen - 1)] - 128) << 8) : a;
            out[j] = (int16_t)(a + ((b - a) * (os - uc)) / os);
            out[j + 1] = out[j];
            j += 2;
        }
        uc--;
        if (uc == 0) {
            uc = os;
            i++;
        }
    }
    if (ph) { ph[0] = os; ph[1] = is; ph[2] = uc; ph[3] = dc; }
    rs_produced = j;
    return i;
}

static int resample_u8s(int *ph, int16_t *out, int olen, int os,
                        uint8_t *in, int ip, int ilen, int itlen, int is)
{
    approx_frac_cached(&os, &is);
    int uc, dc;
    if (ph && ph[0] == os && ph[1] == is) {
        uc = ph[2]; dc = ph[3];
    } else {
        uc = os; dc = is;
    }
    int i = 0;
    int j = 0;
    while (i + 1 < ilen && j + 1 < olen) {
        dc--;
        if (dc == 0) {
            dc = is;
            /* MESHPUNK: linear interpolation -- see resample_u8m. Stereo
               advances i by 2, so the NEXT frame's pair is at i+2 / i+3. */
            int ph = os - uc, nxt = (i + 3 < ilen);
            int al = ((int)in[(ip + i) & (itlen - 1)] - 128) << 8;
            int ar = ((int)in[(ip + i + 1) & (itlen - 1)] - 128) << 8;
            int bl = nxt ? (((int)in[(ip + i + 2) & (itlen - 1)] - 128) << 8) : al;
            int br = nxt ? (((int)in[(ip + i + 3) & (itlen - 1)] - 128) << 8) : ar;
            out[j] = (int16_t)(al + ((bl - al) * ph) / os);
            out[j + 1] = (int16_t)(ar + ((br - ar) * ph) / os);
            j += 2;
        }
        uc--;
        if (uc == 0) {
            uc = os;
            i += 2;
        }
    }
    if (ph) { ph[0] = os; ph[1] = is; ph[2] = uc; ph[3] = dc; }
    rs_produced = j;
    return i;
}

// XXX: There are races, but we accept them for now.
/* MESHPUNK: guest audio rate scale.
   The guest's SoundBlaster interrupt handler is the single largest consumer of
   emulated CPU (measured ~27% of wall for MS Pac-Man), and its RATE is ours:
   the handler only runs because our DMA pump consumes the guest's buffer, so
   IRQs arrive in proportion to how fast we drain it. Telling the resampler the
   input is at freq*R makes it consume input R times faster, so the DMA advances
   R times faster and the guest gets R times MORE interrupts -- while the host
   still receives a full 44100 samples/s, so nothing underruns.
   The audio plays at R pitch and speed, because that work IS the audio: below
   100 it stretches and costs the guest less CPU, above 100 it compresses and
   polls more often at more CPU. PERCENT, so 25 is exact: 100 = unchanged,
   50 = half, 25 = quarter, 150 = one-and-a-half, 200 = double. */
int meshpunk_sb_rate_pct = 100;

MESHPUNK_HOT void sb16_audio_callback (void *opaque, uint8_t *stream, int free)
{
    SB16State *s = opaque;
    s->audio_free = free;

    unsigned int len = s->audio_q - s->audio_p;
    if (len > AUDIO_BUF_LEN) {
        s->audio_p = s->audio_q;
        return;
    }

    if (!s->active_out && !len) {
        return;
    }

    /* MESHPUNK: stay silent until the ring has primed.
       While it is filling, the resamplers stop early on `i < ilen` and the tail
       of the output buffer keeps mixer_callback's memset zeros -- a run of
       partial buffers with a discontinuity at the end of each, which is audible
       as a crackle right when a sound starts. Emitting real silence for the
       ~30ms it takes to fill is inaudible by comparison. Re-armed by control()
       whenever the guest (re)starts the DMA, and again below if the ring is ever
       drained dry, so a mid-stream underrun recovers the same way instead of
       chattering. 512 bytes is ~32ms at the 8-bit rates DOS games use, and half
       the AUDIO_BUF_LIMIT_LOW the ring settles at. */
    if (s->priming) {
        /* MESHPUNK: a COMPLETED transfer can never grow the ring past the
           threshold, so a one-shot shorter than 512 bytes would be held
           forever -- once its DMA has stopped, emit what it delivered. */
        if (len < 512 && s->dma_running)
            return;
        s->priming = 0;
    } else if (!len) {
        s->priming = 1;
        return;
    }

    unsigned int p = s->audio_p % AUDIO_BUF_LEN;

    /* MESHPUNK: see meshpunk_sb_rate_pct above. */
    int pct = meshpunk_sb_rate_pct > 0 ? meshpunk_sb_rate_pct : 100;
    int rfreq = (int)((long)s->freq * pct / 100);
    if (rfreq < 1) rfreq = 1;

    int i = 0;
    rs_produced = 0;
    switch (s->fmt) {
    case AUDIO_FORMAT_S16:
        if (s->fmt_stereo) {
            i = resample_s16s((int16_t *) stream, free / 2, 44100,
                              (int16_t *) s->audio_buf, p / 2, len / 2,
                              AUDIO_BUF_LEN / 2, rfreq);
        } else {
            i = resample_s16m((int16_t *) stream, free / 2, 44100,
                              (int16_t *) s->audio_buf, p / 2, len / 2,
                              AUDIO_BUF_LEN / 2, rfreq);
        }
        s->audio_p = (s->audio_p + i * 2) & ~1;
        break;
    case AUDIO_FORMAT_U16:
        if (s->fmt_stereo) {
            i = resample_u16s((int16_t *) stream, free / 2, 44100,
                              (int16_t *) s->audio_buf, p / 2, len / 2,
                              AUDIO_BUF_LEN / 2, rfreq);
        } else {
            i = resample_u16m((int16_t *) stream, free / 2, 44100,
                              (int16_t *) s->audio_buf, p / 2, len / 2,
                              AUDIO_BUF_LEN / 2, rfreq);
        }
        s->audio_p = (s->audio_p + i * 2) & ~1;
        break;
    case AUDIO_FORMAT_U8:
        if (s->fmt_stereo) {
            i = resample_u8s(s->rs_phase, (int16_t *) stream, free / 2, 44100,
                             s->audio_buf, p, len, AUDIO_BUF_LEN, rfreq);
        } else {
            i = resample_u8m(s->rs_phase, (int16_t *) stream, free / 2, 44100,
                             s->audio_buf, p, len, AUDIO_BUF_LEN, rfreq);
        }
        s->audio_p += i;
        break;
    default:
        dolog("bad format %d\n", s->fmt);
    }

    /* MESHPUNK: tail-hold. The resamplers stop when the input ring runs out, and
       everything past that point keeps mixer_callback's memset zeros -- so a
       ring drained low mid-buffer produces a hard step to silence, once per
       call, audible as a crackle exactly when the guest's ISR is too busy to
       keep the ring topped up (dense music). Holding the last sample instead
       removes the step. This is correct on any short read, and costs nothing
       when the ring is healthy because the loop simply does not run.
       NOTE the empty-ring case never reaches here -- the priming gate above
       returns early -- so this catches only the PARTIAL fill, which is the case
       the [clip] `dry` counter could not see. */
    int olen = free / 2;
    if (rs_produced > 0 && rs_produced < olen) {
        int16_t *out = (int16_t *) stream;
        int16_t hold = out[rs_produced - 1];
        for (int k = rs_produced; k < olen; k++)
            out[k] = hold;
    }
}

#if 0
static int sb16_post_load (void *opaque, int version_id)
{
    SB16State *s = opaque;

    if (s->voice) {
//        AUD_close_out (&s->card, s->voice);
        s->voice = NULL;
    }

    if (s->dma_running) {
        if (s->freq) {
            set_audio(s, s->fmt, s->freq, 1 << s->fmt_stereo);
            s->voice = s;
        }

        control (s, 1);
        speaker (s, s->speaker);
    }
    return 0;
}

static const MemoryRegionPortio sb16_ioport_list[] = {
    {  4, 1, 1, .write = mixer_write_indexb },
    {  5, 1, 1, .read = mixer_read, .write = mixer_write_datab },
    {  6, 1, 1, .read = dsp_read, .write = dsp_write },
    { 10, 1, 1, .read = dsp_read },
    { 12, 1, 1, .write = dsp_write },
    { 12, 4, 1, .read = dsp_read },
    PORTIO_END_OF_LIST (),
};
#endif

SB16State *sb16_new(
    int port, // 0x220
    int irq, // 5
    void *isa_dma,
    void *isa_hdma,
    void *pic,
    void (*set_irq)(void *pic, int irq, int level))
{
    SB16State *s = pcmalloc(sizeof(SB16State));
    memset(s, 0, sizeof(SB16State));
    s->voice = s;

    s->ver = 0x0405;
    s->port = port;
    s->irq = irq;
    s->dma = 1;
    s->hdma = 5;
    s->cmd = -1;

    s->isa_hdma = isa_hdma;
    s->isa_dma = isa_dma;

    s->pic = pic;
    s->set_irq = set_irq;

    s->mixer_regs[0x80] = magic_of_irq (s->irq);
    s->mixer_regs[0x81] = (1 << s->dma) | (1 << s->hdma);
    s->mixer_regs[0x82] = 2 << 5;

    s->csp_regs[5] = 1;
    s->csp_regs[9] = 0xf8;

    reset_mixer (s);
//    s->aux_ts = timer_new_ns(QEMU_CLOCK_VIRTUAL, aux_timer, s);
//    if (!s->aux_ts) {
//        error_setg(errp, "warning: Could not create auxiliary timer");
//    }

    i8257_dma_register_channel(s->isa_hdma, s->hdma, SB_read_DMA, s);

    i8257_dma_register_channel(s->isa_dma, s->dma, SB_read_DMA, s);

    s->can_write = 1;

    return s;
}
