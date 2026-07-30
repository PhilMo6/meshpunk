#ifndef Z80_H
#define Z80_H

#ifdef __cplusplus
extern "C" {
#endif

#include "DrZ80.h"

/****************************************************************************/
/* The Z80 registers. HALT is set to 1 when the CPU is halted, the refresh  */
/* register is calculated as follows: refresh=(Regs.R&127)|(Regs.R2&128)    */
/****************************************************************************/
typedef struct Z80_Regs {
	struct DrZ80 regs;
} Z80_Regs_s;

extern struct Z80_Regs Z80;

#define Z80_IGNORE_INT	-1 /* Ignore interrupt */
#define Z80_NMI_INT 	-2    /* Execute NMI */
#define Z80_IRQ_INT 	-1000 /* Execute IRQ */

extern int Z80_GetPreviousPC (void);
extern void Z80_GetRegs (struct Z80_Regs *Regs);  /* Get registers */
extern void Z80_SetRegs (struct Z80_Regs *Regs);  /* Set registers */

extern void Z80_Reset(void);
extern int Z80_Execute(int cycles); /* Execute cycles T-States - returns number of cycles actually run */
extern int Z80_Interrupt(void);
extern void Z80_Cause_Interrupt(int type);
extern void Z80_Clear_Pending_Interrupts(void);
extern void Interrupt(void); /* required for DrZ80 int hack */
extern void z80_relink_callbacks(void); /* re-bind host pointers after state load */

#ifdef __cplusplus
} /* End of extern "C" */
#endif

#endif

