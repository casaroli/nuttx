/****************************************************************************
 * boards/arm/rp23xx/clockworkpi-picocalc/src/picocalc_audio.h
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

#ifndef __BOARDS_ARM_RP23XX_CLOCKWORKPI_PICOCALC_SRC_PICOCALC_AUDIO_H
#define __BOARDS_ARM_RP23XX_CLOCKWORKPI_PICOCALC_SRC_PICOCALC_AUDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GP26 and GP27 are channels A and B of PWM slice 5: the RP2350 derives a
 * GPIO's slice as (gpio >> 1) & 7, and both give 5.  Sharing a slice is what
 * makes stereo possible, since the two channels share the counter.
 */

#define PICOCALC_AUDIO_PWM_SLICE  5

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: picocalc_audio_initialize
 *
 * Description:
 *   Register /dev/pwm5 on the audio pin pair, and ask the co-processor to
 *   follow the headphone jack.
 *
 *   Call after picocalc_coproc_initialize(): the amplifier enable is on the
 *   far side of that link.
 *
 * Returned Value:
 *   OK, or a negated errno.  A failure leaves the board otherwise
 *   working and simply silent.
 *
 ****************************************************************************/

int picocalc_audio_initialize(void);

#endif /* __BOARDS_ARM_RP23XX_CLOCKWORKPI_PICOCALC_SRC_PICOCALC_AUDIO_H */
