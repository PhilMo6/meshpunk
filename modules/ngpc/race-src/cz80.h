/********************************************************************************/
/*                                                                              */
/* CZ80 include file                                                            */
/* C Z80 emulator version 0.92                                                  */
/* Copyright 2004-2005 Stéphane Dallongeville                                   */
/*                                                                              */
/********************************************************************************/

#ifndef _CZ80_H_
#define _CZ80_H_

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdint.h>

#define CZ80_FASTCALL

/*************************************/
/* Z80 core Structures & definitions */
/*************************************/

#define CZ80_FETCH_BITS         4   /* [4-12]   default = 8 */

#define CZ80_FETCH_SFT          (16 - CZ80_FETCH_BITS)
#define CZ80_FETCH_BANK         (1 << CZ80_FETCH_BITS)

#define CZ80_USE_JUMPTABLE      1
#define CZ80_IRQ_CYCLES			1
#define CZ80_SIZE_OPT           1
#define CZ80_USE_WORD_HANDLER   0
#define CZ80_EXACT              0
#define CZ80_DEBUG              0

#define CZ80_SF_SFT     7
#define CZ80_ZF_SFT     6
#define CZ80_YF_SFT     5
#define CZ80_HF_SFT     4
#define CZ80_XF_SFT     3
#define CZ80_PF_SFT     2
#define CZ80_VF_SFT     2
#define CZ80_NF_SFT     1
#define CZ80_CF_SFT     0

#define CZ80_SF         (1 << CZ80_SF_SFT)
#define CZ80_ZF         (1 << CZ80_ZF_SFT)
#define CZ80_YF         (1 << CZ80_YF_SFT)
#define CZ80_HF         (1 << CZ80_HF_SFT)
#define CZ80_XF         (1 << CZ80_XF_SFT)
#define CZ80_PF         (1 << CZ80_PF_SFT)
#define CZ80_VF         (1 << CZ80_VF_SFT)
#define CZ80_NF         (1 << CZ80_NF_SFT)
#define CZ80_CF         (1 << CZ80_CF_SFT)

#define CZ80_IFF_SFT    CZ80_PF_SFT
#define CZ80_IFF        CZ80_PF

#define CZ80_HAS_INT    CZ80_IFF
#define CZ80_HAS_NMI    0x08

#define CZ80_RUNNING    0x10
#define CZ80_HALTED     0x20
#define CZ80_FAULTED    0x80
#define CZ80_DISABLE    0x40

typedef uint32_t  CZ80_FASTCALL CZ80_READ(uint32_t adr);
typedef void CZ80_FASTCALL CZ80_WRITE(uint32_t adr, uint32_t data);

typedef void CZ80_FASTCALL CZ80_RETI_CALLBACK();
typedef int32_t  CZ80_FASTCALL CZ80_INT_CALLBACK(int32_t param);

typedef union
{
    uint8_t B;
    int8_t SB;
} union8;

typedef union
{
	struct
	{
#ifdef MSB_FIRST
		uint8_t H;
		uint8_t L;
#else
		uint8_t L;
		uint8_t H;
#endif
	} B;
	struct
	{
#ifdef MSB_FIRST
		int8_t H;
		int8_t L;
#else
		int8_t L;
		int8_t H;
#endif
	} SB;
	uint16_t W;
	int16_t SW;
} union16;

typedef struct
{
    union
    {
        uint8_t      r8[8];
        union16 r16[4];
        struct
        {
            union16 BC;         /* 32 bytes aligned */
            union16 DE;
            union16 HL;
            union16 FA;
        };
    };
    
    union16 IX;
    union16 IY;
    union16 SP;
    uint8_t      *PC;
    
    union16 BC2;
    union16 DE2;
    union16 HL2;
    union16 FA2;

    union16 R;
    union16 IFF;

    uint8_t I;
    uint8_t IM;
    uint8_t IntVect;
    uint8_t Status;

    uint32_t BasePC;
    uint32_t CycleIO;

    uint32_t CycleToDo;     /* 32 bytes aligned */
    uint32_t CycleSup;
} cz80_struc;

/*************************/
/* Publics Z80 functions */
/*************************/

void    Cz80_Init(cz80_struc *cpu);
uint32_t     Cz80_Reset(cz80_struc *cpu);

uint32_t     Cz80_Read_Byte(cz80_struc *cpu, uint32_t adr);
uint32_t     Cz80_Read_Word(cz80_struc *cpu, uint32_t adr);
void    Cz80_Write_Byte(cz80_struc *cpu, uint32_t adr, uint32_t data);
void    Cz80_Write_Word(cz80_struc *cpu, uint32_t adr, uint32_t data);

