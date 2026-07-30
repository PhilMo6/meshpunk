/*---------------------------------------------------------------------------
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version. See also the license.txt file for
 *  additional informations.
 *---------------------------------------------------------------------------

 * state.cpp: state saving
 *
 *  01/20/2009 Cleaned up interface, added loading from memory
 *             Moved signature-related stuff out of race_state (A.K.)
 *  09/11/2008 Initial version (Akop Karapetyan)
 */

#include "cz80.h"
#ifdef DRZ80
#include "DrZ80_support.h"
#endif
#include "neopopsound.h"
#include "neopop_blip.h"

#include <string.h>
#include "state.h"
#include "tlcs900h.h"
#include "race-memory.h"

#ifdef PC
#undef PC
#endif

#define CURRENT_SAVE_STATE_VERSION 0x12

struct race_state_header
{
  uint8_t state_version;       /* State version */
  uint8_t rom_signature[0x40]; /* Rom signature, for verification */
};

struct race_state_0x11
{
	/* Memory */
	uint8_t ram[0xc000];
  uint8_t cpuram[0x08a0];

	/* TLCS-900h Registers */
	uint32_t pc, sr;
	uint8_t f_dash;
	uint32_t gpr[23];

  /* Z80 Registers */
#ifdef CZ80
  cz80_struc RACE_cz80_struc;
  uint32_t PC_offset;
  int32_t Z80_ICount;
#elif DRZ80
  struct Z80_Regs Z80;
#endif
  /* Sound */
  int sndCycles;
  SoundChip toneChip;
  SoundChip noiseChip;

	/* Timers */
  int timer0, timer1, timer2, timer3;

	/* DMA */
  uint8_t ldcRegs[64];

  /* Real-time clock (appended): persisting these makes a loaded state
   * reproduce the same emulated clock on every instance, so RTC reads stay
   * deterministic across netplay peers and rewind/runahead. */
  int64_t rtc_base_time;
  uint32_t     rtc_frame_counter;
};

struct race_state_0x10 /* Older state format */
{
   /* Save state version */
   uint8_t state_version; /* = 0x10 */

   /* Rom signature */
   uint8_t rom_signature[0x40];

   /* Memory */
   uint8_t ram[0xc000];
   uint8_t cpuram[0x08a0]; /* 0xC000]; 0x38000  */

   /* TLCS-900h Registers */
   uint32_t pc, sr;
   uint8_t f_dash;
   uint32_t gpr[23];

   /* Z80 Registers */
#ifdef CZ80
   cz80_struc RACE_cz80_struc;
   uint32_t PC_offset;
   int32_t Z80_ICount;
#elif DRZ80
   struct Z80_Regs Z80;
#endif

   /* Sound Chips */
   int sndCycles;
   SoundChip toneChip;
   SoundChip noiseChip;

   /* Timers */
   int timer0, timer1, timer2, timer3;

   /* DMA */
   uint8_t ldcRegs[64];
};

typedef struct race_state_0x11 race_state_t;

