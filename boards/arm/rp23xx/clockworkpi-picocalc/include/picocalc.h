/****************************************************************************
 * boards/arm/rp23xx/clockworkpi-picocalc/include/picocalc.h
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

#ifndef __BOARDS_ARM_RP23XX_CLOCKWORKPI_PICOCALC_INCLUDE_PICOCALC_H
#define __BOARDS_ARM_RP23XX_CLOCKWORKPI_PICOCALC_INCLUDE_PICOCALC_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ClockworkPi PicoCalc mainboard v2.0 pin assignments.
 *
 * The module plugs into two 20-pin sockets, J301 and J302:
 *   J302 pin N  == module pin N        (module pins 1-20, GP0-GP15)
 *   J301 pin N  == module pin (41 - N) (module pins 40-21, VBUS..GP16)
 *
 * Only GP0-GP22 and GP26-GP28 are on the castellations, so nothing here can
 * collide with the Pimoroni module's own GP23-GP25, GP29 (CYW43439) or GP47
 * (QSPI PSRAM).
 */

/* LCD -- ST7365P, 320x320, 4-line SPI on SPI1 ******************************/

#define GPIO_PICOCALC_LCD_SCK    10
#define GPIO_PICOCALC_LCD_MOSI   11
#define GPIO_PICOCALC_LCD_MISO   12   /* panel SDO; readable only at <=6MHz */
#define GPIO_PICOCALC_LCD_CS     13
#define GPIO_PICOCALC_LCD_DC     14   /* NOT the SPI RX pin -- see rp23xx_spi.c */
#define GPIO_PICOCALC_LCD_RST    15

/* microSD -- SPI0, 1-bit SPI mode ******************************************/

#define GPIO_PICOCALC_SD_MISO    16
#define GPIO_PICOCALC_SD_CS      17
#define GPIO_PICOCALC_SD_SCK     18
#define GPIO_PICOCALC_SD_MOSI    19
#define GPIO_PICOCALC_SD_DET     22   /* active low, R301 10k pull-up */

/* Keyboard co-processor -- STM32F103R8T6 on I2C1 ***************************/

#define GPIO_PICOCALC_KBD_SDA    6
#define GPIO_PICOCALC_KBD_SCL    7
#define PICOCALC_KBD_I2C_ADDR    0x1f  /* legacy protocol */
#define PICOCALC_KBD_I2C_ADDR_V2 0x1e  /* extended protocol; NACK => stock fw */

/* The two spare wires to the co-processor (the unused USART3 pair).  The
 * stock BIOS v1.6 drives neither; both require the extended firmware, which
 * is probed for by trying PICOCALC_KBD_I2C_ADDR_V2.
 */

#define GPIO_PICOCALC_KBD_ATTN   9    /* in:  STM32 PC10, attention/IRQ */
#define GPIO_PICOCALC_KBD_WAKE   8    /* out: STM32 PC11, host wake request */

/* PWM audio ****************************************************************/

#define GPIO_PICOCALC_AUDIO_L    26
#define GPIO_PICOCALC_AUDIO_R    27

/* Mainboard PSRAM -- ESP-PSRAM64H ******************************************/

/* Redundant on a Pimoroni Plus 2/2W, which carries its own 8MB APS6404 on
 * the QMI at GP47, memory-mapped at 0x11000000.  The mainboard part would
 * need a PIO QSPI engine NuttX does not have, and is not memory-mappable, so
 * these pins are parked instead (see rp23xx_pinpark.c).
 */

#define GPIO_PICOCALC_MBRAM_SIO0 2
#define GPIO_PICOCALC_MBRAM_SIO1 3
#define GPIO_PICOCALC_MBRAM_SIO2 4
#define GPIO_PICOCALC_MBRAM_SIO3 5
#define GPIO_PICOCALC_MBRAM_CS   20   /* R302 10k pull-up to 3V3_OUT */
#define GPIO_PICOCALC_MBRAM_SCK  21

/* Free *********************************************************************/

/* GP28 is brought out to the mainboard debug header J703 pin 7 and is
 * otherwise unused -- the only genuinely spare GPIO on the connector.
 */

#define GPIO_PICOCALC_SPARE      28

#endif /* __BOARDS_ARM_RP23XX_CLOCKWORKPI_PICOCALC_INCLUDE_PICOCALC_H */
