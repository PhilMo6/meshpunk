# Meshpunk Sound System

The sound system provides synthesized tone generation and audio file playback over the T-Deck's I2S speaker. Tones are software-synthesized at 44,100 Hz and pre-rendered into stereo 16-bit PCM buffers stored in PSRAM. Multiple tones can play simultaneously and can be mixed with audio file playback.

All sound functionality is available to Lua apps through the `sound` library.

## Source Files

| File | Purpose |
|------|---------|
| `src/sound.h` | Public types and API declarations |
| `src/sound.cpp` | Sound engine implementation and Lua bindings |
| `data/lua/lib/sound.lua` | Lua wrapper library |
| `data/lua/apps/Settings/Sound/main.lua` | Sound settings UI with demo buttons |

## Quick Start

```lua
local sound = require("lib/sound")

-- Simple beep
local beep = sound.generateTone(880, 300)
beep:play()

-- Chord
local chord = sound.generateChord({262, 330, 392}, 500)
chord:play()

-- Melody
local melody = sound.generateMelody({
    {freq=523, ms=200}, {freq=659, ms=200}, {freq=784, ms=400}
})
melody:play()

-- Clean up when done
beep:delete()
chord:delete()
melody:delete()
```

## Tone Generation

### sound.generateTone(freq, duration_ms [, opts])

Generates a tone and returns a sound object. The tone is pre-rendered into a PCM buffer at creation time, so there is no CPU cost during playback.

**Required arguments:**

| Argument | Type | Range | Description |
|----------|------|-------|-------------|
| `freq` | integer | 20 - 20,000 | Frequency in Hz |
| `duration_ms` | integer | 10 - 10,000 | Duration in milliseconds |

**Optional third argument:** a table with any combination of the following fields. All fields are optional. If the table is omitted or a field is absent, the default value is used.

### Waveforms

Select the oscillator shape with the `waveform` field.

| Value | Shape | Description |
|-------|-------|-------------|
| `"sine"` | Smooth curve | Default. Pure tone, no harmonics. Best for FM synthesis carrier. |
| `"square"` | Flat top/bottom | Hollow, reedy sound. Rich in odd harmonics. |
| `"saw"` | Linear ramp down | Bright, buzzy. Contains all harmonics. Good for bass and leads. |
| `"triangle"` | Linear peak | Softer than square, similar harmonic content. Flute-like. |
| `"noise"` | Random samples | White noise. Frequency parameter is ignored. Good for percussion, wind, static effects. |

```lua
-- Square wave at 220 Hz
local sq = sound.generateTone(220, 500, { waveform = "square" })
sq:play()

-- Noise burst for a snare-like hit
local snare = sound.generateTone(100, 80, {
    waveform = "noise",
    attack = 1,
    decay = 40,
    sustain = 0.0,
    release = 40
})
snare:play()
```

### ADSR Envelope

The ADSR (Attack, Decay, Sustain, Release) envelope shapes the amplitude over the tone's duration. It replaces the default 10ms fade-in/fade-out.

```
Amplitude
  1.0 |    /\
      |   /  \
  S   |  /    \________
      | /              \
  0.0 |/________________\___
      A    D    S       R
```

| Field | Type | Default | Range | Description |
|-------|------|---------|-------|-------------|
| `attack` | integer (ms) | 10 | 0+ | Time to ramp from silence to full amplitude |
| `decay` | integer (ms) | 0 | 0+ | Time to ramp from full amplitude to sustain level |
| `sustain` | float | 1.0 | 0.0 - 1.0 | Amplitude level held during the sustain phase |
| `release` | integer (ms) | 10 | 0+ | Time to ramp from sustain level to silence |

The sustain phase fills whatever time remains after attack + decay + release. If the ADSR times exceed the tone duration, they are proportionally scaled down to fit.

**Default behavior:** With attack=10 and release=10, the default envelope produces a 10ms fade-in, full amplitude body, and 10ms fade-out. This matches the original tone generator behavior exactly and prevents audio clicks.

