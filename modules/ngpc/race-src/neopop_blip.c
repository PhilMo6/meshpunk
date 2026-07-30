/*---------------------------------------------------------------------------
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *---------------------------------------------------------------------------
 *
 * neopop_blip.c: band-limited ("accurate") audio path for the NGPC sound.
 *
 * This is an alternative to the per-sample synthesis in neopopsound.c. Instead
 * of computing an output level once per output sample, each square-wave toggle
 * and each DAC change is emitted into a Blip_Buffer as a band-limited
 * transition at its exact sub-sample time, then read_samples() integrates the
 * result. This matches the approach used by the Mednafen/beetle-ngp reference
 * and removes the aliasing of the per-sample path, at a higher CPU cost.
 *
 * It is selected at runtime by the 'race_audio_quality' = 'accurate' core
 * option; the default 'fast' path stays in neopopsound.c.
 */

#include "types.h"
#include <string.h>
#include <Blip_Buffer.h>
#include "neopopsound.h"
#include "neopop_blip.h"

#define NGPC_PSG_CLOCK 3072000      /* PSG chip clock (Hz) */
#define NGPC_CPU_CLOCK 6144000      /* CPU clock; soundStep cycles are in these units */
/* Synth volume range. With Blip_Synth_set_volume(v, range) the output is
 * delta * (v/range) * 2^16, so range = 2^17 makes a transition delta map to
 * delta/2 of output. The decoded per-channel volume (a VolTable value, 0..
 * MAX_OUTPUT/3) is emitted as that delta, which reproduces the per-sample
 * path's amplitude (validated: matched RMS, no clipping) and keeps three
 * channels at full volume within the 16-bit range. */
#define BLIP_TONE_RANGE 131072      /* 2^17 */

/* Per-channel state for the three tones and the noise channel. We re-derive
 * frequency/volume from the shared toneChip/noiseChip register state, but keep
 * our own running phase clocked in chip cycles for sub-sample accuracy. */
typedef struct
{
   int   period;    /* half-period in chip cycles (square toggles each period) */
   int   counter;   /* chip cycles until next toggle */
   int   phase;     /* current square-wave level: 0 or 1 */
   int   amp;       /* last amplitude written into blip (for delta) */
   int   volume;    /* current volume (0..max) */
} blip_tone_t;

static Blip_Buffer  bbuf;
static Blip_Synth   synth_tone;
static Blip_Synth   synth_dac;
static blip_tone_t  tones[3];
static struct
{
   int      period;
   int      counter;
   unsigned rng;
   int      fb;
   int      phase;
   int      amp;
   int      volume;
} noise;

static int   dac_last;            /* last DAC amplitude (for delta) */
static long  blip_ts;             /* current sub-frame timestamp (chip cycles) */
static int   blip_ready = 0;
static int   blip_rate  = 44100;

void neopop_blip_init(int sample_rate)
{
   int i;
   blip_rate = sample_rate;

   Blip_Buffer_init(&bbuf);
   Blip_Buffer_set_sample_rate(&bbuf, sample_rate, 1000 / 10);
   Blip_Buffer_set_clock_rate(&bbuf, NGPC_CPU_CLOCK);
   Blip_Buffer_bass_freq(&bbuf, 20);

   Blip_Synth_set_volume(&synth_tone, 1.0, BLIP_TONE_RANGE);
   Blip_Synth_set_volume(&synth_dac,  0.40, 0xFF);

   memset(tones, 0, sizeof(tones));
   memset(&noise, 0, sizeof(noise));
   for (i = 0; i < 3; i++)
      tones[i].period = 1;
   noise.period = 1;
   noise.rng    = 0x8000;
   noise.fb     = 0x6000;
   dac_last     = 0;
   blip_ts      = 0;
   blip_ready   = 1;
}

void neopop_blip_reset(void)
{
   if (blip_ready)
      Blip_Buffer_clear(&bbuf, 1);
   neopop_blip_init(blip_rate);
}

