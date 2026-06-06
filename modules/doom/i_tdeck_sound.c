// T-Deck sound driver for Doom — implements sound_module_t.
// Mixes up to 8 SFX channels at 11025 Hz mono and pushes the result
// to the firmware via host_audio_push(), where it is upsampled to
// 44100 Hz stereo and mixed with notification tones.
// Music is not handled here (use -nomusic on the command line).

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

// Host function provided by the firmware
extern void host_audio_push(const int16_t* samples, int count, int sample_rate);

// Required by i_sound.c I_BindSoundVariables() when FEATURE_SOUND is enabled.
// We don't use libsamplerate — these just need to exist to satisfy the linker.
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

#define NUM_CHANNELS     8
#define MIX_RATE         11025
#define MIX_SAMPLES_PER_TIC  (MIX_RATE / 35)  // 315 samples per game tic

// Per-channel state
typedef struct {
    const uint8_t*  data;       // 8-bit unsigned PCM (WAD lump body, past header)
    unsigned int    length;     // number of samples
    unsigned int    pos;        // current playback position (fixed-point 16.16)
    unsigned int    step;       // playback step (fixed-point 16.16, for resampling)
    int             vol;        // volume 0-127
    int             sep;        // stereo separation 0-254 (unused — we mix mono)
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

// ── sound_module_t implementation ───────────────────────────────────────────

static boolean I_TDeck_InitSound(boolean use_sfx_prefix)
{
    (void)use_sfx_prefix;
    memset(channels, 0, sizeof(channels));
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
    ch->data    = cache->samples;
    ch->length  = cache->length;
    ch->pos     = 0;
    // Fixed-point 16.16 step for resampling from source rate to MIX_RATE.
    // 22050 << 16 = 1.4B, fits uint32_t — no 64-bit division needed.
    ch->step    = ((uint32_t)cache->sample_rate << 16) / (uint32_t)MIX_RATE;
    ch->vol     = vol;
    ch->sep     = sep;
    ch->playing = true;
    ch->sfxinfo = sfxinfo;

    return channel;
}

static void I_TDeck_StopSound(int channel)
{
    if (channel < 0 || channel >= NUM_CHANNELS) return;
    channels[channel].playing = false;
}

static boolean I_TDeck_SoundIsPlaying(int channel)
{
    if (channel < 0 || channel >= NUM_CHANNELS) return false;
    return channels[channel].playing;
}

// ── Mixer — called once per FRAME (not per tic!) from S_UpdateSounds ────────
// The frame rate is variable (15-35 FPS on ESP32-S3), so we produce the exact
// number of samples the elapsed wall-clock time demands.  This keeps the audio
// output rate at MIX_RATE regardless of game performance.

#define MAX_MIX_SAMPLES 2048  // cap per call (~186 ms at 11025 Hz)

static uint32_t s_last_mix_ms = 0;

static void I_TDeck_UpdateSound(void)
{
    if (!sound_initialized) return;

    // Compute how many samples are needed based on real elapsed time.
    uint32_t now = host_get_ticks_ms();
    if (s_last_mix_ms == 0) { s_last_mix_ms = now; return; }
    uint32_t elapsed = now - s_last_mix_ms;
    if (elapsed == 0) return;
    s_last_mix_ms = now;

    int num_samples = (int)((uint32_t)MIX_RATE * elapsed / 1000);
    if (num_samples <= 0) return;
    if (num_samples > MAX_MIX_SAMPLES) num_samples = MAX_MIX_SAMPLES;

    // Use int32_t for accumulation to avoid clipping when multiple channels mix.
    int32_t mix32[MAX_MIX_SAMPLES];
    memset(mix32, 0, num_samples * sizeof(int32_t));

    for (int c = 0; c < NUM_CHANNELS; c++) {
        channel_t* ch = &channels[c];
        if (!ch->playing || !ch->data) continue;

        int vol = ch->vol;

        for (int i = 0; i < num_samples; i++) {
            unsigned int sample_idx = ch->pos >> 16;
            if (sample_idx >= ch->length) {
                ch->playing = false;
                break;
            }

            // Linear interpolation between adjacent WAD samples
            unsigned int frac = (ch->pos >> 8) & 0xFF; // 0..255
            int s0 = (int)ch->data[sample_idx] - 128;
            int s1 = (sample_idx + 1 < ch->length)
                    ? ((int)ch->data[sample_idx + 1] - 128) : s0;
            int sample = (s0 * (256 - frac) + s1 * frac) / 256;
            mix32[i] += (sample * vol / 127) * 256;

            ch->pos += ch->step;
        }
    }

    // Clamp and convert to int16_t for output
    int16_t mixbuf[MAX_MIX_SAMPLES];
    for (int i = 0; i < num_samples; i++) {
        int32_t v = mix32[i];
        if (v > 32767)  v = 32767;
        if (v < -32768) v = -32768;
        mixbuf[i] = (int16_t)v;
    }

    // Push mixed buffer to firmware
    host_audio_push(mixbuf, num_samples, MIX_RATE);
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

// ── Stub music module (no MIDI synth — use -nomusic) ────────────────────────

static boolean  MusicInit(void)         { return true; }
static void     MusicShutdown(void)     {}
static void     MusicSetVol(int v)      { (void)v; }
static void     MusicPause(void)        {}
static void     MusicResume(void)       {}
static void*    MusicRegister(void* d, int l) { (void)d; (void)l; return NULL; }
static void     MusicUnregister(void* h) { (void)h; }
static void     MusicPlay(void* h, boolean l) { (void)h; (void)l; }
static void     MusicStop(void)         {}
static boolean  MusicPlaying(void)      { return false; }
static void     MusicPoll(void)         {}

music_module_t DG_music_module = {
    NULL, 0,
    MusicInit, MusicShutdown, MusicSetVol, MusicPause, MusicResume,
    MusicRegister, MusicUnregister, MusicPlay, MusicStop, MusicPlaying,
    MusicPoll,
};

#endif /* FEATURE_SOUND */
