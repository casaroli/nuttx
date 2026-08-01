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
 * Resume deliberately takes the long way round.  POWMAN can hold a boot
 * vector that the bootrom jumps to as soon as the core is back, but that
 * vector runs before execute-in-place has been set up, so it may touch only
 * RAM and registers.  An earlier version of this file jumped back into the
 * OS from there and lost the core on its first call into flash, because
 * putting the vector itself in RAM does nothing for what it calls.
 *
 * So no vector is installed.  The bootrom completes an ordinary boot, which
 * brings up XIP and the clock tree through the path every cold boot already
 * exercises, and the resume is picked up in __start -- which asks
 * rp23xx_pm_resume_pending() before it can destroy the retained state, and
 * hands control back through rp23xx_pm_resume() where it would otherwise
 * have started the OS.
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

#ifdef CONFIG_RP23XX_PM_AUTOSUSPEND
#  include <stdlib.h>
#  include <unistd.h>
#  include <sched.h>
#  include <nuttx/kthread.h>
#endif

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/power/pm.h>
#include <nuttx/clock.h>

#include "arm_internal.h"
#include "nvic.h"

#include "rp23xx_pm.h"
#include "rp23xx_gpio.h"

#include "hardware/rp23xx_powman.h"
#include "hardware/rp23xx_pads_bank0.h"
#include "hardware/rp23xx_memorymap.h"

#ifdef CONFIG_RP23XX_PM_SUSPEND

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

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

/* Left in SCRATCH0 on the way down and consumed by the next boot, which is
 * how that boot knows it is a resume rather than a cold start.  SCRATCH0
 * lives in the always-on domain, so it survives the very thing that makes
 * the question worth asking; a magic word in RAM would too, but only for as
 * long as the assumption that RAM was retained holds, and that assumption is
 * exactly what must not be taken on trust before the bss is cleared.
 */

#define POWMAN_SUSPEND_MAGIC 0x50575231  /* 'PWR1' */

#ifdef CONFIG_RP23XX_PM_AUTOSUSPEND
/* The automatic suspend thread does very little itself, but it is the thread
 * the resume returns onto, so it carries whatever rp23xx_pm_suspend() needs.
 */

#  define RP23XX_PM_AUTOSUSPEND_STACKSIZE 2048
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* The stack a resume boots on.  Defined here rather than in the start-up
 * code so that a build without suspend to RAM does not carry it.
 */

uint32_t g_pm_resume_stack[RP23XX_PM_RESUME_STACK_WORDS]
  __attribute__((aligned(8)));

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Where the resume vector jumps back to.  Lives in .bss, which is in SRAM
 * and therefore retained across P1.0.
 */

static jmp_buf g_suspend_ctx;

/* Set on the way back so the caller can report what woke the chip. */

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

static void rp23xx_pm_nvic_restore(void)
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
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rp23xx_pm_pwrup_gpio
 *
 * Description:
 *   Arm a GPIO in a POWMAN power-up detector, so that it can end a P1.0
 *   suspend.  This is a different mechanism from the dormant-wake detector
 *   in rp23xx_pm.c: that one watches the pad through the IO bank, which is
 *   in the switched core and therefore gone here.  These detectors belong to
 *   the power manager and keep watching with the core powered down.
 *
 * Input Parameters:
 *   gpio - Pin to watch.
 *   edge - True for a transition, false for a level.
 *   high - True for rising/high, false for falling/low.
 *
 ****************************************************************************/

