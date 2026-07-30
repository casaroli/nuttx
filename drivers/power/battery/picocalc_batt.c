/****************************************************************************
 * drivers/power/battery/picocalc_batt.c
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

/* A battery gauge over the ClockworkPi PicoCalc's keyboard co-processor.
 *
 * The AXP2101 PMU this ultimately reports is not on the host's bus at all.
 * It hangs off a second I2C bus belonging to the STM32F103 co-processor,
 * which polls it and serves the result as a cached register, so a read here
 * is one short transaction that never waits on the PMU.  It also means the
 * numbers are as fresh as the co-processor's own refresh, not as fresh as
 * the read -- fine for a battery, which moves over minutes.
 *
 * Two protocols are handled.  The stock BIOS serves a percentage with a
 * charging bit and nothing else; the replacement firmware serves a twelve
 * byte summary that adds the cell voltage, the charge state, and the one
 * distinction the legacy register cannot make -- no battery, as opposed to a
 * battery reading zero.  Which is in use is decided once, by probing.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/kmalloc.h>
#include <nuttx/power/battery_gauge.h>
#include <nuttx/power/battery_ioctl.h>
#include <nuttx/power/picocalc_batt.h>

#ifdef CONFIG_BATTERY_PICOCALC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BATT_ADDR_LEGACY     0x1f
#define BATT_ADDR_V2         0x1e

/* Legacy: one register, replying [reg, percent | charging << 7]. */

#define LEGACY_REG_BAT       0x0b
#define LEGACY_BAT_CHARGING  0x80
#define LEGACY_BAT_PERCENT   0x7f

/* New protocol. */

#define V2_REG_WHOAMI        0x00
#define V2_REG_CAPS          0x02
#define V2_REG_PMU_SUMMARY   0x20

#define V2_CAPS_PMU_SUMMARY  (1 << 4)

#define SUMMARY_LEN          12

/* Byte 0 of the summary. */

#define SUM_BATT_PRESENT     (1 << 0)
#define SUM_CHARGING         (1 << 1)
#define SUM_VBUS_PRESENT     (1 << 2)
#define SUM_LOW_BATT         (1 << 4)
#define SUM_CHARGE_DONE      (1 << 5)
#define SUM_ONLINE           (1 << 7)

/* Byte 2 when the charge is not known, or there is no cell. */

#define SUM_PERCENT_UNKNOWN  0xff

/* Reported by BATIOC_CHIPID, so that a caller can tell which of the two
 * protocols it is actually being served by.  There is no identity register
 * in the legacy protocol to report instead.
 */

