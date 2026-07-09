// T-Deck sound driver for Doom — implements sound_module_t.
// Mixes up to 8 SFX channels plus OPL FM music at OPL_TDECK_MIX_RATE mono
// and pushes the result to the firmware via host_audio_push(), where it is
// upsampled to 44100 Hz stereo and mixed with notification tones.
// Music (music_opl_module, opl/i_oplmusic.c) renders into the same mix
// buffer via OPL_TDeck_Mix() — see opl/opl_tdeck.c.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "doomfeatures.h"

#ifdef FEATURE_SOUND

#include "deh_str.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#include "opl_tdeck.h"

// Host functions provided by the firmware
extern void host_audio_push(const int16_t* samples, int count, int sample_rate);
extern uint32_t host_get_ticks_ms(void);

// Required by i_sound.c I_BindSoundVariables() when FEATURE_SOUND is enabled.
// We don't use libsamplerate — these just need to exist to satisfy the linker.
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

#define NUM_CHANNELS     8
#define MIX_RATE         OPL_TDECK_MIX_RATE   // shared with the OPL synth (opl_tdeck.h)

// Per-channel state
typedef struct {
    const uint8_t*  data;       // 8-bit unsigned PCM (WAD lump body, past header)
    unsigned int    length;     // number of samples
    unsigned int    pos;        // current playback position (fixed-point 16.16)
    unsigned int    step;       // playback step (fixed-point 16.16, for resampling)
    int             vol;        // volume 0-127
    int             sep;        // stereo separation 0-254 (unused — we mix mono)
    int             fade_in;    // declick ramp samples left at sound start
    boolean         playing;
    sfxinfo_t*      sfxinfo;
} channel_t;

static channel_t channels[NUM_CHANNELS];
static boolean sound_initialized = false;

// ── WAD SFX format ──────────────────────────────────────────────────────────
// Header: [u8 format=3][u8 pad][u16 sample_rate][u32 length][...samples...]
// DMX convention: skip first 16 and last 16 bytes of the lump.

typedef struct {
    const uint8_t*  samples;
    unsigned int    length;
    int             sample_rate;
} cached_sfx_t;

static boolean CacheSFX(sfxinfo_t* sfxinfo)
{
    int lumpnum = sfxinfo->lumpnum;
    if (lumpnum < 0) return false;  // lump not found in WAD
    unsigned int lumplen = W_LumpLength(lumpnum);
    byte* data = W_CacheLumpNum(lumpnum, PU_STATIC);

    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00) {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }

    int samplerate = (data[3] << 8) | data[2];
    unsigned int length = (data[7] << 24) | (data[6] << 16)
                        | (data[5] << 8)  | data[4];

    if (length > lumplen - 8 || length <= 48) {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }

    // DMX skips first/last 16 bytes
    cached_sfx_t* cache = Z_Malloc(sizeof(cached_sfx_t), PU_STATIC, NULL);
    cache->samples    = data + 24;      // 8 header + 16 skip
    cache->length     = length - 32;
    cache->sample_rate = samplerate;
    sfxinfo->driver_data = cache;

    // Keep lump pinned (PU_STATIC) — small cost, avoids re-reading from SD.
    return true;
}

// ── Declick: release pool ───────────────────────────────────────────────────
// Doom hard-cuts playing sounds constantly: same-origin retriggers (every
// repeated shot), priority steals once all 8 channels saturate, and
// inaudibility stops from S_UpdateSounds. An instant cut is a full-amplitude
// step — a broadband click — and at busy-scene rates (dozens/sec) the clicks
// form a static-like crackle bed over the mix. Instead of silencing
// instantly, a cut channel's playback state moves here and fades out over
// FADE_SAMPLES; new sounds fade in over the same window.

#define FADE_SAMPLES  64   // ~2.9 ms at 22050
#define NUM_RELEASES  8

typedef struct {
    const uint8_t*  data;
    unsigned int    length;
    unsigned int    pos;        // 16.16
    unsigned int    step;       // 16.16
    int             gain;       // channel gain at cut time
    int             remaining;  // fade samples left; 0 = slot free
} release_t;

static release_t releases[NUM_RELEASES];