static int state_store(race_state_t *rs)
{
  int i = 0;
#ifdef CZ80
  extern cz80_struc *RACE_cz80_struc;
  extern int32_t Z80_ICount;
  int size_of_z80;
#elif DRZ80
  extern struct Z80_Regs Z80;
#endif
  extern int sndCycles;

  /* Zero the whole structure first so that compiler padding between fields
   * is written deterministically. Without this, padding holds uninitialised
   * memory and two saves of identical emulator state can differ byte-for-byte,
   * which breaks netplay state hashing, runahead and rewind dedup. */
  memset(rs, 0, sizeof(*rs));

  /* TLCS-900h Registers */
  rs->pc = gen_regsPC;
  rs->sr = gen_regsSR;
  rs->f_dash = F2;

  rs->gpr[i++] = gen_regsXWA0;
  rs->gpr[i++] = gen_regsXBC0;
  rs->gpr[i++] = gen_regsXDE0;
  rs->gpr[i++] = gen_regsXHL0;

  rs->gpr[i++] = gen_regsXWA1;
  rs->gpr[i++] = gen_regsXBC1;
  rs->gpr[i++] = gen_regsXDE1;
  rs->gpr[i++] = gen_regsXHL1;

  rs->gpr[i++] = gen_regsXWA2;
  rs->gpr[i++] = gen_regsXBC2;
  rs->gpr[i++] = gen_regsXDE2;
  rs->gpr[i++] = gen_regsXHL2;

  rs->gpr[i++] = gen_regsXWA3;
  rs->gpr[i++] = gen_regsXBC3;
  rs->gpr[i++] = gen_regsXDE3;
  rs->gpr[i++] = gen_regsXHL3;

  rs->gpr[i++] = gen_regsXIX;
  rs->gpr[i++] = gen_regsXIY;
  rs->gpr[i++] = gen_regsXIZ;
  rs->gpr[i++] = gen_regsXSP;

  rs->gpr[i++] = gen_regsSP;
  rs->gpr[i++] = gen_regsXSSP;
  rs->gpr[i++] = gen_regsXNSP;

  /* Z80 Registers */
#ifdef CZ80
  size_of_z80 = 
     (uintptr_t)(&(RACE_cz80_struc->CycleSup)) - (uintptr_t)(&(RACE_cz80_struc->BC));
  memcpy(&rs->RACE_cz80_struc, RACE_cz80_struc, size_of_z80);
  rs->Z80_ICount = Z80_ICount;
  rs->PC_offset = Cz80_Get_PC(RACE_cz80_struc);
#elif DRZ80
  memcpy(&rs->Z80, &Z80, sizeof(Z80));
  /* Z80PC/Z80SP and the *_BASE fields hold absolute host addresses into
   * mainram, and the callback slots hold absolute function addresses; none
   * of these are valid in another process.  Persist PC/SP as guest-relative
   * offsets and zero everything that will be re-derived on restore. */
  rs->Z80.regs.Z80PC      = Z80.regs.Z80PC - Z80.regs.Z80PC_BASE;
  rs->Z80.regs.Z80SP      = Z80.regs.Z80SP - Z80.regs.Z80SP_BASE;
  rs->Z80.regs.Z80PC_BASE = 0;
  rs->Z80.regs.Z80SP_BASE = 0;
  rs->Z80.regs.z80_rebasePC     = NULL;
  rs->Z80.regs.z80_rebaseSP     = NULL;
  rs->Z80.regs.z80_read8        = NULL;
  rs->Z80.regs.z80_read16       = NULL;
  rs->Z80.regs.z80_write8       = NULL;
  rs->Z80.regs.z80_write16      = NULL;
  rs->Z80.regs.z80_in           = NULL;
  rs->Z80.regs.z80_out          = NULL;
  rs->Z80.regs.z80_irq_callback = NULL;
#endif

  /* Sound */
  rs->sndCycles = sndCycles;
  memcpy(&rs->toneChip, &toneChip, sizeof(SoundChip));
  memcpy(&rs->noiseChip, &noiseChip, sizeof(SoundChip));

  /* Timers */
  rs->timer0 = timer0;
  rs->timer1 = timer1;
  rs->timer2 = timer2;
  rs->timer3 = timer3;

  /* DMA */
  memcpy(&rs->ldcRegs, &ldcRegs, sizeof(ldcRegs));

  /* Real-time clock */
  rs->rtc_base_time     = (int64_t)rtc_base_time;
  rs->rtc_frame_counter = rtc_frame_counter;

  /* Memory */
  memcpy(rs->ram, mainram, sizeof(rs->ram));
  memcpy(rs->cpuram, &mainram[0x20000], sizeof(rs->cpuram));

  return 1;
}

