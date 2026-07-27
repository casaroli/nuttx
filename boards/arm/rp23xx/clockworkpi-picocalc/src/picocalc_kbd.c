/****************************************************************************
 * boards/arm/rp23xx/clockworkpi-picocalc/src/picocalc_kbd.c
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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/ascii.h>
#include <nuttx/clock.h>
#include <nuttx/wqueue.h>
#include <nuttx/kmalloc.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/keyboard.h>

#include <arch/board/board.h>

#include "rp23xx_i2c.h"
#include "rp23xx_pico.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The co-processor has no interrupt line to the Pico.  Only four signals
 * cross -- the I2C pair and the unused USART3 pair -- so the only way to
 * learn about a keypress under the legacy protocol is to poll.  20ms matches
 * the co-processor's own 16ms KEY_POLL_TIME closely enough to feel instant
 * while leaving the bus almost entirely idle.
 *
 * Polling is also mandatory rather than merely convenient: if neither the
 * receive nor the request callback runs for 2500ms, the co-processor tears
 * down and re-initialises its I2C slave.  Anything under ~2s prevents that.
 */

#define PICOCALC_KBD_POLL_TICKS   MSEC2TICK(20)

/* Bound the work done in a single poll so a wedged co-processor cannot spin
 * the low-priority work queue.  The hardware FIFO is 31 deep; draining eight
 * per tick empties a full FIFO in under 80ms.
 */

#define PICOCALC_KBD_MAX_DRAIN    8

/* The stock BIOS v1.6 driver clocks this bus at 10kHz, but that is a
 * workaround for the Arduino firmware's slave implementation.  The Rust
 * co-processor firmware has been verified at 400kHz.
 */

#define PICOCALC_KBD_FREQUENCY    400000

/* Legacy (0x1F) register ids.  Only these two are ever touched: REG_ID_RST
 * and REG_ID_OFF reboot and shut down the co-processor respectively, so this
 * driver must never walk the register space.
 */

#define PICOCALC_KBD_REG_KEY      0x04  /* [count | caps<<5 | num<<6, 0x00] */
#define PICOCALC_KBD_REG_FIF      0x09  /* [state, keycode]                 */

#define PICOCALC_KBD_COUNT_MASK   0x1f  /* FIFO depth is 31, so five bits   */

/* FIFO event states */

#define PICOCALC_KBD_STATE_IDLE     0
#define PICOCALC_KBD_STATE_PRESSED  1
#define PICOCALC_KBD_STATE_HOLD     2
#define PICOCALC_KBD_STATE_RELEASED 3

/* Co-processor key codes.  Printable ASCII passes through unchanged; these
 * are the ones that do not.
 */

#define PICOCALC_KBD_CODE_ESC     0xb1
#define PICOCALC_KBD_CODE_LEFT    0xb4
#define PICOCALC_KBD_CODE_UP      0xb5
#define PICOCALC_KBD_CODE_DOWN    0xb6
#define PICOCALC_KBD_CODE_RIGHT   0xb7

#define PICOCALC_KBD_CODE_MOD_MIN 0xa1  /* Alt                              */
#define PICOCALC_KBD_CODE_CTRL    0xa5
#define PICOCALC_KBD_CODE_MOD_MAX 0xa5  /* Ctrl                             */

/* The convention apps/examples/lvglterm expects, shared with the M5
 * Cardputer: printable ASCII verbatim, arrows at 0x80-0x83.
 */