// Perceptual (squared) volume curve at the half-scale base — shared by the
// live-channel mixer and the release fades so both use identical math.
// (Base 128: a full-volume channel peaks at ±16k, the firmware's loudness
// convention; full-scale summing used to hard-clip. Floor of 1 keeps
// distant sounds present — vol < 12 truncates to 0 otherwise.)
static int channel_gain(int vol)
{
    int gain = (vol * vol * 128) / (127 * 127);
    if (gain == 0 && vol > 0) gain = 1;
    return gain;
}

// Move a playing channel's state into a release slot to fade out.
static void spawn_release(channel_t* ch)
{
    // Free slot, else steal the one closest to finishing.
    release_t* slot = &releases[0];
    for (int i = 0; i < NUM_RELEASES; i++) {
        if (releases[i].remaining <= 0) { slot = &releases[i]; break; }
        if (releases[i].remaining < slot->remaining) slot = &releases[i];
    }

    slot->data      = ch->data;
    slot->length    = ch->length;
    slot->pos       = ch->pos;
    slot->step      = ch->step;
    slot->gain      = channel_gain(ch->vol);
    slot->remaining = FADE_SAMPLES;
}

// ── sound_module_t implementation ───────────────────────────────────────────

static boolean I_TDeck_InitSound(boolean use_sfx_prefix)
{
    (void)use_sfx_prefix;
    memset(channels, 0, sizeof(channels));
    memset(releases, 0, sizeof(releases));
    sound_initialized = true;
    return true;
}

static void I_TDeck_ShutdownSound(void)
{
    sound_initialized = false;
}

static int I_TDeck_GetSfxLumpNum(sfxinfo_t* sfxinfo)
{
    char namebuf[20];
    M_snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfxinfo->name));
    return W_CheckNumForName(namebuf);  // returns -1 if lump missing (non-fatal)
}

static void I_TDeck_UpdateSoundParams(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= NUM_CHANNELS) return;
    channels[channel].vol = vol;
    channels[channel].sep = sep;
}

static int I_TDeck_StartSound(sfxinfo_t* sfxinfo, int channel, int vol, int sep)
{
    if (channel < 0 || channel >= NUM_CHANNELS) return -1;

    // Cache the sound if not already done
    if (!sfxinfo->driver_data) {
        if (!CacheSFX(sfxinfo))
            return -1;
    }

    cached_sfx_t* cache = (cached_sfx_t*)sfxinfo->driver_data;

    channel_t* ch = &channels[channel];

    // Channel steal: fade the old sound out instead of clicking it off.
    if (ch->playing && ch->data) {
        spawn_release(ch);
    }

    ch->data    = cache->samples;
    ch->length  = cache->length;
    ch->pos     = 0;
    // Fixed-point 16.16 step for resampling from source rate to MIX_RATE.
    // 22050 << 16 = 1.4B, fits uint32_t — no 64-bit division needed.
    ch->step    = ((uint32_t)cache->sample_rate << 16) / (uint32_t)MIX_RATE;
    ch->vol     = vol;
    ch->sep     = sep;
    ch->fade_in = FADE_SAMPLES;
    ch->playing = true;
    ch->sfxinfo = sfxinfo;

    return channel;
}

static void I_TDeck_StopSound(int channel)
{
    if (channel < 0 || channel >= NUM_CHANNELS) return;
    channel_t* ch = &channels[channel];
    if (ch->playing && ch->data) {
        spawn_release(ch);   // fade out instead of clicking off
    }
    ch->playing = false;
}

static boolean I_TDeck_SoundIsPlaying(int channel)
{
    if (channel < 0 || channel >= NUM_CHANNELS) return false;
    return channels[channel].playing;
}

// ── Mixer — called once per FRAME (not per tic!) from S_UpdateSounds ────────
// The frame rate is variable (15-35 FPS on ESP32-S3), so production is paced
// by an ABSOLUTE sample clock: target = wall-clock-since-epoch * MIX_RATE
// plus a standing lead. The lead keeps a ~70 ms cushion in the firmware's
// extern ring so frame-time spikes (heavy scenes, SD lump loads) don't drain
// it dry — the ring running empty between pushes was the constant crackle
// under load. Any deficit from a capped long frame is repaid on later calls.

#define MAX_MIX_SAMPLES OPL_TDECK_MAX_SAMPLES          // per-call cap (~93 ms at 22050)
// Production lead (cushion in the firmware ring). Constraint: LEAD +
// MAX_MIX_SAMPLES must stay under the 4096-sample firmware ring or catch-up
// pushes silently drop samples. At 22050 that caps the lead at ~90 ms;
// 85 ms (1874) + 2048 = 3922 < 4096. hw telemetry showed typical worst
// frames of 67-97 ms, but the mixer now pumps TWICE per frame (see
// I_TDeck_AudioPump), so per-pump gaps are ~half that — 85 ms covers them.
#define LEAD_SAMPLES    ((OPL_TDECK_MIX_RATE * 85) / 1000)