static void rp23xx_pm_pwrup_gpio(int gpio, bool edge, bool high)
{
  uint32_t regval;

  /* The detector reads the pad, so the input buffer has to be on -- and the
   * isolation latch has to be off.  Setting IE alone is not enough: a pad
   * still isolated reads a constant low and no amount of configuration
   * makes it see anything.
   */

  modifyreg32(RP23XX_PADS_BANK0_GPIO(gpio), RP23XX_PADS_BANK0_GPIO_ISO,
              RP23XX_PADS_BANK0_GPIO_IE);

  /* And a defined idle level, opposite to whatever it triggers on.  This is
   * not tidiness: the pad default is a pull-down, and a pull-down on a line
   * that idles high -- a UART receive, say -- is a resistor across the
   * supply for as long as the board is asleep.  Measured at 66 uA on the
   * console pin, which is over a tenth of the whole suspend current.
   *
   * A cold boot gets this right via rp23xx_pm_gpio_wakeup(), but a resume
   * never runs arm_pminitialize(), so doing it here is what makes the two
   * paths agree.
   */

  if (high)
    {
      modifyreg32(RP23XX_PADS_BANK0_GPIO(gpio), RP23XX_PADS_BANK0_GPIO_PUE,
                  RP23XX_PADS_BANK0_GPIO_PDE);
    }
  else
    {
      modifyreg32(RP23XX_PADS_BANK0_GPIO(gpio), RP23XX_PADS_BANK0_GPIO_PDE,
                  RP23XX_PADS_BANK0_GPIO_PUE);
    }

  /* Disable while the source changes, or a spurious request can be latched
   * from the previous configuration.
   */

  powman_clrbits(RP23XX_POWMAN_PWRUP0, RP23XX_POWMAN_PWRUP0_ENABLE);

  regval = (uint32_t)gpio & RP23XX_POWMAN_PWRUP0_SOURCE_MASK;

  if (edge)
    {
      regval |= RP23XX_POWMAN_PWRUP0_MODE;
    }

  if (high)
    {
      regval |= RP23XX_POWMAN_PWRUP0_DIRECTION;
    }

  putreg32(POWMAN_PASSWORD | regval, RP23XX_POWMAN_PWRUP0);

  /* Clear anything already latched *before* enabling, not after.  An edge
   * from before the pin was configured would otherwise end the suspend the
   * instant it began.
   */

  powman_clrbits(RP23XX_POWMAN_PWRUP0, RP23XX_POWMAN_PWRUP0_STATUS);
  powman_setbits(RP23XX_POWMAN_PWRUP0, RP23XX_POWMAN_PWRUP0_ENABLE);
}

/****************************************************************************
 * Name: rp23xx_pm_resume_pending
 *
 * Description:
 *   Whether this boot is a resume from suspend to RAM, and therefore whether
 *   the retained contents of SRAM are live data rather than something to be
 *   initialised over.
 *
 *   Called from __start before the bss is cleared, so it must touch no
 *   global state of its own; everything it consults is a register in the
 *   always-on domain.  It is also one-shot: the marker is consumed here, so
 *   that a boot which fails somewhere after this point comes up cold rather
 *   than trying to resume into whatever it left behind.
 *
 *   The marker alone is not enough.  It survives a plain reset as happily as
 *   it survives the power down, so a board reset while suspended -- pulling
 *   RUN low, or a debugger reflashing it -- would otherwise look exactly
 *   like a resume, and jumping into a stale context on a freshly flashed
 *   image is a hang that needs BOOTSEL to clear.  CHIP_RESET reports the
 *   cause of the last reset, so require it to say the switched core was
 *   powered down.
 *
 * Returned Value:
 *   True if the rest of the boot should preserve SRAM and finish through
 *   rp23xx_pm_resume().
 *
 ****************************************************************************/

bool rp23xx_pm_resume_pending(void)
{
  uint32_t marker;
  uint32_t cause;

  marker = getreg32(RP23XX_POWMAN_SCRATCH0);
  cause  = getreg32(RP23XX_POWMAN_CHIP_RESET);

  putreg32(0, RP23XX_POWMAN_SCRATCH0);

  return marker == POWMAN_SUSPEND_MAGIC &&
         (cause & RP23XX_POWMAN_CHIP_RESET_HAD_SWCORE_PD) != 0;
}

/****************************************************************************
 * Name: rp23xx_pm_resume
 *
 * Description:
 *   Finish a resume by handing control back to the thread that suspended,
 *   in place of starting the OS.  Called from __start once the ordinary
 *   boot has rebuilt everything in the switched core: the clock tree, the
 *   pin muxing, the console and the board's own hardware are all back by
 *   this point, through the same code a cold boot runs.
 *
 *   What that boot cannot rebuild is anything whose state lived only in
 *   RAM, which is why the interrupt controller is put back from a snapshot
 *   here rather than reinitialised.
 *
 *   Does not return.
 *
 ****************************************************************************/

void rp23xx_pm_resume(void)
{
  /* Report what woke the chip.  LAST_SWCORE_PWRUP is in the always-on
   * domain and holds until the next power up, so there is no hurry to read
   * it, and reading it here keeps it clear of the bss decision above.
   */

  g_suspend_wake_source = getreg32(RP23XX_POWMAN_LAST_SWCORE_PWRUP);

  /* Nothing may be delivered between the interrupt controller coming back
   * and the suspending thread getting its stack back: until the longjmp the
   * core is still on the stack the resume booted on, which belongs to no
   * thread at all, and every handler the NVIC is about to point at expects
   * to be running on one.
   */

  up_irq_save();

  rp23xx_pm_nvic_restore();

  /* Back into the suspending thread, which resumes as though
   * rp23xx_pm_suspend() had simply returned.  The critical section it took
   * on the way down is released there, which is also what re-enables
   * interrupts.
   */

  longjmp(g_suspend_ctx, 1);

  /* longjmp() does not come back, and there is nothing sane to do if it
   * ever did: the caller's alternative was to start the OS, and the OS is
   * already running as far as everything in RAM is concerned.
   */

  for (; ; );
}

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

