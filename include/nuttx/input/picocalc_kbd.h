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

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The co-processor's legacy slave address, which is what this driver
 * speaks.  The extended protocol lives at 0x1e and is not handled here.
 */

#define PICOCALC_KBD_ADDR      0x1f

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
 * Name: picocalc_kbd_register
 *
 * Description:
 *   Register the ClockworkPi PicoCalc's STM32F103 keyboard co-processor as
 *   a keyboard device, and start polling it.
 *
 *   The co-processor has no interrupt line, so the driver polls.  That is
 *   not merely a convenience: it re-initialises its own I2C slave if
 *   neither of its callbacks has run for 2500ms, so a host that goes quiet
 *   gets a link that resets underneath it.
 *
 * Input Parameters:
 *   devpath   - The device path, e.g. "/dev/kbd0".
 *   i2c       - An instance of the I2C interface to use.
 *   addr      - The co-processor's I2C address, normally
 *               PICOCALC_KBD_ADDR.
 *   frequency - The bus frequency in Hz.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

int picocalc_kbd_register(FAR const char *devpath,
                          FAR struct i2c_master_s *i2c,
                          uint8_t addr, uint32_t frequency);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __INCLUDE_NUTTX_INPUT_PICOCALC_KBD_H */