```lua
-- Plucky sound: fast attack, quick decay to low sustain, short release
local pluck = sound.generateTone(330, 600, {
    attack = 5,
    decay = 80,
    sustain = 0.15,
    release = 100
})
pluck:play()

-- Pad sound: slow attack, full sustain, slow release
local pad = sound.generateTone(440, 2000, {
    waveform = "triangle",
    attack = 400,
    decay = 0,
    sustain = 1.0,
    release = 500
})
pad:play()

-- Staccato hit: instant attack, no sustain
local hit = sound.generateTone(880, 100, {
    attack = 1,
    decay = 90,
    sustain = 0.0,
    release = 10
})
hit:play()
```

### Frequency Sweeps

Sweeps smoothly change the frequency from the starting value to an end value over the tone's duration. Useful for laser sounds, sirens, risers, and UI feedback.

| Field | Type | Default | Range | Description |
|-------|------|---------|-------|-------------|
| `end_freq` | integer (Hz) | none | 20 - 20,000 | Target frequency at the end of the tone |
| `sweep` | string | `"linear"` | `"linear"` or `"exp"` | Interpolation type |

**Linear sweep** changes frequency at a constant rate. Sounds like it accelerates as it goes up.

**Exponential sweep** changes frequency logarithmically. Sounds perceptually even because human pitch perception is logarithmic. This is the more natural-sounding option for most uses.

```lua
-- Laser shot: high to low, fast
local laser = sound.generateTone(2000, 150, {
    end_freq = 200,
    sweep = "exp"
})
laser:play()

-- Rising alert
local alert = sound.generateTone(300, 400, {
    end_freq = 1200,
    sweep = "exp",
    attack = 10,
    decay = 0,
    sustain = 1.0,
    release = 50
})
alert:play()

-- Siren effect (two tones back to back)
local up = sound.generateTone(400, 500, { end_freq = 800, sweep = "linear" })
local down = sound.generateTone(800, 500, { end_freq = 400, sweep = "linear" })
up:play()
-- (play down after up finishes)

-- Wobble bass with square wave
local wobble = sound.generateTone(80, 800, {
    waveform = "saw",
    end_freq = 200,
    sweep = "exp"
})
wobble:play()
```

### FM Synthesis

FM (Frequency Modulation) synthesis uses a modulator oscillator to vary the carrier's instantaneous frequency. This is the technique used by classic Yamaha DX synthesizers. It can produce bells, metallic sounds, electric piano, bass, and many other timbres that simple waveforms cannot achieve.

The synthesis formula is: `output = sin(carrier_phase + fm_index * sin(modulator_phase))`

The modulator frequency is always a multiple of the carrier frequency.

| Field | Type | Default | Range | Description |
|-------|------|---------|-------|-------------|
| `fm_ratio` | float | 0.0 | 0.0 - 32.0 | Modulator frequency as a multiple of the carrier. 0 = no FM. |
| `fm_index` | float | 0.0 | 0.0 - 20.0 | Modulation depth in radians. Higher = more harmonics/brightness. |

When `fm_ratio` is 0 (default), FM is disabled and there is no performance cost.

**Understanding fm_ratio:**
- Integer ratios (1, 2, 3...) produce harmonic timbres (musical, tonal)
- Non-integer ratios (1.4, 2.7, 3.1...) produce inharmonic timbres (bells, metallic, clangorous)
- Low ratios (0.5 - 2) affect the fundamental character
- High ratios (3+) add upper harmonics and brightness

**Understanding fm_index:**
- 0: No modulation (pure carrier waveform)
- 0.5 - 1.5: Subtle warmth and brightness
- 2 - 5: Strong tonal change, clearly synthesized
- 5 - 10: Aggressive, metallic, bell-like
- 10+: Harsh, noisy, distorted

**FM works with all carrier waveforms.** The classic FM sound uses a sine carrier, but FM-modulated square, saw, and triangle waves produce unique timbres. FM has no effect on the noise waveform since noise ignores phase.

**FM composes with sweeps.** The modulator frequency tracks the carrier through sweeps, so the timbre stays consistent as the pitch changes.

