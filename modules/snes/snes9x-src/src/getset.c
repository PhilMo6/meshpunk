/* This file is part of Snes9x. See LICENSE file. */

#include "ppu.h"
#include "dsp.h"
#include "cpuexec.h"
#include "obc1.h"
#include "sa1.h"

extern uint8_t OpenBus;

/* MESHPUNK: the four bus accessors run once or more per 65816 instruction,
 * called from ~250KB of opcode handler code that owns the shared 16KB
 * instruction cache — so these are the most-evicted hot functions in the
 * module. Placed in internal SRAM (same escape as fastrender/fxemu; loader
 * copies .iram.text) they take no misses and evict nothing. This is the
 * platform-correct form of the classic "inline getset" speed-up: inlining
 * would grow the handlers by ~30-40KB against that same 16KB cache. The
 * function bodies are untouched — WaitAddress arming (the idle-loop skip
 * depends on it) and cycle accounting keep their exact semantics.
 * build.ps1 gives this file -mtext-section-literals -fno-jump-tables so
 * the section stays self-contained (the slow-path switches would otherwise
 * emit jump tables the l32r audit reads as code). */
#define GS_HOT __attribute__((section(".iram.text")))


GS_HOT uint8_t S9xGetByte(uint32_t Address)
{
   int32_t block = (Address >> MEMMAP_SHIFT) & MEMMAP_MASK;
   uint8_t* GetAddress = Memory.Map [block];

   if ((intptr_t) GetAddress != MAP_CPU || !CPU.InDMA)
      CPU.Cycles += Memory.MapInfo[block].Speed;

   if (GetAddress >= (uint8_t*) MAP_LAST)
   {
      if (Memory.MapInfo[block].Type == MAP_TYPE_RAM)
         CPU.WaitAddress = CPU.PCAtOpcodeStart;
      return GetAddress[Address & 0xffff];
   }

   switch ((intptr_t) GetAddress)
   {
   case MAP_PPU:
      return S9xGetPPU(Address & 0xffff);
   case MAP_CPU:
      return S9xGetCPU(Address & 0xffff);
   case MAP_DSP:
      return S9xGetDSP(Address & 0xffff);
   case MAP_SA1RAM:
   case MAP_LOROM_SRAM:
      /*Address & 0x7FFF - offset into bank
       *Address & 0xFF0000 - bank
       *bank >> 1 | offset = s-ram address, unbound
       *unbound & SRAMMask = Sram offset */
      return Memory.SRAM[(((Address & 0xFF0000) >> 1) | (Address & 0x7FFF)) &Memory.SRAMMask];
   case MAP_RONLY_SRAM:
   case MAP_HIROM_SRAM:
      return Memory.SRAM[((Address & 0x7fff) - 0x6000 + ((Address & 0xf0000) >> 3)) & Memory.SRAMMask];
   case MAP_C4:
      return S9xGetC4(Address & 0xffff);
   case MAP_BWRAM:
      return Memory.BWRAM[(Address & 0x7fff) - 0x6000];
   case MAP_SPC7110_ROM:
   case MAP_SPC7110_DRAM:
   case MAP_OBC_RAM:
   case MAP_SETA_DSP:
   case MAP_SETA_RISC:
   default:
      return OpenBus;
   }
}

