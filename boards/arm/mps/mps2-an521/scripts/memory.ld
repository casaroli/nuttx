/****************************************************************************
 * boards/arm/mps/mps2-an521/scripts/memory.ld
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

/* The BUILD_PROTECTED memory map for the AN521 image.
 *
 * The AN521 has two 4MB banks, and that is what makes this different from
 * mps3-an547:  that board has separate banks for the kernel and the user blob
 * (see its memory.ld and flash.ld, which name different regions entirely), so
 * its split falls out of the hardware.  Here there are only the two, so both
 * have to be divided down the middle:
 *
 *   SSRAM1   0x10000000, 4MB -- used as the code/"flash" bank
 *   SSRAM2_3 0x38000000, 4MB -- used as RAM
 *
 * The RAM boundary is not free to choose.  arch/arm/src/mps/mps_allocateheap.c
 * bounds the user heap with PRIMARY_RAM_END, which for this chip is
 * MPS_SRAM23_START + MPS_SRAM23_SIZE == 0x38400000 (see
 * arch/arm/src/mps/hardware/mps_memorymap.h).  usram therefore has to *end*
 * exactly there, or the heap it hands out runs past the region the MPU opens.
 *
 * That file also rounds the user heap down to a power of two and places it at
 * the top of PRIMARY RAM, which is a PMSAv7 habit rather than an Armv8-M
 * requirement -- the Cortex-M33's PMSAv8 regions are an arbitrary base/limit
 * pair at 32-byte granularity.  It costs about 1MB of the user half here and
 * is left alone: it is shared with mps3-an547 and is not this board's to
 * change.
 */

MEMORY
{
  kflash (rx)  : ORIGIN = 0x10000000, LENGTH = 2M
  uflash (rx)  : ORIGIN = 0x10200000, LENGTH = 2M

  ksram (rwx)  : ORIGIN = 0x38000000, LENGTH = 2M
  usram (rwx)  : ORIGIN = 0x38200000, LENGTH = 2M
}

_ram_size = LENGTH(ksram) + LENGTH(usram);
