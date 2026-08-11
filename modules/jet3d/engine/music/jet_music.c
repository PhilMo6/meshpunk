// jet3d music: the public API from jet_music.h, plus the handful of Doom
// functions the carried-in OPL player expects to link against.
//
// Everything Doom-shaped lives here so the upstream files stay untouched.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "compat/doomtype.h"
#include "compat/i_sound.h"
#include "jet_music.h"
#include "jet_audio.h"
#include "opl_tdeck.h"
#include "genmidi_bank.h"

// The player reads this once, to decide the OPL chip's sample rate.
int snd_samplerate = OPL_TDECK_MIX_RATE;

extern music_module_t music_opl_module;
extern char *snd_dmxoption;   // defined in i_oplmusic.c

static int s_ready;
static void *s_song;
static int s_playing;
// Remembered across init: the player starts at volume ZERO, and a game that
// sets the volume before the first play would otherwise have the call
// dropped and wonder why the music is silent.
static float s_vol = 1.0f;
// The voice a pre-rendered loop is playing on, or -1 when the live synth is
// in use. Declared here because stop/pause/volume all steer it.
static int s_clip_voice = -1;

// --- the Doom functions the player links against -------------------------

// One lump exists here, and it is the FM instrument bank. Anything else is
// reported missing, which the player already handles by disabling music.
int W_CheckNumForName(const char *name)
{
    return (name && !strncmp(name, "GENMIDI", 8)) ? 0 : -1;
}

void *W_CacheLumpName(const char *name, int tag)
{
    (void)tag;
    if (name && !strncmp(name, "GENMIDI", 8))
    {
        return (void *)jet_genmidi_bank;
    }
    return NULL;
}

void W_ReleaseLumpName(const char *name)
{
    // The bank is compiled in; there is nothing to release.
    (void)name;
}

boolean M_snprintf(char *buf, size_t buf_len, const char *s, ...)
{
    va_list args;
    int result;
    va_start(args, s);
    result = vsnprintf(buf, buf_len, s, args);
    va_end(args);
    // Upstream returns whether the write fit.
    return result >= 0 && (size_t)result < buf_len;
}

boolean M_StringConcat(char *dest, const char *src, size_t dest_size)
{
    size_t offset = strlen(dest);
    if (offset > dest_size)
    {
        offset = dest_size;
    }
    return M_snprintf(dest + offset, dest_size - offset, "%s", src);
}

// The player calls this only on a genuinely broken instrument bank. A game
// module has nowhere to abort to, so say so and carry on without music.
void I_Error(const char *error, ...)
{
    va_list args;
    va_start(args, error);
    fprintf(stderr, "[music] ");
    vfprintf(stderr, error, args);
    fprintf(stderr, "\n");
    va_end(args);
}

// Doom's driver uses this to pump the mixer when SFX are disabled. jet_audio
// always mixes, so the music is already being pulled.
void I_TDeck_MusicPoll(void)
{
}

// --- public API -----------------------------------------------------------

int jet_music_init(void)
{
    if (s_ready)
    {
        return 1;
    }
    // Ask the player for OPL3. Our backend reports the chip as OPL3, and
    // the player only believes it if this option says so too -- that is the
    // gate for 18 voices and waveforms 4-7.
    snd_dmxoption = "-opl3";
    if (jet_genmidi_bank_len < 8
        || memcmp(jet_genmidi_bank, "#OPL_II#", 8) != 0)
    {
        fprintf(stderr, "[music] instrument bank missing or malformed\n");
        return 0;
    }
    if (!music_opl_module.Init())
    {
        fprintf(stderr, "[music] OPL init failed\n");
        return 0;
    }
    s_ready = 1;
    jet_music_volume(s_vol);
    return 1;
}

void jet_music_shutdown(void)
{
    if (!s_ready)
    {
        return;
    }
    jet_music_stop();
    music_opl_module.Shutdown();
    s_ready = 0;
}

int jet_music_play(const void *midi, size_t len, int looping)
{
    if (!s_ready || !midi || !len)
    {
        return 0;
    }
    jet_music_stop();
    // RegisterSong parses into its own structures; the caller's buffer is
    // not retained (see MIDI_LoadBuffer in midifile.h).
    s_song = music_opl_module.RegisterSong((void *)midi, (int)len);
    if (!s_song)
    {
        fprintf(stderr, "[music] could not parse song (%u bytes)\n",
                (unsigned)len);
        return 0;
    }
    music_opl_module.PlaySong(s_song, looping ? 1 : 0);
    s_playing = 1;
    return 1;
}

void jet_music_stop(void)
{
    if (s_clip_voice >= 0)
    {
        jet_audio_stop(s_clip_voice);
        s_clip_voice = -1;
    }
    if (!s_ready)
    {
        return;
    }
    if (s_playing)
    {
        music_opl_module.StopSong();
        s_playing = 0;
    }
    if (s_song)
    {
        music_opl_module.UnRegisterSong(s_song);
        s_song = NULL;
    }
}

