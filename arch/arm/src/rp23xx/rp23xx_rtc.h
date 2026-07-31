/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_rtc.h
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

#ifndef __ARCH_ARM_SRC_RP23XX_RP23XX_RTC_H
#define __ARCH_ARM_SRC_RP23XX_RP23XX_RTC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>

#ifdef CONFIG_RP23XX_RTC

/****************************************************************************
 * Public Types
 ****************************************************************************/

#ifndef __ASSEMBLY__

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

#ifdef CONFIG_RTC_ALARM

/* The always-on timer carries exactly one alarm comparator, so the RTC
 * driver interface exposes exactly one alarm id.
 */

enum rp23xx_alarm_id_e
{
  RP23XX_ALARM0 = 0,
  RP23XX_ALARM_LAST
};

/* Callback invoked from the POWMAN timer interrupt when the alarm fires. */

typedef CODE void (*rp23xx_alarm_callback_t)(FAR void *arg);

/* A snapshot of the alarm hardware, used to save and restore an application
 * alarm across a driver-internal use of the single POWMAN alarm.
 */

struct rp23xx_alarm_state_s
{
  uint64_t                 time;    /* Absolute alarm time, milliseconds */
  rp23xx_alarm_callback_t  cb;      /* Callback, NULL when none */
  FAR void                *arg;     /* Callback argument */
  bool                     active;  /* True when the alarm was armed */
};

#endif /* CONFIG_RTC_ALARM */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rp23xx_rtc_getms
 *
 * Description:
 *   Read the POWMAN always-on timer, in milliseconds since the epoch.
 *
 ****************************************************************************/

uint64_t rp23xx_rtc_getms(void);

#ifdef CONFIG_RTC_ALARM

/****************************************************************************
 * Name: rp23xx_rtc_setalarm
 *
 * Description:
 *   Arm the POWMAN alarm at an absolute time.  The RP2350 has exactly one
 *   alarm comparator, so this replaces any alarm already armed.
 *
 * Input Parameters:
 *   time - Absolute alarm time in milliseconds since the epoch.
 *   cb   - Called from the interrupt when the alarm fires.  May be NULL,
 *          in which case the alarm only latches its status bit (and wakes
 *          the chip when rp23xx_rtc_alarm_pwrup() has been enabled).
 *   arg  - Argument passed back to the callback.
 *
 * Returned Value:
 *   Zero on success; a negated errno on failure.  -EINVAL is returned when
 *   the requested time is not in the future.
 *
 ****************************************************************************/

int rp23xx_rtc_setalarm(uint64_t time, rp23xx_alarm_callback_t cb,
                        FAR void *arg);

/****************************************************************************
 * Name: rp23xx_rtc_cancelalarm
 *
 * Description:
 *   Disarm the POWMAN alarm.  Safe to call when no alarm is armed.
 *
 ****************************************************************************/

int rp23xx_rtc_cancelalarm(void);

/****************************************************************************
 * Name: rp23xx_rtc_rdalarm
 *
 * Description:
 *   Read back the absolute time of the armed alarm.
 *
 * Returned Value:
 *   Zero on success, -ENODATA when no alarm is armed.
 *
 ****************************************************************************/

int rp23xx_rtc_rdalarm(FAR uint64_t *time);

/****************************************************************************
 * Name: rp23xx_rtc_savealarm
 *
 * Description:
 *   Capture the current alarm so it can be restored later, then leave the
 *   alarm disarmed.  Used by the power management logic, which borrows the
 *   single alarm comparator as a wakeup source.
 *
 ****************************************************************************/

void rp23xx_rtc_savealarm(FAR struct rp23xx_alarm_state_s *state);

/****************************************************************************
 * Name: rp23xx_rtc_restorealarm
 *
 * Description:
 *   Re-arm an alarm captured by rp23xx_rtc_savealarm().  If the saved alarm
 *   time has already passed, its callback is invoked immediately rather
 *   than being lost.
 *
 ****************************************************************************/

void rp23xx_rtc_restorealarm(FAR const struct rp23xx_alarm_state_s *state);

/****************************************************************************
 * Name: rp23xx_rtc_alarm_pwrup
 *
 * Description:
 *   Select whether a firing alarm also requests a chip power-up.  This is
 *   what allows the always-on timer to wake the core from a dormant state,
 *   where its interrupt alone cannot run any code.
 *
 ****************************************************************************/

void rp23xx_rtc_alarm_pwrup(bool enable);

#endif /* CONFIG_RTC_ALARM */

/****************************************************************************
 * Name: rp23xx_rtc_lowerhalf
 *
 * Description:
 *   Instantiate the RTC lower half driver for the POWMAN always-on timer.
 *   General usage:
 *
 *     #include <nuttx/timers/rtc.h>
 *     #include "rp23xx_rtc.h"
 *
 *     struct rtc_lowerhalf_s *lower;
 *     lower = rp23xx_rtc_lowerhalf();
 *     rtc_initialize(0, lower);
 *
 * Returned Value:
 *   On success, a non-NULL RTC lower interface is returned.  NULL is
 *   returned on any failure.
 *
 ****************************************************************************/

#ifdef CONFIG_RTC_DRIVER
FAR struct rtc_lowerhalf_s *rp23xx_rtc_lowerhalf(void);
#endif

#undef EXTERN
#if defined(__cplusplus)
}
#endif
#endif /* __ASSEMBLY__ */
#endif /* CONFIG_RP23XX_RTC */
#endif /* __ARCH_ARM_SRC_RP23XX_RP23XX_RTC_H */
