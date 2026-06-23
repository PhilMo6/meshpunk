#include "Audio.h"
#include "synth.h"
#include "filter.h"
#include "hostVmShared.h"
#include "mathhelpers.h"

#include <cstdint>
#include <string>
#include <algorithm>
#include <cmath>
#include <float.h>
#include <cassert>

//playback implementation based on zepto8's
//https://github.com/samhocevar/zepto8/blob/master/src/pico8/sfx.cpp

// Audio experiment mode, set by the platform host at launch (T-Deck reads
// /sd/p8carts/audiomode.txt — see TDeckHost.cpp for the mode list).
extern int g_p8_audio_mode;

// Hot-path lookup tables. The synth runs per sample (22050 Hz x 4 channels);
// on FPUs without hardware divide/exp these expressions dominate the mix cost.
static float s_key_freq_lut[64];          // key_to_freq(0..63)
static float s_offset_per_second_lut[256]; // 22050/(183*speed), speed 1..255
static float s_vol_lut[8];                 // getVolume()/7 (0..7), avoids a per-sample divide
static float key_to_freq(float key);

// Fast positive-float fractional part (== fmodf(x,1) for 0<=x<2^31), skips the
// software fmodf() call on the per-sample, per-channel effect/phase path.
static inline float fract1f(float x) { return x - (float)(int)x; }

Audio::Audio(PicoRam* memory){
    _memory = memory;
    _paused = false;

    for (int k = 0; k < 64; k++)
        s_key_freq_lut[k] = key_to_freq((float)k);
    s_offset_per_second_lut[0] = 22050.0f / 183.0f; // speed clamped to >= 1
    for (int s = 1; s < 256; s++)
        s_offset_per_second_lut[s] = 22050.0f / (183.0f * s);
    for (int v = 0; v < 8; v++)
        s_vol_lut[v] = (float)v / 7.f;

    resetAudioState();
}

void Audio::setPaused(bool paused) {
    _paused = paused;
}

void Audio::resetAudioState() {
    _audioState._musicChannel.count = -1;
    _audioState._musicChannel.pattern = -1;
    _audioState._musicChannel.mask = 0;
    _audioState._musicChannel.volume_music = 0.5f;
    _audioState._musicChannel.volume_sfx = 0.5f;
    _audioState._musicChannel.fade_volume = 0.f;
    _audioState._musicChannel.fade_volume_step = 0.f;
    _audioState._musicChannel.offset = -1;
    _audioState._musicChannel.length = 0;

    for(int i = 0; i < 4; i++) {
        _audioState._sfxChannels[i].main_sfx.sfx = -1;
        _audioState._sfxChannels[i].main_sfx.offset = 0;
        _audioState._sfxChannels[i].main_sfx.time = 0;
        _audioState._sfxChannels[i].main_sfx.prev_key = 24;
        _audioState._sfxChannels[i].main_sfx.prev_vol = 0;
        _audioState._sfxChannels[i].main_sfx.scan_sfx = -1;  // sfx data may have changed (cart load)

        _audioState._sfxChannels[i].custom_sfx.sfx = -1;
        _audioState._sfxChannels[i].custom_sfx.scan_sfx = -1;
        _audioState._sfxChannels[i].sfx_music = -1;
        _audioState._sfxChannels[i].length = 0;
        _audioState._sfxChannels[i].can_loop = true;
        _audioState._sfxChannels[i].is_music = false;
        _audioState._sfxChannels[i].fade = 0.0f;
        _audioState._sfxChannels[i].last_main_instrument = 0;
        _audioState._sfxChannels[i].last_main_key = 0;
        _audioState._sfxChannels[i].reverb_index = 0;
        
        // Reset reverb buffers
        std::memset(_audioState._sfxChannels[i].reverb_2, 0, sizeof(_audioState._sfxChannels[i].reverb_2));
        std::memset(_audioState._sfxChannels[i].reverb_4, 0, sizeof(_audioState._sfxChannels[i].reverb_4));
        
        // Reset damp filters
        _audioState._sfxChannels[i].damp1 = z8::filter(z8::filter::type::highshelf, 2400.0f, 1.0f, -6.0f);
        _audioState._sfxChannels[i].damp2 = z8::filter(z8::filter::type::highshelf, 1000.0f, 1.0f, -12.0f);
    }
}

audioState_t* Audio::getAudioState() {
    return &_audioState;
}

