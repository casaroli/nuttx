/****************************************************************************
 * boards/arm/rp23xx/clockworkpi-picocalc/src/picocalc_lcd.c
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
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/st7365p.h>
#include <nuttx/spi/spi.h>

#include <arch/board/board.h>

#include "rp23xx_gpio.h"
#include "rp23xx_spi.h"
#include "rp23xx_pico.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR struct lcd_dev_s *g_lcd = NULL;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_lcd_initialize
 *
 * Description:
 *   Initialize the LCD video hardware.  The initial state of the LCD is
 *   fully initialized, display memory cleared, and the LCD ready to use, but
 *   with the power setting at 0 (full off).
 *
 ****************************************************************************/

int board_lcd_initialize(void)
{
  FAR struct spi_dev_s *spi;

  /* The Data/Command and reset lines are plain GPIOs.  Note that the panel's
   * SDO is wired to SPI1 RX (GPIO 12), so unlike most rp23xx boards the
   * Data/Command line cannot share that pin; it is GPIO 14 and is selected
   * by CONFIG_RP23XX_SPI1_DC_GPIO.
   */

  rp23xx_gpio_init(GPIO_PICOCALC_LCD_DC);
  rp23xx_gpio_setdir(GPIO_PICOCALC_LCD_DC, true);
  rp23xx_gpio_put(GPIO_PICOCALC_LCD_DC, true);

  rp23xx_gpio_init(GPIO_PICOCALC_LCD_RST);
  rp23xx_gpio_setdir(GPIO_PICOCALC_LCD_RST, true);

  /* Hardware reset: hold low, then release and let the controller settle.
   * The panel needs a long tail here; every known driver for it waits
   * around 200ms before talking to the controller.
   */

  rp23xx_gpio_put(GPIO_PICOCALC_LCD_RST, true);
  up_mdelay(10);
  rp23xx_gpio_put(GPIO_PICOCALC_LCD_RST, false);
  up_mdelay(10);
  rp23xx_gpio_put(GPIO_PICOCALC_LCD_RST, true);
  up_mdelay(200);

  spi = rp23xx_spibus_initialize(1);
  if (spi == NULL)
    {
      lcderr("ERROR: Failed to initialize SPI1 for the LCD\n");
      return -ENODEV;
    }

  g_lcd = st7365p_lcdinitialize(spi);
  if (g_lcd == NULL)
    {
      lcderr("ERROR: Failed to bind SPI1 to the ST7365P\n");
      return -ENODEV;
    }

  return OK;
}

/****************************************************************************
 * Name: board_lcd_getdev
 *
 * Description:
 *   Return a reference to the LCD object for the specified LCD.  This allows
 *   support for multiple LCD devices.
 *
 ****************************************************************************/

FAR struct lcd_dev_s *board_lcd_getdev(int lcddev)
{
  if (lcddev == 0)
    {
      return g_lcd;
    }

  return NULL;
}

/****************************************************************************
 * Name: board_lcd_uninitialize
 *
 * Description:
 *   Uninitialize the LCD support.
 *
 ****************************************************************************/

void board_lcd_uninitialize(void)
{
  if (g_lcd != NULL)
    {
      g_lcd->setpower(g_lcd, 0);
    }
}
