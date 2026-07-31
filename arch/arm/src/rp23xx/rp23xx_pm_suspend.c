/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_pm_suspend.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 *
 * Suspend to RAM using the POWMAN power state machine.
 *
 * This is a different mechanism from the dormant state in rp23xx_pm.c, and
 * the distinction is the whole point of the file.  Dormancy stops the
 * clocks but leaves the switched core powered, which is a P0 state in the
 * datasheet's naming and cannot go below a couple of milliamps.  The
 * RP2350's actual low power states are the P1.m states, in which the
 * switched core is powered off entirely; they are quoted in the hundreds of
 * microamps.
 *
 * The cost of that is everything in the switched core: the processors, the
 * bus fabric and every peripheral lose their registers.  Execution does not
 * resume by returning from an instruction, it resumes through the boot path.
 * So this cannot be an idle state the governor drops into -- it has to be
 * something an application asks for, having decided it is willing to lose
 * that state.
 *
 * Target state is P1.0: switched core off, XIP cache and both SRAM banks
 * left powered.  The datasheet notes that memory left powered while the
 * switched core is down automatically drops to a retention mode and keeps
 * its contents, so RAM survives and the resume path is a warm restart.
 *
 * Reference: RP2350 datasheet sections 5.2.3 (POWMAN boot vector), 6.2.2
 * (power states) and 6.2.3 (power state transitions).
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <setjmp.h>
#include <debug.h>

#include <arch/board/board.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>

#include "arm_internal.h"
#include "nvic.h"

#include "rp23xx_pm.h"
#include "rp23xx_clock.h"
#include "rp23xx_uart.h"

#include "hardware/rp23xx_powman.h"
#include "hardware/rp23xx_pads_bank0.h"
#include "hardware/rp23xx_memorymap.h"

#ifdef CONFIG_RP23XX_PM_SUSPEND

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The resume vector runs before anything has been set up, out of memory the
 * bootrom has just handed control to.  It must not be in flash: reaching it
 * is the very thing that has not been arranged yet.
 */

#define RP23XX_PM_RAMFUNC \
  __attribute__((section(".time_critical.rp23xx_pm_suspend"), noinline))

/* Most POWMAN registers carry a password in the top 16 bits and take only
 * 16 bits of data, which is why the alarm time is split across four of
 * them.  Everything above offset 0xac is exempt and takes a plain 32-bit
 * write: SCRATCH0-7, BOOT0-3 and the interrupt registers.  BOOT0-3 hold
 * full 32-bit values and must NOT be written with the password.
 */

#define POWMAN_PASSWORD    0x5afe0000
#define POWMAN_SET_ALIAS   0x2000
#define POWMAN_CLR_ALIAS   0x3000

/* STATE.REQ occupies bits 7:4 and is active low: a set bit powers a domain
 * DOWN.  bit3 = SWCORE, bit2 = XIP, bit1 = SRAM0, bit0 = SRAM1.
 *
 * P1.0 is therefore "switched core down, everything else up" = 0x8, which
 * lands in the register as 0x80.  Note the pico-sdk uses the opposite
 * convention in its powman_power_state helpers and inverts on write; do not
 * copy its constants here.
 */

#define POWMAN_STATE_P1_0  (0x8 << 4)

/* Magic values the bootrom looks for in BOOT0..BOOT3 (datasheet 5.2.3). */

#define POWMAN_BOOT_MAGIC  0xb007c0d3
#define POWMAN_BOOT_XOR    0x4ff83f2d

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Where the resume vector jumps back to.  Lives in .bss, which is in SRAM
 * and therefore retained across P1.0.
 */

static jmp_buf g_suspend_ctx;

/* The resume vector runs on whatever stack pointer was handed to the
 * bootrom in BOOT2, before longjmp() puts the suspending thread's stack
 * back.  It needs somewhere real to run: it calls into the clock setup,
 * which is not a leaf.  Retained in SRAM like everything else.
 */

