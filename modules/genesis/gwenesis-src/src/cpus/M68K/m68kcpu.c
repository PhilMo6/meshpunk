/* ======================================================================== */
/*                            MAIN 68K CORE                                 */
/* ======================================================================== */

extern int vdp_68k_irq_ack(int int_level);

#define m68ki_cpu m68k
#define MUL (7)

/* ======================================================================== */
/* ================================ INCLUDES ============================== */
/* ======================================================================== */

#ifndef BUILD_TABLES
  #ifndef TABLES_FULL
    #include "m68ki_cycles.h"
  #else
    #include "m68ki_cycles_full.h"
  #endif
#endif

/* MESHPUNK: m68ki_cycles has to be declared before the headers that use it.
 * Upstream declares it further down, under its own #ifdef BUILD_TABLES, which
 * is after m68kcpu.h and m68kops.h have already referenced it -- so the
 * BUILD_TABLES path does not compile as shipped. */
#ifdef BUILD_TABLES
static unsigned char m68ki_cycles[0x10000];
#endif

#include "m68kconf.h"
#include "m68kcpu.h"
#include "m68kops.h"
#include "gwenesis_savestate.h"

/* ======================================================================== */
/* ================================= DATA ================================= */
/* ======================================================================== */

/* MESHPUNK: m68ki_cycles moved above the includes (see note there). */

static int irq_latency;

m68ki_cpu_core m68k;


/* ======================================================================== */
/* =============================== CALLBACKS ============================== */
/* ======================================================================== */

/* Default callbacks used if the callback hasn't been set yet, or if the
 * callback is set to NULL
 */

#if M68K_EMULATE_INT_ACK == OPT_ON
/* Interrupt acknowledge */
static int default_int_ack_callback(int int_level)
{
  CPU_INT_LEVEL = 0;
  return M68K_INT_ACK_AUTOVECTOR;
}
#endif

#if M68K_EMULATE_RESET == OPT_ON
/* Called when a reset instruction is executed */
static void default_reset_instr_callback(void)
{
}
#endif

#if M68K_TAS_HAS_CALLBACK == OPT_ON
/* Called when a tas instruction is executed */
static int default_tas_instr_callback(void)
{
  return 1; // allow writeback
}
#endif

#if M68K_EMULATE_FC == OPT_ON
/* Called every time there's bus activity (read/write to/from memory */
static void default_set_fc_callback(unsigned int new_fc)
{
}
#endif


/* ======================================================================== */
/* ================================= API ================================== */
/* ======================================================================== */

/* Access the internals of the CPU */
unsigned int m68k_get_reg(m68k_register_t regnum)
{
  switch(regnum)
  {
    case M68K_REG_D0:  return m68ki_cpu.dar[0];
    case M68K_REG_D1:  return m68ki_cpu.dar[1];
    case M68K_REG_D2:  return m68ki_cpu.dar[2];
    case M68K_REG_D3:  return m68ki_cpu.dar[3];
    case M68K_REG_D4:  return m68ki_cpu.dar[4];
    case M68K_REG_D5:  return m68ki_cpu.dar[5];
    case M68K_REG_D6:  return m68ki_cpu.dar[6];
    case M68K_REG_D7:  return m68ki_cpu.dar[7];
    case M68K_REG_A0:  return m68ki_cpu.dar[8];
    case M68K_REG_A1:  return m68ki_cpu.dar[9];
    case M68K_REG_A2:  return m68ki_cpu.dar[10];
    case M68K_REG_A3:  return m68ki_cpu.dar[11];
    case M68K_REG_A4:  return m68ki_cpu.dar[12];
    case M68K_REG_A5:  return m68ki_cpu.dar[13];
    case M68K_REG_A6:  return m68ki_cpu.dar[14];
    case M68K_REG_A7:  return m68ki_cpu.dar[15];
    case M68K_REG_PC:  return MASK_OUT_ABOVE_32(m68ki_cpu.pc);
    case M68K_REG_SR:  return  m68ki_cpu.t1_flag        |
                  (m68ki_cpu.s_flag << 11)              |
                   m68ki_cpu.int_mask                   |
                  ((m68ki_cpu.x_flag & XFLAG_SET) >> 4) |
                  ((m68ki_cpu.n_flag & NFLAG_SET) >> 4) |
                  ((!m68ki_cpu.not_z_flag) << 2)        |
                  ((m68ki_cpu.v_flag & VFLAG_SET) >> 6) |
                  ((m68ki_cpu.c_flag & CFLAG_SET) >> 8);
    case M68K_REG_SP:  return m68ki_cpu.dar[15];
    case M68K_REG_USP:  return m68ki_cpu.s_flag ? m68ki_cpu.sp[0] : m68ki_cpu.dar[15];
    case M68K_REG_ISP:  return m68ki_cpu.s_flag ? m68ki_cpu.dar[15] : m68ki_cpu.sp[4];
#if M68K_EMULATE_PREFETCH
    case M68K_REG_PREF_ADDR:  return m68ki_cpu.pref_addr;
    case M68K_REG_PREF_DATA:  return m68ki_cpu.pref_data;
#endif
    case M68K_REG_IR:  return m68ki_cpu.ir;
    default:      return 0;
  }
}

