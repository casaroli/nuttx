/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_allocateheap.c
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

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/userspace.h>

#include <arch/board/board.h>

#include "mpu.h"
#include "arm_internal.h"

#ifdef CONFIG_BUILD_PROTECTED

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* PMSAv8 MPU regions are a base/limit pair with 32-byte granularity.  The
 * user heap's base is rounded *up* to that so the region cannot be made to
 * start below it -- mpu_modify_region() masks the base down, which would
 * otherwise quietly hand unprivileged code the last few bytes of the
 * kernel's side of the boundary.
 */

#define MPU_ALIGN_UP(a)   (((uintptr_t)(a) + 31) & ~((uintptr_t)31))

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* Defined by the board's memory.ld -- see the comment there.  These are
 * linker-provided addresses, not storage.
 */

extern uint8_t _ksram_end[];
extern uint8_t _usram_end[];

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_allocate_heap
 *
 * Description:
 *   In the protected build this provides the *user* heap:  the unprivileged,
 *   user-space heap that task stacks and application allocations come from.
 *
 *   The generic arm_allocateheap.c cannot be used here for two reasons, and
 *   both of them are silent failures rather than build errors:
 *
 *   1. It never calls mpu_user_intsram() for the heap it hands out.  Only
 *      the user blob's .data and .bss are opened to unprivileged code, by
 *      rp23xx_userspace(); a task stack allocated from an unopened heap
 *      then faults on the first push.  That presents as a hardfault at the
 *      very first instruction of nxtask_startup() with a stack pointer that
 *      already looks correct, which reads like a bad entry point and is not.
 *
 *   2. It assumes one flat RAM in which the kernel heap sits between the
 *      user .bss and the user heap, and bounds the result with
 *      CONFIG_RAM_END.  This board's memory.ld splits SRAM into an explicit
 *      ksram/usram pair, so that layout would put the kernel heap inside the
 *      user half and, with CONFIG_RAM_SIZE describing only ksram, would
 *      compute a negative size.
 *
 ****************************************************************************/

void up_allocate_heap(void **heap_start, size_t *heap_size)
{
  uintptr_t ubase = MPU_ALIGN_UP(USERSPACE->us_bssend);
  uintptr_t uend  = (uintptr_t)_usram_end;

  DEBUGASSERT(ubase < uend);

  board_autoled_on(LED_HEAPALLOCATE);

  *heap_start = (void *)ubase;
  *heap_size  = uend - ubase;

  /* Open the heap to unprivileged code.  This is a second user region on top
   * of the .data/.bss one rp23xx_userspace() programmed; they are disjoint
   * because the kernel heap does not sit between them on this board, but
   * they are kept separate anyway so that the gap between _ebss and the
   * aligned heap base is not silently granted.
   */

  mpu_user_intsram(ubase, uend - ubase);
}

/****************************************************************************
 * Name: up_allocate_kheap
 *
 * Description:
 *   Provide the kernel heap.  It occupies whatever is left of ksram above
 *   the kernel's own .bss and the IDLE thread stack, and no MPU region is
 *   opened for it:  privileged code reaches it through the default map
 *   (mpu_control() is called with privdefena true), and unprivileged code
 *   must not reach it at all.
 *
 *   CONFIG_MM_KERNEL_HEAPSIZE is deliberately not consulted.  On this board
 *   the kernel half is a linker-script region rather than a slice carved
 *   out of a shared RAM, so its size is already decided by memory.ld and
 *   taking a second bite out of the user side would only shrink the user
 *   heap.
 *
 ****************************************************************************/

#ifdef CONFIG_MM_KERNEL_HEAP
void up_allocate_kheap(void **heap_start, size_t *heap_size)
{
  uintptr_t kbase = (uintptr_t)g_idle_topstack;
  uintptr_t kend  = (uintptr_t)_ksram_end;

  DEBUGASSERT(kbase < kend);

  *heap_start = (void *)kbase;
  *heap_size  = kend - kbase;
}
#endif /* CONFIG_MM_KERNEL_HEAP */

#endif /* CONFIG_BUILD_PROTECTED */