```lua
-- Bell
local bell = sound.generateTone(440, 1000, {
    fm_ratio = 1.4,
    fm_index = 5,
    attack = 5,
    decay = 200,
    sustain = 0.2,
    release = 300
})
bell:play()

-- Electric piano
local epiano = sound.generateTone(440, 800, {
    fm_ratio = 1.0,
    fm_index = 1.5,
    attack = 10,
    decay = 150,
    sustain = 0.4,
    release = 200
})
epiano:play()

-- Metallic clang
local clang = sound.generateTone(200, 600, {
    fm_ratio = 7.0,
    fm_index = 3.0,
    attack = 1,
    decay = 300,
    sustain = 0.0,
    release = 300
})
clang:play()

-- FM bass
local bass = sound.generateTone(55, 500, {
    fm_ratio = 1.0,
    fm_index = 2.0,
    attack = 5,
    decay = 100,
    sustain = 0.6,
    release = 50
})
bass:play()

-- Bright lead with FM on a saw carrier
local lead = sound.generateTone(660, 400, {
    waveform = "saw",
    fm_ratio = 2.0,
    fm_index = 1.0,
    attack = 20,
    decay = 50,
    sustain = 0.8,
    release = 80
})
lead:play()

-- Alarm with FM + sweep
local alarm = sound.generateTone(300, 600, {
    fm_ratio = 3.0,
    fm_index = 2.0,
    end_freq = 900,
    sweep = "exp",
    attack = 10,
    release = 50
})
alarm:play()
```

### FM Preset Reference

| Sound | freq | fm_ratio | fm_index | ADSR suggestion |
|-------|------|----------|----------|-----------------|
| Bell | 440 | 1.4 | 5 | A:5 D:200 S:0.2 R:300 |
| Electric piano | 440 | 1.0 | 1.5 | A:10 D:150 S:0.4 R:200 |
| Marimba | 330 | 4.0 | 1.0 | A:2 D:150 S:0.0 R:100 |
| Metallic hit | 200 | 7.0 | 3.0 | A:1 D:300 S:0.0 R:300 |
| Organ | 440 | 2.0 | 0.8 | A:30 D:0 S:1.0 R:30 |
| Bass | 55 | 1.0 | 2.0 | A:5 D:100 S:0.6 R:50 |
| Brass stab | 440 | 1.0 | 3.0 | A:30 D:80 S:0.7 R:40 |
| Glass | 880 | 1.414 | 6.0 | A:2 D:400 S:0.1 R:200 |

### Complete Options Reference

All options in a single table:

```lua
sound.generateTone(freq, duration_ms, {
    -- Waveform
    waveform = "sine",      -- "sine"|"square"|"saw"|"triangle"|"noise"

    -- ADSR envelope (milliseconds, except sustain which is 0.0-1.0)
    attack   = 10,          -- ramp to full amplitude
    decay    = 0,           -- ramp to sustain level
    sustain  = 1.0,         -- held amplitude level
    release  = 10,          -- ramp to silence

    -- Frequency sweep
    end_freq = nil,         -- target Hz (nil = no sweep)
    sweep    = "linear",    -- "linear" or "exp"

    -- FM synthesis
    fm_ratio = 0.0,         -- modulator/carrier frequency ratio (0 = off)
    fm_index = 0.0,         -- modulation depth in radians
})
```

## Chord Generation

### sound.generateChord(freqs, duration_ms [, opts])

Generates a chord by rendering multiple frequencies into a single buffer. All frequencies share the same duration and options (waveform, ADSR, FM, sweep). Returns a sound object.

| Argument | Type | Description |
|----------|------|-------------|
| `freqs` | table of integers | List of frequencies in Hz (1-16 frequencies) |
| `duration_ms` | integer | Duration in milliseconds (10-10,000) |
| `opts` | table (optional) | Same options as `generateTone` (waveform, ADSR, FM) |

Amplitudes are scaled by `1/sqrt(N)` where N is the number of frequencies, preventing clipping while maintaining perceived loudness.

```lua
-- C major chord
local cmaj = sound.generateChord({262, 330, 392}, 500)
cmaj:play()

-- A minor chord with triangle wave
local amin = sound.generateChord({220, 262, 330}, 600, {
    waveform = "triangle",
    attack = 50,
    decay = 100,
    sustain = 0.6,
    release = 150
})
amin:play()

-- Power chord with FM distortion
local power = sound.generateChord({110, 165}, 800, {
    waveform = "saw",
    fm_ratio = 1.0,
    fm_index = 0.5,
    attack = 20,
    release = 100
})
power:play()

-- DTMF tone (dual-tone multi-frequency, like phone keypads)
-- The digit "1" is 697 Hz + 1209 Hz
local dtmf_1 = sound.generateChord({697, 1209}, 150)
dtmf_1:play()

-- Full diminished chord with bell FM
local dim = sound.generateChord({262, 311, 370, 440}, 1000, {
    fm_ratio = 1.4, fm_index = 3.0,
    attack = 5, decay = 300, sustain = 0.1, release = 400
})
dim:play()
```

