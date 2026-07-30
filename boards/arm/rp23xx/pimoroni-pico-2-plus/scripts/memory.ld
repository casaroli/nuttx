/****************************************************************************
 * boards/arm/rp23xx/pimoroni-pico-2-plus/scripts/memory.ld
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

/* The Pimoroni Pico Plus 2 carries an RP2350B with 16 MB of QSPI flash mapped
 * at 0x10000000 and 520 KB of on-chip SRAM at 0x20000000 (512 KB in one
 * contiguous bank, plus two 4 KB scratch banks at the top which are left to
 * the two cores and are not part of this split).
 *
 * This is the BUILD_PROTECTED memory map.  Two properties drive it:
 *
 * 1. The Cortex-M33 implements PMSAv8, whose MPU regions are an arbitrary
 *    base/limit pair with 32-byte granularity.  It does NOT have PMSAv7's
 *    power-of-two size and natural-alignment rule, so these boundaries are
 *    chosen to fit the images rather than rounded up to a power of two.  On
 *    an armv7-m part the user flash would have had to start at a 4 MB or
 *    8 MB boundary regardless of how big the kernel actually is.
 *
 * 2. The kernel half must start at 0x10000000, because the RP2350 bootrom
 *    looks for the IMAGE_DEF block within the first 4 KB of flash and enters
 *    the image through it.  Only the kernel blob is entered by the bootrom;
 *    nuttx_user.elf is ordinary data as far as boot is concerned, copied and
 *    jumped to by the kernel once the MPU is up.
 *
 * The 1 MB kernel / 15 MB user flash split is far more generous than either
 * blob needs (the flat image is under 300 KB) and costs nothing -- unused
 * flash is simply not programmed.  The SRAM is split evenly at 256 KB.
 */

MEMORY
{
  kflash (rx)  : ORIGIN = 0x10000000, LENGTH = 1M
  uflash (rx)  : ORIGIN = 0x10100000, LENGTH = 15M

  ksram (rwx)  : ORIGIN = 0x20000000, LENGTH = 256K
  usram (rwx)  : ORIGIN = 0x20040000, LENGTH = 256K

  /* The two 4 KB scratch banks sit immediately above usram, at the top of
   * SRAM.  They hold the two cores' startup stacks and stay kernel-owned in
   * a protected build, exactly as they are in the flat one.  usram ends at
   * 0x20080000, which is where SCRATCH_X begins, so nothing overlaps.
   */

  SCRATCH_X (rwx) : ORIGIN = 0x20080000, LENGTH = 4K
  SCRATCH_Y (rwx) : ORIGIN = 0x20081000, LENGTH = 4K
}

_ram_size = LENGTH(ksram) + LENGTH(usram);

/* The heap allocator needs the two halves' boundaries, and this file is the
 * only place that knows them -- rp23xx_allocateheap.c reads them from here
 * rather than recomputing the split from CONFIG_RAM_START/CONFIG_RAM_SIZE,
 * so the map cannot be changed in one place and not the other.
 */

_ksram_start = ORIGIN(ksram);
_ksram_end   = ORIGIN(ksram) + LENGTH(ksram);
_usram_start = ORIGIN(usram);
_usram_end   = ORIGIN(usram) + LENGTH(usram);