void m68k_set_reg(m68k_register_t regnum, unsigned int value)
{
  switch(regnum)
  {
    case M68K_REG_D0:  REG_D[0] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_D1:  REG_D[1] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_D2:  REG_D[2] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_D3:  REG_D[3] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_D4:  REG_D[4] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_D5:  REG_D[5] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_D6:  REG_D[6] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_D7:  REG_D[7] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A0:  REG_A[0] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A1:  REG_A[1] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A2:  REG_A[2] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A3:  REG_A[3] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A4:  REG_A[4] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A5:  REG_A[5] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A6:  REG_A[6] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_A7:  REG_A[7] = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_PC:  m68ki_jump(MASK_OUT_ABOVE_32(value)); return;
    case M68K_REG_SR:  m68ki_set_sr(value); return;
    case M68K_REG_SP:  REG_SP = MASK_OUT_ABOVE_32(value); return;
    case M68K_REG_USP:  if(FLAG_S)
                REG_USP = MASK_OUT_ABOVE_32(value);
              else
                REG_SP = MASK_OUT_ABOVE_32(value);
              return;
    case M68K_REG_ISP:  if(FLAG_S)
                REG_SP = MASK_OUT_ABOVE_32(value);
              else
                REG_ISP = MASK_OUT_ABOVE_32(value);
              return;
    case M68K_REG_IR:  REG_IR = MASK_OUT_ABOVE_16(value); return;
#if M68K_EMULATE_PREFETCH
    case M68K_REG_PREF_ADDR:  CPU_PREF_ADDR = MASK_OUT_ABOVE_32(value); return;
#endif
    default:      return;
  }
}

/* Set the callbacks */
#if M68K_EMULATE_INT_ACK == OPT_ON
void m68k_set_int_ack_callback(int  (*callback)(int int_level))
{
  CALLBACK_INT_ACK = callback ? callback : default_int_ack_callback;
}
#endif

#if M68K_EMULATE_RESET == OPT_ON
void m68k_set_reset_instr_callback(void  (*callback)(void))
{
  CALLBACK_RESET_INSTR = callback ? callback : default_reset_instr_callback;
}
#endif

#if M68K_TAS_HAS_CALLBACK == OPT_ON
void m68k_set_tas_instr_callback(int  (*callback)(void))
{
  CALLBACK_TAS_INSTR = callback ? callback : default_tas_instr_callback;
}
#endif

#if M68K_EMULATE_FC == OPT_ON
void m68k_set_fc_callback(void  (*callback)(unsigned int new_fc))
{
  CALLBACK_SET_FC = callback ? callback : default_set_fc_callback;
}
#endif

#ifdef LOGERROR

extern void error(char *format, ...);
extern uint16 v_counter;
#endif

/* ASG: rewrote so that the int_level is a mask of the IPL0/IPL1/IPL2 bits */
/* KS: Modified so that IPL* bits match with mask positions in the SR
 *     and cleaned out remenants of the interrupt controller.
 */
void m68k_update_irq(unsigned int mask)
{
  /* Update IRQ level */
  CPU_INT_LEVEL |= (mask << 8);
  
#ifdef LOGERROR
  error("[%d(%d)][%d(%d)] m68k IRQ Level = %d(0x%02x) (%x)\n", v_counter, m68k.cycles/3420, m68k.cycles, m68k.cycles%3420,CPU_INT_LEVEL>>8,FLAG_INT_MASK,m68k_get_reg(M68K_REG_PC));
#endif
}