/* Advance one channel up to 'to_ts' (chip cycles), emitting transitions. */
static void run_tone(int idx, long to_ts)
{
   blip_tone_t *t = &tones[idx];
   long now = blip_ts;
   int target_amp;

   if (t->period <= 0)
      t->period = 1;

   while (t->counter <= (to_ts - now))
   {
      now      += t->counter;
      t->phase ^= 1;
      target_amp = t->phase ? t->volume : 0;
      if (target_amp != t->amp)
      {
         Blip_Synth_offset(&synth_tone, now, target_amp - t->amp, &bbuf);
         t->amp = target_amp;
      }
      t->counter = t->period;
   }
   t->counter -= (int)(to_ts - now);
}

static void run_noise(long to_ts)
{
   long now = blip_ts;
   int target_amp;

   if (noise.period <= 0)
      noise.period = 1;

   while (noise.counter <= (to_ts - now))
   {
      now += noise.counter;
      /* clock the LFSR */
      if (noise.rng & 1)
         noise.rng ^= noise.fb;
      noise.rng >>= 1;
      noise.phase = noise.rng & 1;
      target_amp  = noise.phase ? noise.volume : 0;
      if (target_amp != noise.amp)
      {
         Blip_Synth_offset(&synth_tone, now, target_amp - noise.amp, &bbuf);
         noise.amp = target_amp;
      }
      noise.counter = noise.period;
   }
   noise.counter -= (int)(to_ts - now);
}

/* Called from soundStep with the number of chip cycles elapsed. Advances all
 * channels to keep their transitions time-aligned with register writes. */
void neopop_blip_run(int chip_cycles)
{
   long to_ts;
   int i;
   if (!blip_ready || chip_cycles <= 0)
      return;
   to_ts = blip_ts + chip_cycles;
   for (i = 0; i < 3; i++)
      run_tone(i, to_ts);
   run_noise(to_ts);
   blip_ts = to_ts;
}

/* Pull current register state from the shared chips into our synth params.
 * Called after each WriteSoundChip so frequency/volume changes take effect at
 * the right timestamp (channels are already advanced to blip_ts by the caller). */
void neopop_blip_sync_tone(int chan, int period_div, int volume)
{
   if (chan < 0 || chan > 2)
      return;
   /* period_div is the raw tone divider; the tone toggles every divider*16
    * chip cycles, doubled to the CPU-cycle timebase the synth runs on. */
   tones[chan].period = (period_div ? period_div : 1) * 32;
   /* volume is the already-decoded amplitude (a VolTable value), matching what
    * the per-sample path multiplies in. */
   tones[chan].volume = volume;
}

void neopop_blip_sync_noise(int period_div, int volume, int feedback_periodic)
{
   noise.period = (period_div ? period_div : 1) * 32;
   noise.volume = volume;
   noise.fb     = feedback_periodic ? 0x4000 : 0x6000;
}

void neopop_blip_dac(int dac_value)
{
   /* dac_value is the raw 8-bit DAC byte; emit the delta as a transition. */
   int v = (dac_value - 0x80);
   if (v != dac_last)
   {
      Blip_Synth_offset(&synth_dac, blip_ts, v - dac_last, &bbuf);
      dac_last = v;
   }
}

/* End the frame and read out 'frames' stereo samples (duplicated to L/R). */
int neopop_blip_flush(int16_t *stereo_out, int max_frames)
{
   long avail, got, i;

   if (!blip_ready)
      return 0;

   Blip_Buffer_end_frame(&bbuf, blip_ts);
   blip_ts = 0;

   /* Drain everything available so the buffer can never back up; cap to the
    * caller's buffer capacity. read_samples writes one channel at stride 2
    * (interleaved stereo), filling the left slots of stereo_out directly, so
    * we mirror left into right afterwards for mono-duplicated output. */
   avail = Blip_Buffer_samples_avail(&bbuf);
   if (avail > max_frames)
      avail = max_frames;

   got = Blip_Buffer_read_samples(&bbuf, stereo_out, avail);
   for (i = 0; i < got; i++)
      stereo_out[i * 2 + 1] = stereo_out[i * 2];
   return (int)got;
}