#define RP23XX_PM_RESUME_STACK_WORDS 256

static uint32_t g_resume_stack[RP23XX_PM_RESUME_STACK_WORDS]
  __attribute__((aligned(8)));

/* Set by the resume vector so the caller can tell a real resume from the
 * initial pass, and report what woke the chip.
 */

static volatile uint32_t g_suspend_wake_source;

/* The interrupt controller is in the switched core, so it comes back with
 * every enable and priority cleared while the rest of the system still
 * believes it configured them.  Nothing in the NVIC can be recovered from
 * the OS state cheaply, so snapshot the registers on the way down.
 *
 * RP2350 has 52 interrupts, which is two enable words and thirteen priority
 * words; size these from NR_IRQS rather than hard coding.
 */

#define RP23XX_PM_NVIC_ENABLE_REGS  2
#define RP23XX_PM_NVIC_PRIO_REGS    13

struct rp23xx_pm_nvic_s
{
  uint32_t enable[RP23XX_PM_NVIC_ENABLE_REGS];
  uint32_t prio[RP23XX_PM_NVIC_PRIO_REGS];
  uint32_t systick_ctrl;
  uint32_t systick_reload;
  uint32_t shpr2;
  uint32_t shpr3;
  uint32_t vectab;
};

static struct rp23xx_pm_nvic_s g_suspend_nvic;

/****************************************************************************
 * Name: rp23xx_pm_nvic_save / rp23xx_pm_nvic_restore
 *
 * Description:
 *   Snapshot and reinstate the interrupt controller across a power down of
 *   the switched core.  Re-running up_irqinitialize() is not an option: it
 *   would reset the handler table as well, and the handlers are exactly
 *   what has survived in RAM.
 *
 ****************************************************************************/

static void rp23xx_pm_nvic_save(void)
{
  int i;

  for (i = 0; i < RP23XX_PM_NVIC_ENABLE_REGS; i++)
    {
      g_suspend_nvic.enable[i] = getreg32(NVIC_IRQ_ENABLE(i * 32));
    }

  for (i = 0; i < RP23XX_PM_NVIC_PRIO_REGS; i++)
    {
      g_suspend_nvic.prio[i] = getreg32(NVIC_IRQ0_3_PRIORITY + i * 4);
    }

  g_suspend_nvic.systick_ctrl   = getreg32(NVIC_SYSTICK_CTRL);
  g_suspend_nvic.systick_reload = getreg32(NVIC_SYSTICK_RELOAD);
  g_suspend_nvic.shpr2          = getreg32(NVIC_SYSH8_11_PRIORITY);
  g_suspend_nvic.shpr3          = getreg32(NVIC_SYSH12_15_PRIORITY);

  /* The vector table offset is the one that makes every other restore
   * pointless if it is missed: without it the core takes exceptions
   * through whatever table the bootrom left behind, so no NuttX handler
   * ever runs and the system is dead while still perfectly clocked.
   */

  g_suspend_nvic.vectab         = getreg32(NVIC_VECTAB);
}