void m68k_set_irq(unsigned int int_level)
{
  /* Set IRQ level */
  CPU_INT_LEVEL = int_level << 8;
  
#ifdef LOGERROR
  error("[%d(%d)][%d(%d)] m68k IRQ Level = %d(0x%02x) (%x)\n", v_counter, m68k.cycles/3420, m68k.cycles, m68k.cycles%3420,CPU_INT_LEVEL>>8,FLAG_INT_MASK,m68k_get_reg(M68K_REG_PC));
#endif
}

/* IRQ latency (Fatal Rewind, Sesame's Street Counting Cafe)*/
void m68k_set_irq_delay(unsigned int int_level)
{
  /* Prevent reentrance */
  if (!irq_latency)
  {
    /* This is always triggered from MOVE instructions (VDP CTRL port write) */
    /* We just make sure this is not a MOVE.L instruction as we could be in */
    /* the middle of its execution (first memory write).                   */
    if ((REG_IR & 0xF000) != 0x2000)
    {
      /* Finish executing current instruction */
      USE_CYCLES(CYC_INSTRUCTION[REG_IR]);

      /* One instruction delay before interrupt */
      irq_latency = 1;
      m68ki_trace_t1() /* auto-disable (see m68kcpu.h) */
      m68ki_use_data_space() /* auto-disable (see m68kcpu.h) */
      REG_IR = m68ki_read_imm_16();
      M68KI_DISPATCH(REG_IR);   /* MESHPUNK: see m68kops.h */
      m68ki_exception_if_trace() /* auto-disable (see m68kcpu.h) */
      irq_latency = 0;
    }

    /* Set IRQ level */
    CPU_INT_LEVEL = int_level << 8;
  }
  
#ifdef LOGERROR
  error("[%d(%d)][%d(%d)] m68k IRQ Level = %d(0x%02x) (%x)\n", v_counter, m68k.cycles/3420, m68k.cycles, m68k.cycles%3420,CPU_INT_LEVEL>>8,FLAG_INT_MASK,m68k_get_reg(M68K_REG_PC));
#endif

  /* Check interrupt mask to process IRQ  */
  m68ki_check_interrupts(); /* Level triggered (IRQ) */
}

