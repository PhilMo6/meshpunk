/*---------------------------------------------------------------------------
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version. See also the license.txt file for
 *	additional informations.
 *---------------------------------------------------------------------------
 */

/*
 * Support functions for interfacing with DrZ80
 */

#include "types.h"
#include "main.h"
#include "race-memory.h"

#include "DrZ80_support.h"

#define INT_IRQ 0x01
#define NMI_IRQ 0x08

struct Z80_Regs Z80;

#define Z80_INT()          Z80.regs.Z80_IRQ = INT_IRQ
#define Z80_NMI()          Z80.regs.Z80IF |= NMI_IRQ

unsigned int z80_rebaseSP(unsigned short address)
{
   Z80.regs.Z80SP_BASE = (uintptr_t)&mainram[0x3000];
   Z80.regs.Z80SP = Z80.regs.Z80SP_BASE + address;
   return Z80.regs.Z80SP_BASE + address;
}

unsigned int z80_rebasePC(unsigned short address)
{
   Z80.regs.Z80PC_BASE = (uintptr_t)&mainram[0x3000];
   Z80.regs.Z80PC = Z80.regs.Z80PC_BASE + address;
   return Z80.regs.Z80PC_BASE + address;
}

static void z80_irq_callback(void)
{
   Z80.regs.Z80_IRQ = 0x00;
}

/* Re-bind all host-address fields (PC/SP bases and the I/O callback
 * pointers) of the live Z80 context to the current process image.  Used
 * after loading a savestate, whose stored pointers belong to whatever
 * process wrote the state and are meaningless here. */
void z80_relink_callbacks(void)
{
   Z80.regs.z80_rebasePC     = z80_rebasePC;
   Z80.regs.z80_rebaseSP     = z80_rebaseSP;
   Z80.regs.z80_read8        = z80MemReadB;
   Z80.regs.z80_read16       = z80MemReadW;
   Z80.regs.z80_write8       = DrZ80ngpMemWriteB;
   Z80.regs.z80_write16      = DrZ80ngpMemWriteW;
   Z80.regs.z80_in           = DrZ80ngpPortReadB;
   Z80.regs.z80_out          = DrZ80ngpPortWriteB;
   Z80.regs.z80_irq_callback = z80_irq_callback;
}


/****************************************************************************
 * Reset registers to their initial values
 ****************************************************************************/
void Z80_Reset(void)
{
   memset (&Z80, 0, sizeof(struct Z80_Regs));
   Z80.regs.z80_rebasePC=z80_rebasePC;
   Z80.regs.z80_rebaseSP=z80_rebaseSP /* 0 */;
   Z80.regs.z80_read8   =z80MemReadB; /* z80_read8 */
   Z80.regs.z80_read16  =z80MemReadW;
   Z80.regs.z80_write8  =DrZ80ngpMemWriteB;
   Z80.regs.z80_write16 =DrZ80ngpMemWriteW;
   Z80.regs.z80_in      =DrZ80ngpPortReadB;
   Z80.regs.z80_out     =DrZ80ngpPortWriteB;

   Z80.regs.z80_irq_callback=z80_irq_callback;
   Z80.regs.Z80BC = 0013;
   Z80.regs.Z80DE = 0x00D8;
   Z80.regs.Z80HL = 0x014D;

   Z80.regs.Z80PC=Z80.regs.z80_rebasePC(0);
   Z80.regs.Z80SP=Z80.regs.z80_rebaseSP(0xFFFE);

   Z80_Clear_Pending_Interrupts();
}

void Z80_Cause_Interrupt(int type)
{
   if (type == Z80_NMI_INT) 
   {
      Z80_NMI();
   }
   else if (type != Z80_IGNORE_INT)
   {
      Z80_INT();
   }
}

void Z80_Clear_Pending_Interrupts(void)
{
    Z80.regs.Z80_IRQ = 0x00;
}

/****************************************************************************
 * Execute IPeriod T-states. Return number of T-states really executed
 ****************************************************************************/
int Z80_Execute(int cycles)
{
    Z80.regs.cycles = cycles;

    DrZ80Run(&Z80.regs, cycles);
    return cycles; /* (cycles-Z80.regs.cycles); */
}
