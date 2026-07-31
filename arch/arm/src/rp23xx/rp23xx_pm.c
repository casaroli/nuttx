/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_pm.c
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
 *
 * The two low-power states this file implements map onto the RP2350 power
 * modes described in the "Power Management" section of the datasheet
 * (https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf):
 *
 *   PM_STANDBY -> SLEEP.  CLOCKS gates the peripheral clocks listed in
 *                 SLEEP_EN0/1 for the duration of a WFI.  Oscillators and
 *                 PLLs keep running, so wake-up is immediate and every
 *                 peripheral resumes with its state intact.
 *
 *   PM_SLEEP   -> DORMANT.  The PLLs are stopped, clk_sys is dropped to the
 *                 crystal oscillator, and the crystal oscillator is then
 *                 halted.  Only the always-on domain (POWMAN, the low-power
 *                 oscillator and the GPIO power-up detectors) stays alive.
 *
 * The dormant entry and exit sequence mirrors the pico-extras
 * sleep_goto_dormant_until_*() functions.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <debug.h>

#include <arch/board/board.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/clock.h>

#include "arm_internal.h"
#include "nvic.h"

#include "rp23xx_pm.h"
#include "rp23xx_gpio.h"
#include "rp23xx_pll.h"

#ifdef CONFIG_RP23XX_RTC
#  include "rp23xx_rtc.h"
#endif

#include "hardware/rp23xx_clocks.h"
#include "hardware/rp23xx_powman.h"
#include "hardware/rp23xx_xosc.h"
#include "hardware/rp23xx_pll.h"
#include "hardware/rp23xx_pads_bank0.h"
#include "hardware/rp23xx_io_bank0.h"
#include "hardware/rp23xx_memorymap.h"
#include "hardware/rp23xx_uart.h"

#ifdef CONFIG_RP23XX_PM

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Stopping the crystal oscillator stalls clk_sys, and with it every bus in
 * the switched core.  Execute-in-place cannot service an instruction fetch
 * across that window, so the entry and exit sequence must already be in RAM
 * when it runs.  The board linker scripts copy .time_critical there.
 */

#define RP23XX_PM_RAMFUNC \
  __attribute__((section(".time_critical.rp23xx_pm"), noinline))

/* Every POWMAN write must carry this password in the top 16 bits or it is
 * ignored.  Only the low 16 bits are data.
 */

#define POWMAN_PASSWORD   0x5afe0000

/* Atomic set/clear aliases of a bus register (RP2350 memory map). */

#define POWMAN_SET_ALIAS  0x2000
#define POWMAN_CLR_ALIAS  0x3000

/* Which clocks keep running through a PM_STANDBY WFI.
 *
 * SLEEP_EN0/1 use the same bit assignment as WAKE_EN0/1, so the WAKE_EN
 * constants name the fields for both registers.
 *
 * A block is gated only when its driver is not built into this
 * configuration.  That is the one criterion under which gating is
 * unconditionally safe: with no driver there can be no transfer in flight
 * and no register state that a stopped clock could corrupt.  Blocks whose
 * driver is present keep their clock even while idle, because the rp23xx
 * drivers do not yet register PM callbacks and so cannot be asked whether
 * they are busy.
 */

/* Core infrastructure: the core cannot wake without these. */

#define RP23XX_PM_SLEEP_EN0_BASE \
  (RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_CLOCKS     | \
   RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_ACCESSCTRL | \
   RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_BOOTRAM    | \
   RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_BUSCTRL    | \
   RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_BUSFABRIC  | \
   RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_IO         | \
   RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_PADS       | \
   RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_PLL_SYS    | \
   RP23XX_CLOCKS_WAKE_EN0_CLK_REF_POWMAN     | \
   RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_POWMAN     | \
   RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_PSM        | \
   RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_RESETS     | \
   RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_ROM        | \
   RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_ROSC       | \
   RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_SIO)

