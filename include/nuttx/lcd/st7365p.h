/****************************************************************************
 * include/nuttx/lcd/st7365p.h
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

#ifndef __INCLUDE_NUTTX_LCD_ST7365P_H
#define __INCLUDE_NUTTX_LCD_ST7365P_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/spi/spi.h>

#ifdef CONFIG_LCD_ST7365P

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Set the panel's backlight, 0..CONFIG_LCD_MAXPOWER.
 *
 * The backlight is not on a pin of this controller.  On the boards this
 * panel appears on it is a separate boost converter, often behind something
 * else again -- on the ClockworkPi PicoCalc it belongs to a co-processor
 * reached over I2C -- so a board that can dim its own passes a hook here and
 * the LCD power setting is passed through to it.  A board that cannot passes
 * NULL, and CONFIG_LCD_MAXPOWER should then be 1: any non-zero value is
 * simply "on".
 */

typedef CODE int (*st7365p_backlight_t)(int level);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name:  st7365p_lcdinitialize
 *
 * Description:
 *   Initialize the ST7365P video hardware.  The initial state of the LCD is
 *   fully initialized, display memory cleared, and the LCD ready to use, but
 *   with the power setting at 0 (full off).
 *
 * Input Parameters:
 *   spi       - A reference to the SPI driver instance the panel is on.
 *   backlight - Board hook that sets the backlight, or NULL if the board
 *               cannot dim.  See st7365p_backlight_t.
 *
 * Returned Value:
 *   On success, this function returns a reference to the LCD object for the
 *   specified LCD.  NULL is returned on any failure.
 *
 ****************************************************************************/

FAR struct lcd_dev_s *st7365p_lcdinitialize(FAR struct spi_dev_s *spi,
                                            st7365p_backlight_t backlight);

/****************************************************************************
 * Name:  st7365p_scroll
 *
 * Description:
 *   Scroll the display using the controller's vertical scrolling hardware.
 *
 *   The controller has more GRAM rows than the panel has physical rows (480
 *   against 320 on the PicoCalc), so scrolling is a two byte register write
 *   rather than a repaint of the whole framebuffer.  The offset is applied
 *   transparently by the driver's set-area calculation, so callers that know
 *   nothing about scrolling keep addressing rows 0..yres-1 and still land on
 *   the right pixels.
 *
 *   This is what makes a text console usable: scrolling one line costs a
 *   register write plus one repainted line, instead of a full-screen redraw.
 *
 *   When CONFIG_LCD_ST7365P_HWSCROLL is enabled this is also installed as
 *   the device's vscroll() method, which is how NX reaches it; a board only
 *   needs to call it directly if it drives the panel without NX.
 *
 * Input Parameters:
 *   dev   - The LCD device returned by st7365p_lcdinitialize().
 *   lines - Number of display lines to scroll by.  Positive scrolls content
 *           up (the usual direction for a terminal); negative scrolls down.
 *
 * Returned Value:
 *   OK on success; a negated errno on failure.
 *
 ****************************************************************************/

int st7365p_scroll(FAR struct lcd_dev_s *dev, int lines);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* CONFIG_LCD_ST7365P */
#endif /* __INCLUDE_NUTTX_LCD_ST7365P_H */
