/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_start.c
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
#include <stdbool.h>
#include <assert.h>
#include <nuttx/debug.h>
#include <sys/param.h>

#include <nuttx/init.h>
#include <arch/board/board.h>

#include "arm_internal.h"
#include "rp23xx_config.h"
#include "rp23xx_clock.h"
#include "rp23xx_uart.h"
#include "hardware/rp23xx_sio.h"

#ifdef CONFIG_RP23XX_PM_SUSPEND
#  include "rp23xx_pm.h"
#  include "rp23xx_serial.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define IDLE_STACK ((uint32_t)_ebss + CONFIG_IDLETHREAD_STACKSIZE)

/****************************************************************************
 * Public Data
 ****************************************************************************/

const uintptr_t g_idle_topstack = IDLE_STACK;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: showprogress
 *
 * Description:
 *   Print a character on the UART to show boot status.
 *
 ****************************************************************************/

#if defined(CONFIG_DEBUG_FEATURES) && defined(HAVE_SERIAL_CONSOLE)
#  define showprogress(c) arm_lowputc((uint32_t)c)
#else
#  define showprogress(c)
#endif

/****************************************************************************
 * Name: rp23xx_hwinit
 *
 * Description:
 *   Bring up everything that lives in the switched core: the clock tree, the
 *   board's early pin setup, the spinlocks, the FPU, the console and the
 *   board's own hardware.
 *
 *   This is split out of __start because a resume from suspend to RAM needs
 *   precisely this and nothing else.  Powering the switched core down loses
 *   every register in it, so all of it has to be built again; what must not
 *   happen on that path is the memory initialisation around it, since RAM is
 *   the one thing that was retained.  Sharing the sequence rather than
 *   copying it is the point -- a second copy of the boot path is what makes
 *   a resume drift away from a cold boot over time.
 *
 * Input Parameters:
 *   resume - True when this is a resume rather than a cold boot.
 *
 ****************************************************************************/

