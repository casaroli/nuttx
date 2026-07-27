/****************************************************************************
 * boards/arm/rp23xx/clockworkpi-picocalc/src/rp23xx_pinpark.c
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
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <arch/board/board.h>

#include "arm_internal.h"
#include "rp23xx_gpio.h"
#include "hardware/rp23xx_pads_bank0.h"

#include "rp23xx_pico.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* RP2350-E9.
 *
 * On A2 silicon a Bank 0 pad left as a floating input leaks roughly 120uA
 * and settles near 2.2V -- the internal pull-down is around 50-80k and
 * cannot win against it.  On an RP2350B with 48 pads that is milliamps,
 * which is more than the entire deep-sleep budget for this design, so it has
 * to be dealt with at bring-up rather than left for the power work.
 *
 * Only two mitigations actually work: drive the pad as an output, or clear
 * the pad's input enable so the leaky input buffer is disconnected.  Merely
 * enabling the internal pull-down does not.
 *
 * Pins that already carry an external pull, or that a driver will claim
 * later, are left alone.  In particular GP45 (BOOT/USER_SW) keeps its input
 * enabled and its pull-up, and GP47 (QSPI PSRAM CS), GP23/24/25/29
 * (CYW43439) are owned by their own drivers.
 */

/* Driven low as outputs.  These are the mainboard's ESP-PSRAM64H data and
 * clock lines.  That part is redundant here -- the Pimoroni module carries
 * its own memory-mapped QSPI PSRAM on GP47 -- and driving the bus low is
 * safe because its chip select (GP20) is held high by R302.
 */

static const uint8_t g_park_drive_low[] =
{
  GPIO_PICOCALC_MBRAM_SIO0,     /* GP2  */
  GPIO_PICOCALC_MBRAM_SIO1,     /* GP3  */
  GPIO_PICOCALC_MBRAM_SIO2,     /* GP4  */
  GPIO_PICOCALC_MBRAM_SIO3,     /* GP5  */
  GPIO_PICOCALC_MBRAM_SCK,      /* GP21 */
};

/* Input buffer disabled.  Either an external pull already defines the level,
 * or the pin is not routed anywhere on this module.
 */

static const uint8_t g_park_clear_ie[] =
{
  GPIO_PICOCALC_MBRAM_CS,       /* GP20: R302 10k pull-up, leave it high  */

  /* GP30-GP39: brought out on the RP2350B package but not connected on the
   * Pimoroni Pico Plus 2 W.
   */

  30, 31, 32, 33, 34, 35, 36, 37, 38, 39,

  /* GP40-GP43: the module's A0-A2 and the VSYS voltage monitor.  ADC use
   * wants the digital input buffer off anyway.
   */

  40, 41, 42, 43,

  /* GP44, GP46: not connected on the module. */

  44, 46,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: picocalc_pin_park
 *
 * Description:
 *   Put every GPIO that nothing owns into a state that does not leak, as a
 *   mitigation for erratum RP2350-E9.  Must run before any driver claims a
 *   pin, i.e. from rp23xx_boardearlyinitialize().
 *
 *   Note that rp23xx_common_earlyinitialize() has already cleared IE on
 *   GP26-GP29.  GP26/GP27 are the PicoCalc's PWM audio outputs and will need
 *   their input enable restored if and when an audio driver claims them.
 *
 ****************************************************************************/

void picocalc_pin_park(void)
{
  uint32_t i;

  for (i = 0; i < sizeof(g_park_drive_low); i++)
    {
      uint32_t gpio = g_park_drive_low[i];

      rp23xx_gpio_init(gpio);
      rp23xx_gpio_setdir(gpio, true);
      rp23xx_gpio_put(gpio, false);
    }

  for (i = 0; i < sizeof(g_park_clear_ie); i++)
    {
      clrbits_reg32(RP23XX_PADS_BANK0_GPIO_IE,
                    RP23XX_PADS_BANK0_GPIO(g_park_clear_ie[i]));
    }
}
