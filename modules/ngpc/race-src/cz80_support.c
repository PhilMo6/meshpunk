#include <stdlib.h>
#include <string.h>

#include "race-memory.h"
#include "cz80.h"

unsigned char *mame4all_cz80_rom = &mainram[0x3000];
unsigned char *mame4all_cz80_ram = &mainram[0x3000];

cz80_struc RACE_cz80_struc_alloc;
cz80_struc *RACE_cz80_struc=&RACE_cz80_struc_alloc;

void Z80_Init(void)
{
	Cz80_Init(RACE_cz80_struc);
}

void Z80_Reset(void)
{
	Cz80_Reset(RACE_cz80_struc);
}

void Z80_Cause_Interrupt(int type)
{
	if (type == Z80_NMI_INT)
		Cz80_Set_NMI(RACE_cz80_struc);
	else if (type >= 0)
		Cz80_Set_IRQ(RACE_cz80_struc, type & 0xff);
}

void Z80_Clear_Pending_Interrupts(void)
{
	Cz80_Clear_IRQ(RACE_cz80_struc);
	Cz80_Clear_NMI(RACE_cz80_struc);
}


int Z80_Execute(int cycles)
{
	return Cz80_Exec(RACE_cz80_struc,cycles);
}

int cpu_readmem16(int address)
{
	return z80ngpMemReadB(address);
}

void cpu_writemem16(int address,int data)
{
	DrZ80ngpMemWriteB(data,address);
}

int cpu_readport(int Port)
{
	return DrZ80ngpPortReadB(Port);
}

void cpu_writeport(int Port,int Value)
{
	DrZ80ngpPortWriteB(Port,Value);
}