static boolean  s_mix_started  = false;
static uint32_t s_mix_epoch_ms = 0;
static uint32_t s_produced     = 0;   // samples pushed since epoch (mod 2^32)

// The mix buffers are locals in I_TDeck_MixAndPush (task stack = internal
// SRAM): keeps the hot accumulation loop off the PSRAM bus and module BSS
// small. ~12.5 KB of the 64 KB module task stack.

// Core mixer, shared by the SFX module's Update and the music-only Poll
// pump (with -nosfx the engine has no sound module to drive the mixer, so
// music_opl_module.Poll calls this instead — see I_TDeck_MusicPoll).
static void I_TDeck_MixAndPush(void)
{
    uint32_t now = host_get_ticks_ms();
    if (!s_mix_started) {
        s_mix_started  = true;
        s_mix_epoch_ms = now;
        s_produced     = 0;
    }

    // 64-bit INTEGER math only (ms * MIX_RATE overflows u32 after ~3 min);
    // the u32 truncation makes `target` wrap at the same 2^32-sample point
    // as s_produced, so their difference stays valid.
    uint32_t target = (uint32_t)(((uint64_t)(now - s_mix_epoch_ms)
                                  * MIX_RATE) / 1000) + LEAD_SAMPLES;

    int num_samples = (int32_t)(target - s_produced);
    if (num_samples <= 0) return;

    // Catch-up discipline: cap each call at the buffer size (worst ring
    // level = lead + one cap ≈ 2.8k of 4096, so repay bursts can't
    // overflow the ring, which silently drops the newest samples). A
    // deficit beyond repair (a real stall) is resynced away instead:
    // one clean gap beats chasing the clock with bursts.
    if (num_samples > 2 * MAX_MIX_SAMPLES) {
        s_produced  = target - LEAD_SAMPLES;
        num_samples = LEAD_SAMPLES;
    } else if (num_samples > MAX_MIX_SAMPLES) {
        num_samples = MAX_MIX_SAMPLES;
    }
    s_produced += (uint32_t)num_samples;

    // Internal-RAM (task stack) accumulation buffers — see note above.
    int32_t mix32[MAX_MIX_SAMPLES];
    int16_t mixbuf[MAX_MIX_SAMPLES];

    // Use int32_t for accumulation to avoid clipping when multiple channels mix.
    memset(mix32, 0, num_samples * sizeof(int32_t));

    for (int c = 0; c < NUM_CHANNELS; c++) {
        channel_t* ch = &channels[c];
        if (!ch->playing || !ch->data) continue;

        int gain = channel_gain(ch->vol);

        // Hot channel state in locals: per-sample read-modify-write of
        // channel fields would otherwise be PSRAM traffic (see note above).
        const uint8_t* data    = ch->data;
        unsigned int   length  = ch->length;
        unsigned int   pos     = ch->pos;
        unsigned int   step    = ch->step;
        int            fade_in = ch->fade_in;

        for (int i = 0; i < num_samples; i++) {
            unsigned int sample_idx = pos >> 16;
            if (sample_idx >= length) {
                ch->playing = false;
                break;
            }

            // Linear interpolation between adjacent WAD samples.
            // frac MUST be signed: as `unsigned int` it promoted the whole
            // expression below to unsigned, so the /256 compiled to a
            // LOGICAL shift — every negative sample wrapped to a ~2^24
            // positive spike (the busy-scene "static", present since this
            // driver's first version; root-caused via disassembly).
            int frac = (int)((pos >> 8) & 0xFF); // 0..255
            int s0 = (int)data[sample_idx] - 128;
            int s1 = (sample_idx + 1 < length)
                    ? ((int)data[sample_idx + 1] - 128) : s0;
            int sample = (s0 * (256 - frac) + s1 * frac) / 256;

            // Declick: ramp new sounds in over the first FADE_SAMPLES.
            if (fade_in > 0) {
                sample = sample * (FADE_SAMPLES - fade_in) / FADE_SAMPLES;
                fade_in--;
            }

            mix32[i] += sample * gain;

            pos += step;
        }

        ch->pos     = pos;
        ch->fade_in = fade_in;
    }

    // Declick: fade-out tails of cut/stolen sounds (see release pool).
    for (int r = 0; r < NUM_RELEASES; r++) {
        release_t* rl = &releases[r];
        if (rl->remaining <= 0) continue;

        // Hot release state in locals (same PSRAM-RMW avoidance as channels).
        const uint8_t* data      = rl->data;
        unsigned int   length    = rl->length;
        unsigned int   pos       = rl->pos;
        unsigned int   step      = rl->step;
        int            gain      = rl->gain;
        int            remaining = rl->remaining;

        for (int i = 0; i < num_samples && remaining > 0; i++) {
            unsigned int sample_idx = pos >> 16;
            if (sample_idx >= length) {
                remaining = 0;
                break;
            }

            int s = (int)data[sample_idx] - 128;
            // s(±128) × gain(≤128) × remaining(≤64) ≈ ±1M — fits int32.
            mix32[i] += (s * gain * remaining) / FADE_SAMPLES;

            pos += step;
            remaining--;
        }

        rl->pos       = pos;
        rl->remaining = remaining;
    }

    // Add FM music (sample-accurate MIDI sequencing happens inside).
    OPL_TDeck_Mix(mix32, num_samples);

    // Soft-knee limit and convert to int16_t. Stacked loud sounds used to
    // hard-clip at the int16 rail (harsh static-like distortion whenever
    // several things fired at once); the sum is now transparent below the
    // knee, compressed 8:1 above it, with the rail as a last resort.
    for (int i = 0; i < num_samples; i++) {
        int32_t v = mix32[i];
        int32_t a = (v < 0) ? -v : v;
        if (a > 20000) {
            a = 20000 + ((a - 20000) >> 3);
            if (a > 32767) a = 32767;
        }
        mixbuf[i] = (int16_t)((v < 0) ? -a : a);
    }

    // Push mixed buffer to firmware
    host_audio_push(mixbuf, num_samples, MIX_RATE);
}

