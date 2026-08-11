/****************************************************************************
 * include/nuttx/audio/pwm_audio.h
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

#ifndef __INCLUDE_NUTTX_AUDIO_PWM_AUDIO_H
#define __INCLUDE_NUTTX_AUDIO_PWM_AUDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <nuttx/fs/ioctl.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Read how the output is coping.
 *
 * A producer that cannot see this is guessing:  it has no way to tell
 * whether it is comfortably ahead or one late buffer away from a dropout,
 * and no way to report a fault it caused.  The alternative -- attaching a
 * debugger -- halts the CPU, which stops the very thread being measured and
 * manufactures the dropouts it is looking for.
 *
 * The number is high on purpose, to stay clear of the generic audio ioctls
 * as they grow.
 *
 * Argument: a pointer to struct pwm_audio_status_s.
 */

#define AUDIOIOC_PWMAUDIOSTATUS  _AUDIOIOC(64)

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct pwm_audio_status_s
{
  uint32_t samprate;      /* Frames per second, to read the rest in time */
  uint32_t bufframes;     /* Frames in each of the two halves */
  uint32_t buffers;       /* Halves played since the stream started */
  uint32_t underruns;     /* ...of which found nothing ready to follow */
  uint32_t occupancy;     /* Frames still unplayed at the last boundary */
  uint32_t occlow;        /* The lowest that has been, in frames */
  uint32_t refillmax;     /* Longest refill, us, against a half-period */
};

#endif /* __INCLUDE_NUTTX_AUDIO_PWM_AUDIO_H */
