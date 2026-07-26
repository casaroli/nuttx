/****************************************************************************
 * boards/arm/qemu/qemu-armv7a/scripts/memory.ld
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

#ifdef CONFIG_BUILD_PROTECTED

/* Split the flash and the RAM between the kernel and the user blob.
 *
 * Every boundary here has to be 1MB aligned.  What draws the kernel/user
 * boundary on this board is a set of L1 MMU sections programmed once at boot
 * by qemu_userspace(), and an L1 section is 1MB -- so a boundary that fell
 * inside a section would either leave part of the kernel reachable from user
 * code or leave part of user space unreachable.  CONFIG_NUTTX_USERSPACE is
 * what has to be 1MB aligned for the flash split; the RAM split is a fixed
 * fraction and is aligned by construction.
 *
 * The user RAM deliberately stops short of the end of RAM.  The L1 page
 * table itself lives in the last PGTABLE_SIZE bytes of it (see
 * PGTABLE_BASE_PADDR in arch/arm/src/qemu/chip.h), and user code must not be
 * able to reach the table that defines what it can reach.
 */

#  define KFLASH_SIZE (CONFIG_NUTTX_USERSPACE - CONFIG_FLASH_START)
#  define KSRAM_SIZE  (CONFIG_RAM_SIZE / 2)
#  define USRAM_SIZE  (CONFIG_RAM_SIZE / 4)

#else

/* Give all of the flash and RAM to the kernel */

#  define KFLASH_SIZE (CONFIG_FLASH_SIZE)
#  define KSRAM_SIZE  (CONFIG_RAM_SIZE)
#  define USRAM_SIZE  (0)

#endif

#define KFLASH_START  (CONFIG_FLASH_START)
#define KSRAM_START   (CONFIG_RAM_START)

#define UFLASH_START  (KFLASH_START + KFLASH_SIZE)
#define UFLASH_SIZE   (CONFIG_FLASH_SIZE - KFLASH_SIZE)
#define USRAM_START   (KSRAM_START + KSRAM_SIZE)

MEMORY
{
  kflash (rx) : ORIGIN = KFLASH_START, LENGTH = KFLASH_SIZE
  uflash (rx) : ORIGIN = UFLASH_START, LENGTH = UFLASH_SIZE
  ksram (rwx) : ORIGIN = KSRAM_START,  LENGTH = KSRAM_SIZE
  usram (rwx) : ORIGIN = USRAM_START,  LENGTH = USRAM_SIZE
}