#ifdef CONFIG_RTC
      /* The system tick stopped for the whole suspend and was never caught
       * up, so the time of day is now slow by however long the chip was
       * down.  The always-on timer kept counting, being in the domain that
       * stayed powered, so it is the one thing that still knows.
       *
       * Only CLOCK_REALTIME is corrected by this, and that is deliberate.
       * CLOCK_MONOTONIC is defined as elapsed time *excluding* suspend --
       * see clock_gettime() -- so a monotonic clock that skipped the
       * interval is not a defect to be repaired.  Winding the tick forward
       * would break it rather than fix it.
       */

      clock_synchronize(NULL);
#endif
      return OK;
    }

  rp23xx_pm_nvic_save();

  /* Make sure the bootrom is not handed a resume vector.  This build never
   * installs one -- the whole point is to come back through the ordinary
   * boot -- but BOOT0 is in the always-on domain and outlives the image
   * that wrote it, so a firmware update from a version that did install one
   * would otherwise leave the bootrom jumping into a stale address.
   */

  putreg32(0, RP23XX_POWMAN_BOOT0);

  /* Tell the next boot what it is.  This is the last thing set before the
   * chip goes down and the first thing the next boot consumes.
   */

  putreg32(POWMAN_SUSPEND_MAGIC, RP23XX_POWMAN_SCRATCH0);

#if CONFIG_RP23XX_PM_WAKEUP_GPIO >= 0
  /* Arm the board's wake pin as well.  The timer is what this state is
   * normally left by, but a pin gives a way back in from outside without
   * waiting for the alarm -- the console receive line, by default, whose
   * start bit is a falling edge.
   */

#  ifdef CONFIG_RP23XX_PM_WAKEUP_GPIO_EDGE
#    define RP23XX_PM_SUSPEND_WAKE_EDGE true
#  else
#    define RP23XX_PM_SUSPEND_WAKE_EDGE false
#  endif

#  ifdef CONFIG_RP23XX_PM_WAKEUP_GPIO_HIGH
#    define RP23XX_PM_SUSPEND_WAKE_HIGH true
#  else
#    define RP23XX_PM_SUSPEND_WAKE_HIGH false
#  endif

  rp23xx_pm_pwrup_gpio(CONFIG_RP23XX_PM_WAKEUP_GPIO,
                       RP23XX_PM_SUSPEND_WAKE_EDGE,
                       RP23XX_PM_SUSPEND_WAKE_HIGH);
