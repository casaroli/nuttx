/****************************************************************************
 * boards/arm64/qemu/qemu-armv8a/scripts/memory.ld
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

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Split DRAM between kernel and user spaces.
 *
 *   +---------------------+ CONFIG_RAM_START
 *   | Device tree         |  placed here by QEMU
 *   +---------------------+ KSRAM_START
 *   | Kernel text         |
 *   | Kernel data         |
 *   +---------------------+ CONFIG_NUTTX_USERSPACE
 *   | User text           |
 *   | User data           |
 *   +---------------------+
 *   | Kernel heap         |  CONFIG_MM_KERNEL_HEAPSIZE, above the user .bss
 *   +---------------------+
 *   | User heap           |
 *   +---------------------+ CONFIG_RAM_END
 *
 * The kernel does not start at CONFIG_RAM_START:  QEMU's virt machine puts
 * the flattened device tree at the base of DRAM, and the kernel image is
 * linked to the address u-boot would load it to.  That leaves the first
 * 2.5MiB of DRAM alone, which is also where the device tree is read from.
 *
 * The kernel heap sits above the user .bss rather than inside the kernel
 * region because that is where up_allocate_kheap() in arm64_allocateheap.c
 * puts it.  qemu_userspace_mmuregions() leaves that window mapped
 * privileged-only, so it is protected wherever it lands.
 */

#define KSRAM_START   0x40280000
#define KSRAM_END     CONFIG_NUTTX_USERSPACE
#define KSRAM_SIZE    (KSRAM_END - KSRAM_START)
#define USRAM_START   CONFIG_NUTTX_USERSPACE
#define USRAM_END     CONFIG_RAM_END
#define USRAM_SIZE    (USRAM_END - USRAM_START)

MEMORY
{
  ksram (rwx) : ORIGIN = KSRAM_START, LENGTH = KSRAM_SIZE
  usram (rwx) : ORIGIN = USRAM_START, LENGTH = USRAM_SIZE
}
