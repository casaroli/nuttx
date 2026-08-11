/****************************************************************************
 * arch/arm64/src/bcm2712/bcm2712_boot.c
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
#include <nuttx/debug.h>

#include <nuttx/cache.h>
#include <nuttx/syslog/syslog_rpmsg.h>
#ifdef CONFIG_LEGACY_PAGING
#  include <nuttx/page.h>
#endif

#include <arch/chip/chip.h>

#ifdef CONFIG_SMP
#include "arm64_smp.h"
#endif

#include "arm64_arch.h"
#include "arm64_internal.h"
#include "arm64_mmu.h"
#include "bcm2712_boot.h"
#include "bcm2712_serial.h"

#ifdef CONFIG_DEVICE_TREE
#  include <nuttx/fdt.h>
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The BCM2712 puts its peripherals at 0x10_7c000000, above the 4 GiB line,
 * so the device region needs a 40-bit physical address.  The GIC-400 sits at
 * the top of that same window, which is why one entry covers both.
 */

static const struct arm_mmu_region g_mmu_regions[] =
{
  MMU_REGION_FLAT_ENTRY("DEVICE_REGION",
                        CONFIG_DEVICEIO_BASEADDR, CONFIG_DEVICEIO_SIZE,
                        MT_DEVICE_NGNRNE | MT_RW | MT_SECURE),

  MMU_REGION_FLAT_ENTRY("DRAM0_S0",
                        CONFIG_RAMBANK1_ADDR, CONFIG_RAMBANK1_SIZE,
                        MT_NORMAL | MT_RW | MT_SECURE),
};

const struct arm_mmu_config g_mmu_config =
{
  .num_regions = nitems(g_mmu_regions),
  .mmu_regions = g_mmu_regions,
};

#ifdef CONFIG_SYSLOG_RPMSG
static char g_syslog_rpmsg_buf[4096];
#endif

#ifdef CONFIG_RPI5B_DEBUG_BOOT

/****************************************************************************
 * Name: bcm2712_report_uart_divisor
 *
 * Description:
 *   Print the PL011 baud divisor the firmware programmed, as
 *   "IBRD=xxxx FBRD=xx".  The reference clock is then
 *   baud * 16 * (IBRD + FBRD / 64), which is what CONFIG_UART0_CLK_FREQ has
 *   to hold, because the PL011 driver recomputes the divisor when the
 *   console is opened.  Measuring it beats trusting a device tree that has
 *   already been wrong once.
 *
 *   Uses arm64_lowputc() and runs before arm64_mmu_init(), so the UART is
 *   reached by its physical address with the MMU still off.
 *
 ****************************************************************************/

static void bcm2712_puthex(uint32_t value, int digits)
{
  static const char hex[] = "0123456789abcdef";
  int shift;

  for (shift = (digits - 1) * 4; shift >= 0; shift -= 4)
    {
      arm64_lowputc(hex[(value >> shift) & 0xf]);
    }
}

static void bcm2712_putstr(const char *s)
{
  while (*s != '\0')
    {
      arm64_lowputc(*s++);
    }
}

static void bcm2712_report_uart_divisor(void)
{
  volatile const uint32_t *uart =
    (volatile const uint32_t *)CONFIG_UART0_BASE;

  bcm2712_putstr("\r\nBCM2712 UART IBRD=");
  bcm2712_puthex(uart[0x24 / 4], 4);
  bcm2712_putstr(" FBRD=");
  bcm2712_puthex(uart[0x28 / 4], 2);
  bcm2712_putstr("\r\n");
}

#endif /* CONFIG_RPI5B_DEBUG_BOOT */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef CONFIG_SMP

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm64_get_mpid
 *
 * Description:
 *   The function from cpu index to get cpu mpid which is reading
 * from mpidr_el1 register. Different ARM64 Core will use different
 * Affn define, the mpidr_el1 value is not CPU number, So we need
 * to change CPU number to mpid and vice versa
 *
 ****************************************************************************/

uint64_t arm64_get_mpid(int cpu)
{
  return CORE_TO_MPID(cpu, 0);
}

/****************************************************************************
 * Name: arm64_get_cpuid
 *
 * Description:
 *   The function from mpid to get cpu id
 *
 ****************************************************************************/

int arm64_get_cpuid(uint64_t mpid)
{
  return MPID_TO_CORE(mpid);
}

#endif /* CONFIG_SMP */

/****************************************************************************
 * Name: arm64_el_init
 *
 * Description:
 *   The function called from arm64_head.S at very early stage for these
 * platform, it's use to:
 *   - Handling special hardware initialize routine which is need to
 *     run at high ELs
 *   - Initialize system software such as hypervisor or security firmware
 *     which is need to run at high ELs
 *
 ****************************************************************************/

void arm64_el_init(void)
{
}

/****************************************************************************
 * Name: arm64_chip_boot
 *
 * Description:
 *   Complete boot operations started in arm64_head.S
 *
 ****************************************************************************/

void arm64_chip_boot(void)
{
#ifdef CONFIG_RPI5B_DEBUG_BOOT
  bcm2712_report_uart_divisor();
#endif

  /* MAP IO and DRAM, enable MMU. */

  arm64_mmu_init(true);

#ifdef CONFIG_ARM64_MTE
  arm64_mte_init();
#endif

#ifdef CONFIG_DEVICE_TREE
  fdt_register((const char *)0x40000000);
#endif

#ifdef CONFIG_ARM64_PSCI
  arm64_psci_init("smc");
#endif

  /* Perform board-specific device initialization. This would include
   * configuration of board specific resources such as GPIOs, LEDs, etc.
   */

#ifndef CONFIG_ARCH_CHIP_CUSTOM
  bcm2712_board_initialize();
#endif

#ifdef USE_EARLYSERIALINIT
  /* Perform early serial initialization if we are going to use the serial
   * driver.
   */

  arm64_earlyserialinit();
#endif

#ifdef CONFIG_SYSLOG_RPMSG
  syslog_rpmsg_init_early(g_syslog_rpmsg_buf, sizeof(g_syslog_rpmsg_buf));
#endif

#ifdef CONFIG_ARCH_PERF_EVENTS
  up_perf_init((void *)CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC);
#endif
}