void m68k_run(unsigned int cycles) 
{
    //  printf("m68K_run current_cycles=%d add=%d STOP=%x\n",m68k.cycles,cycles,CPU_STOPPED);

  /* Make sure CPU is not already ahead */
  if (m68k.cycles >= cycles)
  {
    return;
  }

  /* Check interrupt mask to process IRQ if needed */
  m68ki_check_interrupts();

  /* Make sure we're not stopped */
  if (CPU_STOPPED)
  {
    m68k.cycles = cycles;
    return;
  }

  /* Save end cycles count for when CPU is stopped */
  m68k.cycle_end = cycles;

  /* Return point for when we have an address error (TODO: use goto) */
  m68ki_set_address_error_trap() /* auto-disable (see m68kcpu.h) */

#ifdef LOGERROR
  error("[%d][%d] m68k run to %d cycles (%x), irq mask = %x (%x)\n", v_counter, m68k.cycles, cycles, m68k.pc,FLAG_INT_MASK, CPU_INT_LEVEL);
#endif

  while (m68k.cycles < cycles)
  {
    /* Set tracing accodring to T1. */
    m68ki_trace_t1() /* auto-disable (see m68kcpu.h) */

    /* Set the address space for reads */
    m68ki_use_data_space() /* auto-disable (see m68kcpu.h) */

#ifdef HOOK_CPU
    /* Trigger execution hook */
    if (cpu_hook)
      cpu_hook(HOOK_M68K_E, 0, REG_PC, 0);
#endif

    /* MESHPUNK: PC before the fetch, for the idle-loop check below. */
    uint m68ki_pc_before = REG_PC;

    /* Decode next instruction */
    REG_IR = m68ki_read_imm_16();

//    printf("PC=%x IR=%x CYCLES=%d \n",m68k.pc,REG_IR,CYC_INSTRUCTION[REG_IR]);

    /* Execute instruction */
    M68KI_DISPATCH(REG_IR);   /* MESHPUNK: see m68kops.h */
    USE_CYCLES(CYC_INSTRUCTION[REG_IR]);

    /* MESHPUNK: bounded idle-loop skip. A short backward branch landing on the
       same address repeatedly, with no write and no hardware read since, is a
       spin waiting on an interrupt. Give it the rest of the slice.

       The skip only ever reaches `cycles`, which is this scanline's end, so it
       can never run past an interrupt: the caller raises those between lines.
       Emulated cycle counts and interrupt timing are unchanged -- the CPU is
       simply not made to execute instructions whose results get discarded.
       Worst case, something outside the 68000 clears the flag mid-slice and we
       leave the loop up to one scanline late.

       m68ki_idle_hits is deliberately not reset after a skip, so the next slice
       recognises the same loop on its first branch instead of paying the
       threshold again -- at ~19 spin iterations per slice that is most of the
       win. A write or port read restarts the count, which is what pulls the
       CPU back out cleanly when the interrupt handler finally runs. */
    if (m68ki_idle_enable
        && REG_PC < m68ki_pc_before && (m68ki_pc_before - REG_PC) <= 16)
    {
      int m68ki_idle_ok = (REG_PC == m68ki_idle_pc)
                          && !m68ki_wrote && !m68ki_io_read;

      /* The register compare is the expensive part, so it runs only on the
         iteration that would actually skip -- once per threshold for a loop
         that IS making progress, which is what keeps ordinary dbf/copy loops
         cheap. If they match, the loop did nothing across all 8 iterations. */
      if (m68ki_idle_ok && m68ki_idle_hits + 1 >= 8)
      {
        int r;
        for (r = 0; r < 16; r++)
          if (REG_D[r] != m68ki_idle_regs[r])
          {
            m68ki_idle_ok = 0;
            break;
          }
      }

      if (m68ki_idle_ok)
      {
        if (++m68ki_idle_hits >= 8)
          m68k.cycles = cycles;
      }
      else
      {
        int r;
        m68ki_idle_pc   = REG_PC;
        m68ki_idle_hits = 1;
        m68ki_wrote     = 0;
        m68ki_io_read   = 0;
        for (r = 0; r < 16; r++)
          m68ki_idle_regs[r] = REG_D[r];
      }
    }

    /* Trace m68k_exception, if necessary */
    m68ki_exception_if_trace(); /* auto-disable (see m68kcpu.h) */
  }
}

int m68k_cycles(void)
{
  return CYC_INSTRUCTION[REG_IR];
}

int m68k_cycles_run(void)
{
	return m68k.cycle_end - m68k.cycles;
}

int m68k_cycles_master(void)
{
	return m68k.cycles;
}

void m68k_init(void)
{
#ifdef BUILD_TABLES
  static uint emulation_initialized = 0;

  /* The first call to this function initializes the opcode handler jump table */
  if(!emulation_initialized)
  {
    m68ki_build_opcode_table();
    emulation_initialized = 1;
  }
#endif

#ifdef M68K_OVERCLOCK_SHIFT
  m68k.cycle_ratio = 1 << M68K_OVERCLOCK_SHIFT;
#endif

#if M68K_EMULATE_INT_ACK == OPT_ON
  m68k_set_int_ack_callback(NULL);
#endif
#if M68K_EMULATE_RESET == OPT_ON
  m68k_set_reset_instr_callback(NULL);
#endif
#if M68K_TAS_HAS_CALLBACK == OPT_ON
  m68k_set_tas_instr_callback(NULL);
#endif
#if M68K_EMULATE_FC == OPT_ON
  m68k_set_fc_callback(NULL);
#endif
}

