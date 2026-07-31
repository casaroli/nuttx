/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_pm.h
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

#ifndef __ARCH_ARM_SRC_RP23XX_RP23XX_PM_H
#define __ARCH_ARM_SRC_RP23XX_RP23XX_PM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>

#ifdef CONFIG_RP23XX_PM

#ifndef __ASSEMBLY__

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rp23xx_pm_standby
 *
 * Description:
 *   Enter the RP2350 SLEEP state: peripheral clocks that no configured
 *   driver needs are gated for the duration of a WFI, while the oscillators
 *   and PLLs keep running.  All processor and peripheral state is retained
 *   and any enabled interrupt wakes the core, so this is transparent to
 *   drivers and to the tickless scheduler.
 *
 *   Called with interrupts disabled; returns after the wake event.
 *
 ****************************************************************************/

void rp23xx_pm_standby(void);

/****************************************************************************
 * Name: rp23xx_pm_sleep
 *
 * Description:
 *   Enter the RP2350 DORMANT state: the PLLs are stopped, the system clock
 *   is dropped to the crystal oscillator and then the crystal oscillator
 *   itself is halted.  Nothing in the switched core is clocked, so the only
 *   thing that can restart execution is the dormant-wake detector in the IO
 *   bank, which is asynchronous and so keeps watching with no clock at all.
 *   Arm it with rp23xx_pm_gpio_wakeup().
 *
 *   The always-on timer alarm cannot be used here, and no timed wake-up is
 *   available in this state.  The alarm belongs to the POWMAN power-state
 *   machine, which powers domains down rather than stopping the oscillator,
 *   and it has no way to restart a halted crystal.  A timed deep sleep would
 *   need that machine instead, which does not resume in place.
 *
 *   The core resumes at the instruction after the dormant request with all
 *   RAM and register state intact; this function then restarts the PLLs and
 *   restores the clock tree before returning.
 *
 *   Called with interrupts disabled.
 *
 *   WARNING: with no GPIO armed the chip can only be recovered by a reset.
 *   rp23xx_pm_sleep() therefore refuses to enter dormancy unless at least
 *   one wake GPIO is armed, falling back to standby.
 *
 ****************************************************************************/

void rp23xx_pm_sleep(void);

/****************************************************************************
 * Name: rp23xx_pm_gpio_wakeup
 *
 * Description:
 *   Arm a GPIO in the dormant-wake detector, making it able to end a
 *   PM_SLEEP dormant period.  Any number of GPIOs may be armed.
 *
 *   The detector watches the pad itself, so the pin keeps whatever function
 *   it is already assigned; a console receive pin can be armed without
 *   taking it away from the UART.
 *
 * Input Parameters:
 *   gpio - GPIO number to watch.
 *   edge - True to trigger on a transition, false on a level.
 *   high - True for rising edge / high level, false for falling / low.
 *
 * Returned Value:
 *   Zero on success; a negated errno on failure.
 *
 ****************************************************************************/

int rp23xx_pm_gpio_wakeup(int gpio, bool edge, bool high);

/****************************************************************************
 * Name: rp23xx_pm_gpio_wakeup_disable
 *
 * Description:
 *   Disarm a GPIO previously passed to rp23xx_pm_gpio_wakeup().
 *
 ****************************************************************************/

int rp23xx_pm_gpio_wakeup_disable(int gpio);

/****************************************************************************
 * Name: rp23xx_pm_pads_quiesce
 *
 * Description:
 *   Disable the input buffer of every pad that nothing is using, removing
 *   the static current an unconnected input draws when it floats to a level
 *   the buffer cannot resolve.
 *
 ****************************************************************************/

#ifdef CONFIG_RP23XX_PM_QUIESCE_PADS
void rp23xx_pm_pads_quiesce(void);
#endif

/****************************************************************************
 * Name: rp23xx_pm_suspend
 *
 * Description:
 *   Power the switched core down (POWMAN P1.0) and return once something
 *   wakes it.  Memory is retained, but every peripheral comes back reset,
 *   so the caller must have saved anything it cares about.
 *
 * Input Parameters:
 *   wake_ms - Timed wake in milliseconds, or 0 for none.
 *
 * Returned Value:
 *   Zero once resumed; a negated errno if the request was refused and the
 *   chip never went down.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: g_pm_resume_stack
 *
 * Description:
 *   The stack a resume from suspend to RAM boots on.
 *
 *   A resume cannot use the stack it is handed, because that is the IDLE
 *   thread's stack and the idle thread's frames are live on it: a resume
 *   does not restart the idle thread, it merely schedules it again, and it
 *   returns into exactly those frames.  Sized like the idle stack, since
 *   the work it carries is the same boot sequence.
 *
 ****************************************************************************/

#ifdef CONFIG_RP23XX_PM_SUSPEND
#  define RP23XX_PM_RESUME_STACK_WORDS (CONFIG_IDLETHREAD_STACKSIZE / 4)

EXTERN uint32_t g_pm_resume_stack[RP23XX_PM_RESUME_STACK_WORDS];
#endif

/****************************************************************************
 * Name: rp23xx_pm_resume_pending / rp23xx_pm_resume
 *
 * Description:
 *   The two halves of a resume from suspend to RAM, both called from
 *   __start and from nowhere else.
 *
 *   Suspending powers the switched core off, so there is no returning from
 *   the WFI: the chip comes back through the bootrom and boots as though it
 *   had been reset, except that SRAM still holds a running system.
 *   rp23xx_pm_resume_pending() says whether that is the case, and must be
 *   asked before the bss is cleared or the data section copied, both of
 *   which would destroy exactly the state being resumed.
 *   rp23xx_pm_resume() is then called in place of nx_start(), once the boot
 *   has rebuilt the hardware, and does not return.
 *
 ****************************************************************************/

#ifdef CONFIG_RP23XX_PM_SUSPEND
int rp23xx_pm_suspend(uint32_t wake_ms);
uint32_t rp23xx_pm_wake_source(void);
bool rp23xx_pm_resume_pending(void);
void rp23xx_pm_resume(void);
#endif

#undef EXTERN
#if defined(__cplusplus)
}
#endif
#endif /* __ASSEMBLY__ */
#endif /* CONFIG_RP23XX_PM */
#endif /* __ARCH_ARM_SRC_RP23XX_RP23XX_PM_H */