### Common Chord Frequencies

| Chord | Frequencies |
|-------|-------------|
| C major | 262, 330, 392 |
| C minor | 262, 311, 392 |
| A minor | 220, 262, 330 |
| G major | 196, 247, 294 |
| F major | 175, 220, 262 |
| D minor | 147, 175, 220 |
| C7 | 262, 330, 392, 466 |
| Am7 | 220, 262, 330, 392 |

## Melody Generation

### sound.generateMelody(notes, opts)

Generates a melody by rendering a sequence of notes into a single buffer. Each note is played one after another. Returns a sound object.

| Argument | Type | Description |
|----------|------|-------------|
| `notes` | table of tables | List of `{freq=Hz, ms=duration}` entries (max 256 notes) |
| `opts` | table (optional) | Shared options applied to every note (waveform, ADSR, FM) |

Each note entry has two fields:

| Field | Type | Description |
|-------|------|-------------|
| `freq` | integer | Frequency in Hz. Use 0 for a rest (silence). |
| `ms` | integer | Duration of this note in milliseconds |

The ADSR envelope is applied independently to each note, so there are no clicks between notes. The shared `opts` table sets the waveform, envelope, and FM parameters used for all notes in the melody.

```lua
-- Simple ascending scale
local scale = sound.generateMelody({
    {freq=262, ms=200}, {freq=294, ms=200}, {freq=330, ms=200},
    {freq=349, ms=200}, {freq=392, ms=200}, {freq=440, ms=200},
    {freq=494, ms=200}, {freq=523, ms=400},
})
scale:play()

-- Melody with rests
local tune = sound.generateMelody({
    {freq=523, ms=150}, {freq=0, ms=30},
    {freq=659, ms=150}, {freq=0, ms=30},
    {freq=784, ms=150}, {freq=0, ms=30},
    {freq=1047, ms=300},
}, { attack = 5, decay = 30, sustain = 0.6, release = 30 })
tune:play()

-- Notification jingle with FM bell sound
local jingle = sound.generateMelody({
    {freq=880, ms=120}, {freq=0, ms=20},
    {freq=1047, ms=120}, {freq=0, ms=20},
    {freq=1319, ms=250},
}, {
    fm_ratio = 1.4, fm_index = 3.0,
    attack = 2, decay = 60, sustain = 0.2, release = 60
})
jingle:play()

-- Alert pattern: two short, one long
local alert = sound.generateMelody({
    {freq=1000, ms=80}, {freq=0, ms=40},
    {freq=1000, ms=80}, {freq=0, ms=40},
    {freq=1000, ms=300},
}, { attack = 2, release = 10 })
alert:play()

-- Descending warning with square wave
local warning = sound.generateMelody({
    {freq=880, ms=200}, {freq=0, ms=20},
    {freq=660, ms=200}, {freq=0, ms=20},
    {freq=440, ms=400},
}, {
    waveform = "square",
    attack = 5, decay = 50, sustain = 0.5, release = 30
})
warning:play()

-- Morse code SOS (... --- ...)
local dit = 80
local dah = 240
local gap = 80
local word_gap = 200
local sos = sound.generateMelody({
    {freq=800, ms=dit}, {freq=0, ms=gap},
    {freq=800, ms=dit}, {freq=0, ms=gap},
    {freq=800, ms=dit}, {freq=0, ms=word_gap},
    {freq=800, ms=dah}, {freq=0, ms=gap},
    {freq=800, ms=dah}, {freq=0, ms=gap},
    {freq=800, ms=dah}, {freq=0, ms=word_gap},
    {freq=800, ms=dit}, {freq=0, ms=gap},
    {freq=800, ms=dit}, {freq=0, ms=gap},
    {freq=800, ms=dit},
}, { attack = 2, release = 2 })
sos:play()
```

### Note Frequency Reference

