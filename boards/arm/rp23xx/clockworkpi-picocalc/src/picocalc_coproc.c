/****************************************************************************
 * boards/arm/rp23xx/clockworkpi-picocalc/src/picocalc_coproc.c
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

/* The STM32F103 co-processor owns rather more of this board than its name
 * suggests: the keyboard, both backlights, the indicator LED, the speaker
 * enable, and the AXP2101 PMU behind it.  Everything here reaches it over
 * I2C1, and this file exists so that the several drivers that need it share
 * one bus handle and one answer to "which firmware is this".
 *
 * That question has two answers.  The stock ClockworkPi BIOS serves only the
 * legacy protocol at 0x1F, where the backlight is quantised to sixteen steps
 * and cannot be turned fully off.  The replacement firmware also serves a
 * second address, 0x1E, where it is linear 0-255 and zero really is off.
 * Probing 0x1E once at bringup and remembering the answer is what lets the
 * same image drive both.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/i2c/i2c_master.h>

#include "rp23xx_i2c.h"
#include "picocalc_coproc.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Bus speed.  The stock BIOS driver uses 10kHz, but that is a workaround for
 * the Arduino firmware's slave implementation; the replacement firmware is
 * verified at 400kHz over two million transactions.  Nothing on this bus is
 * hurt by the higher rate if the stock firmware is present -- it simply
 * NAKs 0x1E and the legacy path is used, which is timing-tolerant.
 */

#define COPROC_FREQUENCY   400000

/* Legacy backlight registers.  Both quantise: the value read back is never
 * the value written, and neither reaches zero.
 */

#define LEGACY_REG_BKL     0x05   /* LCD backlight, 16 steps            */
#define LEGACY_REG_BK2     0x0a   /* Keyboard backlight, 8 steps        */
#define LEGACY_WRITE_MASK  0x80

/* New protocol registers. */

#define V2_REG_WHOAMI      0x00
#define V2_REG_CAPS        0x02
#define V2_REG_BL_LCD      0x10
#define V2_REG_BL_KEY      0x11
#define V2_WRITE_MASK      0x80

/* CAPS bit 11: the backlights are linear rather than quantised. */

#define V2_CAPS_BL_LINEAR  (1 << 11)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR struct i2c_master_s *g_coproc_i2c = NULL;
static bool g_coproc_v2 = false;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: coproc_read
 *
 * Description:
 *   Register select and read, in one transfer so that a repeated START is
 *   emitted between them.
 *
 *   This framing is required on 0x1E and merely allowed on 0x1F, and using
 *   it everywhere is deliberate.  The legacy protocol keeps a single global
 *   reply buffer, so a select and a read in two transfers can be interleaved
 *   by another client on the bus -- the keyboard driver polls continuously
 *   -- and the reader gets somebody else's answer.  One transfer holds the
 *   NuttX bus lock across both phases, which closes that window.
 *
 ****************************************************************************/

static int coproc_read(uint8_t addr, uint8_t reg,
                       FAR uint8_t *buf, size_t len)
{
  struct i2c_msg_s msg[2];

  msg[0].frequency = COPROC_FREQUENCY;
  msg[0].addr      = addr;
  msg[0].flags     = 0;
  msg[0].buffer    = &reg;
  msg[0].length    = 1;

  msg[1].frequency = COPROC_FREQUENCY;
  msg[1].addr      = addr;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = buf;
  msg[1].length    = len;

  return I2C_TRANSFER(g_coproc_i2c, msg, 2);
}

/****************************************************************************
 * Name: coproc_write
 ****************************************************************************/

static int coproc_write(uint8_t addr, uint8_t reg, uint8_t value)
{
  struct i2c_msg_s msg;
  uint8_t buf[2];

  buf[0] = reg;
  buf[1] = value;

  msg.frequency = COPROC_FREQUENCY;
  msg.addr      = addr;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = 2;

  return I2C_TRANSFER(g_coproc_i2c, &msg, 1);
}

/****************************************************************************
 * Name: coproc_probe_v2
 *
 * Description:
 *   Decide once whether the co-processor speaks the new protocol.
 *
 *   Two things are checked, and both matter.  A NAK of 0x1E means stock
 *   firmware.  An answer whose WHO_AM_I is not "PKB" means something else
 *   entirely is at that address and must not be written to.  Only then is
 *   the capability bitmap consulted -- and it is consulted rather than
 *   assumed, because it is the firmware's own statement of what works, and a
 *   build with the backlight left quantised would say so there.
 *
 ****************************************************************************/