#define RP23XX_PM_SLEEP_EN1_BASE \
  (RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_SRAM0    | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_SRAM1    | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_SRAM2    | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_SRAM3    | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_SRAM4    | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_SRAM5    | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_SRAM6    | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_SRAM7    | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_SRAM8    | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_SRAM9    | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_SYSCFG   | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_SYSINFO  | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_TBMAN    | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_REF_TICKS    | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_TICKS    | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_WATCHDOG | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_XIP      | \
   RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_XOSC)

/* Optional blocks, kept only when their driver is configured. */

#ifdef CONFIG_RP23XX_ADC
#  define RP23XX_PM_EN0_ADC  (RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_ADC | \
                              RP23XX_CLOCKS_WAKE_EN0_CLK_ADC_ADC)
#else
#  define RP23XX_PM_EN0_ADC  0
#endif

#ifdef CONFIG_RP23XX_DMAC
#  define RP23XX_PM_EN0_DMA  RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_DMA
#else
#  define RP23XX_PM_EN0_DMA  0
#endif

#ifdef CONFIG_RP23XX_I2C0
#  define RP23XX_PM_EN0_I2C0 RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_I2C0
#else
#  define RP23XX_PM_EN0_I2C0 0
#endif

#ifdef CONFIG_RP23XX_I2C1
#  define RP23XX_PM_EN0_I2C1 RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_I2C1
#else
#  define RP23XX_PM_EN0_I2C1 0
#endif

#ifdef CONFIG_RP23XX_PWM
#  define RP23XX_PM_EN0_PWM  RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_PWM
#else
#  define RP23XX_PM_EN0_PWM  0
#endif

#ifdef CONFIG_RP23XX_OTP
#  define RP23XX_PM_EN0_OTP  (RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_OTP | \
                              RP23XX_CLOCKS_WAKE_EN0_CLK_REF_OTP)
#else
#  define RP23XX_PM_EN0_OTP  0
#endif

#ifdef CONFIG_CRYPTO_CRYPTODEV_HARDWARE
#  define RP23XX_PM_EN0_SHA  RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_SHA256
#else
#  define RP23XX_PM_EN0_SHA  0
#endif

/* The PIO blocks are shared by several unrelated drivers, so they stay
 * clocked if any one of them is present.
 */

#if defined(CONFIG_RP23XX_I2S) || defined(CONFIG_WS2812) || \
    defined(CONFIG_IEEE80211_INFINEON_CYW43439)
#  define RP23XX_PM_EN0_PIO  (RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_PIO0 | \
                              RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_PIO1 | \
                              RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_PIO2)
#else
#  define RP23XX_PM_EN0_PIO  0
#endif

#ifdef CONFIG_USBDEV
#  define RP23XX_PM_EN0_USB  RP23XX_CLOCKS_WAKE_EN0_CLK_SYS_PLL_USB
#  define RP23XX_PM_EN1_USB  (RP23XX_CLOCKS_WAKE_EN1_CLK_USB | \
                              RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_USBCTRL)
#else
#  define RP23XX_PM_EN0_USB  0
#  define RP23XX_PM_EN1_USB  0
#endif

#ifdef CONFIG_RP23XX_UART0
#  define RP23XX_PM_EN1_UART0 (RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_UART0 | \
                               RP23XX_CLOCKS_WAKE_EN1_CLK_PERI_UART0)
#else
#  define RP23XX_PM_EN1_UART0 0
#endif

#ifdef CONFIG_RP23XX_UART1
#  define RP23XX_PM_EN1_UART1 (RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_UART1 | \
                               RP23XX_CLOCKS_WAKE_EN1_CLK_PERI_UART1)
#else
#  define RP23XX_PM_EN1_UART1 0
#endif

#ifdef CONFIG_RP23XX_SPI0
#  define RP23XX_PM_EN1_SPI0  (RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_SPI0 | \
                               RP23XX_CLOCKS_WAKE_EN1_CLK_PERI_SPI0)
