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

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Wake-up channel numbers.  POWMAN provides four independent PWRUP slots,
 * each of which can watch one GPIO.
 */

#define RP23XX_PM_PWRUP_NCHANNELS  4

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
 *   itself is halted.  Nothing in the switched core is clocked, so only a
 *   configured wake source can restart execution:
 *
 *     - a GPIO edge or level registered through a POWMAN PWRUP channel, or
 *     - the always-on timer alarm, which is clocked from the low-power
 *       oscillator and therefore survives dormancy.
 *
 *   The core resumes at the instruction after the dormant request with all
 *   RAM and register state intact; this function then restarts the PLLs and
 *   restores the clock tree before returning.
 *
 *   Called with interrupts disabled.
 *
 *   WARNING: with no wake source configured the chip can only be recovered
 *   by a reset.  rp23xx_pm_sleep() therefore refuses to enter dormancy
 *   unless at least one wake source is armed, falling back to standby.
 *
 ****************************************************************************/

void rp23xx_pm_sleep(void);

/****************************************************************************
 * Name: rp23xx_pm_gpio_wakeup
 *
 * Description:
 *   Configure one of the four POWMAN power-up channels to watch a GPIO.
 *
 * Input Parameters:
 *   channel - Power-up channel, 0 to RP23XX_PM_PWRUP_NCHANNELS - 1.
 *   gpio    - GPIO number to watch.
 *   edge    - True to trigger on a transition, false on a level.
 *   high    - True for rising edge / high level, false for falling / low.
 *
 * Returned Value:
 *   Zero on success; a negated errno on failure.
 *
 ****************************************************************************/

int rp23xx_pm_gpio_wakeup(int channel, int gpio, bool edge, bool high);

/****************************************************************************
 * Name: rp23xx_pm_gpio_wakeup_disable
 *
 * Description:
 *   Release a power-up channel previously configured with
 *   rp23xx_pm_gpio_wakeup().
 *
 ****************************************************************************/

int rp23xx_pm_gpio_wakeup_disable(int channel);

#undef EXTERN
#if defined(__cplusplus)
}
#endif
#endif /* __ASSEMBLY__ */
#endif /* CONFIG_RP23XX_PM */
#endif /* __ARCH_ARM_SRC_RP23XX_RP23XX_PM_H */
