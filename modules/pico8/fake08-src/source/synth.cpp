//
//  ZEPTO-8 — Fantasy console emulator
//
//  Copyright © 2017—2020 Sam Hocevar <sam@hocevar.net>
//
//  This program is free software. It comes without any warranty, to
//  the extent permitted by applicable law. You can redistribute it
//  and/or modify it under the terms of the Do What the Fuck You Want
//  to Public License, Version 2, as published by the WTFPL Task Force.
//  See http://www.wtfpl.net/ for more details.
//

#include "synth.h"

#include <cmath>     // std::fabs
#include <cstdint>   // uint32_t

namespace z8
{

// Fast positive-float fractional parts. For 0 <= x < 2^31 these are bit-exact
// equal to fmod(x,1) / fmod(x,2) but skip the software fmod() call, which
// otherwise ran on every per-sample, per-channel waveform evaluation.
static inline float fract1(float x) { return x - (float)(int)x; }
static inline float fract2(float x) { return x - 2.f * (float)(int)(x * 0.5f); }

// Noise PRNG. Replaces rand()/RAND_MAX (a libc call plus a per-sample divide)
// with an inline xorshift mapped to a uniform [-1,1). Different sequence than
// rand() but the same distribution, so noise/drums sound the same.
static uint32_t s_noise_rng = 0x2545F491u;
static inline float rand_float()
{
    uint32_t x = s_noise_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_noise_rng = x;
    return (float)(x >> 8) * (1.0f / 8388608.0f) - 1.0f; // (x>>8)/2^23 - 1 -> [-1,1)
}

float synth::waveform(synth_param &params)
{
    using std::fabs;

    float advance = params.phi;
    float t = fract1(advance);
    float ret = 0.f;

    bool noiz = params.filters & 0x2;
    bool buzz = params.filters & 0x4;

    // Multipliers were measured from PICO-8 WAV exports. Waveforms are
    // inferred from those exports by guessing what the original formulas
    // could be.
    switch (params.instrument)
    {
        case INST_TRIANGLE:
            ret = 1.0f - fabs(4.f * t - 2.0f);
            if (buzz) {
                // seems to be averaged with a tilted saw
                static float const a = 0.875f;
                float bret = t < a ? 2.f * t / a - 1.f
                            : 2.f * (1.f - t) / (1.f - a) - 1.f;
                ret = ret * 0.75f + bret * 0.25f;
            }
            return ret * 0.5f;
        case INST_TILTED_SAW:
        {
            float a = buzz ? 0.975f : 0.875f;
            ret = t < a ? 2.f * t / a - 1.f
                        : 2.f * (1.f - t) / (1.f - a) - 1.f;
            return ret * 0.5f;
        }
        case INST_SAW:
            ret = (t < 0.5f ? t : t - 1.f);
            // slight offset looping on 2x period
            if (buzz) ret = ret * 0.83f - (fabs(fract2(advance) - 1.0f) < 0.5 ? 0.085f : 0.0f);
            return 0.653f * ret;
        case INST_SQUARE:
            return t < (buzz ? 0.4f : 0.5f) ? 0.25f : -0.25f;
        case INST_PULSE:
            return t < (buzz ? 0.255f : 0.316f) ? 0.25f : -0.25f;
        case INST_ORGAN:
            ret = t < 0.5f ? 3.f - fabs(24.f * t - 6.f)
                           : 1.f - fabs(16.f * t - 12.f);
            if (buzz)
            {
                // add a cut on the first of the two triangles
                ret = t < 0.5f ? ret * 2.0f + 3.0f : ret;
                ret = (t < 0.5f && ret>-1.875f) ? ret * 0.2f - 1.0f : ret + 0.5f;
            }
            return ret / 9.f;
        case INST_NOISE:
        {
            //const float tscale = 22050 / key_to_freq(63);
            const float tscale = 8.858923f;
            float scale = (advance - params.last_advance) * tscale;
            float new_sample = (params.last_sample + scale * rand_float()) / (1.0f + scale);
            
            float factor = 1.0f - params.key / 63.0f;
            ret = new_sample * 1.5f * (1.0f + factor * factor);
                        
            if (noiz)
            {
                // sound a bit like a saw tooth but not quite
                ret *= 2.0f*(t < 0.5f ? t : t - 1.f);
            }

            params.last_advance = advance;
            params.last_sample = new_sample;

            return ret;
        }
        case INST_PHASER:
        {   // sum of two triangle waves with a slightly different frequency
            // the ratio between the frequency seems to be around 110 for c2
            // but it is 97 for c0 and 127 for c5, not sure how to adjust that
            ret = 2.f - fabs(8.f * t - 4.f);
            ret += 1.f - fabs(4.f * fract1(advance * (109.f/110.f)) - 2.f);
            if (buzz)
            {
                // original triangle has freq 1, 3, 5, 7, 9 ...
                // add waves at 2, 6, 10, 14
                ret += 0.25f - fabs(1.f * fract1(advance * 2.0f + 0.5f) - 0.5f);
                // add waves at 4, 12, 20, 28
                ret += 0.125f - fabs(0.5f * fract1(advance * 4.0f) - 0.25f);
            }
            return ret / 6.f;
        }
    }

    return 0.0f;
}

} // namespace z8
