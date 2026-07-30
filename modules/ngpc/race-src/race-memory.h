/*---------------------------------------------------------------------------
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version. See also the license.txt file for
 *	additional informations.
 *---------------------------------------------------------------------------
 */


#ifndef _MEMORYH_
#define _MEMORYH_

#include <retro_inline.h>
#include "types.h"
#include "neopopsound.h"
#include "sound.h"
#include "graphics.h"
#include "main.h"
#include "flash.h"

#ifdef DRZ80
#include "DrZ80_support.h"
#else
#ifdef CZ80
#include "cz80_support.h"
#else
#include "z80.h"
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t	ngpInputState;

/* Maximum ROM size is 4 megabytes */
#define MAINROM_SIZE_MAX (4*1024*1024)

extern unsigned char mainram[];			/* All RAM areas */
extern unsigned char *mainrom;			/* ROM image area (runtime-allocated) */
extern unsigned char cpurom[];			/* Bios ROM image area */

/* TLCS 900h memory */
extern unsigned char *cpuram;
void mem_init(void);

/* Z80 memory functions */

extern unsigned char (*z80MemReadB)(unsigned short addr);
extern unsigned short (*z80MemReadW)(unsigned short addr);
extern void (*z80MemWriteB)(unsigned short addr, unsigned char data);
extern void (*z80MemWriteW)(unsigned short addr, unsigned short data);
extern void (*z80PortWriteB)(unsigned char port, unsigned char data);
extern unsigned char (*z80PortReadB)(unsigned char port);

#if defined(DRZ80) || defined(CZ80)
unsigned char z80ngpMemReadB(unsigned short addr);
unsigned short z80ngpMemReadW(unsigned short addr);
void DrZ80ngpMemWriteB(unsigned char data, unsigned short addr);
void DrZ80ngpMemWriteW(unsigned short data, unsigned short addr);
void DrZ80ngpPortWriteB(unsigned short port, unsigned char data);
unsigned char DrZ80ngpPortReadB(unsigned short port);
#endif

static INLINE unsigned char *get_address(unsigned int addr)
{
   addr&= 0x00FFFFFF;
   if (addr<0x00200000)
   {
      if (addr<0x000008a0)
         return &cpuram[addr];
      if (addr>0x00003fff && addr<0x00018000)
      {
         switch (addr)  /* Thanks Koyote */
         {
            case 0x6F80:
               mainram[addr-0x00004000] = 0xFF;
               break;
            case 0x6F80+1:
               mainram[addr-0x00004000] = 0x03;
               break;
            case 0x6F85:
               mainram[addr-0x00004000] = 0x00;
               break;
            case 0x6F82:
               mainram[addr-0x00004000] = ngpInputState;
               break;
            case 0x6DA2:
               mainram[addr-0x00004000] = 0x80;
               break;
         }
         return &mainram[addr-0x00004000];
      }
   }
   else
   {
      if (addr<0x00400000)
         return &mainrom[(addr-0x00200000) /*&cartAddrMask*/];
      if(addr<0x00800000) /* Flavor added */
         return 0;
      if (addr<0x00A00000)
         return &mainrom[(addr-(0x00800000-0x00200000))/*&cartAddrMask*/];
      if(addr<0x00FF0000) /* Flavor added */
         return 0;

      return &cpurom[addr-0x00ff0000];
   }
   return 0;  /* Flavor ERROR */
}

/* read a byte from a memory address (addr) */
static INLINE unsigned char tlcsMemReadB(unsigned int addr)
{
	addr&= 0x00FFFFFF;

	if(currentCommand == COMMAND_INFO_READ)
        return flashReadInfo(addr);

	if (addr < 0x00200000)
   {
      if (addr < 0x000008A0)
      {
         if(addr == 0xBC)
            ngpSoundExecute();
         return cpuram[addr];
      }
      else if (addr > 0x00003FFF && addr < 0x00018000)
      {
         switch (addr)  /* Thanks Koyote */
         {
            case 0x6DA2:
               return 0x80;
            case 0x6F80:
               return 0xFF;
            case 0x6F80+1:
               return 0x03;
            case 0x6F85:
               return 0x00;
            case 0x6F82:
               return ngpInputState;
            default:
               break;
         }
         return mainram[addr-0x00004000];
      }
   }
	else
	{
		if (addr<0x00400000)
            return mainrom[(addr-0x00200000)/*&cartAddrMask*/];
		if (addr<0x00800000)
            return 0xFF;
		if (addr<0x00a00000)
            return mainrom[(addr-(0x00800000-0x00200000))/*&cartAddrMask*/];
		if (addr<0x00ff0000)
            return 0xFF;
		return cpurom[addr-0x00ff0000];
	}
	return 0xFF;
}