GS_HOT uint16_t S9xGetWord(uint32_t Address)
{
   if ((Address & 0x0fff) == 0x0fff)
   {
      OpenBus = S9xGetByte(Address);
      return OpenBus | (S9xGetByte(Address + 1) << 8);
   }

   int32_t block = (Address >> MEMMAP_SHIFT) & MEMMAP_MASK;
   uint8_t* GetAddress = Memory.Map[block];

   if ((intptr_t) GetAddress != MAP_CPU || !CPU.InDMA)
      CPU.Cycles += (Memory.MapInfo[block].Speed << 1);

   if (GetAddress >= (uint8_t*) MAP_LAST)
   {
      if (Memory.MapInfo[block].Type == MAP_TYPE_RAM)
         CPU.WaitAddress = CPU.PCAtOpcodeStart;
#ifdef FAST_LSB_WORD_ACCESS
      return *(uint16_t*) (GetAddress + (Address & 0xffff));
#else
      return *(GetAddress + (Address & 0xffff)) | (*(GetAddress + (Address & 0xffff) + 1) << 8);
#endif
   }

   switch ((intptr_t) GetAddress)
   {
   case MAP_PPU:
      return S9xGetPPU(Address & 0xffff) | (S9xGetPPU((Address + 1) & 0xffff) << 8);
   case MAP_CPU:
      return S9xGetCPU(Address & 0xffff) | (S9xGetCPU((Address + 1) & 0xffff) << 8);
   case MAP_DSP:
      return S9xGetDSP(Address & 0xffff) | (S9xGetDSP((Address + 1) & 0xffff) << 8);
   case MAP_SA1RAM:
   case MAP_LOROM_SRAM:
      /*Address & 0x7FFF - offset into bank
       *Address & 0xFF0000 - bank
       *bank >> 1 | offset = s-ram address, unbound
       *unbound & SRAMMask = Sram offset */
      /* BJ: no FAST_LSB_WORD_ACCESS here, since if Memory.SRAMMask=0x7ff
       * then the high byte doesn't follow the low byte. */
      return *(Memory.SRAM + ((((Address & 0xFF0000) >> 1) | (Address & 0x7FFF)) & Memory.SRAMMask)) | ((*(Memory.SRAM + (((((Address + 1) & 0xFF0000) >> 1) | ((Address + 1) & 0x7FFF)) & Memory.SRAMMask))) << 8);
   case MAP_RONLY_SRAM:
   case MAP_HIROM_SRAM:
      /* BJ: no FAST_LSB_WORD_ACCESS here, since if Memory.SRAMMask=0x7ff
       * then the high byte doesn't follow the low byte. */
      return *(Memory.SRAM + (((Address & 0x7fff) - 0x6000 + ((Address & 0xf0000) >> 3)) & Memory.SRAMMask)) | (*(Memory.SRAM + ((((Address + 1) & 0x7fff) - 0x6000 + (((Address + 1) & 0xf0000) >> 3)) & Memory.SRAMMask)) << 8);
   case MAP_C4:
      return S9xGetC4(Address & 0xffff) | (S9xGetC4((Address + 1) & 0xffff) << 8);
   case MAP_BWRAM:
      /* The window wraps per byte, so no 16-bit load. */
      return *(Memory.BWRAM + ((Address & 0x7fff) - 0x6000)) |
             (*(Memory.BWRAM + (((Address + 1) & 0x7fff) - 0x6000)) << 8);
   case MAP_SPC7110_ROM:
   case MAP_SPC7110_DRAM:
   case MAP_OBC_RAM:
   case MAP_SETA_DSP:
   case MAP_SETA_RISC:
   default:
      return OpenBus | (OpenBus << 8);
   }
}

GS_HOT void S9xSetByte(uint8_t Byte, uint32_t Address)
{
   int32_t block = (Address >> MEMMAP_SHIFT) & MEMMAP_MASK;
   uint8_t* SetAddress = Memory.Map[block];

   if (Memory.MapInfo[block].Type == MAP_TYPE_ROM)
      SetAddress = (uint8_t*) MAP_NONE;

   CPU.WaitAddress = NULL;

   if ((intptr_t) SetAddress != MAP_CPU || !CPU.InDMA)
      CPU.Cycles += Memory.MapInfo[block].Speed;

   if (SetAddress >= (uint8_t*) MAP_LAST)
   {
      SetAddress += Address & 0xffff;
      /* MESHPUNK (SA-1): a store to a watched mailbox byte wakes the parked
       * chip. NULL for non-SA-1 carts, so the compares never match there. */
      if (SetAddress == SA1.WaitByteAddress1 || SetAddress == SA1.WaitByteAddress2)
      {
         SA1.Executing = SA1.S9xOpcodes != NULL;
         SA1.WaitCounter = 0;
      }
      *SetAddress = Byte;
      return;
   }

   switch ((intptr_t) SetAddress)
   {
   case MAP_PPU:
      S9xSetPPU(Byte, Address & 0xffff);
      return;
   case MAP_CPU:
      S9xSetCPU(Byte, Address & 0xffff);
      return;
   case MAP_DSP:
      S9xSetDSP(Byte, Address & 0xffff);
      return;
   case MAP_LOROM_SRAM:
      if (Memory.SRAMMask)
      {
         *(Memory.SRAM + ((((Address & 0xFF0000) >> 1) | (Address & 0x7FFF)) & Memory.SRAMMask)) = Byte;
         CPU.SRAMModified = true;
      }
      return;
   case MAP_HIROM_SRAM:
      if (Memory.SRAMMask)
      {
         *(Memory.SRAM + (((Address & 0x7fff) - 0x6000 + ((Address & 0xf0000) >> 3)) & Memory.SRAMMask)) = Byte;
         CPU.SRAMModified = true;
      }
      return;
   case MAP_BWRAM:
      *(Memory.BWRAM + ((Address & 0x7fff) - 0x6000)) = Byte;
      CPU.SRAMModified = true;
      return;
   case MAP_SA1RAM:
      *(Memory.SRAM + (Address & 0xffff)) = Byte;
      SA1.Executing = !SA1.Waiting;
      break;
   case MAP_C4:
      S9xSetC4(Byte, Address & 0xffff);
      return;
   case MAP_OBC_RAM:
      SetOBC1(Byte, Address & 0xFFFF);
      return;
   case MAP_SETA_DSP:
   case MAP_SETA_RISC:
   default:
      return;
   }
}

