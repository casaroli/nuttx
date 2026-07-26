/****************************************************************************
 * arch/arm/src/qemu/qemu_userspace.c
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

#include <stdint.h>
#include <assert.h>
#include <sys/param.h>
#include <nuttx/userspace.h>
#include <arch/barriers.h>

#include <arch/board/board_memorymap.h>

#ifdef CONFIG_ARCH_ARMV7A
#  include "mmu.h"
#else
#  include "mpu.h"
#endif

#include "qemu_userspace.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: qemu_user_regions
 *
 * Description:
 *   Hand user space the two regions it is entitled to:  its own text and
 *   rodata read-only and executable, and its own data, bss, heap and stacks
 *   read/write and execute-never.  Everything else -- the kernel blob, the
 *   kernel heap and the page tables -- is left privileged-only by the
 *   mappings made at boot, and that is the whole of the protection in this
 *   build.
 *
 ****************************************************************************/

#ifdef CONFIG_ARCH_ARMV7A

static void qemu_map_sections(uintptr_t start, size_t size,
                              uint32_t mmuflags)
{
  uintptr_t addr;

  /* An L1 section is the smallest thing these flags can describe, so the
   * region has to be section aligned.  boards/.../scripts/memory.ld is where
   * that is arranged; see the note there.
   */

  DEBUGASSERT((start & SECTION_MASK) == 0 && (size & SECTION_MASK) == 0);

  for (addr = start; addr < start + size; addr += SECTION_SIZE)
    {
      /* Identity mapped, as everything on this board is */

      mmu_l1_setentry(addr, addr, mmuflags);
    }
}

static void qemu_user_regions(void)
{
  qemu_map_sections(UFLASH_START, UFLASH_SIZE, MMU_UTEXTSECTFLAGS);
  qemu_map_sections(USRAM_START, USRAM_SIZE, MMU_UDATASECTFLAGS);
}

#else

static void qemu_user_regions(void)
{
  mpu_showtype();
  mpu_user_flash(UFLASH_START, UFLASH_SIZE);
  mpu_user_intsram(USRAM_START, USRAM_SIZE);
  mpu_control(true);
}

#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: qemu_userspace
 *
 * Description:
 *   For the case of the separate user-/kernel-space build, perform whatever
 *   platform specific initialization of the user memory is required.
 *   Normally this just means initializing the user space .data and .bss
 *   segments.
 *
 * Assumptions:
 *   The D-Cache has not yet been enabled.
 *
 ****************************************************************************/

void qemu_userspace(void)
{
  uint8_t   *src;
  uint8_t   *dest;
  uint8_t   *end;
  uintptr_t  datastart;
  uintptr_t  dataend;

  /* Clear all of user-space .bss */

  DEBUGASSERT(USERSPACE->us_bssstart != 0 &&
              USERSPACE->us_bssend != 0 &&
              USERSPACE->us_bssstart <= USERSPACE->us_bssend);

  dest = (uint8_t *)USERSPACE->us_bssstart;
  end  = (uint8_t *)USERSPACE->us_bssend;

  while (dest != end)
    {
      *dest++ = 0;
    }

  /* Initialize all of user-space .data */

  DEBUGASSERT(USERSPACE->us_datasource != 0 &&
              USERSPACE->us_datastart != 0 &&
              USERSPACE->us_dataend != 0 &&
              USERSPACE->us_datastart <= USERSPACE->us_dataend);

  src  = (uint8_t *)USERSPACE->us_datasource;
  dest = (uint8_t *)USERSPACE->us_datastart;
  end  = (uint8_t *)USERSPACE->us_dataend;

  while (dest != end)
    {
      *dest++ = *src++;
    }

  DEBUGASSERT(USERSPACE->us_textend >= USERSPACE->us_textstart);

  datastart = MIN(USERSPACE->us_datastart, USERSPACE->us_bssstart);
  dataend   = MAX(USERSPACE->us_dataend,   USERSPACE->us_bssend);

  DEBUGASSERT(dataend >= datastart);

  /* Configure user FLASH and SRAM spaces */

  qemu_user_regions();
}
