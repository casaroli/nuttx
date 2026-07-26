/****************************************************************************
 * arch/arm64/src/qemu/qemu_userspace.c
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
#include <string.h>
#include <assert.h>
#include <nuttx/userspace.h>
#include <arch/barriers.h>

#include "arm64_internal.h"
#include "arm64_mmu.h"
#include "qemu_userspace.h"

#ifdef CONFIG_BUILD_PROTECTED

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The kernel/user split below is the one up_allocate_heap() and
 * up_allocate_kheap() in arm64_allocateheap.c build, and the MMU can only
 * draw the boundaries on page edges.
 */

#ifndef CONFIG_MM_KERNEL_HEAP
#  error "CONFIG_BUILD_PROTECTED needs CONFIG_MM_KERNEL_HEAP on this board"
#endif

#if (CONFIG_MM_KERNEL_HEAPSIZE & MMU_PAGE_MASK) != 0
#  error "CONFIG_MM_KERNEL_HEAPSIZE must be a multiple of the MMU page size"
#endif

#if (CONFIG_NUTTX_USERSPACE & MMU_PAGE_MASK) != 0
#  error "CONFIG_NUTTX_USERSPACE must be a multiple of the MMU page size"
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: qemu_user_mmuregion
 *
 * Description:
 *   Re-map one window of DRAM so that unprivileged code can reach it.  Every
 *   region is mapped privileged-only by the DRAM entry in g_mmu_regions[],
 *   so this is what opens up the parts that belong to user space -- and
 *   anything not opened here, the kernel blob and the kernel heap, stays
 *   unreachable from EL0.  That is the whole of the protection here.
 *
 * Input Parameters:
 *   name  - Region name, for the MMU debug output
 *   start - First address of the window; must be page aligned
 *   end   - One past the last address of the window; must be page aligned
 *   attrs - MT_* attributes to apply, over and above MT_USER
 *
 ****************************************************************************/

static void qemu_user_mmuregion(const char *name, uintptr_t start,
                                uintptr_t end, unsigned int attrs)
{
  const struct arm_mmu_region region =
    MMU_REGION_FLAT_ENTRY(name, start, end - start,
                          MT_NORMAL | MT_SECURE | MT_USER | attrs);

  DEBUGASSERT(end > start);
  DEBUGASSERT((start & MMU_PAGE_MASK) == 0);
  DEBUGASSERT((end & MMU_PAGE_MASK) == 0);

  arm64_mmu_set_memregion(&region);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: qemu_userspace
 *
 * Description:
 *   For the case of the separate user-/kernel-space build, perform whatever
 *   platform specific initialization of the user memory is required.
 *
 *   The user blob is loaded into DRAM alongside the kernel rather than being
 *   copied out of ROM, so .data is already where it belongs and only .bss
 *   has to be cleared.  What is left is to hand user space the MMU mappings
 *   it needs to run at all.
 *
 * Assumptions:
 *   Called from arm64_chip_boot() once the MMU is enabled.
 *
 ****************************************************************************/

void qemu_userspace(void)
{
  uintptr_t uheap;

  /* Clear all of user-space .bss */

  DEBUGASSERT(USERSPACE->us_bssstart != 0 &&
              USERSPACE->us_bssend != 0 &&
              USERSPACE->us_bssstart <= USERSPACE->us_bssend);

  memset((void *)USERSPACE->us_bssstart, 0,
         USERSPACE->us_bssend - USERSPACE->us_bssstart);

  /* The read-only half of the blob:  everything from the userspace_s
   * structure at CONFIG_NUTTX_USERSPACE through .text and .rodata.  EL0 may
   * read and execute it; the MT_USER handling in arm64_mmu.c also makes it
   * privileged-execute-never.
   */

  qemu_user_mmuregion("USER_TEXT", CONFIG_NUTTX_USERSPACE,
                      USERSPACE->us_datastart, MT_RO | MT_EXECUTE);

  /* The writable half:  .data, .init_section and .bss */

  qemu_user_mmuregion("USER_DATA", USERSPACE->us_datastart,
                      USERSPACE->us_bssend, MT_RW | MT_EXECUTE_NEVER);

  /* The user heap.  up_allocate_kheap() takes CONFIG_MM_KERNEL_HEAPSIZE off
   * the top of the user .bss for the kernel heap, and the user heap is
   * everything above that; skipping the kernel heap here is what keeps it
   * out of reach of user code.
   */

  uheap = (uintptr_t)USERSPACE->us_bssend + CONFIG_MM_KERNEL_HEAPSIZE;

  qemu_user_mmuregion("USER_HEAP", uheap, CONFIG_RAM_END,
                      MT_RW | MT_EXECUTE_NEVER);

  /* The windows above were already mapped, privileged-only, by the DRAM
   * entry in g_mmu_regions[], so drop any translations cached from that.
   */

  mmu_invalidate_tlbs();
  UP_ISB();
}

#endif /* CONFIG_BUILD_PROTECTED */