int Audio::api_sfx(int sfx, int channel, int offset, int length){
    // SFX index: valid values are 0..63 for actual samples,
    // -1 to stop sound on a channel, -2 to stop looping on a channel
    // Audio channel: valid values are 0..3, -1 (autoselect), or -2 (stop sfx on any channel)

    // channel: 0..3 are real channels, -1 = autoselect, -2 = stop-on-any.
    // The old guard accepted channel == 4 (an off-by-one inherited from
    // upstream zepto8); every branch below then indexed _sfxChannels[4] —
    // one full sfxChannel (~4.6KB) past the array, with launch_sfx writing
    // its init pattern (float zeros + prev_key=24=0x18) straight across
    // this allocation's heap tail canary and the next block's header. That
    // was the PSRAM "zeroed tail" corruption. Never index by an unclamped
    // channel here.
    if (sfx < -2 || sfx > 63 || channel < -2 || offset > 31)
        return 0;
    // Out-of-range channels behave like autoselect instead of dropping the
    // sound: boulder run ships sfx(8, 4) (gsfx(sfx_diamond, flr(rnd(3))*2))
    // and its author evidently still heard those dings on real PICO-8.
    if (channel > 3)
        channel = -1;

    // CHANNEL -2: to stop the given sound from playing on any channel
    if (channel == -2) {
        for (int i = 0; i < 4; ++i) {
            if (_audioState._sfxChannels[i].main_sfx.sfx == sfx) {
                _audioState._sfxChannels[i].main_sfx.sfx = -1;
            }
        }
        return 0;
    }

    if (sfx == -1)
    {
        // Stop playing if sfx is a non musical channel
        if (channel != -1)
        {
            if (!_audioState._sfxChannels[channel].is_music)
                _audioState._sfxChannels[channel].main_sfx.sfx = -1;
        }
        else
        {
            // stop playing all non musical channels
            for (int i = 0; i < 4; ++i)
            {
                if (!_audioState._sfxChannels[i].is_music)
                    _audioState._sfxChannels[i].main_sfx.sfx = -1;
            }
        }
        return 0;
    }

    if (sfx == -2)
    {
        // Stop looping if sfx is a non musical channel
        if (channel != -1)
        {
            if (!_audioState._sfxChannels[channel].is_music)
                _audioState._sfxChannels[channel].can_loop = false;
        }
        else
        {
            // stop looping all non musical channels
            for (int i = 0; i < 4; ++i)
            {
                if (!_audioState._sfxChannels[i].is_music)
                    _audioState._sfxChannels[i].can_loop = false;
            }
        }
        return 0;
    }

    // Find the first available channel: either a channel that plays
    // nothing, or a channel that is already playing this sample (in
    // this case PICO-8 decides to forcibly reuse that channel, which
    // is reasonable)
    if (channel == -1)
    {
        for (int i = 0; i < 4; ++i)
        {
            if (((1 << i) & _audioState._musicChannel.mask) != 0)
                continue;

            if (_audioState._sfxChannels[i].main_sfx.sfx == -1 ||
                _audioState._sfxChannels[i].main_sfx.sfx == sfx)
            {
                channel = i;
                break;
            }
        }
    }

    // if no free channel is found, stop music's first interruptable channel
    if (channel == -1)
    {
        for (int i = 0; i < 4; ++i)
        {
            if (((1 << i) & _audioState._musicChannel.mask) != 0)
                continue;

            if (_audioState._sfxChannels[i].is_music)
            {
                channel = i;
                break;
            }
        }
    }

    // If still no channel found, the PICO-8 strategy seems to be to
    // stop the channel with fastest speed (if there are several, take the latest one)
    if (channel == -1)
    {
        uint8_t fastest_speed = 255;
        for (int i = 0; i < 4; ++i)
        {
            if (((1 << i) & _audioState._musicChannel.mask) != 0)
                continue;

            int const index = _audioState._sfxChannels[i].main_sfx.sfx;
            if (index < 0 || index >= 64)
                continue;
                
            struct sfx const& sfx_data = _memory->sfx[index];
            if (sfx_data.speed <= fastest_speed)
            {
                channel = i;
                fastest_speed = sfx_data.speed;
            }
        }
    }

    // still no channel found, the sfx is ignored
    if (channel == -1)
        return 0;

    // Stop any channel playing the same sfx
    for (int i = 0; i < 4; ++i)
        if (_audioState._sfxChannels[i].main_sfx.sfx == sfx)
            _audioState._sfxChannels[i].main_sfx.sfx = -1;

    // if there is already a music playing sfx, store it to be picked back up later before it's replaced
    if (_audioState._sfxChannels[channel].main_sfx.sfx != -1 && _audioState._sfxChannels[channel].is_music)
    {
        _audioState._sfxChannels[channel].sfx_music = _audioState._sfxChannels[channel].main_sfx.sfx;
    }

    // Play this sound!
    launch_sfx(sfx, channel, (float)std::max(0, offset), (float)std::max(0, length), false);

    return channel;
}

void Audio::api_music(int pattern, int16_t fade_len, int16_t mask){
    // pattern: 0..63, -1 to stop music.
    // fade_len: fade length in milliseconds (default 0)
    // mask: reserved channels

    if (pattern < -1 || pattern > 63)
        return;

    if (pattern == -1)
    {
        // Music will stop when fade out is finished
        _audioState._musicChannel.fade_volume_step = fade_len <= 0 ? -FLT_MAX
                                  : -_audioState._musicChannel.fade_volume * (1000.f / fade_len);
        return;
    }

    // Initialise music state for the whole song
    _audioState._musicChannel.count = 0;
    _audioState._musicChannel.mask = mask & 0xf;

    _audioState._musicChannel.fade_volume = 1.f;
    _audioState._musicChannel.fade_volume_step = 0.f;
    if (fade_len > 0)
    {
        _audioState._musicChannel.fade_volume = 0.f;
        _audioState._musicChannel.fade_volume_step = 1000.f / fade_len;
    }

    set_music_pattern(pattern);
}

void Audio::set_music_pattern(int pattern) {
    using std::max, std::min;

    // stop all previously playing music sounds
    for (int n = 0; n < 4; ++n)
        if (_audioState._sfxChannels[n].is_music)
        {
            _audioState._sfxChannels[n].main_sfx.sfx = -1;
            _audioState._sfxChannels[n].sfx_music = -1;
        }

    if (pattern < 0 || pattern > 63)
    {
        _audioState._musicChannel.pattern = -1;
        _audioState._musicChannel.count = -1;
        _audioState._musicChannel.offset = -1;
        _audioState._musicChannel.mask = 0;
        _audioState._musicChannel.length = 0.0f;
        return;
    }

    // Get song channels
    uint8_t channels[] = {
        _memory->songs[pattern].getSfx0(),
        _memory->songs[pattern].getSfx1(),
        _memory->songs[pattern].getSfx2(),
        _memory->songs[pattern].getSfx3(),
    };

    // Find music duration
    // if there is at least one non-looping channel:
    // length of the first non-looping channel
    // if not (all channels are looping):
    // length of slowest channel (so it stops when all channels have reached at least 32 steps)

    int16_t duration_looping = -1;
    int16_t duration_no_loop = -1;
    for (int i = 0; i < 4; ++i)
    {
        int n = channels[i];
        if (n & 0x40)
            continue;

        auto &sfx_data = _memory->sfx[n & 0x3f];
        bool has_loop = sfx_data.loopRangeEnd > 0 && sfx_data.loopRangeEnd > sfx_data.loopRangeStart;
        if (has_loop)
        {
            int16_t sfx_duration = 32 * sfx_data.speed;
            duration_looping = max(duration_looping, sfx_duration);
        }
        else
        {
            // take duration of first non_looping channel
            int16_t end_time = 32;
            if (sfx_data.loopRangeEnd == 0 && sfx_data.loopRangeStart > 0)
            {
                end_time = min<int16_t>(end_time, sfx_data.loopRangeStart);
            }
            duration_no_loop = end_time * sfx_data.speed;
            break;
        }
    }

    int16_t duration = duration_no_loop > 0 ? duration_no_loop : duration_looping;
    if (duration <= 0)
    {
        // Default duration if no valid sfx found
        duration = 32;
    }

    // Initialise music state for the current pattern
    _audioState._musicChannel.pattern = pattern;
    _audioState._musicChannel.offset = 0;
    _audioState._musicChannel.length = (float)duration;

    // Play music sfx on active channels
    for (int i = 0; i < 4; ++i)
    {
        int n = channels[i];
        if (n & 0x40)
            continue;

        if (_audioState._sfxChannels[i].main_sfx.sfx == -1)
        {
            launch_sfx(n, i, 0, 0, true);
        }
        else
        {
            // if there is already a sfx playing, we store the music one to be played later, when the current sfx stop
            _audioState._sfxChannels[i].sfx_music = n;
        }
    }
}