#define PICOCALC_KBD_KEY_UP       0x80
#define PICOCALC_KBD_KEY_DOWN     0x81
#define PICOCALC_KBD_KEY_LEFT     0x82
#define PICOCALC_KBD_KEY_RIGHT    0x83

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct picocalc_kbd_dev_s
{
  struct keyboard_lowerhalf_s lower;
  FAR struct i2c_master_s *i2c;
  struct work_s work;
  bool ctrl;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct picocalc_kbd_dev_s g_picocalc_kbd;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: picocalc_kbd_read
 *
 * Description:
 *   Run one complete legacy transaction: select a register, then read the
 *   two byte reply.
 *
 *   Both phases are separate transfers because the protocol requires a STOP
 *   between them -- the co-processor assembles its reply inside the receive
 *   callback, so a repeated START would read whatever the previous
 *   transaction left behind.  Phase one is mandatory even for a pure read.
 *
 ****************************************************************************/

static int picocalc_kbd_read(FAR struct picocalc_kbd_dev_s *priv,
                             uint8_t reg, FAR uint8_t *reply)
{
  struct i2c_msg_s msg;
  int ret;

  msg.frequency = PICOCALC_KBD_FREQUENCY;
  msg.addr      = PICOCALC_KBD_I2C_ADDR;
  msg.flags     = 0;
  msg.buffer    = &reg;
  msg.length    = 1;

  ret = I2C_TRANSFER(priv->i2c, &msg, 1);
  if (ret < 0)
    {
      return ret;
    }

  msg.frequency = PICOCALC_KBD_FREQUENCY;
  msg.addr      = PICOCALC_KBD_I2C_ADDR;
  msg.flags     = I2C_M_READ;
  msg.buffer    = reply;
  msg.length    = 2;

  return I2C_TRANSFER(priv->i2c, &msg, 1);
}

/****************************************************************************
 * Name: picocalc_kbd_translate
 *
 * Description:
 *   Map a co-processor key code onto the code the upper half reports.
 *
 *   The co-processor deliberately does not synthesise control codes: with
 *   CFG_USE_MODS and CFG_REPORT_MODS set it reports Ctrl as its own event
 *   and still delivers the plain character, so Ctrl-C arrives as 0xA5
 *   followed by 'c'.  Folding that into 0x01-0x1A is the client's job, and
 *   this is where it happens.
 *
 ****************************************************************************/

static uint32_t picocalc_kbd_translate(FAR struct picocalc_kbd_dev_s *priv,
                                       uint8_t code)
{
  switch (code)
    {
      case PICOCALC_KBD_CODE_UP:
        return PICOCALC_KBD_KEY_UP;

      case PICOCALC_KBD_CODE_DOWN:
        return PICOCALC_KBD_KEY_DOWN;

      case PICOCALC_KBD_CODE_LEFT:
        return PICOCALC_KBD_KEY_LEFT;

      case PICOCALC_KBD_CODE_RIGHT:
        return PICOCALC_KBD_KEY_RIGHT;

      case PICOCALC_KBD_CODE_ESC:
        return ASCII_ESC;

      default:
        break;
    }

  if (priv->ctrl)
    {
      if (code >= 'a' && code <= 'z')
        {
          return code - 'a' + 1;
        }

      if (code >= 'A' && code <= 'Z')
        {
          return code - 'A' + 1;
        }
    }

  return code;
}

/****************************************************************************
 * Name: picocalc_kbd_event
 *
 * Description:
 *   Turn one FIFO entry into at most one upper half event.
 *
 ****************************************************************************/

static void picocalc_kbd_event(FAR struct picocalc_kbd_dev_s *priv,
                               uint8_t state, uint8_t code)
{
  uint32_t type;

  /* Track Ctrl and swallow every modifier.  Reporting 0xA1-0xA5 verbatim
   * would hand consumers codes that collide with nothing meaningful and
   * that lvglterm would print as garbage.
   */

  if (code >= PICOCALC_KBD_CODE_MOD_MIN && code <= PICOCALC_KBD_CODE_MOD_MAX)
    {
      if (code == PICOCALC_KBD_CODE_CTRL)
        {
          priv->ctrl = (state != PICOCALC_KBD_STATE_RELEASED);
        }

      return;
    }

  /* Auto-repeat is delivered as further pressed events rather than as hold,
   * so hold only ever arrives for keys that do not repeat.  Treat it as a
   * press: a consumer that wants key-repeat semantics gets them, and one
   * that does not is no worse off than with the stock firmware.
   */

  if (state == PICOCALC_KBD_STATE_RELEASED)
    {
      type = KEYBOARD_RELEASE;
    }
  else
    {
      type = KEYBOARD_PRESS;
    }

  keyboard_event(&priv->lower, picocalc_kbd_translate(priv, code), type);
}

/****************************************************************************
 * Name: picocalc_kbd_worker
 *
 * Description:
 *   Poll the co-processor and re-arm.  Errors are not fatal and are not
 *   logged per-tick: the co-processor reinitialises its own I2C slave when
 *   it believes the link has stalled, and a handful of NAKs while that
 *   happens is normal.
 *
 ****************************************************************************/

static void picocalc_kbd_worker(FAR void *arg)
{
  FAR struct picocalc_kbd_dev_s *priv = arg;
  uint8_t reply[2];
  uint8_t pending;
  int i;

  if (picocalc_kbd_read(priv, PICOCALC_KBD_REG_KEY, reply) >= 0)
    {
      pending = reply[0] & PICOCALC_KBD_COUNT_MASK;
      if (pending > PICOCALC_KBD_MAX_DRAIN)
        {
          pending = PICOCALC_KBD_MAX_DRAIN;
        }

      for (i = 0; i < pending; i++)
        {
          if (picocalc_kbd_read(priv, PICOCALC_KBD_REG_FIF, reply) < 0)
            {
              break;
            }

          /* A zero state means the FIFO drained early.  It should not
           * happen when the count says otherwise, but the count and the pop
           * are separate transactions and nothing holds the co-processor
           * still between them.
           */

          if (reply[0] == PICOCALC_KBD_STATE_IDLE)
            {
              break;
            }

          picocalc_kbd_event(priv, reply[0], reply[1]);
        }
    }

  work_queue(LPWORK, &priv->work, picocalc_kbd_worker, priv,
             PICOCALC_KBD_POLL_TICKS);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: picocalc_kbd_initialize
 *
 * Description:
 *   Register the keyboard co-processor as /dev/kbdN and start polling it.
 *
 * Input Parameters:
 *   devno - The device number, used to build the device path.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

int picocalc_kbd_initialize(int devno)
{
  FAR struct picocalc_kbd_dev_s *priv = &g_picocalc_kbd;
  char devpath[16];
  int ret;

  memset(priv, 0, sizeof(*priv));

  priv->i2c = rp23xx_i2cbus_initialize(1);
  if (priv->i2c == NULL)
    {
      ierr("ERROR: Failed to initialize I2C1 for the keyboard\n");
      return -ENODEV;
    }

  snprintf(devpath, sizeof(devpath), "/dev/kbd%d", devno);

  ret = keyboard_register(&priv->lower, devpath,
                          CONFIG_PICOCALC_KBD_BUFFER_SIZE);
  if (ret < 0)
    {
      ierr("ERROR: keyboard_register(%s) failed: %d\n", devpath, ret);
      return ret;
    }

  ret = work_queue(LPWORK, &priv->work, picocalc_kbd_worker, priv,
                   PICOCALC_KBD_POLL_TICKS);
  if (ret < 0)
    {
      ierr("ERROR: Failed to start the keyboard poll: %d\n", ret);
      keyboard_unregister(&priv->lower, devpath);
      return ret;
    }

  return OK;
}
