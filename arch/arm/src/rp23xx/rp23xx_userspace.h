/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_userspace.h
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

#ifndef __ARCH_ARM_SRC_RP23XX_RP23XX_USERSPACE_H
#define __ARCH_ARM_SRC_RP23XX_RP23XX_USERSPACE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_BUILD_PROTECTED

/****************************************************************************
 * Name: rp23xx_userspace
 *
 * Description:
 *   For the case of the separate user-/kernel-space build, perform whatever
 *   platform specific initialization of the user memory is required.
 *   Normally this just means initializing the user space .data and .bss
 *   segments, and programming the MPU regions that make the user blob
 *   reachable from unprivileged mode.
 *
 *   rp23xx_start.c has always called this function under
 *   CONFIG_BUILD_PROTECTED, but nothing ever defined it -- the call was
 *   unreachable because ARCH_CHIP_RP23XX selected neither ARCH_HAVE_MPU nor
 *   ARM_HAVE_MPU_UNIFIED, so BUILD_PROTECTED could not be selected for this
 *   chip at all.
 *
 ****************************************************************************/

void rp23xx_userspace(void);

#endif /* CONFIG_BUILD_PROTECTED */

#endif /* __ARCH_ARM_SRC_RP23XX_RP23XX_USERSPACE_H */