#define CHIPID_LEGACY        0x1f
#define CHIPID_V2            0x1e

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct picocalc_batt_dev_s
{
  struct battery_gauge_dev_s dev;    /* Upper half, must come first     */
  FAR struct i2c_master_s *i2c;      /* Bus the co-processor is on      */
  uint32_t frequency;                /* Transfer frequency              */
  bool v2;                           /* Replacement firmware present    */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int picocalc_batt_state(FAR struct battery_gauge_dev_s *dev,
                               FAR int *status);
static int picocalc_batt_online(FAR struct battery_gauge_dev_s *dev,
                                FAR bool *status);
static int picocalc_batt_voltage(FAR struct battery_gauge_dev_s *dev,
                                 FAR int *value);
static int picocalc_batt_capacity(FAR struct battery_gauge_dev_s *dev,
                                  FAR int *value);
static int picocalc_batt_chipid(FAR struct battery_gauge_dev_s *dev,
                                FAR unsigned int *value);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct battery_gauge_operations_s g_picocalc_battops =
{
  .state    = picocalc_batt_state,
  .online   = picocalc_batt_online,
  .voltage  = picocalc_batt_voltage,
  .capacity = picocalc_batt_capacity,
  .chipid   = picocalc_batt_chipid,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: picocalc_batt_read
 *
 * Description:
 *   Register select and read in one transfer, so a repeated START is emitted
 *   between them.
 *
 *   The framing is not a preference.  The legacy protocol keeps one global
 *   reply buffer for the whole device, so a select and a read issued as two
 *   transfers can have another client's select land in the gap -- and the
 *   keyboard driver on this same bus polls continuously.  Holding the bus
 *   lock across both phases is what closes that window.  The new protocol
 *   requires a repeated START outright: a bare read there returns the
 *   keyboard event block, not the register that was asked for.
 *
 ****************************************************************************/

static int picocalc_batt_read(FAR struct picocalc_batt_dev_s *priv,
                              uint8_t addr, uint8_t reg,
                              FAR uint8_t *buf, size_t len)
{
  struct i2c_msg_s msg[2];

  msg[0].frequency = priv->frequency;
  msg[0].addr      = addr;
  msg[0].flags     = 0;
  msg[0].buffer    = &reg;
  msg[0].length    = 1;

  msg[1].frequency = priv->frequency;
  msg[1].addr      = addr;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = buf;
  msg[1].length    = len;

  return I2C_TRANSFER(priv->i2c, msg, 2);
}

/****************************************************************************
 * Name: picocalc_batt_summary
 *
 * Description:
 *   Fetch the cached PMU summary.  New protocol only.
 *
 ****************************************************************************/

static int picocalc_batt_summary(FAR struct picocalc_batt_dev_s *priv,
                                 FAR uint8_t *buf)
{
  int ret = picocalc_batt_read(priv, BATT_ADDR_V2, V2_REG_PMU_SUMMARY,
                               buf, SUMMARY_LEN);
  if (ret < 0)
    {
      return ret;
    }

  /* The co-processor answers even when it has never reached the PMU, and
   * says so with this bit clear.  Everything else in the block is undefined
   * then, so it is an error rather than a reading.
   */

  if ((buf[0] & SUM_ONLINE) == 0)
    {
      baterr("ERROR: co-processor has not reached the PMU\n");
      return -EIO;
    }

  return OK;
}

/****************************************************************************
 * Name: picocalc_batt_legacy
 *
 * Description:
 *   Fetch the legacy battery byte: percentage in the low seven bits, the
 *   charging flag in bit 7.  The reply is [register, value], so the byte
 *   that matters is the second one.
 *
 ****************************************************************************/

static int picocalc_batt_legacy(FAR struct picocalc_batt_dev_s *priv,
                                FAR uint8_t *value)
{
  uint8_t reply[2];
  int ret;

  ret = picocalc_batt_read(priv, BATT_ADDR_LEGACY, LEGACY_REG_BAT,
                           reply, sizeof(reply));
  if (ret < 0)
    {
      return ret;
    }

  *value = reply[1];
  return OK;
}

/****************************************************************************
 * Name: picocalc_batt_state
 ****************************************************************************/

static int picocalc_batt_state(FAR struct battery_gauge_dev_s *dev,
                               FAR int *status)
{
  FAR struct picocalc_batt_dev_s *priv =
    (FAR struct picocalc_batt_dev_s *)dev;

  *status = BATTERY_UNKNOWN;

  if (priv->v2)
    {
      uint8_t sum[SUMMARY_LEN];
      int ret = picocalc_batt_summary(priv, sum);

      if (ret < 0)
        {
          return ret;
        }

      if ((sum[0] & SUM_BATT_PRESENT) == 0)
        {
          /* Running from USB with no cell fitted.  Not a fault, and not a
           * flat battery either; there is no battery to have a state.
           */

          *status = BATTERY_UNKNOWN;
        }
      else if (sum[0] & SUM_CHARGING)
        {
          *status = BATTERY_CHARGING;
        }
      else if (sum[0] & SUM_CHARGE_DONE)
        {
          *status = BATTERY_FULL;
        }
      else if (sum[0] & SUM_VBUS_PRESENT)
        {
          /* Externally powered and not charging: idle, not discharging. */

          *status = BATTERY_IDLE;
        }
      else
        {
          *status = BATTERY_DISCHARGING;
        }

      return OK;
    }

    {
      uint8_t value;
      int ret = picocalc_batt_legacy(priv, &value);

      if (ret < 0)
        {
          return ret;
        }

      /* One bit to work from.  "Full" is inferred from 100% and not
       * charging, which is the best the legacy protocol supports; it has no
       * way to distinguish a finished charge from a cell that happens to be
       * full and idle, and no way to say there is no cell at all.
       */

      if (value & LEGACY_BAT_CHARGING)
        {
          *status = BATTERY_CHARGING;
        }
      else if ((value & LEGACY_BAT_PERCENT) >= 100)
        {
          *status = BATTERY_FULL;
        }
      else
        {
          *status = BATTERY_DISCHARGING;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: picocalc_batt_online
 ****************************************************************************/

static int picocalc_batt_online(FAR struct battery_gauge_dev_s *dev,
                                FAR bool *status)
{
  FAR struct picocalc_batt_dev_s *priv =
    (FAR struct picocalc_batt_dev_s *)dev;

  if (priv->v2)
    {
      uint8_t sum[SUMMARY_LEN];
      int ret = picocalc_batt_summary(priv, sum);

      if (ret < 0)
        {
          return ret;
        }

      *status = (sum[0] & SUM_BATT_PRESENT) != 0;
      return OK;
    }

  /* The legacy protocol has no presence bit, so the only honest answer is
   * that something is answering.  A board running from USB with the cells
   * out reports online here and 0% below, which is exactly the ambiguity the
   * new protocol exists to remove.
   */

    {
      uint8_t value;
      int ret = picocalc_batt_legacy(priv, &value);

      if (ret < 0)
        {
          return ret;
        }

      *status = true;
    }

  return OK;
}

/****************************************************************************
 * Name: picocalc_batt_voltage
 ****************************************************************************/

static int picocalc_batt_voltage(FAR struct battery_gauge_dev_s *dev,
                                 FAR int *value)
{
  FAR struct picocalc_batt_dev_s *priv =
    (FAR struct picocalc_batt_dev_s *)dev;
  uint8_t sum[SUMMARY_LEN];
  int ret;

  if (!priv->v2)
    {
      /* The legacy protocol reports a percentage and nothing else. */

      return -ENOSYS;
    }

  ret = picocalc_batt_summary(priv, sum);
  if (ret < 0)
    {
      return ret;
    }

  if ((sum[0] & SUM_BATT_PRESENT) == 0)
    {
      return -ENODEV;
    }

  /* Bytes 4 and 5, little-endian, already in millivolts. */

  *value = (int)sum[4] | ((int)sum[5] << 8);
  return OK;
}

/****************************************************************************
 * Name: picocalc_batt_capacity
 ****************************************************************************/

static int picocalc_batt_capacity(FAR struct battery_gauge_dev_s *dev,
                                  FAR int *value)
{
  FAR struct picocalc_batt_dev_s *priv =
    (FAR struct picocalc_batt_dev_s *)dev;

  if (priv->v2)
    {
      uint8_t sum[SUMMARY_LEN];
      int ret = picocalc_batt_summary(priv, sum);

      if (ret < 0)
        {
          return ret;
        }

      if (sum[2] == SUM_PERCENT_UNKNOWN)
        {
          /* No cell, or a gauge that has not settled.  Refusing is the point
           * of the sentinel: returning zero here would be indistinguishable
           * from a flat battery, which is the legacy protocol's defect.
           */

          return -ENODEV;
        }

      *value = sum[2];
      return OK;
    }

    {
      uint8_t v;
      int ret = picocalc_batt_legacy(priv, &v);

      if (ret < 0)
        {
          return ret;
        }

      *value = v & LEGACY_BAT_PERCENT;
    }

  return OK;
}

/****************************************************************************
 * Name: picocalc_batt_chipid
 ****************************************************************************/

static int picocalc_batt_chipid(FAR struct battery_gauge_dev_s *dev,
                                FAR unsigned int *value)
{
  FAR struct picocalc_batt_dev_s *priv =
    (FAR struct picocalc_batt_dev_s *)dev;

  *value = priv->v2 ? CHIPID_V2 : CHIPID_LEGACY;
  return OK;
}

/****************************************************************************
 * Name: picocalc_batt_probe
 *
 * Description:
 *   Decide which protocol to use.  The identity register is checked as well
 *   as the address answering, because an unrelated device at 0x1e must not
 *   be read as a PMU summary, and the capability bit is checked as well as
 *   the identity, because a firmware build without the PMU would answer the
 *   first two and refuse the third.
 *
 ****************************************************************************/

static bool picocalc_batt_probe(FAR struct picocalc_batt_dev_s *priv)
{
  uint8_t who[4];
  uint8_t caps[4];
  uint32_t bits;

  if (picocalc_batt_read(priv, BATT_ADDR_V2, V2_REG_WHOAMI,
                         who, sizeof(who)) < 0)
    {
      return false;
    }

  if (who[0] != 'P' || who[1] != 'K' || who[2] != 'B')
    {
      batwarn("WARNING: 0x1e is not the co-processor\n");
      return false;
    }

  if (picocalc_batt_read(priv, BATT_ADDR_V2, V2_REG_CAPS,
                         caps, sizeof(caps)) < 0)
    {
      return false;
    }

  bits = (uint32_t)caps[0] | ((uint32_t)caps[1] << 8) |
         ((uint32_t)caps[2] << 16) | ((uint32_t)caps[3] << 24);

  return (bits & V2_CAPS_PMU_SUMMARY) != 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: picocalc_batt_register
 ****************************************************************************/

int picocalc_batt_register(FAR const char *devpath,
                           FAR struct i2c_master_s *i2c,
                           uint32_t frequency)
{
  FAR struct picocalc_batt_dev_s *priv;
  int ret;

  DEBUGASSERT(devpath != NULL && i2c != NULL);

  priv = kmm_zalloc(sizeof(struct picocalc_batt_dev_s));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  priv->dev.ops   = &g_picocalc_battops;
  priv->i2c       = i2c;
  priv->frequency = frequency;
  priv->v2        = picocalc_batt_probe(priv);

  /* batlock is initialised by battery_gauge_register(); the lower half must
   * not touch it.
   */

  if (!priv->v2)
    {
      /* Fall back to the legacy register, but only if it answers.  Nothing
       * on either address means the co-processor is not running, and a
       * gauge that can never produce a reading is worse than an absent one.
       */

      uint8_t value;

      ret = picocalc_batt_legacy(priv, &value);
      if (ret < 0)
        {
          baterr("ERROR: no co-processor on either address: %d\n", ret);
          kmm_free(priv);
          return -ENODEV;
        }
    }

  ret = battery_gauge_register(devpath, &priv->dev);
  if (ret < 0)
    {
      baterr("ERROR: battery_gauge_register(%s) failed: %d\n", devpath, ret);
      kmm_free(priv);
      return ret;
    }

  batinfo("%s: co-processor battery, %s protocol\n",
          devpath, priv->v2 ? "new" : "legacy");

  return OK;
}

#endif /* CONFIG_BATTERY_PICOCALC */