static int state_restore(race_state_t *rs)
{
  int i = 0;
#ifdef CZ80
  extern cz80_struc *RACE_cz80_struc;
  extern int32_t Z80_ICount;
  int size_of_z80;
#elif DRZ80
  extern struct Z80_Regs Z80;
#endif
  extern int sndCycles;

  /* TLCS-900h Registers */
  gen_regsPC = rs->pc;
  gen_regsSR = rs->sr;
  F2 = rs->f_dash;

  gen_regsXWA0 = rs->gpr[i++];
  gen_regsXBC0 = rs->gpr[i++];
  gen_regsXDE0 = rs->gpr[i++];
  gen_regsXHL0 = rs->gpr[i++];

  gen_regsXWA1 = rs->gpr[i++];
  gen_regsXBC1 = rs->gpr[i++];
  gen_regsXDE1 = rs->gpr[i++];
  gen_regsXHL1 = rs->gpr[i++];

  gen_regsXWA2 = rs->gpr[i++];
  gen_regsXBC2 = rs->gpr[i++];
  gen_regsXDE2 = rs->gpr[i++];
  gen_regsXHL2 = rs->gpr[i++];

  gen_regsXWA3 = rs->gpr[i++];
  gen_regsXBC3 = rs->gpr[i++];
  gen_regsXDE3 = rs->gpr[i++];
  gen_regsXHL3 = rs->gpr[i++];

  gen_regsXIX = rs->gpr[i++];
  gen_regsXIY = rs->gpr[i++];
  gen_regsXIZ = rs->gpr[i++];
  gen_regsXSP = rs->gpr[i++];

  gen_regsSP = rs->gpr[i++];
  gen_regsXSSP = rs->gpr[i++];
  gen_regsXNSP = rs->gpr[i++];

  /* Z80 Registers */
#ifdef CZ80
  size_of_z80 = 
     (uintptr_t)(&(RACE_cz80_struc->CycleSup)) - (uintptr_t)(&(RACE_cz80_struc->BC));

  memcpy(RACE_cz80_struc, &rs->RACE_cz80_struc, size_of_z80);
  Z80_ICount = rs->Z80_ICount;
  Cz80_Set_PC(RACE_cz80_struc, rs->PC_offset);
#elif DRZ80
  {
    /* Saved PC/SP are guest-relative offsets (see state_store).  Pull them
     * out before the memcpy clobbers the live context, copy the register
     * file, then re-bind all host pointers and rebase PC/SP against the
     * mainram of *this* process. */
    unsigned int pc_off = rs->Z80.regs.Z80PC;
    unsigned int sp_off = rs->Z80.regs.Z80SP;
    memcpy(&Z80, &rs->Z80, sizeof(Z80));
    z80_relink_callbacks();
    Z80.regs.z80_rebasePC(pc_off);
    Z80.regs.z80_rebaseSP(sp_off);
  }
#endif

  /* Sound */
  sndCycles = rs->sndCycles;
  memcpy(&toneChip, &rs->toneChip, sizeof(SoundChip));
  memcpy(&noiseChip, &rs->noiseChip, sizeof(SoundChip));

  /* The band-limited (accurate) audio path is a pure observer of the chip
   * registers above and re-derives its parameters each step, so it needs no
   * dedicated savestate fields; clearing it here drops the in-flight Blip
   * buffer and oscillator phase so playback resumes cleanly from the restored
   * chip state (at most an imperceptible discontinuity at the load point). */
  if (neopop_audio_accurate)
     neopop_blip_reset();

  /* Timers */
  timer0 = rs->timer0;
  timer1 = rs->timer1;
  timer2 = rs->timer2;
  timer3 = rs->timer3;

  /* DMA */
  memcpy(&ldcRegs, &rs->ldcRegs, sizeof(ldcRegs));

  /* Real-time clock */
  rtc_base_time     = (time_t)rs->rtc_base_time;
  rtc_frame_counter = rs->rtc_frame_counter;

  /* Memory */
  memcpy(mainram, rs->ram, sizeof(rs->ram));
  memcpy(&mainram[0x20000], rs->cpuram, sizeof(rs->cpuram));

  /* Reinitialize TLCS (mainly redirect pointers) */
  tlcs_reinit();

  return 1;
}

int state_store_mem(void *state)
{
  return state_store((race_state_t*)state);
}

int state_restore_mem(void *state)
{
  return state_restore((race_state_t*)state);
}

int state_get_size(void)
{
  return sizeof(race_state_t);
}
