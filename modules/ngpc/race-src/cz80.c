/********************************************************************************/
/*                                                                              */
/* CZ80 (Z80 CPU emulator) version 0.92                                         */
/* Compiled with Dev-C++                                                        */
/* Copyright 2004-2005 Stéphane Dallongeville                                   */
/*                                                                              */
/********************************************************************************/

#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "main.h"
#include "race-memory.h"

#include "cz80.h"


/* include macro file */

#include "cz80.inc"

/* lookup tables */

static uint8_t SZXY[256];            /* zero and sign flags */
static uint8_t SZXYP[256];           /* zero, sign and parity flags */
static uint8_t SZXY_BIT[256];        /* zero, sign and parity/overflow (=zero) flags for BIT opcode */
static uint8_t SZXYHV_inc[256];      /* zero, sign, half carry and overflow flags INC R8 */
static uint8_t SZXYHV_dec[256];      /* zero, sign, half carry and overflow flags DEC R8 */

#define fast_memset memset

/* core main functions */

void Cz80_Init(cz80_struc *cpu)
{
    uint32_t i, j, p;

    fast_memset(cpu, 0, sizeof(cz80_struc));
    
    /* flags tables initialisation */
    for (i = 0; i < 256; i++)
    {
        SZXY[i] = i & (CZ80_SF | CZ80_YF | CZ80_XF);
        if (!i) SZXY[i] |= CZ80_ZF;

        SZXY_BIT[i] = i & (CZ80_SF | CZ80_YF | CZ80_XF);
        if (!i) SZXY_BIT[i] |= CZ80_ZF | CZ80_PF;
        
        for (j = 0, p = 0; j < 8; j++) if (i & (1 << j)) p++;
        SZXYP[i] = SZXY[i];
        if (!(p & 1)) SZXYP[i] |= CZ80_PF;
        
        SZXYHV_inc[i] = SZXY[i];
        if(i == 0x80) SZXYHV_inc[i] |= CZ80_VF;
        if((i & 0x0F) == 0x00) SZXYHV_inc[i] |= CZ80_HF;
        
        SZXYHV_dec[i] = SZXY[i] | CZ80_NF;
        if (i == 0x7F) SZXYHV_dec[i] |= CZ80_VF;
        if ((i & 0x0F) == 0x0F) SZXYHV_dec[i] |= CZ80_HF;
    }
}

uint32_t Cz80_Reset(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    
    fast_memset(CPU, 0, (uintptr_t)(&(CPU->CycleSup)) - (uintptr_t)(&(CPU->BC)));

    Cz80_Set_PC(CPU, 0);
    zIX = 0xFFFF;
    zIY = 0xFFFF;
#if CZ80_DEBUG
    zF = CZ80_ZF;
#else
    zSP = 0xFFFE;
    zFA = 0xFFFF;
#endif

    return CPU->Status;
}

void CZ80_FASTCALL Cz80_Enable(cz80_struc *cpu)
{
    cpu->Status &= ~CZ80_DISABLE;
}

void CZ80_FASTCALL Cz80_Disable(cz80_struc *cpu)
{
    cpu->Status |= CZ80_DISABLE;
}

#include "cz80exec.inc"

void CZ80_FASTCALL Cz80_Set_IRQ(cz80_struc *cpu, int32_t vector)
{
    cpu->IntVect = vector;
    cpu->Status |= CZ80_HAS_INT;
    cpu->CycleSup = cpu->CycleIO;
    cpu->CycleIO = 0;
}

void CZ80_FASTCALL Cz80_Set_NMI(cz80_struc *cpu)
{
    cpu->Status |= CZ80_HAS_NMI;
    cpu->CycleSup = cpu->CycleIO;
    cpu->CycleIO = 0;
}

void CZ80_FASTCALL Cz80_Clear_IRQ(cz80_struc *cpu)
{
    cpu->Status &= ~CZ80_HAS_INT;
}

void CZ80_FASTCALL Cz80_Clear_NMI(cz80_struc *cpu)
{
    cpu->Status &= ~CZ80_HAS_NMI;
}

int32_t CZ80_FASTCALL Cz80_Get_CycleToDo(cz80_struc *cpu)
{
    if (!(cpu->Status & CZ80_RUNNING)) return 0;

    return cpu->CycleToDo;
}

int32_t CZ80_FASTCALL Cz80_Get_CycleRemaining(cz80_struc *cpu)
{
    if (!(cpu->Status & CZ80_RUNNING)) return 0;

    return (cpu->CycleIO + cpu->CycleSup);
}

int32_t CZ80_FASTCALL Cz80_Get_CycleDone(cz80_struc *cpu)
{
    if (!(cpu->Status & CZ80_RUNNING)) return 0;

    return (cpu->CycleToDo - (cpu->CycleIO + cpu->CycleSup));
}

void CZ80_FASTCALL Cz80_End_Execute(cz80_struc *cpu)
{
    cpu->CycleToDo -= cpu->CycleIO + cpu->CycleSup;
    cpu->CycleIO = cpu->CycleSup = 0;
}

