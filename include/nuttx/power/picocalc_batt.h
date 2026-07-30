/****************************************************************************
 * include/nuttx/power/picocalc_batt.h
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

#ifndef __INCLUDE_NUTTX_POWER_PICOCALC_BATT_H
#define __INCLUDE_NUTTX_POWER_PICOCALC_BATT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/i2c/i2c_master.h>

#ifdef CONFIG_BATTERY_PICOCALC

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
 * Name: picocalc_batt_register
 *
 * Description:
 *   Register a battery gauge backed by the ClockworkPi PicoCalc's keyboard
 *   co-processor.
 *
 *   The AXP2101 PMU is not on this bus.  It hangs off a second I2C bus of
 *   the STM32F103 co-processor, which caches its state and serves it as a
 *   register here, so this driver is a gauge over a link rather than a gauge
 *   driver for a chip.  What that buys is that reading the battery costs one
 *   short transaction and never blocks on the PMU.
 *
 *   Two protocols are supported and the right one is chosen by probing.  The
 *   stock ClockworkPi BIOS serves only a percentage with a charging bit,
 *   which is enough for state, online and capacity but not voltage.  The
 *   replacement firmware serves a twelve byte summary with the cell voltage,
 *   VBUS, the charge state and -- the distinction the legacy protocol cannot
 *   express -- whether a battery is present at all, as opposed to present
 *   and flat.
 *
 * Input Parameters:
 *   devpath   - Device to register, conventionally "/dev/batt0".
 *   i2c       - An I2C master the co-processor is on.  May be shared: every
 *               access here is a single locked transfer.
 *   frequency - Bus frequency for those transfers.
 *
 * Returned Value:
 *   OK, or a negated errno.  -ENODEV means nothing answered on either
 *   address, which on this board means the co-processor is not running.
 *
 ****************************************************************************/

int picocalc_batt_register(FAR const char *devpath,
                           FAR struct i2c_master_s *i2c,
                           uint32_t frequency);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* CONFIG_BATTERY_PICOCALC */
#endif /* __INCLUDE_NUTTX_POWER_PICOCALC_BATT_H */
