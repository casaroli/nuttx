/****************************************************************************
 * boards/arm64/bcm2712/raspberrypi-5b/src/rpi5b_bringup.c
 *
 * SPDX-License-Identifier: Apache-2.0
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

#include <sys/types.h>
#include <syslog.h>

#include <nuttx/debug.h>
#include "rpi5b.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rpi5b_bringup
 *
 * Description:
 *   Bring up board features.
 *
 *   The Raspberry Pi 5 keeps almost every peripheral behind the RP1
 *   southbridge, which this port does not drive yet.  What the BCM2712
 *   provides directly is the debug UART, the GIC and the architected timer,
 *   and those are brought up by the chip layer.  So there is nothing to do
 *   here until a peripheral lands.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rpi5b_bringup(void)
{
  return OK;
}