GS_HOT void S9xSetWord(uint16_t Word, uint32_t Address)
{
   if ((Address & 0x0FFF) == 0x0FFF)
   {
      S9xSetByte(Word & 0x00FF, Address);
      S9xSetByte(Word >> 8, Address + 1);
      return;
   }

   int32_t block = (Address >> MEMMAP_SHIFT) & MEMMAP_MASK;
   uint8_t* SetAddress = Memory.Map[block];

   CPU.WaitAddress = NULL;

   if (Memory.MapInfo[block].Type == MAP_TYPE_ROM)
      SetAddress = (uint8_t*) MAP_NONE;

   if ((intptr_t) SetAddress != MAP_CPU || !CPU.InDMA)
      CPU.Cycles += Memory.MapInfo[block].Speed << 1;

   if (SetAddress >= (uint8_t*) MAP_LAST)
   {
      SetAddress += Address & 0xffff;
      /* MESHPUNK (SA-1): see S9xSetByte. */
      if (SetAddress == SA1.WaitByteAddress1 || SetAddress == SA1.WaitByteAddress2)
      {
         SA1.Executing = SA1.S9xOpcodes != NULL;
         SA1.WaitCounter = 0;
      }
#ifdef FAST_LSB_WORD_ACCESS
      *(uint16_t*)SetAddress = Word;
#else
      *SetAddress = (uint8_t) Word;
      *(SetAddress + 1) = Word >> 8;
#endif
      return;
   }

   switch ((intptr_t) SetAddress)
   {
   case MAP_PPU:
      S9xSetPPU((uint8_t) Word, Address & 0xffff);
      S9xSetPPU(Word >> 8, (Address & 0xffff) + 1);
      return;
   case MAP_CPU:
      S9xSetCPU((uint8_t) Word, Address & 0xffff);
      S9xSetCPU(Word >> 8, (Address & 0xffff) + 1);
      return;
   case MAP_DSP:
      S9xSetDSP((uint8_t) Word, Address & 0xffff);
      S9xSetDSP(Word >> 8, (Address & 0xffff) + 1);
      return;
   case MAP_LOROM_SRAM:
      if (Memory.SRAMMask)
      {
         /* BJ: no FAST_LSB_WORD_ACCESS here, since if Memory.SRAMMask=0x7ff
          * then the high byte doesn't follow the low byte. */
         *(Memory.SRAM + ((((Address & 0xFF0000) >> 1) | (Address & 0x7FFF)) & Memory.SRAMMask)) = (uint8_t) Word;
         *(Memory.SRAM + (((((Address + 1) & 0xFF0000) >> 1) | ((Address + 1) & 0x7FFF))& Memory.SRAMMask)) = Word >> 8;
         CPU.SRAMModified = true;
      }
      return;
   case MAP_HIROM_SRAM:
      if (Memory.SRAMMask)
      {
         /* BJ: no FAST_LSB_WORD_ACCESS here, since if Memory.SRAMMask=0x7ff
          * then the high byte doesn't follow the low byte. */
         *(Memory.SRAM + (((((Address & 0x7fff) - 0x6000) + ((Address & 0xf0000) >> 3)) & Memory.SRAMMask))) = (uint8_t) Word;
         *(Memory.SRAM + ((((((Address + 1) & 0x7fff) - 0x6000) + (((Address + 1) & 0xf0000) >> 3)) & Memory.SRAMMask))) = (uint8_t)(Word >> 8);
         CPU.SRAMModified = true;
      }
      return;
   case MAP_BWRAM:
      /* The window wraps per byte, so no 16-bit store. */
      *(Memory.BWRAM + ((Address & 0x7fff) - 0x6000)) = (uint8_t) Word;
      *(Memory.BWRAM + (((Address + 1) & 0x7fff) - 0x6000)) = (uint8_t)(Word >> 8);
      CPU.SRAMModified = true;
      return;
   case MAP_SA1RAM:
      *(Memory.SRAM + (Address & 0xffff)) = (uint8_t) Word;
      *(Memory.SRAM + ((Address + 1) & 0xffff)) = (uint8_t)(Word >> 8);
      SA1.Executing = !SA1.Waiting;
      break;
   case MAP_C4:
      S9xSetC4(Word & 0xff, Address & 0xffff);
      S9xSetC4((uint8_t)(Word >> 8), (Address + 1) & 0xffff);
      return;
   case MAP_OBC_RAM:
      SetOBC1(Word & 0xff, Address & 0xFFFF);
      SetOBC1((uint8_t)(Word >> 8), (Address + 1) & 0xffff);
      return;
   case MAP_SETA_DSP:
   case MAP_SETA_RISC:
   default:
      return;
   }
}