void jet_music_pause(int paused)
{
    if (s_clip_voice >= 0)
    {
        // A clip cannot be suspended, so pausing mutes it and lets it run.
        // The loop is where it would have been on resume, which matters not
        // at all and costs no state.
        jet_audio_voice_set(s_clip_voice, -1.0f, paused ? 0.0f : s_vol);
        return;
    }
    if (!s_ready || !s_playing)
    {
        return;
    }
    if (paused)
    {
        music_opl_module.PauseMusic();
    }
    else
    {
        music_opl_module.ResumeMusic();
    }
}

void jet_music_volume(float vol)
{
    int v;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    // The player's scale is 0..127, matching MIDI channel volume.
    s_vol = vol;
    if (s_clip_voice >= 0)
    {
        jet_audio_voice_set(s_clip_voice, -1.0f, vol);
    }
    v = (int)(vol * 127.0f + 0.5f);
    if (s_ready)
    {
        music_opl_module.SetMusicVolume(v);
    }
}

int jet_music_playing(void)
{
    // A pre-rendered loop plays through jet_audio's own voices, so the
    // mixer already knows about it -- reporting it here would make
    // jet_audio_service think it had to run the synth as well.
    if (s_clip_voice >= 0)
    {
        return 0;
    }
    return (s_ready && s_playing && music_opl_module.MusicIsPlaying()) ? 1 : 0;
}

void jet_music_mix(int32_t *mix, int n)
{
    if (!s_ready || n <= 0)
    {
        return;
    }
    OPL_TDeck_Mix(mix, n);
}

// --- pre-rendered playback -------------------------------------------------
//
// The device does not synthesise music. tools/build_music.sh renders each
// track to PCM with this same OPL code, and what plays here is a looping
// sample: an index and an add per sample against a whole emulated chip.
//
// The shipped tracks are IMA ADPCM at the mixer's own rate, so playback is
// 1:1 -- no resampling, which is also what the ADPCM decoder requires. The
// .raw fallback is uncompressed PCM at half rate, hence its own ratio; it is
// a development convenience and is not shipped.
#define MUSIC_ADPCM_RATE_RATIO 1.0f
#define MUSIC_PCM_RATE_RATIO 0.5f
// Eight, not four: the soundtrack is menu + THREE race tracks + finish = five,
// and a cache that filled would fall through to the .raw fallback and then to
// the LIVE SYNTH -- silently putting back the FM cost that the pre-rendering
// exists to remove. Sized with room for the next track rather than exactly.
#define MUSIC_MAX_CACHED 8

static struct { char path[160]; int clip; } s_cache[MUSIC_MAX_CACHED];
static int s_cached;

// Clips cannot be freed individually, so every track is loaded at most once
// and kept. Five of them ship: menu, three race tracks and the finish.
static int cached_clip(const char *path)
{
    int i;
    for (i = 0; i < s_cached; i++)
    {
        if (!strcmp(s_cache[i].path, path))
        {
            return s_cache[i].clip;
        }
    }
    if (s_cached >= MUSIC_MAX_CACHED)
    {
        return -1;
    }
    {
        const int clip = jet_audio_load(path);
        if (clip < 0)
        {
            return -1;
        }
        snprintf(s_cache[s_cached].path, sizeof(s_cache[s_cached].path),
                 "%s", path);
        s_cache[s_cached].clip = clip;
        s_cached++;
        return clip;
    }
}

static int swap_extension(const char *in, const char *ext, char *out,
                          size_t out_len)
{
    const char *dot = strrchr(in, '.');
    size_t stem = dot ? (size_t)(dot - in) : strlen(in);
    if (stem + strlen(ext) + 1 >= out_len)
    {
        return 0;
    }
    memcpy(out, in, stem);
    memcpy(out + stem, ext, strlen(ext) + 1);
    return 1;
}

int jet_music_play_file(const char *midi_path, int looping)
{
    char raw[192];

    if (!midi_path)
    {
        return 0;
    }
    jet_music_stop();

    // Compressed first, then the uncompressed fallback.
    if (swap_extension(midi_path, ".adp", raw, sizeof(raw)))
    {
        const int clip = cached_clip(raw);
        if (clip >= 0)
        {
            s_clip_voice = jet_audio_play(clip, s_vol, looping ? 1 : 0,
                                          MUSIC_ADPCM_RATE_RATIO);
            if (s_clip_voice >= 0)
            {
                return 1;
            }
        }
    }
    if (swap_extension(midi_path, ".raw", raw, sizeof(raw)))
    {
        const int clip = cached_clip(raw);
        if (clip >= 0)
        {
            s_clip_voice = jet_audio_play(clip, s_vol, looping ? 1 : 0,
                                          MUSIC_PCM_RATE_RATIO);
            if (s_clip_voice >= 0)
            {
                return 1;
            }
        }
    }

    // No pre-rendered loop: fall back to synthesising it. Correct, and far
    // more expensive -- this path is for development, not for the device.
    {
        FILE *f = fopen(midi_path, "rb");
        long n;
        void *buf;
        int ok = 0;
        if (!f)
        {
            return 0;
        }
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < (1 << 20) && (buf = malloc((size_t)n)) != NULL)
        {
            if (fread(buf, 1, (size_t)n, f) == (size_t)n)
            {
                ok = jet_music_init() && jet_music_play(buf, (size_t)n,
                                                        looping);
            }
            free(buf);
        }
        fclose(f);
        return ok;
    }
}
