/****************************************************************************
 * boards/arm/qemu/qemu-armv7a/include/board_memorymap.h
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

#ifndef __BOARDS_ARM_QEMU_QEMU_ARMV7A_INCLUDE_BOARD_MEMORYMAP_H
#define __BOARDS_ARM_QEMU_QEMU_ARMV7A_INCLUDE_BOARD_MEMORYMAP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The kernel/user split, as laid out by scripts/memory.ld.  In a flat build
 * the kernel owns everything and the user regions are empty.
 */

/* Kernel code (RX) and kernel RAM (RW) */

#define KFLASH_START    (uintptr_t)__kflash_start
#define KFLASH_SIZE     (uintptr_t)__kflash_size
#define KSRAM_START     (uintptr_t)__ksram_start
#define KSRAM_SIZE      (uintptr_t)__ksram_size
#define KSRAM_END       (uintptr_t)__ksram_end

/* User code (RX) and user RAM (RW) */

#define UFLASH_START    (uintptr_t)__uflash_start
#define UFLASH_SIZE     (uintptr_t)__uflash_size
#define USRAM_START     (uintptr_t)__usram_start
#define USRAM_SIZE      (uintptr_t)__usram_size
#define USRAM_END       (uintptr_t)__usram_end

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern uint8_t          __kflash_start[];
extern uint8_t          __kflash_size[];
extern uint8_t          __ksram_start[];
extern uint8_t          __ksram_size[];
extern uint8_t          __ksram_end[];

extern uint8_t          __uflash_start[];
extern uint8_t          __uflash_size[];
extern uint8_t          __usram_start[];
extern uint8_t          __usram_size[];
extern uint8_t          __usram_end[];

#endif /* __BOARDS_ARM_QEMU_QEMU_ARMV7A_INCLUDE_BOARD_MEMORYMAP_H */