#else
#  define RP23XX_PM_EN1_SPI0  0
#endif

#ifdef CONFIG_RP23XX_SPI1
#  define RP23XX_PM_EN1_SPI1  (RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_SPI1 | \
                               RP23XX_CLOCKS_WAKE_EN1_CLK_PERI_SPI1)
#else
#  define RP23XX_PM_EN1_SPI1  0
#endif

#ifdef CONFIG_RP23XX_RNG
#  define RP23XX_PM_EN1_TRNG  RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_TRNG
#else
#  define RP23XX_PM_EN1_TRNG  0
#endif

#ifdef CONFIG_RP23XX_TIMER0
#  define RP23XX_PM_EN1_TIMER0 RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_TIMER0
#else
#  define RP23XX_PM_EN1_TIMER0 0
#endif

#ifdef CONFIG_RP23XX_TIMER1
#  define RP23XX_PM_EN1_TIMER1 RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_TIMER1
#else
#  define RP23XX_PM_EN1_TIMER1 0
#endif

/* The tickless scheduler runs from one of the system timer blocks, which
 * must therefore keep its clock regardless of the /dev/timer configuration.
 */

#if defined(CONFIG_RP23XX_SYSTIMER_TICKLESS_TIMER0)
#  define RP23XX_PM_EN1_SYSTIMER RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_TIMER0
#elif defined(CONFIG_RP23XX_SYSTIMER_TICKLESS_TIMER1)
#  define RP23XX_PM_EN1_SYSTIMER RP23XX_CLOCKS_WAKE_EN1_CLK_SYS_TIMER1
#else
#  define RP23XX_PM_EN1_SYSTIMER 0
#endif

#define RP23XX_PM_SLEEP_EN0 \
  (RP23XX_PM_SLEEP_EN0_BASE | RP23XX_PM_EN0_ADC  | RP23XX_PM_EN0_DMA  | \
   RP23XX_PM_EN0_I2C0       | RP23XX_PM_EN0_I2C1 | RP23XX_PM_EN0_PWM  | \
   RP23XX_PM_EN0_OTP        | RP23XX_PM_EN0_SHA  | RP23XX_PM_EN0_PIO  | \
   RP23XX_PM_EN0_USB)

#define RP23XX_PM_SLEEP_EN1 \
  (RP23XX_PM_SLEEP_EN1_BASE  | RP23XX_PM_EN1_USB    | \
   RP23XX_PM_EN1_UART0       | RP23XX_PM_EN1_UART1  | \
   RP23XX_PM_EN1_SPI0        | RP23XX_PM_EN1_SPI1   | \
   RP23XX_PM_EN1_TRNG        | RP23XX_PM_EN1_TIMER0 | \
   RP23XX_PM_EN1_TIMER1      | RP23XX_PM_EN1_SYSTIMER)

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* GPIOs currently armed in the dormant-wake detector.  Dormancy is refused
 * while this is empty, because nothing would then be able to restart the
 * crystal oscillator and the chip would only come back on a reset.
 */

static uint64_t g_pm_wakeup_gpios;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline void powman_setbits(uint32_t reg, uint32_t bits)
{
  putreg32(POWMAN_PASSWORD | bits, reg + POWMAN_SET_ALIAS);
}

static inline void powman_clrbits(uint32_t reg, uint32_t bits)
{
  putreg32(POWMAN_PASSWORD | bits, reg + POWMAN_CLR_ALIAS);
}

/****************************************************************************
 * Name: rp23xx_pm_uart_busy
 *
 * Description:
 *   True while a UART still has a character to get out.  UARTFR.BUSY stays
 *   asserted until the last stop bit has left the shift register, so it
 *   covers both the transmit FIFO and the byte in flight.
 *
 *   Dormancy stops clk_peri along with clk_sys, which strands a transmit in
 *   progress until the next wake.  On a console that turns into output
 *   arriving a couple of characters at a time, indistinguishable from a
 *   hung board.  Standby keeps the UART clocked, so deferring to it costs
 *   nothing and lets the transfer finish.
 *
 ****************************************************************************/