void    CZ80_FASTCALL Cz80_Enable(cz80_struc *cpu);
void    CZ80_FASTCALL Cz80_Disable(cz80_struc *cpu);

int32_t     CZ80_FASTCALL Cz80_Exec(cz80_struc *cpu, int32_t cycles);

void    CZ80_FASTCALL Cz80_Set_IRQ(cz80_struc *cpu, int32_t vector);
void    CZ80_FASTCALL Cz80_Set_NMI(cz80_struc *cpu);
void    CZ80_FASTCALL Cz80_Clear_IRQ(cz80_struc *cpu);
void    CZ80_FASTCALL Cz80_Clear_NMI(cz80_struc *cpu);

int32_t     CZ80_FASTCALL Cz80_Get_CycleToDo(cz80_struc *cpu);
int32_t     CZ80_FASTCALL Cz80_Get_CycleRemaining(cz80_struc *cpu);
int32_t     CZ80_FASTCALL Cz80_Get_CycleDone(cz80_struc *cpu);
void    CZ80_FASTCALL Cz80_End_Execute(cz80_struc *cpu);
void    CZ80_FASTCALL Cz80_Waste_Cycle(cz80_struc *cpu, int32_t cycle);

uint32_t     CZ80_FASTCALL Cz80_Get_BC(cz80_struc *cpu);
uint32_t     CZ80_FASTCALL Cz80_Get_DE(cz80_struc *cpu);
uint32_t     CZ80_FASTCALL Cz80_Get_HL(cz80_struc *cpu);
uint32_t     CZ80_FASTCALL Cz80_Get_AF(cz80_struc *cpu);

uint32_t     CZ80_FASTCALL Cz80_Get_BC2(cz80_struc *cpu);
uint32_t     CZ80_FASTCALL Cz80_Get_DE2(cz80_struc *cpu);
uint32_t     CZ80_FASTCALL Cz80_Get_HL2(cz80_struc *cpu);
uint32_t     CZ80_FASTCALL Cz80_Get_AF2(cz80_struc *cpu);

uint32_t     CZ80_FASTCALL Cz80_Get_IX(cz80_struc *cpu);
uint32_t     CZ80_FASTCALL Cz80_Get_IY(cz80_struc *cpu);
uint32_t     CZ80_FASTCALL Cz80_Get_SP(cz80_struc *cpu);
uint32_t     CZ80_FASTCALL Cz80_Get_PC(cz80_struc *cpu);

uint32_t     CZ80_FASTCALL Cz80_Get_R(cz80_struc *cpu);
uint32_t     CZ80_FASTCALL Cz80_Get_IFF(cz80_struc *cpu);
uint32_t     CZ80_FASTCALL Cz80_Get_IM(cz80_struc *cpu);
uint32_t     CZ80_FASTCALL Cz80_Get_I(cz80_struc *cpu);

void    CZ80_FASTCALL Cz80_Set_BC(cz80_struc *cpu, uint32_t value);
void    CZ80_FASTCALL Cz80_Set_DE(cz80_struc *cpu, uint32_t value);
void    CZ80_FASTCALL Cz80_Set_HL(cz80_struc *cpu, uint32_t value);
void    CZ80_FASTCALL Cz80_Set_AF(cz80_struc *cpu, uint32_t value);

void    CZ80_FASTCALL Cz80_Set_BC2(cz80_struc *cpu, uint32_t value);
void    CZ80_FASTCALL Cz80_Set_DE2(cz80_struc *cpu, uint32_t value);
void    CZ80_FASTCALL Cz80_Set_HL2(cz80_struc *cpu, uint32_t value);
void    CZ80_FASTCALL Cz80_Set_AF2(cz80_struc *cpu, uint32_t value);

void    CZ80_FASTCALL Cz80_Set_IX(cz80_struc *cpu, uint32_t value);
void    CZ80_FASTCALL Cz80_Set_IY(cz80_struc *cpu, uint32_t value);
void    CZ80_FASTCALL Cz80_Set_SP(cz80_struc *cpu, uint32_t value);
void    CZ80_FASTCALL Cz80_Set_PC(cz80_struc *cpu, uint32_t value);

void    CZ80_FASTCALL Cz80_Set_R(cz80_struc *cpu, uint32_t value);
void    CZ80_FASTCALL Cz80_Set_IFF(cz80_struc *cpu, uint32_t value);
void    CZ80_FASTCALL Cz80_Set_IM(cz80_struc *cpu, uint32_t value);
void    CZ80_FASTCALL Cz80_Set_I(cz80_struc *cpu, uint32_t value);

#if defined(__cplusplus)
}
#endif

#endif  /* _CZ80_H_ */