static void RP23XX_PM_RAMFUNC rp23xx_pm_nvic_restore(void)
{
  int i;

  putreg32(g_suspend_nvic.vectab, NVIC_VECTAB);

  for (i = 0; i < RP23XX_PM_NVIC_PRIO_REGS; i++)
    {
      putreg32(g_suspend_nvic.prio[i], NVIC_IRQ0_3_PRIORITY + i * 4);
    }

  putreg32(g_suspend_nvic.shpr2, NVIC_SYSH8_11_PRIORITY);
  putreg32(g_suspend_nvic.shpr3, NVIC_SYSH12_15_PRIORITY);

  putreg32(g_suspend_nvic.systick_reload, NVIC_SYSTICK_RELOAD);
  putreg32(0, NVIC_SYSTICK_CURRENT);
  putreg32(g_suspend_nvic.systick_ctrl, NVIC_SYSTICK_CTRL);

  /* Enables last, so nothing fires before its priority is back. */

  for (i = 0; i < RP23XX_PM_NVIC_ENABLE_REGS; i++)
    {
      putreg32(g_suspend_nvic.enable[i], NVIC_IRQ_ENABLE(i * 32));
    }
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline void powman_write(uint32_t reg, uint32_t value)
{
  putreg32(POWMAN_PASSWORD | (value & 0xffff), reg);
}

static inline void powman_setbits(uint32_t reg, uint32_t bits)
{
  putreg32(POWMAN_PASSWORD | bits, reg + POWMAN_SET_ALIAS);
}

static inline void powman_clrbits(uint32_t reg, uint32_t bits)
{
  putreg32(POWMAN_PASSWORD | bits, reg + POWMAN_CLR_ALIAS);
}

/****************************************************************************
 * Name: rp23xx_pm_resume_vector
 *
 * Description:
 *   Entered from the bootrom after the switched core has been powered back
 *   up.  Nothing in the switched core has any state: the clock tree is at
 *   its reset defaults, every peripheral has been reset, and the stack is
 *   whatever was handed over in BOOT2.
 *
 *   Only enough is rebuilt here to get back into C with a working clock
 *   tree; the rest is the caller's problem, which is why suspending is an
 *   explicit request rather than something the idle loop can do.
 *
 ****************************************************************************/

static void RP23XX_PM_RAMFUNC rp23xx_pm_resume_vector(void)
{
  /* Record what woke us before anything else can disturb it. */

  g_suspend_wake_source = getreg32(RP23XX_POWMAN_LAST_SWCORE_PWRUP);

  /* Rebuild the clock tree.  Until this runs the chip is on its boot
   * defaults, which is not what the rest of the system believes.
   */

  rp23xx_clockconfig();

  /* Put back the pin muxing and the console.  Both live in the switched
   * core, so the UART is not merely unconfigured, its pins are no longer
   * connected to it.
   */

  rp23xx_boardearlyinitialize();
  rp23xx_lowsetup();

  /* And the interrupt controller, which is what actually makes the system
   * run again rather than merely appear to.
   */

  rp23xx_pm_nvic_restore();

  /* Back into the suspending thread, which resumes as though
   * rp23xx_pm_suspend() had simply returned.
   */

  longjmp(g_suspend_ctx, 1);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rp23xx_pm_suspend
 *
 * Description:
 *   Power the switched core down (P1.0) and return when something wakes it.
 *
 *   The caller is asserting that losing the switched core is acceptable:
 *   every peripheral comes back reset, and any driver state that mattered
 *   had to be saved beforehand.
 *
 * Input Parameters:
 *   wake_ms - Wake this many milliseconds from now using the always-on
 *             timer alarm, or zero for no timed wake.  Unlike the dormant
 *             state, a timed wake genuinely works here: the alarm belongs
 *             to this power state machine.
 *
 * Returned Value:
 *   Zero once the chip has resumed.  A negated errno if the request was
 *   refused, in which case the chip never went down.
 *
 ****************************************************************************/

int rp23xx_pm_suspend(uint32_t wake_ms)
{
  irqstate_t flags;
  uint32_t entry;
  uint32_t state;

  /* A debugger asserting CSYSPWRUPREQ forces P0.0 and makes the power
   * manager ignore software requests entirely, silently.  Mask it, or
   * nothing below has any effect while a probe is attached.
   */

  powman_setbits(RP23XX_POWMAN_DBG_PWRCFG, RP23XX_POWMAN_DBG_PWRCFG_IGNORE);

  flags = enter_critical_section();

  /* Returns non-zero when the resume vector longjmps back here. */

  if (setjmp(g_suspend_ctx) != 0)
    {
      leave_critical_section(flags);
      return OK;
    }

  /* Install the resume vector.  The entry point needs its Thumb bit set:
   * the bootrom treats a clear bit as a RISC-V pointer handed to an Arm
   * core and deliberately hangs rather than running it.
   */

  rp23xx_pm_nvic_save();

  entry = (uint32_t)&rp23xx_pm_resume_vector | 1u;

  putreg32(0, RP23XX_POWMAN_BOOT0);
  putreg32(entry ^ POWMAN_BOOT_XOR, RP23XX_POWMAN_BOOT1);
  putreg32((uint32_t)&g_resume_stack[RP23XX_PM_RESUME_STACK_WORDS],
           RP23XX_POWMAN_BOOT2);
  putreg32(entry, RP23XX_POWMAN_BOOT3);
  putreg32(POWMAN_BOOT_MAGIC, RP23XX_POWMAN_BOOT0);

  /* Arm the timed wake if one was asked for.  Both bits are needed: the
   * alarm has to be enabled and it has to be allowed to raise a power up.
   */

  if (wake_ms > 0)
    {
      uint64_t when;

      when = ((uint64_t)getreg32(RP23XX_POWMAN_READ_TIME_UPPER) << 32) |
             getreg32(RP23XX_POWMAN_READ_TIME_LOWER);
      when += wake_ms;

      powman_clrbits(RP23XX_POWMAN_TIMER, RP23XX_POWMAN_TIMER_ALARM_ENAB);

      powman_write(RP23XX_POWMAN_ALARM_TIME_15TO0,  (uint32_t)(when));
      powman_write(RP23XX_POWMAN_ALARM_TIME_31TO16, (uint32_t)(when >> 16));
      powman_write(RP23XX_POWMAN_ALARM_TIME_47TO32, (uint32_t)(when >> 32));
      powman_write(RP23XX_POWMAN_ALARM_TIME_63TO48, (uint32_t)(when >> 48));

      powman_clrbits(RP23XX_POWMAN_TIMER, RP23XX_POWMAN_TIMER_ALARM);
      powman_setbits(RP23XX_POWMAN_TIMER,
                     RP23XX_POWMAN_TIMER_PWRUP_ON_ALARM |
                     RP23XX_POWMAN_TIMER_ALARM_ENAB);
    }

  /* Request the state change.  Clear the ignored flag first so that the
   * check below reflects this request and not an older one.
   */

  powman_clrbits(RP23XX_POWMAN_STATE, RP23XX_POWMAN_STATE_REQ_IGNORED);
  powman_write(RP23XX_POWMAN_STATE, POWMAN_STATE_P1_0);

  state = getreg32(RP23XX_POWMAN_STATE);

  if ((state & RP23XX_POWMAN_STATE_REQ_IGNORED) != 0)
    {
      /* Something already wanted the chip powered up.  Most often this is
       * still a debugger, despite DBG_PWRCFG.IGNORE above.
       */

      putreg32(0, RP23XX_POWMAN_BOOT0);
      leave_critical_section(flags);
      return -EBUSY;
    }

  if ((state & RP23XX_POWMAN_STATE_BAD_SW_REQ) != 0)
    {
      putreg32(0, RP23XX_POWMAN_BOOT0);
      leave_critical_section(flags);
      return -EINVAL;
    }

  /* WAITING stays set until the processors halt; the sequencer only starts
   * the transition then.  This does not return -- the core loses power part
   * way through, and execution picks up again in the resume vector.
   */

  for (; ; )
    {
      __asm__ __volatile__ ("dsb" ::: "memory");
      __asm__ __volatile__ ("wfi");
    }
}

/****************************************************************************
 * Name: rp23xx_pm_wake_source
 *
 * Description:
 *   What caused the most recent resume, as the raw LAST_SWCORE_PWRUP value.
 *
 ****************************************************************************/

uint32_t rp23xx_pm_wake_source(void)
{
  return g_suspend_wake_source;
}

#endif /* CONFIG_RP23XX_PM_SUSPEND */