static void I_TDeck_UpdateSound(void)
{
    if (!sound_initialized) return;
    I_TDeck_MixAndPush();
}

// music_module_t Poll — runs every I_UpdateSound. With SFX enabled the
// engine already drives the mixer via sound_module->Update, so this only
// takes over when the SFX module is absent (-nosfx, music still on).
// The channels[] array is all-zero then, so only music is mixed.
void I_TDeck_MusicPoll(void)
{
    if (!sound_initialized)
    {
        I_TDeck_MixAndPush();
    }
}

// Second production point, called from DG_DrawFrame AFTER rendering+blit
// (see doomgeneric_tdeck.c). S_UpdateSounds pumps before the render; this
// pumps after it, so the ring is refilled on both sides of the frame's
// long pole instead of once per frame.
void I_TDeck_AudioPump(void)
{
    if (sound_initialized)
    {
        I_TDeck_MixAndPush();
    }
}

static void I_TDeck_PrecacheSounds(sfxinfo_t* sounds, int num_sounds)
{
    char namebuf[20];
    for (int i = 0; i < num_sounds; i++) {
        if (sounds[i].link) continue;
        M_snprintf(namebuf, sizeof(namebuf), "ds%s",
                   DEH_String(sounds[i].name));
        sounds[i].lumpnum = W_CheckNumForName(namebuf);
        if (sounds[i].lumpnum >= 0)
            CacheSFX(&sounds[i]);
    }
}

// ── Module registration ─────────────────────────────────────────────────────

static snddevice_t sound_tdeck_devices[] = {
    SNDDEVICE_SB,  // matches default snd_sfxdevice
};

sound_module_t DG_sound_module = {
    sound_tdeck_devices,
    sizeof(sound_tdeck_devices) / sizeof(*sound_tdeck_devices),
    I_TDeck_InitSound,
    I_TDeck_ShutdownSound,
    I_TDeck_GetSfxLumpNum,
    I_TDeck_UpdateSound,
    I_TDeck_UpdateSoundParams,
    I_TDeck_StartSound,
    I_TDeck_StopSound,
    I_TDeck_SoundIsPlaying,
    I_TDeck_PrecacheSounds,
};

// Music is provided by music_opl_module (opl/i_oplmusic.c), selected in
// i_sound.c InitMusicModule(); its synth output is mixed in above.

#endif /* FEATURE_SOUND */
