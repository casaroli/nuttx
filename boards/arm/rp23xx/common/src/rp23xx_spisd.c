/****************************************************************************
 * boards/arm/rp23xx/common/src/rp23xx_spisd.c
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

#include <errno.h>
#include <syslog.h>

#include <nuttx/debug.h>
#include <nuttx/mmcsd.h>
#include <nuttx/board.h>
#include <nuttx/fs/fs.h>
#include <sys/mount.h>

#include <nuttx/wqueue.h>
#include <nuttx/irq.h>

#include "rp23xx_spi.h"
#include "rp23xx_gpio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if defined(CONFIG_RP23XX_SPISD_CD_GPIO) && CONFIG_RP23XX_SPISD_CD_GPIO >= 0
#  define HAVE_CARD_DETECT 1
#  define SD_CD_GPIO       CONFIG_RP23XX_SPISD_CD_GPIO
#endif

#define SD_BLOCK  "/dev/mmcsd0"
#define SD_MOUNT  "/mnt/sd0"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Configuration ************************************************************/

#ifndef CONFIG_RP23XX_SPISD_SLOT_NO
#  define CONFIG_RP23XX_SPISD_SLOT_NO 0
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef HAVE_CARD_DETECT
/* Debounce and the mount both run here.  The switch bounces for a few
 * milliseconds and mounting a filesystem is far too much to do in an
 * interrupt, so the handler only schedules this.
 */

static struct work_s g_cd_work;
static bool          g_mounted;
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef HAVE_CARD_DETECT

/****************************************************************************
 * Name: board_spisd_present
 *
 * Description:
 *   True if the socket's card-detect switch says a card is in it.
 *
 ****************************************************************************/

static bool board_spisd_present(void)
{
  bool level = rp23xx_gpio_get(SD_CD_GPIO);

#ifdef CONFIG_RP23XX_SPISD_CD_ACTIVE_LOW
  return !level;
#else
  return level;
#endif
}

/****************************************************************************
 * Name: board_spisd_cdwork
 *
 * Description:
 *   Mount the card that was just inserted, or take down the filesystem of
 *   the one that was just removed.
 *
 *   The unmount is the half that matters.  A card pulled while mounted
 *   leaves the filesystem believing it still owns blocks that are no longer
 *   there, and the next write goes somewhere it should not.  Doing it here
 *   is late -- the card is already out -- but it stops the damage spreading
 *   to the card that gets inserted next.
 *
 ****************************************************************************/

static void board_spisd_cdwork(FAR void *arg)
{
  bool present = board_spisd_present();
  int  ret;

  if (present && !g_mounted)
    {
      ret = nx_mount(SD_BLOCK, SD_MOUNT, "vfat", 0, NULL);
      if (ret >= 0)
        {
          g_mounted = true;
          syslog(LOG_INFO, "SD card inserted, mounted at %s\n", SD_MOUNT);
        }
      else
        {
          /* Unformatted, or a filesystem this build cannot read.  The block
           * device is there either way and mkfatfs will still work on it.
           */

          syslog(LOG_INFO, "SD card inserted, no filesystem: %d\n", ret);
        }
    }
  else if (!present && g_mounted)
    {
      ret = nx_umount2(SD_MOUNT, MNT_FORCE);
      g_mounted = false;
      syslog(LOG_INFO, "SD card removed, unmounted %s: %d\n", SD_MOUNT, ret);
    }
}

/****************************************************************************
 * Name: board_spisd_cdirq
 *
 * Description:
 *   Card-detect edge.  Schedules the work and returns; the delay is the
 *   debounce.
 *
 ****************************************************************************/

static int board_spisd_cdirq(int irq, FAR void *context, FAR void *arg)
{
  work_queue(LPWORK, &g_cd_work, board_spisd_cdwork, NULL,
             MSEC2TICK(CONFIG_RP23XX_SPISD_CD_DEBOUNCE_MS));
  return OK;
}