/* read a word from a memory address (addr).
   MESHPUNK: read counterpart of the tlcsFastMemWriteW fast path — direct
   access for whole words inside mainram (outside the 0x6D00-0x6FFF
   special-register window handled in tlcsMemReadB) and inside the primary
   ROM window. Flash-info mode and everything else fall back to the
   per-byte dispatcher. */
static INLINE unsigned short tlcsMemReadW(unsigned int addr)
{
   unsigned int a = addr & 0x00FFFFFF;
   if (currentCommand != COMMAND_INFO_READ)
   {
      if (a > 0x00003fff && a < 0x00018000 - 1
          && (a < 0x6D00 - 1 || a > 0x6FFF))
      {
         const unsigned char *p = mainram + (a - 0x00004000);
         if (!((uintptr_t)p & 1))
            return *(const unsigned short *)p;
         return p[0] | (p[1] << 8);
      }
      if (a >= 0x00200000 && a < 0x00400000 - 1)
      {
         const unsigned char *p = mainrom + (a - 0x00200000);
         if (!((uintptr_t)p & 1))
            return *(const unsigned short *)p;
         return p[0] | (p[1] << 8);
      }
   }
   return tlcsMemReadB(addr) | (tlcsMemReadB(addr+1) << 8);
}

/* read a long word from a memory address (addr) */
static INLINE unsigned int tlcsMemReadL(unsigned int addr)
{
   unsigned int i;
   unsigned char *gA = get_address(addr);

   if(gA == 0)
      return 0;

   i = *(gA++);
   i |= (*(gA++)) << 8;
   i |= (*(gA++)) << 16;
   i |= (unsigned int)(*gA) << 24;

   return i;
}

/* write a byte (data) to a memory address (addr) */
static INLINE void tlcsMemWriteB(unsigned int addr, unsigned char data)
{
   addr&= 0x00FFFFFF;
   if (addr<0x000008a0)
   {
      switch(addr)
      {
         case 0xA0:	/* L CH Sound Source Control Register */
            if (cpuram[0xB8] == 0x55 && cpuram[0xB9] == 0xAA)
               Write_SoundChipNoise(data);/*Flavor SN76496Write(0, data); */
            break;
         case 0xA1:	/* R CH Sound Source Control Register */
            if (cpuram[0xB8] == 0x55 && cpuram[0xB9] == 0xAA)
               Write_SoundChipTone(data); /*Flavor SN76496Write(0, data); */
            break;
         case 0xA2:	/* L CH DAC Control Register */
            ngpSoundExecute();
            if (cpuram[0xB8] == 0xAA)
               dac_writeL(data); /*Flavor DAC_data_w(0,data); */
            break;
         case 0xB8:	/* Z80 Reset */
         case 0xB9:	/* Sourd Source Reset Control Register */
            switch(data)
            {
               case 0x55:
                  ngpSoundStart();
                  break;
               case 0xAA:
                  ngpSoundExecute();
                  ngpSoundOff();
                  break;
            }
            break;
         case 0xBA:
            ngpSoundExecute();
#if defined(DRZ80) || defined(CZ80)
            Z80_Cause_Interrupt(Z80_NMI_INT);
#else
            z80Interrupt(Z80NMI);
#endif
            break;
      }
      cpuram[addr] = data;
      return;
   }
   else if (addr>0x00003fff && addr<0x00018000)
   {
      if (addr == 0x87E2 && mainram[0x47F0] != 0xAA)
         return;		/* disallow writes to GEMODE */

      mainram[addr-0x00004000] = data;
      return;
   }
   else if (addr>=0x00200000 && addr<0x00400000)
      flashChipWrite(addr, data);
   else if (addr>=0x00800000 && addr<0x00A00000)
      flashChipWrite(addr, data);
}

#ifdef __cplusplus
}
#endif

#endif  /* _MEMORYH_ */