uint8_t* GetBasePointer(uint32_t Address)
{
   uint8_t* GetAddress = Memory.Map [(Address >> MEMMAP_SHIFT) & MEMMAP_MASK];
   if (GetAddress >= (uint8_t*) MAP_LAST)
      return GetAddress;
   switch ((intptr_t) GetAddress)
   {
   case MAP_PPU: /*just a guess, but it looks like this should match the CPU as a source. */
   case MAP_CPU: /*fixes Ogre Battle's green lines */
   case MAP_OBC_RAM:
      return Memory.FillRAM;
   case MAP_DSP:
      return Memory.FillRAM - 0x6000;
   case MAP_SA1RAM:
   case MAP_LOROM_SRAM:
   case MAP_SETA_DSP:
      return Memory.SRAM;
   case MAP_BWRAM:
      return Memory.BWRAM - 0x6000;
   case MAP_HIROM_SRAM:
      return Memory.SRAM - 0x6000;
   case MAP_C4:
      return Memory.C4RAM - 0x6000;
   default:
      return NULL;
   }
}

uint8_t* S9xGetMemPointer(uint32_t Address)
{
   uint8_t* GetAddress = Memory.Map [(Address >> MEMMAP_SHIFT) & MEMMAP_MASK];
   if (GetAddress >= (uint8_t*) MAP_LAST)
      return GetAddress + (Address & 0xffff);

   switch ((intptr_t) GetAddress)
   {
   case MAP_PPU:
      return Memory.FillRAM + (Address & 0xffff);
   case MAP_CPU:
      return Memory.FillRAM + (Address & 0xffff);
   case MAP_DSP:
      return Memory.FillRAM - 0x6000 + (Address & 0xffff);
   case MAP_SA1RAM:
   case MAP_LOROM_SRAM:
      return Memory.SRAM + (Address & 0xffff);
   case MAP_BWRAM:
      return Memory.BWRAM - 0x6000 + (Address & 0xffff);
   case MAP_HIROM_SRAM:
      return Memory.SRAM - 0x6000 + (Address & 0xffff);
   case MAP_C4:
      return Memory.C4RAM - 0x6000 + (Address & 0xffff);
   case MAP_OBC_RAM:
      return GetMemPointerOBC1(Address);
   case MAP_SETA_DSP:
      return Memory.SRAM + ((Address & 0xffff) & Memory.SRAMMask);
   default:
      return NULL;
   }
}

void S9xSetPCBase(uint32_t Address)
{
   int32_t block = (Address >> MEMMAP_SHIFT) & MEMMAP_MASK;
   uint8_t* GetAddress = Memory.Map [block];
   CPU.MemSpeed = Memory.MapInfo[block].Speed;
   CPU.MemSpeedx2 = CPU.MemSpeed << 1;

   if (GetAddress >= (uint8_t*) MAP_LAST)
      CPU.PCBase = GetAddress;
   else
   {
      switch ((intptr_t) GetAddress)
      {
      case MAP_PPU:
      case MAP_CPU:
         CPU.PCBase = Memory.FillRAM;
         break;
      case MAP_DSP:
         CPU.PCBase = Memory.FillRAM - 0x6000;
         break;
      case MAP_BWRAM:
         CPU.PCBase = Memory.BWRAM - 0x6000;
         break;
      case MAP_HIROM_SRAM:
         CPU.PCBase = Memory.SRAM - 0x6000;
         break;
      case MAP_C4:
         CPU.PCBase = Memory.C4RAM - 0x6000;
         break;
      default:
         CPU.PCBase = Memory.SRAM;
         break;
      }
   }

   CPU.PC = CPU.PCBase + (Address & 0xffff);
}
