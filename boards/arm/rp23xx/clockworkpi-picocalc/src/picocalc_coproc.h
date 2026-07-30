/****************************************************************************
 * boards/arm/rp23xx/clockworkpi-picocalc/src/picocalc_coproc.h
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

#ifndef __BOARDS_ARM_RP23XX_CLOCKWORKPI_PICOCALC_SRC_PICOCALC_COPROC_H
#define __BOARDS_ARM_RP23XX_CLOCKWORKPI_PICOCALC_SRC_PICOCALC_COPROC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/irq.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The co-processor's two I2C addresses.  The legacy one is what the stock
 * ClockworkPi BIOS serves and what PicoMite, MicroPython and picocalc-nx
 * expect; the second is served only by the replacement firmware, so probing
 * it is the firmware detection.
 */

#define PICOCALC_COPROC_ADDR_LEGACY  0x1f
#define PICOCALC_COPROC_ADDR_V2      0x1e

/* Which backlight picocalc_coproc_backlight() means. */

#define PICOCALC_BACKLIGHT_LCD       0
#define PICOCALC_BACKLIGHT_KBD       1

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: picocalc_coproc_initialize
 *
 * Description:
 *   Bring up I2C1 and work out which firmware the co-processor is running.
 *   Idempotent, and safe to call from anywhere in bringup; the first caller
 *   pays for the probe.
 *
 *   Call this before board_lcd_initialize(), because the panel's backlight
 *   is behind this bus.
 *
 * Returned Value:
 *   OK, or a negated errno if the bus could not be brought up.  A failure
 *   here is not fatal to the board: the keyboard and battery register as
 *   absent and the panel still lights at whatever the co-processor's own
 *   default is.
 *
 ****************************************************************************/

int picocalc_coproc_initialize(void);

/****************************************************************************
 * Name: picocalc_coproc_i2c
 *
 * Description:
 *   The shared bus handle, or NULL if initialization failed.  Shared rather
 *   than one per driver because the NuttX I2C upper half serialises on it,
 *   which is what keeps the keyboard's polling from interleaving with a
 *   battery read.
 *
 ****************************************************************************/

FAR struct i2c_master_s *picocalc_coproc_i2c(void);

/****************************************************************************
 * Name: picocalc_coproc_has_v2
 *
 * Description:
 *   True if the co-processor answers on PICOCALC_COPROC_ADDR_V2 with the
 *   expected identity.  False means stock firmware, and callers should
 *   expect the legacy protocol's limits: quantised backlights that cannot
 *   reach zero, a battery reading with no voltage behind it, and a single
 *   shared reply buffer that two clients must not race for.
 *
 ****************************************************************************/

bool picocalc_coproc_has_v2(void);

/****************************************************************************
 * Name: picocalc_coproc_backlight
 *
 * Description:
 *   Set one of the two backlights.
 *
 * Input Parameters:
 *   which - PICOCALC_BACKLIGHT_LCD or PICOCALC_BACKLIGHT_KBD.
 *   level - 0-255, clamped.  Zero is fully off only on the new protocol;
 *           the legacy registers quantise and their floor is one step.
 *
 * Returned Value:
 *   OK, or a negated errno.
 *
 ****************************************************************************/

int picocalc_coproc_backlight(int which, int level);

/****************************************************************************
 * Name: picocalc_coproc_lcd_backlight
 *
 * Description:
 *   The st7365p_backlight_t hook, scaling CONFIG_LCD_MAXPOWER to the
 *   co-processor's 0-255.  Passed to st7365p_lcdinitialize() so that
 *   LCDDEVIO_SETPOWER on /dev/lcd0 dims the panel.
 *
 ****************************************************************************/

int picocalc_coproc_lcd_backlight(int level);

/****************************************************************************
 * Name: picocalc_coproc_attach_attention
 *
 * Description:
 *   Install a handler for the co-processor's attention line, GPIO 9.
 *
 *   Passed to picocalc_kbd_register() as its picocalc_kbd_attach_t.  That
 *   is open-drain and active low and is driven only by the replacement
 *   firmware, so the driver ignores this if its probe found stock.
 *
 * Returned Value:
 *   OK, or a negated errno.
 *
 ****************************************************************************/

int picocalc_coproc_attach_attention(xcpt_t isr, FAR void *arg);

#endif /* __BOARDS_ARM_RP23XX_CLOCKWORKPI_PICOCALC_SRC_PICOCALC_COPROC_H */