static void rp23xx_hwinit(bool resume)
{
  size_t i;

  /* Which of the steps below care about a resume depends on the
   * configuration, and in the smallest one none of them do.
   */

  UNUSED(resume);

  /* Errata RP2350-E2 SIO SPINLOCK writes are mirrored at +0x80 offset
   * Use only safe SPINLOCKS
   * The following SIO spinlocks can be used normally as they do not alias
   * with writable registers: 5, 6, 7, 10, 11, and 18 through 31.
   */

  const uint8_t safe_spinlocks[] =
  {
    5,  6,  7,  10, 11, 18, 19,
    20, 21, 22, 23, 24, 25, 26,
    27, 28, 29, 30, 31
  };

  /* Set up clock */

  rp23xx_clockconfig();
  rp23xx_boardearlyinitialize();

  /* Initialize spinlock states
   * Errata RP2350-E2 SIO SPINLOCK writes are mirrored at +0x80 offset
   * Use only safe SPINLOCKS
   * The following SIO spinlocks can be used normally as they do not alias
   * with writable registers: 5, 6, 7, 10, 11, and 18 through 31.
   */

  for (i = 0; i < nitems(safe_spinlocks); i++)
    {
      putreg32(0, RP23XX_SIO_SPINLOCK(safe_spinlocks[i]));
    }

  /* Initialize the FPU */

  arm_fpuconfig();

  /* Configure the uart so that we can get debug output as soon as possible */

  rp23xx_lowsetup();
  showprogress('A');

  /* Perform early serial initialization.  A resume takes the other route:
   * the UARTs are reset but the driver is not, so what it remembers about
   * them outranks what they now say about themselves.
   */

#ifdef USE_EARLYSERIALINIT
#  ifdef CONFIG_RP23XX_PM_SUSPEND
  if (resume)
    {
      rp23xx_serial_resume();
    }
  else
#  endif
    {
      arm_earlyserialinit();
    }

#endif
  showprogress('B');

  /* For the case of the separate user-/kernel-space build, perform whatever
   * platform specific initialization of the user memory is required.
   * Normally this just means initializing the user space .data and .bss
   * segments.
   */

#ifdef CONFIG_BUILD_PROTECTED
  /* Not on a resume, though: the user-space data and bss hold the state
   * being resumed, exactly as the kernel's do.
   */

  if (!resume)
    {
      rp23xx_userspace();
      showprogress('C');
    }
#endif

  /* Initialize onboard resources */

  rp23xx_boardinitialize();
  showprogress('D');
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef CONFIG_RP23XX_PM_SUSPEND

/****************************************************************************
 * Name: rp23xx_resume_boot
 *
 * Description:
 *   The tail of a boot that turned out to be a resume from suspend to RAM.
 *   Entered from __start by branch rather than by call, on a stack of its
 *   own, and does not return.
 *
 *   Public only because __start reaches it from inline assembly; nothing
 *   else should call it.
 *
 ****************************************************************************/

void rp23xx_resume_boot(void)
{
  rp23xx_hwinit(true);

  showprogress('\r');
  showprogress('\n');

  /* Hand back to the thread that suspended, in place of starting an OS that
   * is, as far as everything in RAM is concerned, already running.
   */

  rp23xx_pm_resume();

  for (; ; );
}
#endif

/****************************************************************************
 * Name: __start
 *
 * Description:
 *   This is the reset entry point.
 *
 ****************************************************************************/

void __start(void)
{
#ifdef CONFIG_BOOT_RUNFROMFLASH
  const uint32_t *src;
#endif
  uint32_t       *dest;

  /* Set MSP to the top of the IDLE stack */

  __asm__ __volatile__ ("\tmsr msp, %0\n" :: "r" (g_idle_topstack));

  if (this_cpu() != 0)
    {
      while (1)
        {
          __asm__ volatile ("wfe");
        }
    }

#ifdef CONFIG_RP23XX_PM_SUSPEND
  /* A resume from suspend to RAM arrives here indistinguishable from a cold
   * boot: powering the switched core down means the chip comes back through
   * the bootrom, with SRAM still holding a running system.  Ask before
   * anything can destroy the evidence, or the state it protects.
   *
   * Then leave this stack immediately, because it is the IDLE *thread's*
   * stack and on a resume it is in use: a resume does not restart the idle
   * thread, it merely schedules it again, and nx_start()'s frame -- which is
   * where the idle loop itself runs -- reaches all the way to the top of it.
   *
   * Nothing may be called here beyond the check, which is why the boot
   * continues on a stack of its own rather than a few lines further down.
   * What this function has already written is the handful of bytes its own
   * prologue pushed, in the register save slots at the very top of that
   * frame, and those are only read back if nx_start() returns, which is the
   * one thing an idle loop never does.
   */

  if (rp23xx_pm_resume_pending())
    {
      __asm__ __volatile__
      (
        "msr msp, %0\n"
        "b   rp23xx_resume_boot\n"
        :
        : "r" (&g_pm_resume_stack[RP23XX_PM_RESUME_STACK_WORDS])
        : "memory"
      );
    }
#endif

  /* Clear .bss.  We'll do this inline (vs. calling memset) just to be
   * certain that there are no issues with the state of global variables.
   */

  for (dest = (uint32_t *)_sbss; dest < (uint32_t *)_ebss; )
    {
      *dest++ = 0;
    }

  /* Move the initialized data section from its temporary holding spot in
   * FLASH into the correct place in SRAM.  The correct place in SRAM is
   * give by _sdata and _edata.  The temporary location is in FLASH at the
   * end of all of the other read-only data (.text, .rodata) at _eronly.
   */

#ifdef CONFIG_BOOT_RUNFROMFLASH
  for (src = (const uint32_t *)_eronly,
       dest = (uint32_t *)_sdata; dest < (uint32_t *)_edata;
      )
    {
      *dest++ = *src++;
    }
#endif

  rp23xx_hwinit(false);

  /* Then start NuttX */

  showprogress('\r');
  showprogress('\n');

  nx_start();

  /* Shouldn't get here */

  for (; ; );
}