static bool coproc_probe_v2(void)
{
  uint8_t who[4];
  uint8_t caps[4];
  uint32_t bits;

  if (coproc_read(PICOCALC_COPROC_ADDR_V2, V2_REG_WHOAMI,
                  who, sizeof(who)) < 0)
    {
      iinfo("0x1e did not answer: stock firmware\n");
      return false;
    }

  if (who[0] != 'P' || who[1] != 'K' || who[2] != 'B')
    {
      iwarn("WARNING: 0x1e answered %02x %02x %02x, not PKB\n",
            who[0], who[1], who[2]);
      return false;
    }

  if (coproc_read(PICOCALC_COPROC_ADDR_V2, V2_REG_CAPS,
                  caps, sizeof(caps)) < 0)
    {
      return false;
    }

  bits = (uint32_t)caps[0] | ((uint32_t)caps[1] << 8) |
         ((uint32_t)caps[2] << 16) | ((uint32_t)caps[3] << 24);

  iinfo("co-processor protocol %d, caps %08" PRIx32 "\n", who[3], bits);

  return (bits & V2_CAPS_BL_LINEAR) != 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: picocalc_coproc_initialize
 ****************************************************************************/

int picocalc_coproc_initialize(void)
{
  if (g_coproc_i2c != NULL)
    {
      return OK;
    }

  g_coproc_i2c = rp23xx_i2cbus_initialize(1);
  if (g_coproc_i2c == NULL)
    {
      ierr("ERROR: Failed to initialize I2C1\n");
      return -ENODEV;
    }

  g_coproc_v2 = coproc_probe_v2();

  return OK;
}

/****************************************************************************
 * Name: picocalc_coproc_i2c
 ****************************************************************************/

FAR struct i2c_master_s *picocalc_coproc_i2c(void)
{
  return g_coproc_i2c;
}

/****************************************************************************
 * Name: picocalc_coproc_has_v2
 ****************************************************************************/

bool picocalc_coproc_has_v2(void)
{
  return g_coproc_v2;
}

/****************************************************************************
 * Name: picocalc_coproc_backlight
 *
 * Description:
 *   Set a backlight, 0..255.  See picocalc_coproc.h.
 *
 ****************************************************************************/

int picocalc_coproc_backlight(int which, int level)
{
  if (g_coproc_i2c == NULL)
    {
      return -ENODEV;
    }

  if (level < 0)
    {
      level = 0;
    }
  else if (level > 255)
    {
      level = 255;
    }

  if (g_coproc_v2)
    {
      uint8_t reg = (which == PICOCALC_BACKLIGHT_KBD) ? V2_REG_BL_KEY
                                                      : V2_REG_BL_LCD;

      return coproc_write(PICOCALC_COPROC_ADDR_V2, reg | V2_WRITE_MASK,
                          (uint8_t)level);
    }

  /* Legacy.  The register takes the same 0-255 range but quantises it, and
   * the floor is one step rather than off -- writing 0 to the stock firmware
   * leaves the panel at its dimmest, not dark.  Nothing here compensates for
   * that: pretending the value took would make the read-back a lie, and a
   * caller that needs to know can ask picocalc_coproc_has_v2().
   */

    {
      uint8_t reg = (which == PICOCALC_BACKLIGHT_KBD) ? LEGACY_REG_BK2
                                                      : LEGACY_REG_BKL;

      return coproc_write(PICOCALC_COPROC_ADDR_LEGACY,
                          reg | LEGACY_WRITE_MASK, (uint8_t)level);
    }
}

/****************************************************************************
 * Name: picocalc_coproc_lcd_backlight
 *
 * Description:
 *   The st7365p_backlight_t hook.  CONFIG_LCD_MAXPOWER is the panel's power
 *   range and the co-processor's is always 0-255, so the two are scaled
 *   rather than assumed equal -- a board that sets MAXPOWER to 1 still gets
 *   a backlight that goes fully on and fully off.
 *
 ****************************************************************************/

int picocalc_coproc_lcd_backlight(int level)
{
  int scaled;

  if (level <= 0)
    {
      scaled = 0;
    }
  else if (level >= CONFIG_LCD_MAXPOWER)
    {
      scaled = 255;
    }
  else
    {
      scaled = (level * 255) / CONFIG_LCD_MAXPOWER;
    }

  return picocalc_coproc_backlight(PICOCALC_BACKLIGHT_LCD, scaled);
}
