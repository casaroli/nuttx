/****************************************************************************
 * include/nuttx/input/picocalc_kbd.h
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

#ifndef __INCLUDE_NUTTX_INPUT_PICOCALC_KBD_H
#define __INCLUDE_NUTTX_INPUT_PICOCALC_KBD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/irq.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The co-processor's two slave addresses.
 *
 * The legacy one is what the stock ClockworkPi BIOS serves.  The second is
 * served only by the replacement firmware, and probing it is how this driver
 * decides which protocol to use -- a NAK means stock.
 */

#define PICOCALC_KBD_ADDR      0x1f
#define PICOCALC_KBD_ADDR_V2   0x1e

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
 * Name: picocalc_kbd_attach_t
 *
 * Description:
 *   Install the board's handler for the co-processor's attention line.
 *
 *   The replacement firmware drives a line low while it has events waiting.
 *   Which pin that is, and how an interrupt is attached to it, is board
 *   knowledge; that it wants a falling edge is not, because the line is a
 *   level and a level-triggered interrupt would re-fire until serviced.
 *
 *   A board with no such line passes NULL and the driver polls instead.
 *
 ****************************************************************************/

typedef CODE int (*picocalc_kbd_attach_t)(xcpt_t isr, FAR void *arg);

/****************************************************************************
 * Name: picocalc_kbd_register
 *
 * Description:
 *   Register the ClockworkPi PicoCalc's STM32F103 keyboard co-processor as
 *   a keyboard device.
 *
 *   Two protocols are supported and the choice is made once, by probing.
 *
 *   The stock ClockworkPi BIOS serves only the legacy address, where the
 *   only way to learn about a keypress is to poll and every event costs two
 *   transactions -- a register select, a STOP, and a read.  Polling is
 *   mandatory rather than convenient there: the stock firmware re-inits its
 *   own I2C slave if neither of its callbacks has run for 2500ms.
 *
 *   The replacement firmware serves a second address where one bare read
 *   returns up to eight events with their status, and drives an attention
 *   line while any are waiting.  Given `attach`, the driver then does no
 *   periodic polling worth the name: it reads when told to, which takes key
 *   latency from the poll interval down to one transaction.
 *
 * Input Parameters:
 *   devpath   - The device path, e.g. "/dev/kbd0".
 *   i2c       - An instance of the I2C interface to use.
 *   addr      - The co-processor's legacy I2C address, normally
 *               PICOCALC_KBD_ADDR.  The extended address is fixed.
 *   frequency - The bus frequency in Hz.
 *   attach    - Installs the attention line handler, or NULL.  Ignored if
 *               the co-processor turns out to be running stock firmware,
 *               which drives no such line.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

int picocalc_kbd_register(FAR const char *devpath,
                          FAR struct i2c_master_s *i2c,
                          uint8_t addr, uint32_t frequency,
                          picocalc_kbd_attach_t attach);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __INCLUDE_NUTTX_INPUT_PICOCALC_KBD_H */
