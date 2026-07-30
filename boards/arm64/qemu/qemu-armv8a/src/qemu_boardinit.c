/****************************************************************************
 * boards/arm64/qemu/qemu-armv8a/src/qemu_boardinit.c
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
#include <debug.h>
#include <nuttx/board.h>

#ifdef CONFIG_BOARDCTL_ROMDISK
#  include <nuttx/drivers/ramdisk.h>
#  include "romfs.h"
#endif

#include "qemu-armv8a.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_BOARDCTL_ROMDISK
#  define SECTORSIZE   512
#  define NSECTORS(b)  (((b) + SECTORSIZE - 1) / SECTORSIZE)
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: qemu_memory_initialize
 *
 * Description:
 *   All qemu architectures must provide the following entry point.  This
 *   entry point is called early in the initialization before memory has
 *   been configured.  This board-specific function is responsible for
 *   configuring any on-board memories.
 *
 *   Logic in qemu_memory_initialize must be careful to avoid using any
 *   global variables because those will be uninitialized at the time this
 *   function is called.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void qemu_memory_initialize(void)
{
  /* SDRAM was initialized by a bootloader in the supported configurations. */
}

/****************************************************************************
 * Name: qemu_board_initialize
 *
 * Description:
 *   All qemu architectures must provide the following entry point.  This
 *   entry point is called in the initialization phase -- after
 *   qemu_memory_initialize and after all memory has been configured and
 *   mapped but before any devices have been initialized.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void qemu_board_initialize(void)
{
#ifdef CONFIG_ARCH_LEDS
  /* Configure on-board LEDs if LED support has been selected. */

#endif
}

/****************************************************************************
 * Name: board_late_initialize
 *
 * Description:
 *   If CONFIG_BOARD_LATE_INITIALIZE is selected, then an additional
 *   initialization call will be performed in the boot-up sequence to a
 *   function called board_late_initialize(). board_late_initialize() will be
 *   called immediately after up_intitialize() is called and just before the
 *   initial application is started.  This additional initialization phase
 *   may be used, for example, to initialize board-specific device drivers.
 *
 ****************************************************************************/

#ifdef CONFIG_BOARD_LATE_INITIALIZE
void board_late_initialize(void)
{
  /* Perform board initialization */

  qemu_bringup();

#ifdef CONFIG_BOARDCTL_ROMDISK
  /* Register the ROMFS image that holds the applications, so that
   * nx_start_application() can mount it.  A kernel build needs this because
   * its applications are separate ELF files rather than part of the blob;
   * carrying them in the image is what lets it boot without a host
   * filesystem behind semihosting.
   *
   * romfs_img is weak, and the placeholder in romfs_stub.c is one byte, so a
   * length of one sector or less means no image was linked in.
   */

  if (NSECTORS(romfs_img_len) > 1)
    {
      int ret = romdisk_register(0, romfs_img, NSECTORS(romfs_img_len),
                                 SECTORSIZE);
      if (ret < 0)
        {
          ferr("ERROR: Failed to register romfs: %d\n", -ret);
        }
    }
#endif
}
#endif /* CONFIG_BOARD_LATE_INITIALIZE */
