/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_pwm_audio.h
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

#ifndef __ARCH_ARM_SRC_RP23XX_RP23XX_PWM_AUDIO_H
#define __ARCH_ARM_SRC_RP23XX_RP23XX_PWM_AUDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/audio/audio.h>

#ifdef CONFIG_RP23XX_PWM_AUDIO

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
 * Name: rp23xx_pwm_audio_initialize
 *
 * Description:
 *   Create a PCM output device on one PWM slice.
 *
 *   Sound comes out as pulse width: the slice runs far above the audio band
 *   and a DMA channel rewrites its compare register once per sample, paced
 *   by one of the DMA's fractional timers.  What reaches the pin is the
 *   average, which is what the board's RC filter is there to extract.
 *
 *   Both channels of one slice share a counter, so they also share a
 *   compare register -- one 32-bit word holding channel A in the low half
 *   and channel B in the high half.  That is what makes stereo a single
 *   DMA transfer per sample rather than two, and it is why the two pins
 *   have to be the two channels of the *same* slice.
 *
 * Input Parameters:
 *   slice - The PWM slice, which must be the slice both pins belong to
 *   pin_a - GPIO driving channel A, the left channel
 *   pin_b - GPIO driving channel B, the right channel, or -1 for mono
 *
 * Returned Value:
 *   An audio lower half on success; NULL on failure.
 *
 ****************************************************************************/

FAR struct audio_lowerhalf_s *
rp23xx_pwm_audio_initialize(unsigned int slice, int pin_a, int pin_b);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* CONFIG_RP23XX_PWM_AUDIO */
#endif /* __ARCH_ARM_SRC_RP23XX_RP23XX_PWM_AUDIO_H */