void Audio::launch_sfx(int16_t sfx, int16_t chan, float offset, float length, bool is_music)
{
    _audioState._sfxChannels[chan].main_sfx.sfx = sfx;
    _audioState._sfxChannels[chan].main_sfx.offset = std::max(0.f, offset);
    _audioState._sfxChannels[chan].main_sfx.time = 0.f;
    _audioState._sfxChannels[chan].length = std::max(0.f, length);
    _audioState._sfxChannels[chan].can_loop = true;
    _audioState._sfxChannels[chan].is_music = is_music;
    _audioState._sfxChannels[chan].last_main_instrument = 0xff;
    _audioState._sfxChannels[chan].last_main_key = 0xff;
    // Playing an instrument starting with the note C-2 and the
    // slide effect causes no noticeable pitch variation in PICO-8,
    // so I assume this is the default value for "previous key".
    _audioState._sfxChannels[chan].main_sfx.prev_key = 24;
    // There is no default value for "previous volume".
    _audioState._sfxChannels[chan].main_sfx.prev_vol = 0.f;
}

static float key_to_freq(float key)
{
    using std::exp2;
    return 440.f * exp2((key - 33.f) / 12.f);
}

int16_t Audio::getCurrentSfxId(int channel){
    return _audioState._sfxChannels[channel].main_sfx.sfx;
}

int Audio::getCurrentNoteNumber(int channel){
    return _audioState._sfxChannels[channel].main_sfx.sfx < 0 
        ? -1
        : (int)_audioState._sfxChannels[channel].main_sfx.offset;
}

int16_t Audio::getCurrentMusic(){
    return _audioState._musicChannel.pattern;
}

int16_t Audio::getMusicPatternCount(){
    return _audioState._musicChannel.count;
}

int16_t Audio::getMusicTickCount(){
    return (int16_t)_audioState._musicChannel.offset;
}