| Note | Oct 3 | Oct 4 | Oct 5 | Oct 6 |
|------|-------|-------|-------|-------|
| C | 131 | 262 | 523 | 1047 |
| C#/Db | 139 | 277 | 554 | 1109 |
| D | 147 | 294 | 587 | 1175 |
| D#/Eb | 156 | 311 | 622 | 1245 |
| E | 165 | 330 | 659 | 1319 |
| F | 175 | 349 | 698 | 1397 |
| F#/Gb | 185 | 370 | 740 | 1480 |
| G | 196 | 392 | 784 | 1568 |
| G#/Ab | 208 | 415 | 831 | 1661 |
| A | 220 | 440 | 880 | 1760 |
| A#/Bb | 233 | 466 | 932 | 1865 |
| B | 247 | 494 | 988 | 1976 |

## Audio File Playback

The sound system can also play MP3, AAC, and FLAC files from LittleFS or SD card using the ESP32-audioI2S library.

```lua
local f = io.open("L:/sounds/notify.mp3", "r")
if f then
    local player = sound.loadFile(f)
    player:play()
    -- later:
    player:delete()
end
```

File playback and tone playback can happen simultaneously. Tones are mixed into the audio stream automatically.

## Sound Object Methods

Both `generateTone` and `loadFile` return a sound object with these methods:

| Method | Description |
|--------|-------------|
| `obj:play()` | Start playback from the beginning |
| `obj:stop()` | Stop playback and reset position to start |
| `obj:pause()` | Toggle pause/resume |
| `obj:setLoop(bool)` | Enable/disable looping. When true, playback restarts from the beginning when it reaches the end. |
| `obj:delete()` | Free the sound object and its resources |

Always call `delete()` when you are done with a sound object to free PSRAM. This is especially important for tones, which store their entire PCM buffer in memory.

A tone object can be played multiple times. Each call to `play()` restarts from the beginning.

```lua
-- Looping alarm
local siren = sound.generateMelody({
    {freq=800, ms=300}, {freq=600, ms=300}
}, { attack = 10, release = 10 })
siren:setLoop(true)
siren:play()

-- Stop it later
siren:stop()
siren:delete()
```

## Volume and Mute

Volume and mute state are persisted across reboots.

| Function | Description |
|----------|-------------|
| `sound.setVolume(vol)` | Set volume (0-21) |
| `sound.getVolume()` | Get current volume (0-21) |
| `sound.mute()` | Mute all audio |
| `sound.unmute()` | Unmute all audio |
| `sound.toggleMute()` | Toggle mute state |
| `sound.isMuted()` | Returns true if muted |
| `sound.isPlaying()` | Returns true if any tone or file is playing |

```lua
sound.setVolume(15)
sound.mute()
print(sound.isMuted())   -- true
sound.unmute()
```

## Mixing

Multiple tones can play simultaneously. They are summed together and clipped to prevent overflow. Volume scaling is applied after mixing.

```lua
-- Play a chord
local c = sound.generateTone(262, 500)   -- C4
local e = sound.generateTone(330, 500)   -- E4
local g = sound.generateTone(392, 500)   -- G4
c:play()
e:play()
g:play()
```

When audio file playback is active (MP3/AAC/FLAC), tones are mixed into the audio engine's output buffer through a callback. When only tones are playing, the sound engine writes directly to I2S in 256-sample chunks.

## Memory Usage

Tones are stored as stereo 16-bit PCM at 44,100 Hz in PSRAM. Memory usage per tone:

| Duration | Buffer size |
|----------|-------------|
| 100 ms | 17.6 KB |
| 500 ms | 88.2 KB |
| 1 second | 176.4 KB |
| 5 seconds | 882 KB |
| 10 seconds | 1.76 MB |

The maximum tone duration is capped at 10 seconds. Always delete tone objects when no longer needed to free PSRAM.

## Recipe Book

### UI Feedback Sounds

```lua
-- Button click
local click = sound.generateTone(1000, 30, {
    attack = 1, decay = 20, sustain = 0.0, release = 10
})

-- Success chime
local ok = sound.generateTone(880, 200, {
    attack = 5, decay = 50, sustain = 0.3, release = 100
})

-- Error buzz
local err = sound.generateTone(150, 200, {
    waveform = "square",
    attack = 5, decay = 50, sustain = 0.5, release = 50
})

-- Notification ping
local ping = sound.generateTone(1200, 300, {
    fm_ratio = 2.0, fm_index = 1.0,
    attack = 2, decay = 100, sustain = 0.0, release = 200
})
```

