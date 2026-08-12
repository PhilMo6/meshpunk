// Renders a MIDI file to raw PCM using the SAME OPL synthesiser the module
// carries, so what ships is bit-identical to what the chip emulation would
// have produced -- just computed here instead of on the device.
//
// This exists because FM synthesis is far too expensive to run per frame on
// an ESP32-S3: measured at 19-23 ms of CPU per second of audio on a desktop,
// which on the device is a large fraction of a core and cost the game a
// visible chunk of its frame rate. A pre-rendered loop costs an index and an
// add per sample instead.
//
//   render_music <in.mid> <out.raw> <seconds> [decimate]
//
// Output is signed 16-bit mono, little-endian, at 22050/decimate Hz.
// Decimation averages pairs rather than dropping samples, and the synthesis
// still happens at 22050, so halving the rate does not expose the chip's own
// aliasing -- it only band-limits what we ship.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "jet_music.h"
#include "opl_tdeck.h"

#define CHUNK 1024

// jet_music.c can play a pre-rendered clip through jet_audio, but this tool
// is what PRODUCES those clips -- it drives the synth directly and never
// takes that path. Stubs satisfy the linker without dragging the whole
// engine (and its C++ half) into an offline command-line tool.
int jet_audio_load(const char *path) { (void)path; return -1; }
int jet_audio_play(int clip, float volume, int loop, float pitch)
{ (void)clip; (void)volume; (void)loop; (void)pitch; return -1; }
void jet_audio_stop(int voice) { (void)voice; }
void jet_audio_voice_set(int voice, float freq, float volume)
{ (void)voice; (void)freq; (void)volume; }

int main(int argc, char **argv)
{
    const char *in, *out;
    double seconds;
    int decimate = 1;
    FILE *f;
    long len;
    void *buf;
    long total, done;
    int32_t acc[CHUNK];
    int16_t pcm[CHUNK];
    FILE *o;

    if (argc < 4)
    {
        fprintf(stderr, "usage: render_music <in.mid> <out.raw> <seconds>"
                        " [decimate]\n");
        return 2;
    }
    in = argv[1];
    out = argv[2];
    seconds = atof(argv[3]);
    if (argc > 4)
    {
        decimate = atoi(argv[4]);
    }
    if (decimate < 1) decimate = 1;

    f = fopen(in, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", in); return 1; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len)
    {
        fprintf(stderr, "cannot read %s\n", in);
        return 1;
    }
    fclose(f);

    if (!jet_music_init())
    {
        fprintf(stderr, "music init failed\n");
        return 1;
    }
    jet_music_volume(1.0f);
    // Looping, so the render runs straight through the seam: the tail of the
    // loop is rendered with the head already playing, which is exactly what
    // the device will hear when the clip wraps.
    if (!jet_music_play(buf, (size_t)len, 1))
    {
        fprintf(stderr, "could not parse %s\n", in);
        return 1;
    }
    free(buf);

    total = (long)(seconds * 22050.0 + 0.5);
    total -= total % decimate;            // whole output frames only
    done = 0;

    // Render the whole loop to memory first, so it can be NORMALISED.
    // Rendering straight to file left the tracks peaking at 13-20% of full
    // scale -- 14 to 17 dB of headroom thrown away, and then scaled down
    // again by the playback volume, which is why the music was inaudible
    // next to the engine. Offline is exactly where this should be fixed:
    // it costs nothing at run time and it means the in-game volume control
    // starts from a full-scale signal.
    {
        const long outlen = total / decimate;
        int16_t *outbuf = (int16_t *)malloc(sizeof(int16_t) * (size_t)outlen);
        long w = 0;
        long k2;
        int32_t peak = 1;
        double gain;

        if (!outbuf) { fprintf(stderr, "out of memory\n"); return 1; }

        while (done < total)
        {
            int n = (int)(total - done);
            int i;
            if (n > CHUNK) n = CHUNK;
            n -= n % decimate;
            if (n <= 0) break;

            memset(acc, 0, sizeof(int32_t) * (size_t)n);
            jet_music_mix(acc, n);

            for (i = 0; i + decimate <= n; i += decimate)
            {
                int32_t s = 0;
                int k;
                for (k = 0; k < decimate; k++) s += acc[i + k];
                s /= decimate;
                if (w < outlen)
                {
                    // Held at full width for now; the clamp happens after
                    // the gain is known.
                    outbuf[w++] = (int16_t)(s > 32767 ? 32767
                                          : (s < -32768 ? -32768 : s));
                }
                if (s > peak) peak = s;
                if (-s > peak) peak = -s;
            }
            done += n;
        }

        // 0.92 rather than 1.0: playback resamples (the PCM ships at half
        // the mixer rate), and interpolation can overshoot slightly between
        // samples. This leaves room for that without being timid.
        gain = (0.92 * 32767.0) / (double)peak;
        for (k2 = 0; k2 < w; k2++)
        {
            double v = outbuf[k2] * gain;
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            outbuf[k2] = (int16_t)v;
        }

        o = fopen(out, "wb");
        if (!o) { fprintf(stderr, "cannot write %s\n", out); return 1; }
        fwrite(outbuf, sizeof(int16_t), (size_t)w, o);
        printf("  %-46s %5.1f s  %5.2f MB at %5d Hz   normalised +%.1f dB\n",
               out, seconds, w * 2.0 / 1048576.0, 22050 / decimate,
               20.0 * log10(gain));
        free(outbuf);
    }

    fclose(o);
    jet_music_shutdown();
    return 0;
}