void Audio::update_sfx_state(sfx_state& cur_sfx, z8::synth_param& new_synth, 
                              float freq_factor, float length, bool is_music,
                              bool can_loop, bool half_rate, float inv_frames_per_second)
{
    using std::max;

    if (cur_sfx.sfx == -1) return;

    int const index = cur_sfx.sfx;
    assert(index >= 0 && index < 64);
    struct sfx const& sfx_data = _memory->sfx[index];

    // Speed must be 1—255 otherwise the SFX is invalid
    int const speed = max(1, (int)sfx_data.speed);

    float const offset = cur_sfx.offset;
    float const time = cur_sfx.time;

    // PICO-8 exports instruments as 22050 Hz WAV files with 183 samples
    // per speed unit per note, so this is how much we should advance
    float const offset_per_second = s_offset_per_second_lut[speed & 0xff];
    float const offset_per_frame = offset_per_second * inv_frames_per_second;
    float next_offset = offset + offset_per_frame;
    float next_time = time + offset_per_frame;

    // Handle SFX loops. From the documentation: "Looping is turned
    // off when the start index >= end index".
    float const loop_range = float(sfx_data.loopRangeEnd - sfx_data.loopRangeStart);
    if (loop_range > 0.f && next_offset >= sfx_data.loopRangeEnd && can_loop)
    {
        next_offset = fmodf(next_offset - sfx_data.loopRangeStart, loop_range)
            + sfx_data.loopRangeStart;
    }

    bool has_end = false;
    float end_time = 32.f;
    if (length > 0.0f)
    {
        has_end = true;
        end_time = length;
    }
    // in pico 8, strangely, len is not applied to musical sfx except for pattern len calculation
    // it's probably a bug
    if (!is_music && sfx_data.loopRangeEnd == 0 && sfx_data.loopRangeStart > 0)
    {
        has_end = true;
        end_time = std::min<float>(end_time, sfx_data.loopRangeStart);
    }
    // if there is no loop, we end after the length
    if (loop_range <= 0.f)
    {
        has_end = true;
        // if not a music sfx, check where is the last note to early stop.
        // The 32-note scan runs per sample otherwise; memoize it per
        // (sfx id, current note row) so runtime sfx pokes are still picked
        // up at the next note boundary.
        if (!is_music)
        {
            int8_t const cur_note = (int8_t)offset;
            if (cur_sfx.scan_sfx != index || cur_sfx.scan_note != cur_note)
            {
                int last_note = 0;
                for (int n = 0; n < 32; ++n)
                {
                    if (sfx_data.notes[n].getVolume() > 0)
                    {
                        last_note = std::min(32, n + 1);
                    }
                }
                cur_sfx.scan_sfx = index;
                cur_sfx.scan_note = cur_note;
                cur_sfx.scan_last_note = (uint8_t)last_note;
            }
            end_time = std::min(end_time, float(cur_sfx.scan_last_note));
        }
    }

    if (offset < 32)
    {
        int const note_id = (int)offset;
        int const next_note_id = (int)next_offset;

        uint8_t key = sfx_data.notes[note_id].getKey();
        float volume = s_vol_lut[sfx_data.notes[note_id].getVolume() & 7];
        float freq = s_key_freq_lut[key & 63] * freq_factor;

        if (volume > 0.f)
        {
            int const fx = sfx_data.notes[note_id].getEffect();

            // Apply effect, if any
            switch (fx)
            {
            case FX_NO_EFFECT:
                break;
            case FX_SLIDE:
            {
                float t = fract1f(offset);
                // From the documentation: "Slide to the next note and volume",
                // but it's actually _from_ the _prev_ note and volume.
                freq = lerp(s_key_freq_lut[cur_sfx.prev_key & 63], freq, t);
                if (cur_sfx.prev_vol > 0.f)
                    volume = lerp(cur_sfx.prev_vol, volume, t);
                break;
            }
            case FX_VIBRATO:
            {
                // 7.5f and 0.25f were found empirically by matching
                // frequency graphs of PICO-8 instruments.
                float t = fabsf(fract1f(7.5f * offset / offset_per_second)) - 0.5f - 0.25f;
                // Vibrato half a semi-tone, so multiply by pow(2,1/12)
                freq = lerp(freq, freq * 1.059463094359f, t);
                break;
            }
            case FX_DROP:
                freq *= 1.f - fract1f(offset);
                break;
            case FX_FADE_IN:
                volume *= fract1f(offset);
                break;
            case FX_FADE_OUT:
                volume *= 1.f - fract1f(offset);
                break;
            case FX_ARP_FAST:
            case FX_ARP_SLOW:
            {
                // From the documentation:
                // "6 arpeggio fast  //  Iterate over groups of 4 notes at speed of 4
                //  7 arpeggio slow  //  Iterate over groups of 4 notes at speed of 8"
                // "If the SFX speed is <= 8, arpeggio speeds are halved to 2, 4"
                int const m = (speed <= 8 ? 32 : 16) / (fx == FX_ARP_FAST ? 4 : 8);
                int const n = (int)(m * 7.5f * offset / offset_per_second);
                int const arp_note = (note_id & ~3) | (n & 3);
                freq = s_key_freq_lut[sfx_data.notes[arp_note].getKey() & 63];
                break;
            }
            }

            if (half_rate) freq *= 0.5f;

            new_synth.key = key;
            new_synth.freq = freq;
            new_synth.instrument = sfx_data.notes[note_id].getWaveform();
            new_synth.custom = sfx_data.notes[note_id].getCustom();
            new_synth.filters = sfx_data.filters;
            new_synth.volume = volume;
            new_synth.is_music = is_music;

            new_synth.phi = new_synth.phi + freq * inv_frames_per_second;
        }

        if (next_note_id != note_id)
        {
            cur_sfx.prev_key = sfx_data.notes[note_id].getKey();
            cur_sfx.prev_vol = s_vol_lut[sfx_data.notes[note_id].getVolume() & 7];
        }
        // Do NOT rebase phi here to fight float-ulp drift on held notes:
        // the detune/phaser second waves derive their phase as phi*factor
        // with factors like 3/2, 200/199, 109/110 — subtracting even an
        // even integer from phi keeps the fundamental exact but jumps the
        // detuned wave's phase by a fraction, which pops audibly on every
        // wrap (tried 2026-06-12, heard as static clicks in music). The
        // existing fmod() on "harsh" transitions covers the common case.
    }

    cur_sfx.offset = next_offset;
    cur_sfx.time = next_time;

    if (has_end && next_time >= end_time)
    {
        cur_sfx.sfx = -1;
    }
}

float Audio::get_synth_sample(z8::synth_param& params)
{
    // Play note
    float waveform = z8::synth::waveform(params);

    uint8_t detune = (params.filters / 8) % 3;
    if (detune != 0 && params.instrument != z8::synth::INST_NOISE)
    {
        // detune is a second wave slightly offset
        float factor = 1.0f;
        if (params.instrument == z8::synth::INST_TRIANGLE) 
            factor = (detune == 1) ? 3.0f / 4.0f : 3.0f / 2.0f; // triangle detune adds a fourth or a fifth
        else if (params.instrument == z8::synth::INST_ORGAN) 
            factor = (detune == 1) ? 200.0f / 199.0f : 800.0f / 199.0f; // slight offset, detune 2 at 2 octave above
        else if (params.instrument == z8::synth::INST_PHASER) 
            factor = (detune == 1) ? 49.0f / 50.0f : 400.0f / 199.0f;
        else 
            factor = (detune == 1) ? 200.0f / 199.0f : 400.0f / 199.0f; // others are slight offset, detune 2 at 1 octave above

        z8::synth_param second_wave = params;
        second_wave.phi *= factor;
        if (detune == 2 && params.instrument == z8::synth::INST_ORGAN) 
            second_wave.instrument = z8::synth::INST_TRIANGLE; // organ second wave seems to be simpler
        waveform += z8::synth::waveform(second_wave) * 0.5f;
    }

    float volume = params.volume;

    // Apply master music volume from fade in/out
    if (params.is_music)
    {
        volume *= _audioState._musicChannel.fade_volume * _audioState._musicChannel.volume_music;
    }
    else
    {
        volume *= _audioState._musicChannel.volume_sfx;
    }

    return std::clamp(waveform * volume, -1.0f, 1.0f);
}

