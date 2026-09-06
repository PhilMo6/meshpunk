/* This file is part of Snes9x. See LICENSE file.
 * MESHPUNK: SA-1 coprocessor support, ported from libretro/snes9x2005
 * (source/sa1.h, commit deb49d80) — the CATSFC lineage this port's cpuops.c
 * SA1_OPCODES conditionals come from. Local adaptations: port.h supplies
 * INLINE (snes9x2005 used retro_inline.h), and the opcode tables are const
 * here because this port's cpuops.c defines them const. */

#ifndef _sa1_h_
#define _sa1_h_

#include "port.h"
#include "memmap.h"
#include "cpuexec.h"

/* MESHPUNK: the SA-1 bus accessors and main loop run tens of thousands of
 * times per frame from ~90KB of interpreter text that shares the 16KB
 * icache with the S-CPU core — the same shape getset.c's GS_HOT escapes.
 * The loader copies .iram.text into internal SRAM; build.ps1 gives sa1.c
 * and sa1cpu.c -mtext-section-literals -fno-jump-tables to keep the
 * section self-contained. */
#define SA1_HOT __attribute__((section(".iram.text")))

typedef struct
{
   uint8_t   PB;
   uint8_t   DB;
   pair      P;
   pair      A;
   pair      D;
   pair      S;
   pair      X;
   pair      Y;
   uint16_t  PC;
} SSA1Registers;

typedef struct
{
   const SOpcodes* S9xOpcodes;
   uint8_t       _Carry;
   uint8_t       _Zero;
   uint8_t       _Negative;
   uint8_t       _Overflow;
   bool          CPUExecuting;
   uint32_t      ShiftedPB;
   uint32_t      ShiftedDB;
   uint32_t      Flags;
   bool          Executing;
   bool          NMIActive;
   uint8_t       IRQActive;
   bool          WaitingForInterrupt;
   bool          Waiting;
   uint8_t*      PC;
   uint8_t*      PCBase;
   uint8_t*      BWRAM;
   uint8_t*      PCAtOpcodeStart;
   uint8_t*      WaitAddress;
   uint32_t      WaitCounter;
   uint8_t*      WaitByteAddress1;
   uint8_t*      WaitByteAddress2;
   uint8_t*      Map      [MEMMAP_NUM_BLOCKS];
   uint8_t*      WriteMap [MEMMAP_NUM_BLOCKS];
   int16_t       op1;
   int16_t       op2;
   int           arithmetic_op;
   int64_t       sum;
   bool          overflow;
   uint8_t       VirtualBitmapFormat;
   uint8_t       in_char_dma;
   uint8_t       variable_bit_pos;
   SSA1Registers Registers;
} SSA1;

#define SA1CheckZero()      (SA1._Zero == 0)
#define SA1CheckCarry()     (SA1._Carry)
#define SA1CheckIRQ()       (SA1.Registers.PL & IRQ)
#define SA1CheckDecimal()   (SA1.Registers.PL & Decimal)
#define SA1CheckIndex()     (SA1.Registers.PL & IndexFlag)
#define SA1CheckMemory()    (SA1.Registers.PL & MemoryFlag)
#define SA1CheckOverflow()  (SA1._Overflow)
#define SA1CheckNegative()  (SA1._Negative & 0x80)
#define SA1CheckEmulation() (SA1.Registers.P.W & Emulation)

#define SA1ClearFlags(f) (SA1.Registers.P.W &= ~(f))
#define SA1SetFlags(f)   (SA1.Registers.P.W |=  (f))
#define SA1CheckFlag(f)  (SA1.Registers.PL & (f))

uint8_t S9xSA1GetByte(uint32_t);
uint16_t S9xSA1GetWord(uint32_t);
void S9xSA1SetByte(uint8_t, uint32_t);
void S9xSA1SetWord(uint16_t, uint32_t);
void S9xSA1SetPCBase(uint32_t);
uint8_t S9xGetSA1(uint32_t);
void S9xSetSA1(uint8_t, uint32_t);
void S9xSetSA1MemMap(uint32_t which1, uint8_t map);
void S9xSA1SetBWRAMMemMap(uint8_t val);

extern const SOpcodes S9xSA1OpcodesE1   [256];
extern const SOpcodes S9xSA1OpcodesM1X1 [256];
extern const SOpcodes S9xSA1OpcodesM1X0 [256];
extern const SOpcodes S9xSA1OpcodesM0X1 [256];
extern const SOpcodes S9xSA1OpcodesM0X0 [256];
extern SSA1 SA1;

void S9xSA1MainLoop(void);
void S9xSA1Init(void);
void S9xFixSA1AfterSnapshotLoad(void);

/* SA-1-side IRQ sources (SA1.IRQActive bits). Distinct from the S-CPU's
 * SA1_IRQ_SOURCE/SA1_DMA_IRQ_SOURCE in ppu.h. */
#define SNES_IRQ_SOURCE     (1 << 7)
#define TIMER_IRQ_SOURCE    (1 << 6)
#define DMA_IRQ_SOURCE      (1 << 5)

static INLINE void S9xSA1UnpackStatus(void)
{
   SA1._Zero = (SA1.Registers.PL & Zero) == 0;
   SA1._Negative = (SA1.Registers.PL & Negative);
   SA1._Carry = (SA1.Registers.PL & Carry);
   SA1._Overflow = (SA1.Registers.PL & Overflow) >> 6;
}

static INLINE void S9xSA1PackStatus(void)
{
   SA1.Registers.PL &= ~(Zero | Negative | Carry | Overflow);
   SA1.Registers.PL |= SA1._Carry | ((SA1._Zero == 0) << 1) | (SA1._Negative & 0x80) | (SA1._Overflow << 6);
}

static INLINE void S9xSA1FixCycles(void)
{
   if (SA1CheckEmulation())
      SA1.S9xOpcodes = S9xSA1OpcodesE1;
   else if (SA1CheckMemory())
   {
      if (SA1CheckIndex())
         SA1.S9xOpcodes = S9xSA1OpcodesM1X1;
      else
         SA1.S9xOpcodes = S9xSA1OpcodesM1X0;
   }
   else
   {
      if (SA1CheckIndex())
         SA1.S9xOpcodes = S9xSA1OpcodesM0X1;
      else
         SA1.S9xOpcodes = S9xSA1OpcodesM0X0;
   }
}
#endif