#endif

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

      putreg32(0, RP23XX_POWMAN_SCRATCH0);
      leave_critical_section(flags);
      return -EBUSY;
    }

  if ((state & RP23XX_POWMAN_STATE_BAD_SW_REQ) != 0)
    {
      putreg32(0, RP23XX_POWMAN_SCRATCH0);
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

#ifdef CONFIG_RP23XX_PM_AUTOSUSPEND

/****************************************************************************
 * Name: rp23xx_pm_autosuspend_thread
 *
 * Description:
 *   Suspend on a timer for as long as the board is powered, so that it can
 *   be measured with nothing attached.  Runs as a kernel thread because a
 *   suspend has to happen in task context: the resume comes back through
 *   setjmp()/longjmp() onto the stack of whatever asked for it.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: rp23xx_pm_autosuspend_led
 *
 * Description:
 *   Drive the indicator LED, if the configuration named one.  The board
 *   start-up code lights it on every boot and every resume, so without this
 *   it burns a milliamp through every state being measured.
 *
 ****************************************************************************/

static void rp23xx_pm_autosuspend_led(bool on)
{
#if CONFIG_RP23XX_PM_AUTOSUSPEND_LED_GPIO >= 0
  rp23xx_gpio_init(CONFIG_RP23XX_PM_AUTOSUSPEND_LED_GPIO);
  rp23xx_gpio_setdir(CONFIG_RP23XX_PM_AUTOSUSPEND_LED_GPIO, true);
  rp23xx_gpio_put(CONFIG_RP23XX_PM_AUTOSUSPEND_LED_GPIO, on);
#endif
}

/****************************************************************************
 * Name: rp23xx_pm_autosuspend_spin
 *
 * Description:
 *   Burn a phase at full speed.  Deliberately busy rather than asleep: this
 *   is the step the rest of the trace is read against, so the core must be
 *   kept out of any idle state.
 *
 ****************************************************************************/

static void rp23xx_pm_autosuspend_spin(int seconds)
{
  volatile uint32_t sink = 0;
  clock_t start;
  int i;

  start = clock_systime_ticks();

  while (clock_systime_ticks() - start < (clock_t)SEC2TICK(seconds))
    {
      for (i = 0; i < 1000; i++)
        {
          sink = sink + i;
        }
    }
}

/****************************************************************************
 * Name: rp23xx_pm_autosuspend_hold
 *
 * Description:
 *   Pin the idle domain at one state, releasing whatever was held before.
 *   The governor picks the shallowest state carrying a wakelock, so a single
 *   hold is what decides how far down it goes.
 *
 ****************************************************************************/

static void rp23xx_pm_autosuspend_hold(enum pm_state_e want,
                                       enum pm_state_e held)
{
  pm_stay(PM_IDLE_DOMAIN, want);
  pm_relax(PM_IDLE_DOMAIN, held);
}

static int rp23xx_pm_autosuspend_thread(int argc, char **argv)
{
  int ret;

  /* Hold the governor at PM_NORMAL for good.  Two reasons, and the second
   * one strands the board:
   *
   * P1.0 is entered from here rather than from the idle loop, so the state
   * the governor happens to have reached is not something this code has ever
   * been asked to cope with.
   *
   * More seriously, the dormant state is left only by the configured wake
   * GPIO, which is the console receive pin.  The whole point of this option
   * is to run with nothing attached -- so there would be nothing to drive
   * that pin, and a board that went dormant would stay there.
   */

  pm_stay(PM_IDLE_DOMAIN, PM_NORMAL);

  for (; ; )
    {
      /* Every awake phase comes before the suspend, never after: together
       * they are the only window in which the board can be reflashed, and a
       * boot that went straight down would leave no way in short of BOOTSEL.
       *
       * The LED starts lit because the board start-up code lights it, on a
       * resume just as much as on a cold boot.  Giving it a phase of its own
       * rather than extinguishing it immediately puts its cost in the trace
       * as a step, which is worth more than not paying it.
       */

      rp23xx_pm_autosuspend_led(true);
      rp23xx_pm_autosuspend_spin(CONFIG_RP23XX_PM_AUTOSUSPEND_PHASE_S);

      /* Same state, LED off: the difference between these two steps is what
       * the indicator costs.
       */

      rp23xx_pm_autosuspend_led(false);
      rp23xx_pm_autosuspend_spin(CONFIG_RP23XX_PM_AUTOSUSPEND_PHASE_S);

      /* Idle: still PM_NORMAL, so nothing is gated, but the core reaches WFI
       * because this thread is asleep rather than spinning.
       */

      sleep(CONFIG_RP23XX_PM_AUTOSUSPEND_PHASE_S);

      /* Standby: let the governor gate the peripheral clocks, but no
       * further.  It picks the shallowest state holding a wakelock.
       */

      rp23xx_pm_autosuspend_hold(PM_STANDBY, PM_NORMAL);
      sleep(CONFIG_RP23XX_PM_AUTOSUSPEND_PHASE_S);

      /* Back up to PM_NORMAL before suspending.  P1.0 is entered from here
       * rather than from the idle loop, and entering it from a clock-gated
       * state is not something the entry path was written for.
       */

      rp23xx_pm_autosuspend_hold(PM_NORMAL, PM_STANDBY);

      /* And down.  The timer alarm is what brings it back, so this ends by
       * itself with nothing attached to the board; the wake GPIO is armed as
       * well, but only as a way in if one is wanted.
       */

      ret = rp23xx_pm_suspend(CONFIG_RP23XX_PM_AUTOSUSPEND_HOLD_S * 1000);
      if (ret < 0)
        {
          pwrerr("ERROR: automatic suspend refused: %d\n", ret);
        }

#ifndef CONFIG_RP23XX_PM_AUTOSUSPEND_REPEAT
      break;
#endif
    }

  return EXIT_SUCCESS;
}

/****************************************************************************
 * Name: rp23xx_pm_autosuspend_start
 *
 * Description:
 *   Start the automatic suspend thread.  Called from arm_pminitialize().
 *
 ****************************************************************************/

void rp23xx_pm_autosuspend_start(void)
{
  int ret;

  ret = kthread_create("rp23xx_suspend", SCHED_PRIORITY_DEFAULT,
                       RP23XX_PM_AUTOSUSPEND_STACKSIZE,
                       rp23xx_pm_autosuspend_thread, NULL);
  if (ret < 0)
    {
      pwrerr("ERROR: cannot start the automatic suspend thread: %d\n", ret);
    }
}
#endif /* CONFIG_RP23XX_PM_AUTOSUSPEND */

#endif /* CONFIG_RP23XX_PM_SUSPEND */