void Audio::FillAudioBuffer(void *audioBuffer, size_t offset, size_t size){
    if (audioBuffer == nullptr) {
        return;
    }

    uint32_t *buffer = (uint32_t *)audioBuffer;

    // Output silence when paused
    if (_paused) {
        for (size_t i = 0; i < size; ++i){
            buffer[i] = 0;
        }
        return;
    }

    for (size_t i = 0; i < size; ++i){
        int32_t sample = 0;
        float channel_mix = 0.0f;

        bool is_pause = _memory->drawState.soundPauseState == 1;

        for (int chan = 0; chan < 4; ++chan) {
            float inv_frames_per_second = ((_memory->hwState.half_rate & (1 << chan)) ? 0.5f : 1.0f) * (1.0f / 22050.0f);

            sfxChannel& channel_state = _audioState._sfxChannels[chan];

            // Advance music using the first channel
            if (chan == 0 && _audioState._musicChannel.pattern != -1 && !is_pause)
            {
                float const offset_per_second = 22050.0f / 183.0f;
                float const offset_per_frame = offset_per_second * inv_frames_per_second;
                _audioState._musicChannel.offset += offset_per_frame;
                _audioState._musicChannel.fade_volume += (float)(_audioState._musicChannel.fade_volume_step * inv_frames_per_second);
                _audioState._musicChannel.fade_volume = clamp(_audioState._musicChannel.fade_volume, 0.f, 1.f);

                if (_audioState._musicChannel.fade_volume_step < 0 && _audioState._musicChannel.fade_volume <= 0)
                {
                    set_music_pattern(-1);
                }
                else if (_audioState._musicChannel.offset >= _audioState._musicChannel.length)
                {
                    int16_t next_pattern = _audioState._musicChannel.pattern + 1;
                    int16_t next_count = _audioState._musicChannel.count + 1;
                    if (_memory->songs[_audioState._musicChannel.pattern].getStop())
                    {
                        next_pattern = -1;
                        next_count = -1;
                    }
                    else if (_memory->songs[_audioState._musicChannel.pattern].getLoop())
                        while (--next_pattern > 0 && !_memory->songs[next_pattern].getStart())
                            ;

                    _audioState._musicChannel.count = next_count;
                    set_music_pattern(next_pattern);
                }
            }

            // if no sfx is playing and there is a music sfx stored
            if (channel_state.main_sfx.sfx == -1 && channel_state.sfx_music != -1 && !is_pause)
            {
                int const index = channel_state.sfx_music;
                assert(index >= 0 && index < 64);
                struct sfx const& sfx_data = _memory->sfx[index];

                // compute offset to start the sfx to
                bool want_play = true;
                int const speed = std::max(1, (int)sfx_data.speed);
                float new_offset = _audioState._musicChannel.offset / speed;

                float const loop_range = (float)(sfx_data.loopRangeEnd - sfx_data.loopRangeStart);
                if (loop_range > 0.f && channel_state.can_loop)
                {
                    if (new_offset > sfx_data.loopRangeStart)
                        new_offset = fmodf(new_offset - sfx_data.loopRangeStart, loop_range) + sfx_data.loopRangeStart;
                }
                else
                {
                    if (new_offset > 32.0f)
                        want_play = false;
                }

                if (want_play)
                {
                    launch_sfx(index, chan, (float)new_offset, 0, true);
                }
                channel_state.sfx_music = -1;
            }

            z8::synth_param& last_synth = channel_state.last_synth;
            z8::synth_param new_synth;
            new_synth.phi = last_synth.phi;
            new_synth.last_advance = last_synth.last_advance;
            new_synth.last_sample = last_synth.last_sample;
            float value = 0.0f;

            if (!is_pause)
            {
                float main_sfx_base_offset = channel_state.main_sfx.offset;
                bool half_rate = _memory->hwState.half_rate & (1 << (chan + 4));
                // update main sfx
                update_sfx_state(channel_state.main_sfx, new_synth, 1.0f, channel_state.length, 
                                channel_state.is_music, channel_state.can_loop, half_rate, inv_frames_per_second);

                bool restart_custom = new_synth.instrument != channel_state.last_main_instrument || 
                                      new_synth.key != channel_state.last_main_key;
                channel_state.last_main_instrument = new_synth.instrument;
                channel_state.last_main_key = new_synth.key;

                if (new_synth.volume > 0.0f)
                {
                    if (new_synth.custom)
                    {
                        // also need to restart if main_sfx loops (new offset is before base offset)
                        if (channel_state.main_sfx.offset < main_sfx_base_offset) restart_custom = true;
                        // also need to restart if custom_sfx.sfx == -1 (it has ended) and main_sfx.offset is changing integer
                        if (channel_state.custom_sfx.sfx == -1 && 
                            floorf(main_sfx_base_offset) != floorf(channel_state.main_sfx.offset))
                            restart_custom = true;

                        if (restart_custom)
                        {
                            channel_state.custom_sfx.sfx = new_synth.instrument;
                            channel_state.custom_sfx.offset = 0.0;
                            channel_state.custom_sfx.time = 0.0;
                        }
                        new_synth.phi = last_synth.phi;
                        float const freq_base = s_key_freq_lut[24]; // C2
                        float freq_factor = new_synth.freq / freq_base;
                        float main_sfx_volume = new_synth.volume;
                        update_sfx_state(channel_state.custom_sfx, new_synth, freq_factor, 0.0f, false, true, half_rate, inv_frames_per_second);
                        new_synth.volume *= main_sfx_volume;
                    }
                    value = get_synth_sample(new_synth);
                }
            }

            // detect harsh changes of states, and do a small fade
            float freq_threshold = std::min(new_synth.freq, last_synth.freq) * 0.01f;
            if (std::abs(new_synth.volume - last_synth.volume) > 0.1f
                || std::abs(new_synth.freq - last_synth.freq) > freq_threshold
                || new_synth.instrument != last_synth.instrument)
            {
                if (channel_state.fade <= 0.0f) // avoid continuous fades, it messes with noise algo
                {
                    channel_state.fade_synth = last_synth;
                }
                channel_state.fade = 1.0f;
                // Rebase phi so it can't grow into float-precision loss.
                // Mode 3 also shifts the noise instrument's last_advance in
                // lockstep: a bare fmod() yanks phi back by an integer while
                // last_advance stays where it was, so the next noise sample
                // sees a big negative phase delta through 1/(1+scale) — a
                // one-sample transient on every harsh transition that the
                // fade only partially masks. Own mode so its audibility can
                // be A/B'd on speaker.
                if (g_p8_audio_mode == 3) {
                    float const w = (float)(int)new_synth.phi; // floor(phi) for phi>=0
                    new_synth.phi -= w;
                    new_synth.last_advance -= w;
                } else {
                    new_synth.phi = fract1f(new_synth.phi);
                }
            }
            last_synth = new_synth;

            uint8_t reverb = (last_synth.filters / 24) % 3;
            uint8_t dampen = (last_synth.filters / 72) % 3;
            float chan_reverb1_value = reverb == 1 ? 1.0f : 0.0f;
            float chan_reverb2_value = reverb == 2 ? 1.0f : 0.0f;
            float chan_damp1_value = dampen == 1 ? 1.0f : 0.0f;
            float chan_damp2_value = dampen == 2 ? 1.0f : 0.0f;

            if (channel_state.fade > 0.0f)
            {
                channel_state.fade_synth.phi = channel_state.fade_synth.phi + 
                    (float)(channel_state.fade_synth.freq * inv_frames_per_second);
                float value_fade = get_synth_sample(channel_state.fade_synth);
                
                value = lerp(value, value_fade, channel_state.fade);

                // Also mix fade filter values
                uint8_t fade_reverb = (channel_state.fade_synth.filters / 24) % 3;
                uint8_t fade_dampen = (channel_state.fade_synth.filters / 72) % 3;
                chan_reverb1_value = lerp(chan_reverb1_value, fade_reverb == 1 ? 1.0f : 0.0f, channel_state.fade);
                chan_reverb2_value = lerp(chan_reverb2_value, fade_reverb == 2 ? 1.0f : 0.0f, channel_state.fade);
                chan_damp1_value = lerp(chan_damp1_value, fade_dampen == 1 ? 1.0f : 0.0f, channel_state.fade);
                chan_damp2_value = lerp(chan_damp2_value, fade_dampen == 2 ? 1.0f : 0.0f, channel_state.fade);

                // Declick fade rate, selected by g_p8_audio_mode:
                //   0: 130/s everywhere (~7.7ms — stock upstream)
                //   1/3: noise->noise transitions 600/s (~1.7ms). At 130/s
                //        the crossfade spans most of a speed-1/2 drum row,
                //        doubling each hit with its predecessor's tail
                //        ("echo quality" vs real PICO-8 — boulder run).
                //   2: 600/s everywhere (known to add static in note-dense
                //      tonal music — celeste 2; kept for A/B reference)
                float fade_rate = 130.0f;
                if (g_p8_audio_mode == 2) {
                    fade_rate = 600.0f;
                } else if ((g_p8_audio_mode == 1 || g_p8_audio_mode == 3) &&
                           channel_state.fade_synth.instrument == z8::synth::INST_NOISE &&
                           last_synth.instrument == z8::synth::INST_NOISE) {
                    fade_rate = 600.0f;
                }
                channel_state.fade -= fade_rate * inv_frames_per_second;
            }

            // hw can force fx passes
            if (_memory->hwState.reverb & (1 << (chan + 4))) chan_reverb1_value = 1.0f;
            if (_memory->hwState.reverb & (1 << chan)) chan_reverb2_value = 1.0f;
            if (_memory->hwState.lowpass & (1 << (chan + 4))) chan_damp1_value = 1.0f;
            if (_memory->hwState.lowpass & (1 << chan)) chan_damp2_value = 1.0f;
            
            // One index pass per sample: idx366 == idx732 mod 366 (732 is
            // 2*366, so wrapping the counter at 732 preserves both delay-
            // line phases). The explicit wrap also fixes the old unbounded
            // ++reverb_index, which overflowed negative after ~27h of audio
            // and made `% 366` index out of bounds.
            int const idx732 = channel_state.reverb_index;
            int const idx366 = idx732 >= 366 ? idx732 - 366 : idx732;
            if (chan_reverb1_value > 0.0f)
                value += chan_reverb1_value * channel_state.reverb_2[idx366] * 0.5f;
            if (chan_reverb2_value > 0.0f)
                value += chan_reverb2_value * channel_state.reverb_4[idx732] * 0.5f;

            channel_state.reverb_2[idx366] = value;
            channel_state.reverb_4[idx732] = value;
            if (++channel_state.reverb_index >= 732) channel_state.reverb_index = 0;

            // The damp biquads run unconditionally (upstream behavior): they
            // must stay warm on the live signal, or enabling dampen mid-note
            // starts the filter from stale state — an audible tick. (A
            // skip-when-unused version was tried 2026-06-12 and reverted;
            // the saved ~176K biquads/s was ~1% of the core.)
            float value_damp1 = channel_state.damp1.run(value);
            if (chan_damp1_value > 0.0f) value = lerp(value, value_damp1, chan_damp1_value);

            float value_damp2 = channel_state.damp2.run(value);
            if (chan_damp2_value > 0.0f) value = lerp(value, value_damp2, chan_damp2_value);

            int16_t chan_sample = (int16_t)(32767.99f * std::clamp(value, -0.99f, 0.99f));

            // Apply hardware distort
            if (_memory->hwState.distort & (1 << chan))
            {
                chan_sample = chan_sample / 0x1000 * 0x1249;
            }
            else if (_memory->hwState.distort & (1 << (chan + 4)))
            {
                chan_sample = (chan_sample - (chan_sample < 0 ? 0x1000 : 0)) / 0x1000 * 0x1249;
            }
            channel_mix += chan_sample;
        }

        sample = (int32_t)std::clamp(channel_mix, -32767.9f, 32767.9f);

        //buffer is stereo, so just send the mono sample to both channels
        buffer[i] = ((uint32_t)(uint16_t)sample << 16) | ((uint16_t)sample);
    }
}


