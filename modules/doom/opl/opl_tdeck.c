// T-Deck backend for the Chocolate Doom OPL layer — replaces opl.c,
// opl_sdl.c and opl_timer.c from upstream (vendored from
// chocolate-doom-2.2.1 alongside i_oplmusic.c/dbopl.c/opl_queue.c).
//
// Everything runs on the game task: i_oplmusic.c's timer callbacks are
// dispatched sample-accurately from inside OPL_TDeck_Mix(), which is
// called from the SFX mixer in i_tdeck_sound.c. No other task touches
// this state, so OPL_Lock/OPL_Unlock are no-ops.
//
// Time is a uint32 sample counter at OPL_TDECK_MIX_RATE; it wraps after
// ~54-108 hours of continuous play (rate-dependent), which is accepted.

#include <stdio.h>

#include "opl.h"
#include "opl_queue.h"
#include "dbopl.h"
#include "opl_tdeck.h"

static Chip opl_chip;
static opl_callback_queue_t *callback_queue;
static uint32_t current_sample;
static int opl_paused;
static int opl_initialized;

// The synth scratch buffer is a local in OPL_TDeck_Mix (task stack =
// internal SRAM): keeps synth output off the PSRAM bus and module BSS
// small. Chip__GenerateBlock2 zeroes it before accumulating voices.

// Cap on callbacks dispatched per Mix call: a malformed MIDI that
// schedules zero-delay events forever stalls the music, not the game.
#define MAX_DISPATCH_PER_MIX 4096

opl_init_result_t OPL_Init(unsigned int port_base)
{
    (void)port_base;    // emulated chip — no port I/O, no detection

    callback_queue = OPL_Queue_Create();
    current_sample = 0;
    opl_paused = 0;

    DBOPL_InitTables();
    Chip__Chip(&opl_chip);
    Chip__Setup(&opl_chip, OPL_TDECK_MIX_RATE);

    opl_initialized = 1;

    // Report OPL2: keeps i_oplmusic.c on the 9-voice mono path.
    return OPL_INIT_OPL2;
}

void OPL_Shutdown(void)
{
    if (!opl_initialized)
    {
        return;
    }

    OPL_Queue_Destroy(callback_queue);
    callback_queue = NULL;
    opl_initialized = 0;
}

// The chip must run at the mix rate; i_oplmusic.c calls this with the
// (unrelated) snd_samplerate config value, so it is ignored.
void OPL_SetSampleRate(unsigned int rate)
{
    (void)rate;
}

void OPL_WriteRegister(int reg, int value)
{
    if (opl_initialized)
    {
        Chip__WriteReg(&opl_chip, (Bit32u)reg, (Bit8u)value);
    }
}

// Startup register init — body from chocolate-doom-2.2.1 opl/opl.c
// (only OPL_WriteRegister loops; the hardware delays are not needed).

void OPL_InitRegisters(int opl3)
{
    int r;

    // Initialize level registers

    for (r=OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r)
    {
        OPL_WriteRegister(r, 0x3f);
    }

    // Initialize other registers
    // These two loops write to registers that actually don't exist,
    // but this is what Doom does ...
    // Similarly, the <= is also intenational.

    for (r=OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r)
    {
        OPL_WriteRegister(r, 0x00);
    }

    // More registers ...

    for (r=1; r < OPL_REGS_LEVEL; ++r)
    {
        OPL_WriteRegister(r, 0x00);
    }

    // Re-initialize the low registers:

    // Reset both timers and enable interrupts:
    OPL_WriteRegister(OPL_REG_TIMER_CTRL,      0x60);
    OPL_WriteRegister(OPL_REG_TIMER_CTRL,      0x80);

    // "Allow FM chips to control the waveform of each operator":
    OPL_WriteRegister(OPL_REG_WAVEFORM_ENABLE, 0x20);

    if (opl3)
    {
        OPL_WriteRegister(OPL_REG_NEW, 0x01);

        // Initialize level registers

        for (r=OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r)
        {
            OPL_WriteRegister(r | 0x100, 0x3f);
        }

        // Initialize other registers
        // These two loops write to registers that actually don't exist,
        // but this is what Doom does ...
        // Similarly, the <= is also intenational.

        for (r=OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r)
        {
            OPL_WriteRegister(r | 0x100, 0x00);
        }

        // More registers ...

        for (r=1; r < OPL_REGS_LEVEL; ++r)
        {
            OPL_WriteRegister(r | 0x100, 0x00);
        }
    }

    // Keyboard split point on (?)
    OPL_WriteRegister(OPL_REG_FM_MODE,         0x40);

    if (opl3)
    {
        OPL_WriteRegister(OPL_REG_NEW, 0x01);
    }
}