### Alert and Notification Sounds

```lua
-- Two-tone alert as a melody (no separate timing needed)
local alert = sound.generateMelody({
    {freq=800, ms=200}, {freq=0, ms=50}, {freq=1000, ms=200}
}, { attack = 5, release = 20 })

-- Incoming message: gentle bell
local msg = sound.generateTone(660, 600, {
    fm_ratio = 1.4, fm_index = 3.0,
    attack = 5, decay = 150, sustain = 0.1, release = 200
})

-- Warning: rising sweep
local warn = sound.generateTone(400, 300, {
    end_freq = 1200, sweep = "exp",
    attack = 10, release = 30
})

-- Success jingle
local success = sound.generateMelody({
    {freq=523, ms=100}, {freq=0, ms=20},
    {freq=659, ms=100}, {freq=0, ms=20},
    {freq=784, ms=200},
}, { attack = 3, decay = 30, sustain = 0.5, release = 30 })

-- Error chord (dissonant)
local error_sound = sound.generateChord({233, 262}, 300, {
    waveform = "square",
    attack = 5, decay = 100, sustain = 0.3, release = 100
})
```

### LoRa Mesh Events

```lua
-- Node joined network: ascending melody
local joined = sound.generateMelody({
    {freq=523, ms=100}, {freq=0, ms=20},
    {freq=659, ms=100}, {freq=0, ms=20},
    {freq=784, ms=200},
}, {
    fm_ratio = 2.0, fm_index = 0.8,
    attack = 5, decay = 40, sustain = 0.3, release = 50
})

-- Message received
local rx = sound.generateTone(1047, 150, {
    attack = 2, decay = 80, sustain = 0.0, release = 70
})

-- Connection lost: descending minor chord
local lost = sound.generateMelody({
    {freq=440, ms=200}, {freq=0, ms=30},
    {freq=330, ms=200}, {freq=0, ms=30},
    {freq=220, ms=400},
}, {
    waveform = "saw",
    attack = 10, decay = 50, sustain = 0.4, release = 50
})
```

## Technical Details

### Audio Pipeline

1. `sound.generateTone()`, `generateChord()`, and `generateMelody()` pre-render the full PCM buffer into PSRAM at creation time
2. `obj:play()` sets the playback cursor to 0 and marks the tone as active
3. On each main loop tick, `sound_tone_tick()` reads 256 stereo samples from each active tone, mixes them, applies volume, and writes to I2S
4. If the ESP32-audioI2S engine is running (file playback), tones are instead mixed via the `audio_process_extern()` callback into the engine's output buffer
5. When the playback cursor reaches the end of the buffer, the tone stops automatically

### Sample Rate and Format

- Sample rate: 44,100 Hz
- Bit depth: 16-bit signed
- Channels: Stereo (mono content duplicated to both channels)
- Amplitude: Peak amplitude is 16,000 (out of 32,767 max), leaving headroom for mixing

### Synthesis Details

- **Phase accumulator**: All waveforms use a continuously advancing phase accumulator (0 to 2*pi). This ensures phase continuity during frequency sweeps and FM modulation.
- **ADSR clamping**: If attack + decay + release exceed the tone duration, they are proportionally scaled to fit. The sustain phase gets the remaining time (which may be zero).
- **FM modulator**: Always a sine wave oscillator. The modulator frequency tracks the carrier through sweeps.
- **Noise**: Uses the ESP32 hardware random number generator (`esp_random()`). The ADSR envelope shapes the noise amplitude, but frequency and FM parameters have no effect.

### C++ API

For direct use from C++ firmware code:

```cpp
#include "sound.h"

// Simple tone
int id = sound_create_tone(440, 500);
sound_play(id);

// With parameters
ToneParams p{};
p.freq_hz     = 440;
p.duration_ms = 800;
p.waveform    = WAVE_SQUARE;
p.attack_ms   = 50;
p.decay_ms    = 100;
p.sustain_level = 0.5f;
p.release_ms  = 100;
p.fm_ratio    = 2.0f;
p.fm_index    = 1.5f;
int id = sound_create_tone(p);
sound_play(id);

// Don't forget cleanup
sound_delete(id);
```
