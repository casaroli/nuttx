/****************************************************************************
 * boards/arm/rp23xx/clockworkpi-picocalc/src/rp23xx_bringup.c
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

#include <nuttx/debug.h>
#include <stddef.h>
#include <errno.h>

#include <nuttx/fs/fs.h>

#include <nuttx/board.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/lcd_dev.h>
#include <nuttx/video/fb.h>

#include <arch/board/board.h>

#include "rp23xx_gpio.h"
#include "rp23xx_pico.h"

#ifdef CONFIG_ARCH_BOARD_COMMON
#include "rp23xx_common_bringup.h"
#endif /* CONFIG_ARCH_BOARD_COMMON */

#ifdef CONFIG_USERLED
#  include <nuttx/leds/userled.h>
#endif

#ifdef CONFIG_INPUT_BUTTONS
#  include <nuttx/input/buttons.h>
#endif

#ifdef CONFIG_RP23XX_INFINEON_CYW43439
#  include "rp23xx_cyw43439.h"
#endif

#ifdef CONFIG_INPUT_PICOCALC_KBD
#  include <nuttx/input/picocalc_kbd.h>
#endif

#ifdef CONFIG_BATTERY_PICOCALC
#  include <nuttx/power/picocalc_batt.h>
#endif

#ifdef CONFIG_RP23XX_I2C1
#  include "picocalc_coproc.h"
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifdef CONFIG_RP23XX_INFINEON_CYW43439
gspi_dev_t *g_cyw43439 = NULL;
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rp23xx_bringup
 ****************************************************************************/

int rp23xx_bringup(void)
{
  int ret;

  UNUSED(ret);

#ifdef CONFIG_ARCH_BOARD_COMMON

  ret = rp23xx_common_bringup();
  if (ret < 0)
    {
      return ret;
    }

#endif /* CONFIG_ARCH_BOARD_COMMON */

  /* --- Place any board specific bringup code here --- */

#ifdef CONFIG_RP23XX_INFINEON_CYW43439
  /* Bring up the CYW43439 wireless chip.  This registers the wlan0 network
   * device; the chip's firmware is not downloaded until wlan0 is brought up.
   *
   * Do this before the LED driver, which drives a GPIO on this chip.
   */

  g_cyw43439 = rp23xx_cyw_setup(GPIO_CYW43439_ON,
                                GPIO_CYW43439_CS,
                                GPIO_CYW43439_DATA,
                                GPIO_CYW43439_CLOCK,
                                GPIO_CYW43439_DATA);

  if (g_cyw43439 == NULL)
    {
      syslog(LOG_ERR,
             "ERROR: failed to initialize the cyw43439 (WiFi chip): %d\n",
             errno);
    }
#endif

#if defined(CONFIG_USERLED_LOWER) && defined(CONFIG_RP23XX_INFINEON_CYW43439)
  /* Register the LED driver.  The LED sits on the wireless chip, so there is
   * nothing to register without it.
   */

  ret = userled_lower_initialize("/dev/userleds");
  if (ret < 0)
    {
      syslog(LOG_ERR, \
      "ERROR: userled_lower_initialize() failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_INPUT_BUTTONS
  /* Register the BUTTON driver */

  ret = btn_lower_initialize("/dev/buttons");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: btn_lower_initialize() failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_LCD_DEV
#ifdef CONFIG_RP23XX_I2C1
  /* Bring up the co-processor link first.  Everything else on this board
   * that matters goes through it -- the keyboard, both backlights, the PMU
   * -- and the panel in particular needs it before board_lcd_initialize(),
   * because the backlight it will be asked to set is on the far side.
   */

  if (picocalc_coproc_initialize() < 0)
    {
      syslog(LOG_ERR, "ERROR: co-processor link unavailable\n");
    }
#endif

  /* Bring up the ST7365P panel and expose it as /dev/lcd0 */

  ret = board_lcd_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_lcd_initialize() failed: %d\n", ret);
    }
  else
    {
      ret = lcddev_register(0);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: lcddev_register() failed: %d\n", ret);
        }
    }
#endif

  /* Configure the co-processor's attention line as a pulled-up input.
   *
   * This is done unconditionally, and before the keyboard, because an
   * unconfigured pad is worse than an unused one.  At reset the RP2350
   * leaves it isolated, input-disabled and pulled *down*; since the far end
   * is open drain and only ever pulls low, it then reads 0 whether or not
   * the co-processor is asserting, so anything that later polls it sees a
   * permanent request it can never clear.
   */

  rp23xx_gpio_init(GPIO_COPROC_ATTN);
  rp23xx_gpio_setdir(GPIO_COPROC_ATTN, false);
  rp23xx_gpio_set_pulls(GPIO_COPROC_ATTN, true, false);

#ifdef CONFIG_INPUT_PICOCALC_KBD
  /* Register the keyboard co-processor as /dev/kbd0.  This is deliberately
   * after the panel: the co-processor also owns the LCD backlight, so a
   * failure here is worth seeing on a display that is already alive.
   *
   * The stock BIOS v1.6 driver clocks this bus at 10kHz, but that is a
   * workaround for the Arduino firmware's slave implementation.  The Rust
   * co-processor firmware has been verified at 400kHz.
   */

  if (picocalc_coproc_i2c() != NULL)
    {
      ret = picocalc_kbd_register("/dev/kbd0", picocalc_coproc_i2c(),
                                  PICOCALC_KBD_I2C_ADDR, 400000,
                                  picocalc_coproc_attach_attention);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "ERROR: picocalc_kbd_register() failed: %d\n", ret);
        }
    }
#endif

#ifdef CONFIG_BATTERY_PICOCALC
  /* The battery the co-processor caches for us, as /dev/batt0.  It shares
   * the keyboard's bus handle: the I2C upper half serialises on it, which
   * is what keeps a battery read from landing inside the keyboard's poll.
   */

  if (picocalc_coproc_i2c() != NULL)
    {
      ret = picocalc_batt_register("/dev/batt0", picocalc_coproc_i2c(),
                                   400000);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "ERROR: picocalc_batt_register() failed: %d\n", ret);
        }
    }
#endif

#ifdef CONFIG_VIDEO_FB
  /* Wrap the panel in the framebuffer layer as well, so that the stock
   * framebuffer applications can drive it.  With CONFIG_LCD_FRAMEBUFFER this
   * costs a full 320x320x2 shadow buffer in SRAM, so it is not enabled in
   * the configurations that only want /dev/lcd0.
   */

  ret = fb_register(0, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: fb_register() failed: %d\n", ret);
    }
#endif

  return OK;
}
