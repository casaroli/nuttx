/****************************************************************************
 * drivers/input/picocalc_kbd.c
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
#include <nuttx/input/kbd_codec.h>
#include <nuttx/input/keyboard.h>
#include <nuttx/input/picocalc_kbd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Legacy poll period.
 *
 * Under the legacy protocol the only way to learn about a keypress is to
 * poll.  20ms matches the co-processor's own 16ms KEY_POLL_TIME closely
 * enough to feel instant while leaving the bus almost entirely idle.
 *
 * Polling is also mandatory rather than merely convenient there: if neither
 * the receive nor the request callback runs for 2500ms, the stock firmware
 * tears down and re-initialises its I2C slave.  Anything under ~2s prevents
 * that.
 */

#define PICOCALC_KBD_POLL_TICKS   MSEC2TICK(20)

/* Safety poll period for the extended protocol.
 *
 * With an attention line the driver reads when told to, so this is not how
 * keypresses are noticed -- it is what recovers if an edge is ever missed.
 * The line is a *level*, asserted while events are queued, so a lost
 * edge costs one period of latency rather than a lost keystroke, and nothing
 * is dropped in the meantime.
 *
 * Without an attention line this is the poll period, and 100ms would be too
 * slow to type on; see picocalc_kbd_arm(), which uses the legacy period in
 * that case.
 */

#define PICOCALC_KBD_IDLE_TICKS   MSEC2TICK(100)

/* Bound the work done in a single poll so a wedged co-processor cannot spin
 * the low-priority work queue.  The hardware FIFO is 31 deep; draining eight
 * per tick empties a full FIFO in under 80ms.
 */

#define PICOCALC_KBD_MAX_DRAIN    8

/* Legacy (0x1F) register ids.  Only these two are ever touched: REG_ID_RST
 * and REG_ID_OFF reboot and shut down the co-processor respectively, so this
 * driver must never walk the register space.
 */

#define PICOCALC_KBD_REG_KEY      0x04  /* [count | caps<<5 | num<<6, 0x00] */
#define PICOCALC_KBD_REG_FIF      0x09  /* [state, keycode]                 */

#define PICOCALC_KBD_COUNT_MASK   0x1f  /* FIFO depth is 31, so five bits   */

/* Extended (0x1E) protocol.
 *
 * A bare read -- no register byte at all -- returns the "hot block": four
 * status bytes then up to eight events.  That is the whole point of it: one
 * transaction where the legacy protocol needs two per event.
 */

#define PICOCALC_KBD_V2_WHOAMI    0x00  /* "PKB" then the generation        */
#define PICOCALC_KBD_V2_CAPS      0x02  /* u32 LE capability bitmap         */

#define PICOCALC_KBD_CAPS_BURST   (1 << 1)  /* Burst key reads supported    */

#define PICOCALC_KBD_BURST_MAX    8
#define PICOCALC_KBD_HOT_LEN      (4 + 2 * PICOCALC_KBD_BURST_MAX)

/* Byte 0 of the hot block: queue depth in the low six bits. */

#define PICOCALC_KBD_S0_COUNT     0x3f

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

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct picocalc_kbd_dev_s
{
  struct keyboard_lowerhalf_s lower;
  FAR struct i2c_master_s *i2c;
  struct work_s work;
  uint32_t frequency;
  uint8_t addr;
  bool ctrl;
  bool v2;                    /* Extended protocol available              */
  bool attention;             /* ...and an attention line is driving us    */

  /* Reads that line, may be NULL.  See picocalc_kbd_asserted_t. */

  picocalc_kbd_asserted_t asserted;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void picocalc_kbd_worker(FAR void *arg);

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

  msg.frequency = priv->frequency;
  msg.addr      = priv->addr;
  msg.flags     = 0;
  msg.buffer    = &reg;
  msg.length    = 1;

  ret = I2C_TRANSFER(priv->i2c, &msg, 1);
  if (ret < 0)
    {
      return ret;
    }

  msg.frequency = priv->frequency;
  msg.addr      = priv->addr;
  msg.flags     = I2C_M_READ;
  msg.buffer    = reply;
  msg.length    = 2;

  return I2C_TRANSFER(priv->i2c, &msg, 1);
}