void Audio::FillMonoAudioBuffer(void *audioBuffer, size_t offset, size_t size){
    if (audioBuffer == nullptr) {
        return;
    }

    int16_t *buffer = (int16_t *)audioBuffer;

    // Output silence when paused
    if (_paused) {
        for (size_t i = 0; i < size; ++i){
            buffer[i] = 0;
        }
        return;
    }

    for (size_t i = 0; i < size; ++i){
        float channel_mix = 0.0f;

        bool is_pause = _memory->drawState.soundPauseState == 1;

        for (int chan = 0; chan < 4; ++chan) {
            float inv_frames_per_second = ((_memory->hwState.half_rate & (1 << chan)) ? 0.5f : 1.0f) * (1.0f / 22050.0f);

            sfxChannel& channel_state = _audioState._sfxChannels[chan];

            // Advance music using the first channel
            if (chan == 0 && _audioState._musicChannel.pattern != -1 && !is_pause)
            {
                float const offset_per_second = 22050.0f / 183.0f;
                float const offset_per_frame = offset_per_second * inv_frames_per_second;
                _audioState._musicChannel.offset += offset_per_frame;
                _audioState._musicChannel.fade_volume += (float)(_audioState._musicChannel.fade_volume_step * inv_frames_per_second);
                _audioState._musicChannel.fade_volume = clamp(_audioState._musicChannel.fade_volume, 0.f, 1.f);

                if (_audioState._musicChannel.fade_volume_step < 0 && _audioState._musicChannel.fade_volume <= 0)
                {
                    set_music_pattern(-1);
                }
                else if (_audioState._musicChannel.offset >= _audioState._musicChannel.length)
                {
                    int16_t next_pattern = _audioState._musicChannel.pattern + 1;
                    int16_t next_count = _audioState._musicChannel.count + 1;
                    if (_memory->songs[_audioState._musicChannel.pattern].getStop())
                    {
                        next_pattern = -1;
                        next_count = -1;
                    }
                    else if (_memory->songs[_audioState._musicChannel.pattern].getLoop())
                        while (--next_pattern > 0 && !_memory->songs[next_pattern].getStart())
                            ;

                    _audioState._musicChannel.count = next_count;
                    set_music_pattern(next_pattern);
                }
            }

            // if no sfx is playing and there is a music sfx stored
            if (channel_state.main_sfx.sfx == -1 && channel_state.sfx_music != -1 && !is_pause)
            {
                int const index = channel_state.sfx_music;
                assert(index >= 0 && index < 64);
                struct sfx const& sfx_data = _memory->sfx[index];

                bool want_play = true;
                int const speed = std::max(1, (int)sfx_data.speed);
                float new_offset = _audioState._musicChannel.offset / speed;

                float const loop_range = (float)(sfx_data.loopRangeEnd - sfx_data.loopRangeStart);
                if (loop_range > 0.f && channel_state.can_loop)
                {
                    if (new_offset > sfx_data.loopRangeStart)
                        new_offset = fmodf(new_offset - sfx_data.loopRangeStart, loop_range) + sfx_data.loopRangeStart;
                }
                else
                {
                    if (new_offset > 32.0f)
                        want_play = false;
                }

                if (want_play)
                {
                    launch_sfx(index, chan, (float)new_offset, 0, true);
                }
                channel_state.sfx_music = -1;
            }

            z8::synth_param& last_synth = channel_state.last_synth;
            z8::synth_param new_synth;
            new_synth.phi = last_synth.phi;
            new_synth.last_advance = last_synth.last_advance;
            new_synth.last_sample = last_synth.last_sample;
            float value = 0.0f;

            if (!is_pause)
            {
                float main_sfx_base_offset = channel_state.main_sfx.offset;
                bool half_rate = _memory->hwState.half_rate & (1 << (chan + 4));
                update_sfx_state(channel_state.main_sfx, new_synth, 1.0f, channel_state.length, 
                                channel_state.is_music, channel_state.can_loop, half_rate, inv_frames_per_second);

                bool restart_custom = new_synth.instrument != channel_state.last_main_instrument || 
                                      new_synth.key != channel_state.last_main_key;
                channel_state.last_main_instrument = new_synth.instrument;
                channel_state.last_main_key = new_synth.key;

                if (new_synth.volume > 0.0f)
                {
                    if (new_synth.custom)
                    {
                        if (channel_state.main_sfx.offset < main_sfx_base_offset) restart_custom = true;
                        if (channel_state.custom_sfx.sfx == -1 && 
                            floorf(main_sfx_base_offset) != floorf(channel_state.main_sfx.offset))
                            restart_custom = true;

                        if (restart_custom)
                        {
                            channel_state.custom_sfx.sfx = new_synth.instrument;
                            channel_state.custom_sfx.offset = 0.0;
                            channel_state.custom_sfx.time = 0.0;
                        }
                        new_synth.phi = last_synth.phi;
                        float const freq_base = s_key_freq_lut[24];
                        float freq_factor = new_synth.freq / freq_base;
                        float main_sfx_volume = new_synth.volume;
                        update_sfx_state(channel_state.custom_sfx, new_synth, freq_factor, 0.0f, false, true, half_rate, inv_frames_per_second);
                        new_synth.volume *= main_sfx_volume;
                    }
                    value = get_synth_sample(new_synth);
                }
            }

            float freq_threshold = std::min(new_synth.freq, last_synth.freq) * 0.01f;
            if (std::abs(new_synth.volume - last_synth.volume) > 0.1f
                || std::abs(new_synth.freq - last_synth.freq) > freq_threshold
                || new_synth.instrument != last_synth.instrument)
            {
                if (channel_state.fade <= 0.0f)
                {
                    channel_state.fade_synth = last_synth;
                }
                channel_state.fade = 1.0f;
                // Rebase phi so it can't grow into float-precision loss.
                // Mode 3 also shifts the noise instrument's last_advance in
                // lockstep: a bare fmod() yanks phi back by an integer while
                // last_advance stays where it was, so the next noise sample
                // sees a big negative phase delta through 1/(1+scale) — a
                // one-sample transient on every harsh transition that the
                // fade only partially masks. Own mode so its audibility can
                // be A/B'd on speaker.
                if (g_p8_audio_mode == 3) {
                    float const w = (float)(int)new_synth.phi; // floor(phi) for phi>=0
                    new_synth.phi -= w;
                    new_synth.last_advance -= w;
                } else {
                    new_synth.phi = fract1f(new_synth.phi);
                }
            }
            last_synth = new_synth;

            uint8_t reverb = (last_synth.filters / 24) % 3;
            uint8_t dampen = (last_synth.filters / 72) % 3;
            float chan_reverb1_value = reverb == 1 ? 1.0f : 0.0f;
            float chan_reverb2_value = reverb == 2 ? 1.0f : 0.0f;
            float chan_damp1_value = dampen == 1 ? 1.0f : 0.0f;
            float chan_damp2_value = dampen == 2 ? 1.0f : 0.0f;

            if (channel_state.fade > 0.0f)
            {
                channel_state.fade_synth.phi = channel_state.fade_synth.phi + 
                    (float)(channel_state.fade_synth.freq * inv_frames_per_second);
                float value_fade = get_synth_sample(channel_state.fade_synth);
                
                value = lerp(value, value_fade, channel_state.fade);

                uint8_t fade_reverb = (channel_state.fade_synth.filters / 24) % 3;
                uint8_t fade_dampen = (channel_state.fade_synth.filters / 72) % 3;
                chan_reverb1_value = lerp(chan_reverb1_value, fade_reverb == 1 ? 1.0f : 0.0f, channel_state.fade);
                chan_reverb2_value = lerp(chan_reverb2_value, fade_reverb == 2 ? 1.0f : 0.0f, channel_state.fade);
                chan_damp1_value = lerp(chan_damp1_value, fade_dampen == 1 ? 1.0f : 0.0f, channel_state.fade);
                chan_damp2_value = lerp(chan_damp2_value, fade_dampen == 2 ? 1.0f : 0.0f, channel_state.fade);

                // Declick fade rate, selected by g_p8_audio_mode:
                //   0: 130/s everywhere (~7.7ms — stock upstream)
                //   1/3: noise->noise transitions 600/s (~1.7ms). At 130/s
                //        the crossfade spans most of a speed-1/2 drum row,
                //        doubling each hit with its predecessor's tail
                //        ("echo quality" vs real PICO-8 — boulder run).
                //   2: 600/s everywhere (known to add static in note-dense
                //      tonal music — celeste 2; kept for A/B reference)
                float fade_rate = 130.0f;
                if (g_p8_audio_mode == 2) {
                    fade_rate = 600.0f;
                } else if ((g_p8_audio_mode == 1 || g_p8_audio_mode == 3) &&
                           channel_state.fade_synth.instrument == z8::synth::INST_NOISE &&
                           last_synth.instrument == z8::synth::INST_NOISE) {
                    fade_rate = 600.0f;
                }
                channel_state.fade -= fade_rate * inv_frames_per_second;
            }

            if (_memory->hwState.reverb & (1 << (chan + 4))) chan_reverb1_value = 1.0f;
            if (_memory->hwState.reverb & (1 << chan)) chan_reverb2_value = 1.0f;
            if (_memory->hwState.lowpass & (1 << (chan + 4))) chan_damp1_value = 1.0f;
            if (_memory->hwState.lowpass & (1 << chan)) chan_damp2_value = 1.0f;
            
            // One index pass per sample: idx366 == idx732 mod 366 (732 is
            // 2*366, so wrapping the counter at 732 preserves both delay-
            // line phases). The explicit wrap also fixes the old unbounded
            // ++reverb_index, which overflowed negative after ~27h of audio
            // and made `% 366` index out of bounds.
            int const idx732 = channel_state.reverb_index;
            int const idx366 = idx732 >= 366 ? idx732 - 366 : idx732;
            if (chan_reverb1_value > 0.0f)
                value += chan_reverb1_value * channel_state.reverb_2[idx366] * 0.5f;
            if (chan_reverb2_value > 0.0f)
                value += chan_reverb2_value * channel_state.reverb_4[idx732] * 0.5f;

            channel_state.reverb_2[idx366] = value;
            channel_state.reverb_4[idx732] = value;
            if (++channel_state.reverb_index >= 732) channel_state.reverb_index = 0;

            // The damp biquads run unconditionally (upstream behavior): they
            // must stay warm on the live signal, or enabling dampen mid-note
            // starts the filter from stale state — an audible tick. (A
            // skip-when-unused version was tried 2026-06-12 and reverted;
            // the saved ~176K biquads/s was ~1% of the core.)
            float value_damp1 = channel_state.damp1.run(value);
            if (chan_damp1_value > 0.0f) value = lerp(value, value_damp1, chan_damp1_value);

            float value_damp2 = channel_state.damp2.run(value);
            if (chan_damp2_value > 0.0f) value = lerp(value, value_damp2, chan_damp2_value);

            int16_t chan_sample = (int16_t)(32767.99f * std::clamp(value, -0.99f, 0.99f));

            if (_memory->hwState.distort & (1 << chan))
            {
                chan_sample = chan_sample / 0x1000 * 0x1249;
            }
            else if (_memory->hwState.distort & (1 << (chan + 4)))
            {
                chan_sample = (chan_sample - (chan_sample < 0 ? 0x1000 : 0)) / 0x1000 * 0x1249;
            }
            channel_mix += chan_sample;
        }

        buffer[i] = (int16_t)std::clamp(channel_mix, -32767.9f, 32767.9f);
    }
}