void OPL_SetCallback(uint64_t us, opl_callback_t callback, void *data)
{
    uint32_t delay_samples;

    if (!opl_initialized)
    {
        return;
    }

    // 64-bit INTEGER math only: u64<->float conversions would pull libgcc
    // helpers that cannot be resolved in this module (see build audit).
    delay_samples = (uint32_t)((us * OPL_TDECK_MIX_RATE) / 1000000);

    OPL_Queue_Push(callback_queue, callback, data,
                   current_sample + delay_samples);
}

void OPL_ClearCallbacks(void)
{
    if (opl_initialized)
    {
        OPL_Queue_Clear(callback_queue);
    }
}

void OPL_AdjustCallbacks(float factor)
{
    if (opl_initialized)
    {
        OPL_Queue_AdjustCallbacks(callback_queue, current_sample, factor);
    }
}

void OPL_SetPaused(int paused)
{
    opl_paused = paused;
}

// Single-threaded driver: callbacks only ever run inside OPL_TDeck_Mix
// on the same task that calls every other OPL_* function.

void OPL_Lock(void)
{
}

void OPL_Unlock(void)
{
}

// Render `nsamples` of music and ADD them into `mix`, dispatching timer
// callbacks at their exact sample positions along the way.

void OPL_TDeck_Mix(int32_t *mix, int nsamples)
{
    int dispatched = 0;
    Bit32s opl_scratch[OPL_TDECK_MAX_SAMPLES];   // internal RAM (task stack)

    if (!opl_initialized || opl_paused)
    {
        return;     // paused: the sample clock freezes, queue stays intact
    }

    while (nsamples > 0)
    {
        int chunk;
        int i;

        // Dispatch everything due at the current position. Callbacks write
        // registers and re-schedule themselves; zero-delay MIDI events are
        // drained here before any samples are rendered.

        while (!OPL_Queue_IsEmpty(callback_queue)
            && OPL_Queue_Peek(callback_queue) <= current_sample
            && dispatched < MAX_DISPATCH_PER_MIX)
        {
            opl_callback_t callback;
            void *data;

            if (!OPL_Queue_Pop(callback_queue, &callback, &data))
            {
                break;
            }

            callback(data);
            dispatched++;
        }

        // Render up to the next callback's due time. If the dispatch cap
        // was hit, render through regardless so this loop always advances.

        chunk = nsamples;

        if (!OPL_Queue_IsEmpty(callback_queue)
         && dispatched < MAX_DISPATCH_PER_MIX)
        {
            uint32_t until = OPL_Queue_Peek(callback_queue) - current_sample;

            if ((uint32_t)chunk > until)
            {
                chunk = (int)until;
            }
        }

        if (chunk > OPL_TDECK_MAX_SAMPLES)
        {
            chunk = OPL_TDECK_MAX_SAMPLES;
        }

        Chip__GenerateBlock2(&opl_chip, (Bitu)chunk, opl_scratch);

        // >>1: 6 dB headroom, matching the SFX mixer's half-scale
        // convention (see i_tdeck_sound.c) — keeps the summed mix from
        // hard-clipping at the int16 clamp.
        for (i = 0; i < chunk; i++)
        {
            mix[i] += opl_scratch[i] >> 1;
        }

        mix += chunk;
        nsamples -= chunk;
        current_sample += (uint32_t)chunk;
    }
}
