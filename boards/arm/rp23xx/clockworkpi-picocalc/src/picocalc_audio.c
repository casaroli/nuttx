/****************************************************************************
 * boards/arm/rp23xx/clockworkpi-picocalc/src/picocalc_audio.c
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

/* The PicoCalc's audio path is split between the two processors, and neither
 * half is any use alone.
 *
 * The *sound* is the RP2350's: GP26 and GP27 are a PWM pair driving a
 * NC7WZ16 buffer, a two-pole RC filter and an AC coupling stage, into a
 * switch inside the headphone socket that routes it either to the jack or to
 * two AW8010 amplifiers and the speakers.
 *
 * The *switching* is the co-processor's: the amplifier enable is on its PA14
 * and the socket's detect contact is on its PC12.  So this side can generate
 * a perfectly good waveform and be silent, and cannot tell whether anything
 * is plugged in.  Registering the PWM is therefore only half of bringing
 * audio up; the other half is asking the co-processor to follow the jack,
 * which is what picocalc_coproc_audio() is for.
 *
 * GP26 and GP27 are channels A and B of PWM slice 5 -- the RP2350 maps a
 * GPIO's slice as (gpio >> 1) & 7, and 26 and 27 both give 5.  Landing on
 * one slice is what makes stereo possible at all: the two channels share
 * a counter and therefore a frequency.
 *
 * A tone is that PWM run at an audio frequency for a while, which is what
 * apps/examples/pwm already does; `pwm -f 440 -t 1` is a beep.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>

#include "rp23xx_pwmdev.h"
#include "rp23xx_pico.h"
#include "picocalc_audio.h"
#include "picocalc_coproc.h"

#ifdef CONFIG_PWM

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: picocalc_audio_initialize
 ****************************************************************************/

int picocalc_audio_initialize(void)
{
  int ret;

  /* Registers /dev/pwm5 -- the path carries the slice number.
   *
   * Deliberately the plain PWM device rather than drivers/audio/tone.c,
   * which would be the more natural fit.  The tone driver needs a oneshot
   * timer to end a note, and the only oneshot on this chip is built solely
   * under CONFIG_RP23XX_SYSTIMER_TICKLESS.  Moving the whole scheduler to
   * tickless to make the board beep is not a trade worth taking, and
   * nothing about this registration forecloses it later.
   */

  ret = rp23xx_pwmdev_initialize(PICOCALC_AUDIO_PWM_SLICE,
                                 GPIO_PICOCALC_PWM_L,
                                 GPIO_PICOCALC_PWM_R,
                                 0);
  if (ret < 0)
    {
      pwmerr("ERROR: Failed to register the audio PWM: %d\n", ret);
      return ret;
    }

  /* Ask the co-processor to enable the amplifiers when nothing is in the
   * socket, and to drop them when something is.
   *
   * This is done here rather than left to an application because it is the
   * board saying what its own hardware is for.  It is also where the
   * amplifiers first come on and the speakers may make a small noise -- the
   * co-processor boots them off and does not decide this for itself, since
   * it has no idea whether anything is about to be played.
   */

  ret = picocalc_coproc_audio(PICOCALC_AUDIO_AUTO);
  if (ret < 0)
    {
      /* Not fatal.  The PWM works and headphones will still be driven;
       * only the speakers depend on this.
       */

      pwmwarn("WARNING: could not enable the amplifiers: %d\n", ret);
    }

  return OK;
}

#endif /* CONFIG_PWM */
