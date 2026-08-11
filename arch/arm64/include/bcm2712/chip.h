/****************************************************************************
 * arch/arm64/include/bcm2712/chip.h
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

#ifndef __ARCH_ARM64_INCLUDE_BCM2712_CHIP_H
#define __ARCH_ARM64_INCLUDE_BCM2712_CHIP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Number of bytes in x kibibytes/mebibytes/gibibytes */

#define KB(x)           ((x) << 10)
#define MB(x)           (KB(x) << 10)
#define GB(x)           (MB(UINT64_C(x)) << 10)

#if defined(CONFIG_ARCH_CHIP_BCM2712)

/* The BCM2712 has a GIC-400, which is a GICv2.  Both it and every other
 * peripheral live above the 4 GiB line.  The device tree gives the SoC
 * bus as
 * 0x7c000000 translated to 0x10_7c000000, so these are 40-bit
 * addresses and the MMU tables have to reach them.
 */

#define CONFIG_GICD_BASE           0x107fff9000  /* Distributor */
#define CONFIG_GICR_BASE           0x107fffa000  /* CPU interface */

/* DRAM starts at zero.  Only the first gigabyte is described here, which
 * every Raspberry Pi 5 variant has;  the firmware reports the rest.
 */

#define CONFIG_RAMBANK1_ADDR       0x00000000
#define CONFIG_RAMBANK1_SIZE       MB(1024)

/* Every BCM2712 peripheral sits in this one window, the GIC included, so a
 * single device mapping covers the lot.
 */

#define CONFIG_DEVICEIO_BASEADDR   0x107c000000
#define CONFIG_DEVICEIO_SIZE       MB(64)

#define CONFIG_LOAD_BASE           0x00080000

#define MPID_TO_CLUSTER_ID(mpid)   ((mpid) & ~0xff)

#endif

/****************************************************************************
 * Assembly Macros
 ****************************************************************************/

#ifdef __ASSEMBLY__

.macro  get_cpu_id xreg0
  mrs    \xreg0, mpidr_el1
  ubfx   \xreg0, \xreg0, #0, #8
.endm

#endif /* __ASSEMBLY__ */

#endif /* __ARCH_ARM64_INCLUDE_BCM2712_CHIP_H */