/****************************************************************************
 * Name: picocalc_kbd_v2_read
 *
 * Description:
 *   Read a register of the extended protocol: select and read in one
 *   transfer, so a repeated START is emitted between them.
 *
 *   That framing is required rather than preferred.  On this address a read
 *   with no selection in front of it is not an error, it is the hot block --
 *   so a select that ends in a STOP would be answered with events instead of
 *   the register that was asked for.
 *
 ****************************************************************************/

static int picocalc_kbd_v2_read(FAR struct picocalc_kbd_dev_s *priv,
                                uint8_t reg, FAR uint8_t *buf, size_t len)
{
  struct i2c_msg_s msg[2];

  msg[0].frequency = priv->frequency;
  msg[0].addr      = PICOCALC_KBD_ADDR_V2;
  msg[0].flags     = 0;
  msg[0].buffer    = &reg;
  msg[0].length    = 1;

  msg[1].frequency = priv->frequency;
  msg[1].addr      = PICOCALC_KBD_ADDR_V2;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = buf;
  msg[1].length    = len;

  return I2C_TRANSFER(priv->i2c, msg, 2);
}

/****************************************************************************
 * Name: picocalc_kbd_hotread
 *
 * Description:
 *   Read the hot block: one bare read, no register byte, returning the queue
 *   status and up to eight events.
 *
 *   The co-processor pops the events it hands out only when the master has
 *   actually clocked them, so a transfer that fails part way loses nothing.
 *
 ****************************************************************************/

static int picocalc_kbd_hotread(FAR struct picocalc_kbd_dev_s *priv,
                                FAR uint8_t *block)
{
  struct i2c_msg_s msg;

  msg.frequency = priv->frequency;
  msg.addr      = PICOCALC_KBD_ADDR_V2;
  msg.flags     = I2C_M_READ;
  msg.buffer    = block;
  msg.length    = PICOCALC_KBD_HOT_LEN;

  return I2C_TRANSFER(priv->i2c, &msg, 1);
}

/****************************************************************************
 * Name: picocalc_kbd_probe
 *
 * Description:
 *   Decide once whether the co-processor speaks the extended protocol.
 *
 *   Three things are checked, each ruling out a different mistake: that the
 *   address answers at all, which a stock co-processor does not; that its
 *   identity is "PKB" rather than some unrelated device sharing 0x1e, which
 *   must not be read as an event queue; and that the capability bitmap says
 *   burst reads, since it is the firmware's own statement of what works.
 *
 ****************************************************************************/

static bool picocalc_kbd_probe(FAR struct picocalc_kbd_dev_s *priv)
{
  uint8_t who[4];
  uint8_t caps[4];
  uint32_t bits;

  if (picocalc_kbd_v2_read(priv, PICOCALC_KBD_V2_WHOAMI,
                           who, sizeof(who)) < 0)
    {
      return false;
    }

  if (who[0] != 'P' || who[1] != 'K' || who[2] != 'B')
    {
      iwarn("WARNING: 0x1e is not the co-processor\n");
      return false;
    }

  if (picocalc_kbd_v2_read(priv, PICOCALC_KBD_V2_CAPS,
                           caps, sizeof(caps)) < 0)
    {
      return false;
    }

  bits = (uint32_t)caps[0] | ((uint32_t)caps[1] << 8) |
         ((uint32_t)caps[2] << 16) | ((uint32_t)caps[3] << 24);

  return (bits & PICOCALC_KBD_CAPS_BURST) != 0;
}

/****************************************************************************
 * Name: picocalc_kbd_translate
 *
 * Description:
 *   Map a co-processor key code onto the code the upper half reports, and
 *   say whether that code is a keycode rather than a character.
 *
 *   The arrow keys produce no character, so they are reported as special
 *   keys carrying a value from enum kbd_keycode_e and the caller turns
 *   *special into a SPEC event type.  Escape is not one of them:  it is
 *   the control character 0x1B and belongs in the ordinary stream.
 *
 *   Whether a key is special depends only on the code the co-processor
 *   sends, so a release translates the same way its press did and no state
 *   has to be carried between the two.
 *
 *   The co-processor deliberately does not synthesise control codes: with
 *   CFG_USE_MODS and CFG_REPORT_MODS set it reports Ctrl as its own event
 *   and still delivers the plain character, so Ctrl-C arrives as 0xA5
 *   followed by 'c'.  Folding that into 0x01-0x1A is the client's job, and
 *   this is where it happens.
 *
 ****************************************************************************/