/* Pulse the RESET line on the CPU */
void m68k_pulse_reset(void)
{
  /* Clear all stop levels */
  CPU_STOPPED = 0;
#if M68K_EMULATE_ADDRESS_ERROR
  CPU_RUN_MODE = RUN_MODE_BERR_AERR_RESET;
#endif

  /* Turn off tracing */
  FLAG_T1 = 0;
  m68ki_clear_trace()

  /* Interrupt mask to level 7 */
  FLAG_INT_MASK = 0x0700;
  CPU_INT_LEVEL = 0;
  irq_latency = 0;

  /* Go to supervisor mode */
  m68ki_set_s_flag(SFLAG_SET);

  /* Invalidate the prefetch queue */
#if M68K_EMULATE_PREFETCH
  /* Set to arbitrary number since our first fetch is from 0 */
  CPU_PREF_ADDR = 0x1000;
#endif /* M68K_EMULATE_PREFETCH */

  /* Read the initial stack pointer and program counter */
  m68ki_jump(0);
  REG_SP = m68ki_read_imm_32();
  REG_PC = m68ki_read_imm_32();
  m68ki_jump(REG_PC);

#if M68K_EMULATE_ADDRESS_ERROR
  CPU_RUN_MODE = RUN_MODE_NORMAL;
#endif

  USE_CYCLES(CYC_EXCEPTION[EXCEPTION_RESET]);
}

void m68k_pulse_halt(void)
{
  /* Pulse the HALT line on the CPU */
  CPU_STOPPED |= STOP_LEVEL_HALT;
}

void m68k_clear_halt(void)
{
  /* Clear the HALT line on the CPU */
  CPU_STOPPED &= ~STOP_LEVEL_HALT;
}

void gwenesis_m68k_save_state() {
  SaveState *state;
  state = saveGwenesisStateOpenForWrite("m68k");
  
  saveGwenesisStateSetBuffer(state, "REG_D", REG_D, sizeof(REG_D));
  saveGwenesisStateSet(state, "SR", m68ki_get_sr());
  saveGwenesisStateSet(state, "REG_PC", REG_PC);
  saveGwenesisStateSet(state, "REG_SP", REG_SP);
  saveGwenesisStateSet(state, "REG_USP", REG_USP);
  saveGwenesisStateSet(state, "REG_ISP", REG_ISP);
  saveGwenesisStateSet(state, "REG_IR", REG_IR);

  saveGwenesisStateSet(state, "m68k_cycle_end", m68k.cycle_end);
  saveGwenesisStateSet(state, "m68k_cycles", m68k.cycles);
  saveGwenesisStateSet(state, "m68k_int_level", m68k.int_level);
  saveGwenesisStateSet(state, "m68k_stopped", m68k.stopped);
}

void gwenesis_m68k_load_state() {
  SaveState *state = saveGwenesisStateOpenForRead("m68k");
  saveGwenesisStateGetBuffer(state, "REG_D", REG_D, sizeof(REG_D));

  m68ki_set_sr(saveGwenesisStateGet(state, "SR"));
  REG_PC = saveGwenesisStateGet(state, "REG_PC");
  REG_SP = saveGwenesisStateGet(state, "REG_SP");
  REG_USP = saveGwenesisStateGet(state, "REG_USP");
  REG_ISP = saveGwenesisStateGet(state, "REG_ISP");
  REG_IR = saveGwenesisStateGet(state, "REG_IR");

  m68k.cycle_end = saveGwenesisStateGet(state, "m68k_cycle_end");
  m68k.cycles = saveGwenesisStateGet(state, "m68k_cycles");
  m68k.int_level = saveGwenesisStateGet(state, "m68k_int_level");
  m68k.stopped = saveGwenesisStateGet(state, "m68k_stopped");

}

/* MESHPUNK: bind the cartridge SRAM window. buf NULL disables it entirely, so
   carts without a battery pay only a null test on the read/write fast path. */
void m68k_set_sram(uint8_t *buf, uint32_t start, uint32_t end)
{
  m68ki_sram       = buf;
  m68ki_sram_start = start;
  m68ki_sram_span  = (buf && end >= start) ? (end - start) : 0;
  m68ki_sram_dirty = 0;
}

int  m68k_sram_dirty(void)       { return m68ki_sram_dirty; }
void m68k_sram_clear_dirty(void) { m68ki_sram_dirty = 0; }

/* MESHPUNK: launcher toggle for the idle-loop skip. Off restores plain
   interpretation — slower, but immune to a loop shape the detector reads
   wrong. See the guards in m68kcpu.h for what it tests. */
void m68k_set_idle_skip(int on)
{
  m68ki_idle_enable = on ? 1 : 0;
  printf("[m68k] idle-loop skip %s\n", on ? "on" : "off");
}

/* ======================================================================== */
/* ============================== END OF FILE ============================= */
/* ======================================================================== */