void CZ80_FASTCALL Cz80_Waste_Cycle(cz80_struc *cpu, int32_t cycle)
{
    cpu->CycleIO -= cycle;
}

/* externals main functions */

uint32_t CZ80_FASTCALL Cz80_Get_BC(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    return zBC;
}

uint32_t CZ80_FASTCALL Cz80_Get_DE(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    return zDE;
}

uint32_t CZ80_FASTCALL Cz80_Get_HL(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    return zHL;
}

uint32_t CZ80_FASTCALL Cz80_Get_AF(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    return (zF | (zA << 8));
}

uint32_t CZ80_FASTCALL Cz80_Get_BC2(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    return zBC2;
}

uint32_t CZ80_FASTCALL Cz80_Get_DE2(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    return zDE2;
}

uint32_t CZ80_FASTCALL Cz80_Get_HL2(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    return zHL2;
}

uint32_t CZ80_FASTCALL Cz80_Get_AF2(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    return (zF2 | (zA2 << 8));
}

uint32_t CZ80_FASTCALL Cz80_Get_IX(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    return zIX;
}

uint32_t CZ80_FASTCALL Cz80_Get_IY(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    return zIY;
}

uint32_t CZ80_FASTCALL Cz80_Get_SP(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    return zSP;
}

uint32_t CZ80_FASTCALL Cz80_Get_PC(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    uint8_t* PC = cpu->PC;
    return zRealPC;
}

uint32_t CZ80_FASTCALL Cz80_Get_R(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    return zR;
}

uint32_t CZ80_FASTCALL Cz80_Get_IFF(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    uint32_t value = 0;

    if (zIFF1 & CZ80_IFF) value |= 1;
    if (zIFF2 & CZ80_IFF) value |= 2;
    return value;
}

uint32_t CZ80_FASTCALL Cz80_Get_IM(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    return zIM;
}

uint32_t CZ80_FASTCALL Cz80_Get_I(cz80_struc *cpu)
{
    cz80_struc *CPU = cpu;
    return zI;
}


void CZ80_FASTCALL Cz80_Set_BC(cz80_struc *cpu, uint32_t value)
{
    cz80_struc *CPU = cpu;
    zBC = value;
}

void CZ80_FASTCALL Cz80_Set_DE(cz80_struc *cpu, uint32_t value)
{
    cz80_struc *CPU = cpu;
    zDE = value;
}

void CZ80_FASTCALL Cz80_Set_HL(cz80_struc *cpu, uint32_t value)
{
    cz80_struc *CPU = cpu;
    zHL = value;
}

void CZ80_FASTCALL Cz80_Set_AF(cz80_struc *cpu, uint32_t val)
{
    cz80_struc *CPU = cpu;
    zF = val;
    zA = val >> 8;
}

void CZ80_FASTCALL Cz80_Set_BC2(cz80_struc *cpu, uint32_t value)
{
    cz80_struc *CPU = cpu;
    zBC2 = value;
}

void CZ80_FASTCALL Cz80_Set_DE2(cz80_struc *cpu, uint32_t value)
{
    cz80_struc *CPU = cpu;
    zDE2 = value;
}

void CZ80_FASTCALL Cz80_Set_HL2(cz80_struc *cpu, uint32_t value)
{
    cz80_struc *CPU = cpu;
    zHL2 = value;
}

void CZ80_FASTCALL Cz80_Set_AF2(cz80_struc *cpu, uint32_t val)
{
    cz80_struc *CPU = cpu;
    zF2 = val;
    zA2 = val >> 8;
}

void CZ80_FASTCALL Cz80_Set_IX(cz80_struc *cpu, uint32_t value)
{
    cz80_struc *CPU = cpu;
    zIX = value;
}

void CZ80_FASTCALL Cz80_Set_IY(cz80_struc *cpu, uint32_t value)
{
    cz80_struc *CPU = cpu;
    zIY = value;
}

void CZ80_FASTCALL Cz80_Set_SP(cz80_struc *cpu, uint32_t value)
{
    cz80_struc *CPU = cpu;
    zSP = value;
}

void CZ80_FASTCALL Cz80_Set_PC(cz80_struc *cpu, uint32_t val)
{
    cpu->PC = (uint8_t*)&mame4all_cz80_rom[val];
}


void CZ80_FASTCALL Cz80_Set_R(cz80_struc *cpu, uint32_t value)
{
    cz80_struc *CPU = cpu;
    zR = value & 0xFF;
    zR2 = value & 0x80;
}

void CZ80_FASTCALL Cz80_Set_IFF(cz80_struc *cpu, uint32_t value)
{
    cz80_struc *CPU = cpu;
    zIFF = 0;
    if (value & 1) zIFF1 = CZ80_IFF;
    if (value & 2) zIFF2 = CZ80_IFF;
}

void CZ80_FASTCALL Cz80_Set_IM(cz80_struc *cpu, uint32_t value)
{
    cz80_struc *CPU = cpu;
    zIM = value & 3;
}

void CZ80_FASTCALL Cz80_Set_I(cz80_struc *cpu, uint32_t value)
{
    cz80_struc *CPU = cpu;
    zI = value & 0xFF;
}