static uint32_t picocalc_kbd_translate(FAR struct picocalc_kbd_dev_s *priv,
                                       uint8_t code, FAR bool *special)
{
  *special = true;

  switch (code)
    {
      case PICOCALC_KBD_CODE_UP:
        return KEYCODE_UP;

      case PICOCALC_KBD_CODE_DOWN:
        return KEYCODE_DOWN;

      case PICOCALC_KBD_CODE_LEFT:
        return KEYCODE_LEFT;

      case PICOCALC_KBD_CODE_RIGHT:
        return KEYCODE_RIGHT;

      default:
        break;
    }

  *special = false;

  if (code == PICOCALC_KBD_CODE_ESC)
    {
      return ASCII_ESC;
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
  uint32_t keycode;
  uint32_t type;
  bool special;

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

  keycode = picocalc_kbd_translate(priv, code, &special);

  if (state == PICOCALC_KBD_STATE_RELEASED)
    {
      type = special ? KEYBOARD_SPECREL : KEYBOARD_RELEASE;
    }
  else
    {
      type = special ? KEYBOARD_SPECPRESS : KEYBOARD_PRESS;
    }

  keyboard_event(&priv->lower, keycode, type);
}

/****************************************************************************
 * Name: picocalc_kbd_arm
 *
 * Description:
 *   Schedule the next read.
 *
 *   `delay` of zero means "there is known to be more waiting"; otherwise the
 *   period depends on what is actually noticing keypresses.  With an
 *   attention line that is the line, and this is only the recovery path for
 *   a missed edge; without one it is the poll and must be fast to type
 *   against.
 *
 ****************************************************************************/

static void picocalc_kbd_arm(FAR struct picocalc_kbd_dev_s *priv,
                             bool immediate)
{
  clock_t delay;

  if (immediate)
    {
      delay = 0;
    }
  else if (priv->attention)
    {
      delay = PICOCALC_KBD_IDLE_TICKS;
    }
  else
    {
      delay = PICOCALC_KBD_POLL_TICKS;
    }

  work_queue(LPWORK, &priv->work, picocalc_kbd_worker, priv, delay);
}

/****************************************************************************
 * Name: picocalc_kbd_isr
 *
 * Description:
 *   The attention line went low: the co-processor has events.
 *
 *   Nothing is read here.  An I2C transfer takes hundreds of microseconds
 *   can block on the bus lock, neither of which belongs in an interrupt, so
 *   this only brings the read forward.
 *
 *   The line is a level rather than a pulse, and deliberately so: an event
 *   arriving while the block is being read leaves it asserted, so the next
 *   read is scheduled by the queue depth below rather than by an edge that
 *   would have had to arrive at exactly the right moment.
 *
 ****************************************************************************/

static int picocalc_kbd_isr(int irq, FAR void *context, FAR void *arg)
{
  FAR struct picocalc_kbd_dev_s *priv = arg;

  work_queue(LPWORK, &priv->work, picocalc_kbd_worker, priv, 0);
  return OK;
}

/****************************************************************************
 * Name: picocalc_kbd_hot
 *
 * Description:
 *   One hot block: up to eight events in a single transaction.
 *
 *   Returns true if the co-processor still has events after this read, which
 *   is the difference between the queue depth it reported and the number it
 *   handed over.  Both come from the same block, so they describe the same
 *   instant and cannot disagree.
 *
 ****************************************************************************/

static bool picocalc_kbd_hot(FAR struct picocalc_kbd_dev_s *priv)
{
  uint8_t block[PICOCALC_KBD_HOT_LEN];
  uint8_t queued;
  uint8_t count;
  int i;

  if (picocalc_kbd_hotread(priv, block) < 0)
    {
      return false;
    }

  queued = block[0] & PICOCALC_KBD_S0_COUNT;
  count  = block[3];

  if (count > PICOCALC_KBD_BURST_MAX)
    {
      count = PICOCALC_KBD_BURST_MAX;
    }

  for (i = 0; i < count; i++)
    {
      uint8_t state = block[4 + 2 * i];
      uint8_t code  = block[5 + 2 * i];

      /* Unused slots come back as the idle sentinel.  They should not appear
       * below `count`, but the block is fixed width and trusting the count
       * over the contents costs nothing.
       */

      if (state == PICOCALC_KBD_STATE_IDLE)
        {
          break;
        }

      picocalc_kbd_event(priv, state, code);
    }

  if (queued > count)
    {
      return true;
    }

  /* The block said it handed over everything it had -- but it described the
   * queue as it was when it was assembled, and a key pressed since then is
   * not in it and generated no edge either, because the line never went high
   * to fall again.  The line itself is the only thing that knows.
   *
   * Without this test such a key waits for the background read.  Measured on
   * hardware before it was added: a mean of 5.5ms against a floor of 583us,
   * with the worst case past the 65ms the statistic can hold.
   */

  return priv->asserted != NULL && priv->asserted();
}

/****************************************************************************
 * Name: picocalc_kbd_legacy
 *
 * Description:
 *   One legacy poll: a depth read, then a pop per event.  Two transactions
 *   each, which is what the extended protocol exists to avoid.
 *
 ****************************************************************************/

static void picocalc_kbd_legacy(FAR struct picocalc_kbd_dev_s *priv)
{
  uint8_t reply[2];
  uint8_t pending;
  int i;

  if (picocalc_kbd_read(priv, PICOCALC_KBD_REG_KEY, reply) < 0)
    {
      return;
    }

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

      /* A zero state means the FIFO drained early.  It should not happen
       * when the count says otherwise, but the count and the pop are
       * separate transactions and nothing holds the co-processor still
       * between them.
       */

      if (reply[0] == PICOCALC_KBD_STATE_IDLE)
        {
          break;
        }

      picocalc_kbd_event(priv, reply[0], reply[1]);
    }
}