static bool rp23xx_pm_uart_busy(void)
{
#ifdef CONFIG_RP23XX_UART0
  if ((getreg32(RP23XX_UART0_UARTFR) & RP23XX_UART_UARTFR_BUSY) != 0)
    {
      return true;
    }
#endif

#ifdef CONFIG_RP23XX_UART1
  if ((getreg32(RP23XX_UART1_UARTFR) & RP23XX_UART_UARTFR_BUSY) != 0)
    {
      return true;
    }
#endif

  return false;
}

/****************************************************************************
 * Name: rp23xx_pm_dormant
 *
 * Description:
 *   Drop to the crystal oscillator, stop it, and pick the clock tree back up
 *   on the far side.  Runs from RAM: there is no clock to fetch instructions
 *   with between the dormant request and the wake event.
 *
 ****************************************************************************/

static void RP23XX_PM_RAMFUNC rp23xx_pm_dormant(void)
{
  uint32_t sys_ctrl;
  uint32_t sys_div;
  uint32_t ref_ctrl;
  uint32_t ref_div;

  /* Remember how clk_sys and clk_ref were driven so the tree can be put back
   * exactly as it was, including any board divisor.
   */

  sys_ctrl = getreg32(RP23XX_CLOCKS_CLK_SYS_CTRL);
  sys_div  = getreg32(RP23XX_CLOCKS_CLK_SYS_DIV);
  ref_ctrl = getreg32(RP23XX_CLOCKS_CLK_REF_CTRL);
  ref_div  = getreg32(RP23XX_CLOCKS_CLK_REF_DIV);

  /* Switch clk_sys off its aux input (the system PLL) and onto clk_ref.
   * Both muxes are glitchless, so this is safe while running.
   *
   * The divisors are deliberately left alone.  They are 16.16 fixed point
   * and an integer part of zero means divide by 2^16, not divide by one, so
   * a well-meaning write of 1 here drops clk_sys to a few hundred hertz and
   * looks for all the world like a chip that never woke up.  Both are
   * already 1.0 from clocks_init(), and neither needs to change to run from
   * the crystal.
   */

  clrbits_reg32(RP23XX_CLOCKS_CLK_SYS_CTRL_SRC, RP23XX_CLOCKS_CLK_SYS_CTRL);
  while (getreg32(RP23XX_CLOCKS_CLK_SYS_SELECTED) != 1)
    {
    }

  /* Point clk_ref at the crystal oscillator, which is what the dormant
   * request stops and what the wake event restarts.
   */

  modbits_reg32(RP23XX_CLOCKS_CLK_REF_CTRL_SRC_XOSC_CLKSRC,
                RP23XX_CLOCKS_CLK_REF_CTRL_SRC_MASK,
                RP23XX_CLOCKS_CLK_REF_CTRL);
  while (!(getreg32(RP23XX_CLOCKS_CLK_REF_SELECTED) &
           (1u << RP23XX_CLOCKS_CLK_REF_CTRL_SRC_XOSC_CLKSRC)))
    {
    }

  /* Nothing is driven from the PLLs any more, so they can be powered down.
   * They draw a substantial share of the running current.
   */

  putreg32(RP23XX_PLL_PWR_PD | RP23XX_PLL_PWR_VCOPD |
           RP23XX_PLL_PWR_POSTDIVPD,
           RP23XX_PLL_SYS_BASE + RP23XX_PLL_PWR_OFFSET);
  putreg32(RP23XX_PLL_PWR_PD | RP23XX_PLL_PWR_VCOPD |
           RP23XX_PLL_PWR_POSTDIVPD,
           RP23XX_PLL_USB_BASE + RP23XX_PLL_PWR_OFFSET);

  /* Stop the crystal oscillator.  Execution stalls inside this write until
   * the dormant-wake detector restarts it.
   */

  putreg32(RP23XX_XOSC_DORMANT_DORMANT, RP23XX_XOSC_DORMANT);

  /* --- Woken.  The oscillator is running again but not yet stable. --- */

  while (!(getreg32(RP23XX_XOSC_STATUS) & RP23XX_XOSC_STATUS_STABLE))
    {
    }

  /* Bring the PLLs back up.  These are the same parameters clocks_init()
   * uses at boot and must be kept in step with it: 1500MHz / 5 / 2 = 150MHz
   * for the system PLL, 1200MHz / 5 / 5 = 48MHz for the USB PLL.
   */

  rp23xx_pll_init(RP23XX_PLL_SYS_BASE, 1, 1500 * MHZ, 5, 2);
  rp23xx_pll_init(RP23XX_PLL_USB_BASE, 1, 1200 * MHZ, 5, 5);

  /* Restore the two glitchless muxes in the reverse order: clk_ref first,
   * because clk_sys is momentarily fed from it.
   */

  modbits_reg32(ref_ctrl, RP23XX_CLOCKS_CLK_REF_CTRL_SRC_MASK,
                RP23XX_CLOCKS_CLK_REF_CTRL);
  while (!(getreg32(RP23XX_CLOCKS_CLK_REF_SELECTED) &
           (1u << (ref_ctrl & RP23XX_CLOCKS_CLK_REF_CTRL_SRC_MASK))))
    {
    }

  putreg32(ref_div, RP23XX_CLOCKS_CLK_REF_DIV);

  putreg32(sys_ctrl, RP23XX_CLOCKS_CLK_SYS_CTRL);
  while (!(getreg32(RP23XX_CLOCKS_CLK_SYS_SELECTED) &
           (1u << (sys_ctrl & RP23XX_CLOCKS_CLK_SYS_CTRL_SRC))))
    {
    }

  putreg32(sys_div, RP23XX_CLOCKS_CLK_SYS_DIV);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rp23xx_pm_standby
 ****************************************************************************/

void rp23xx_pm_standby(void)
{
  uint32_t saved_en0;
  uint32_t saved_en1;

  /* SLEEP_EN0/1 only take effect while the processors are asleep, so these
   * writes are inert until the WFI below.
   */

  saved_en0 = getreg32(RP23XX_CLOCKS_SLEEP_EN0);
  saved_en1 = getreg32(RP23XX_CLOCKS_SLEEP_EN1);

  putreg32(RP23XX_PM_SLEEP_EN0, RP23XX_CLOCKS_SLEEP_EN0);
  putreg32(RP23XX_PM_SLEEP_EN1, RP23XX_CLOCKS_SLEEP_EN1);

  /* A plain WFI, not a deep sleep: the processor is clock-gated but the
   * switched core stays powered, so any enabled interrupt wakes it.
   */

  putreg32(getreg32(NVIC_SYSCON) & ~NVIC_SYSCON_SLEEPDEEP, NVIC_SYSCON);

  __asm__ __volatile__ ("dsb" ::: "memory");
  __asm__ __volatile__ ("wfi");
  __asm__ __volatile__ ("isb" ::: "memory");

  putreg32(saved_en0, RP23XX_CLOCKS_SLEEP_EN0);
  putreg32(saved_en1, RP23XX_CLOCKS_SLEEP_EN1);
}

/****************************************************************************
 * Name: rp23xx_pm_sleep
 ****************************************************************************/

void rp23xx_pm_sleep(void)
{
  int gpio;

#if CONFIG_RP23XX_PM_SLEEP_GUARD_MS > 0
  /* A dormant chip does not answer SWD, so a configuration that reaches
   * dormancy immediately after boot cannot be reflashed through the debug
   * port at all: the only way back is the BOOTSEL button.  Refuse to go
   * dormant until the guard interval has elapsed, which always leaves a
   * window in which a debugger can attach and take the board over.
   */

  if (clock_systime_ticks() < MSEC2TICK(CONFIG_RP23XX_PM_SLEEP_GUARD_MS))
    {
      rp23xx_pm_standby();
      return;
    }
#endif

  /* Never stop the clocks out from under a transmit in progress. */

  if (rp23xx_pm_uart_busy())
    {
      rp23xx_pm_standby();
      return;
    }

  /* Stopping the crystal oscillator stops everything that is clocked from
   * it, so the only thing that can restart the chip is the dormant-wake
   * detector in the IO bank, which is asynchronous and needs no clock.
   * Note that the always-on timer alarm cannot serve here: it belongs to the
   * POWMAN power-state machine, which is a different mechanism, and it has
   * no way to restart a stopped oscillator.  Without an armed GPIO the chip
   * would stay dormant until it was reset, so degrade to standby instead.
   */

  if (g_pm_wakeup_gpios == 0)
    {
      rp23xx_pm_standby();
      return;
    }

  /* Drop the edges latched by whatever ended the previous dormant period.
   * The detector feeds off the same latch, so leaving it set would end the
   * next period the instant it began and the chip would never actually
   * sleep.
   */

  for (gpio = 0; gpio < RP23XX_GPIO_NUM; gpio++)
    {
      if ((g_pm_wakeup_gpios & (1ull << gpio)) != 0)
        {
          setbits_reg32(0xfu << ((gpio % 8) * 4),
                        RP23XX_IO_BANK0_INTR(gpio));
        }
    }

  rp23xx_pm_dormant();
}

/****************************************************************************
 * Name: rp23xx_pm_gpio_wakeup
 ****************************************************************************/

int rp23xx_pm_gpio_wakeup(int gpio, bool edge, bool high)
{
  uint32_t bit;

  if (gpio < 0 || gpio >= RP23XX_GPIO_NUM)
    {
      return -EINVAL;
    }

  /* The pad has to be input enabled for the detector to see anything. */

  modbits_reg32(RP23XX_PADS_BANK0_GPIO_IE, RP23XX_PADS_BANK0_GPIO_IE,
                RP23XX_PADS_BANK0_GPIO(gpio));

  if (edge)
    {
      bit = high ? RP23XX_IO_BANK0_INTR_GPIO_EDGE_HIGH(gpio)
                 : RP23XX_IO_BANK0_INTR_GPIO_EDGE_LOW(gpio);
    }
  else
    {
      bit = high ? RP23XX_IO_BANK0_INTR_GPIO_LEVEL_HIGH(gpio)
                 : RP23XX_IO_BANK0_INTR_GPIO_LEVEL_LOW(gpio);
    }

  /* Drop anything the detector latched earlier, so that a stale edge does
   * not end the very next dormant period before it starts.
   */

  setbits_reg32(0xfu << ((gpio % 8) * 4), RP23XX_IO_BANK0_INTR(gpio));

  setbits_reg32(bit, RP23XX_IO_BANK0_DORMANT_WAKE_INTE(gpio));

  g_pm_wakeup_gpios |= 1ull << gpio;
  return OK;
}

/****************************************************************************
 * Name: rp23xx_pm_gpio_wakeup_disable
 ****************************************************************************/

int rp23xx_pm_gpio_wakeup_disable(int gpio)
{
  if (gpio < 0 || gpio >= RP23XX_GPIO_NUM)
    {
      return -EINVAL;
    }

  clrbits_reg32(0xfu << ((gpio % 8) * 4),
                RP23XX_IO_BANK0_DORMANT_WAKE_INTE(gpio));

  g_pm_wakeup_gpios &= ~(1ull << gpio);
  return OK;
}

#endif /* CONFIG_RP23XX_PM */