#endif /* HAVE_CARD_DETECT */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_spisd_initialize
 *
 * Description:
 *   Initialize the SPI-based SD card.
 *
 ****************************************************************************/

int board_spisd_initialize(int minor, int bus)
{
  int ret;
  struct spi_dev_s *spi;

  /* Initialize spi device */

  spi = rp23xx_spibus_initialize(bus);
  if (!spi)
    {
      ferr("ERROR: Failed to initialize spi%d.\n", bus);
      return -ENODEV;
    }

  /* Pull up RX */

#ifdef CONFIG_RP23XX_SPI0
  if (bus == 0)
    {
      rp23xx_gpio_set_pulls(CONFIG_RP23XX_SPI0_RX_GPIO, true, false);
    }
#endif

#ifdef CONFIG_RP23XX_SPI1
  if (bus == 1)
    {
      rp23xx_gpio_set_pulls(CONFIG_RP23XX_SPI1_RX_GPIO, true, false);
    }
#endif

  /* Get the SPI driver instance for the SD chip select */

  finfo("Initializing SPI for the MMC/SD slot\n");

  ret = mmcsd_spislotinitialize(minor, CONFIG_RP23XX_SPISD_SLOT_NO, spi);
  if (ret < 0)
    {
      ferr("ERROR: Failed to bind SPI device to MMC/SD slot %d: %d\n",
           CONFIG_RP23XX_SPISD_SLOT_NO, ret);
      return ret;
    }

#ifdef HAVE_CARD_DETECT
  /* Watch the socket.  Both edges: one mounts, the other unmounts. */

  rp23xx_gpio_init(SD_CD_GPIO);
  rp23xx_gpio_setdir(SD_CD_GPIO, false);
  rp23xx_gpio_set_pulls(SD_CD_GPIO, true, false);

  rp23xx_gpio_irq_attach(SD_CD_GPIO, RP23XX_GPIO_INTR_EDGE_LOW,
                         board_spisd_cdirq, NULL);
  rp23xx_gpio_irq_attach(SD_CD_GPIO, RP23XX_GPIO_INTR_EDGE_HIGH,
                         board_spisd_cdirq, NULL);
  rp23xx_gpio_enable_irq(SD_CD_GPIO);
#endif

  /* Mount filesystem */

  ret = nx_mount("/dev/mmcsd0", "/mnt/sd0", "vfat", 0, NULL);
  if (ret == -EINVAL || ret == -ENODEV)
    {
      /* No card in the slot, or a card with no filesystem on it.  Neither is
       * a fault: the block device is registered either way and the card can
       * be formatted with mkfatfs and mounted by hand.  Logging it at error
       * level every boot -- which it did -- teaches people to ignore a
       * channel that should mean something.
       */

      _info("No filesystem on /dev/mmcsd0 (no card, or unformatted)\n");
    }
#ifdef HAVE_CARD_DETECT
  else if (ret >= 0)
    {
      g_mounted = true;
    }
#endif
  else if (ret < 0)
    {
      _err("ERROR: Failed to mount the SDCARD. %d\n", ret);
    }

  return OK;
}

/****************************************************************************
 * Name: board_spisd_status
 *
 * Description:
 *   Get the status whether SD Card is present or not.
 *   This function is called only from rp23xx_spi.c.
 *
 * Returned Value:
 *   Return SPI_STATUS_PRESENT if SD Card is present. Otherwise, return 0.
 *
 ****************************************************************************/

uint8_t board_spisd_status(struct spi_dev_s *dev, uint32_t devid)
{
  uint8_t ret = 0;

  if (devid == SPIDEV_MMCSD(0))
    {
#ifdef HAVE_CARD_DETECT
      if (board_spisd_present())
        {
          ret = SPI_STATUS_PRESENT;
        }
#else
      /* No card-detect pin on this board.  Claiming a card is always there
       * is a lie, but it is the only answer available and it is what the
       * driver did before the pin became configurable.
       */

      ret = SPI_STATUS_PRESENT;
#endif
    }

  return ret;
}