/****************************************************************************
 * Name: picocalc_kbd_worker
 *
 * Description:
 *   Read the co-processor and re-arm.  Errors are not fatal and are not
 *   logged per-tick: the co-processor reinitialises its own I2C slave when
 *   it believes the link has stalled, and a handful of NAKs while that
 *   happens is normal.
 *
 ****************************************************************************/

static void picocalc_kbd_worker(FAR void *arg)
{
  FAR struct picocalc_kbd_dev_s *priv = arg;
  bool more = false;

  if (priv->v2)
    {
      more = picocalc_kbd_hot(priv);
    }
  else
    {
      picocalc_kbd_legacy(priv);
    }

  picocalc_kbd_arm(priv, more);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: picocalc_kbd_register
 *
 * Description:
 *   Register the keyboard co-processor and start polling it.  See
 *   include/nuttx/input/picocalc_kbd.h.
 *
 ****************************************************************************/

int picocalc_kbd_register(FAR const char *devpath,
                          FAR struct i2c_master_s *i2c,
                          uint8_t addr, uint32_t frequency,
                          picocalc_kbd_attach_t attach,
                          picocalc_kbd_asserted_t asserted)
{
  FAR struct picocalc_kbd_dev_s *priv;
  int ret;

  DEBUGASSERT(devpath != NULL && i2c != NULL);

  priv = kmm_zalloc(sizeof(struct picocalc_kbd_dev_s));
  if (priv == NULL)
    {
      ierr("ERROR: Failed to allocate the keyboard state\n");
      return -ENOMEM;
    }

  priv->i2c       = i2c;
  priv->addr      = addr;
  priv->frequency = frequency;
  priv->v2        = picocalc_kbd_probe(priv);

  ret = keyboard_register(&priv->lower, devpath,
                          CONFIG_INPUT_PICOCALC_KBD_BUFFER_SIZE);
  if (ret < 0)
    {
      ierr("ERROR: keyboard_register(%s) failed: %d\n", devpath, ret);
      kmm_free(priv);
      return ret;
    }

  /* The attention line is only offered by the extended protocol.  Attaching
   * it against stock firmware would arm an interrupt on a pin nothing ever
   * drives, and the driver would then wait for an edge that cannot come.
   */

  if (priv->v2 && attach != NULL && attach(picocalc_kbd_isr, priv) >= 0)
    {
      priv->attention = true;
      priv->asserted  = asserted;
    }

  iinfo("%s: %s protocol, %s\n", devpath,
        priv->v2 ? "extended" : "legacy",
        priv->attention ? "attention line" : "polled");

  /* Read once immediately rather than after a period.  The co-processor has
   * been scanning since it powered up, which is long before this, so there
   * may already be events queued -- and if there are, the attention line is
   * already low and no edge is coming to tell us.
   */

  ret = work_queue(LPWORK, &priv->work, picocalc_kbd_worker, priv, 0);
  if (ret < 0)
    {
      ierr("ERROR: Failed to start the keyboard poll: %d\n", ret);
      keyboard_unregister(&priv->lower, devpath);
      kmm_free(priv);
      return ret;
    }

  return OK;
}
