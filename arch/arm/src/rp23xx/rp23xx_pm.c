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
#include "hardware/rp23xx_memorymap.h"

#ifdef CONFIG_RP23XX_PM

/* A timed wake-up needs both a non-zero budget and the alarm half of the
 * always-on timer driver to arm it with.
 */

#if defined(CONFIG_RP23XX_PM_SLEEP_MAX_MS) && \
    CONFIG_RP23XX_PM_SLEEP_MAX_MS > 0 && defined(CONFIG_RTC_ALARM)
#  define RP23XX_PM_HAVE_TIMED_WAKE 1
#endif

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

/* Bit mask of the POWMAN power-up channels currently armed as wake sources.
 * Dormancy is refused when this is empty and no timed wake-up is available,
 * because nothing short of a reset could then restart the chip.
 */

static uint32_t g_pm_wakeup_channels;

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
   */

  putreg32(1, RP23XX_CLOCKS_CLK_SYS_DIV);
  clrbits_reg32(RP23XX_CLOCKS_CLK_SYS_CTRL_SRC, RP23XX_CLOCKS_CLK_SYS_CTRL);
  while (getreg32(RP23XX_CLOCKS_CLK_SYS_SELECTED) != 1)
    {
    }

  /* Point clk_ref at the crystal oscillator, which is what the dormant
   * request stops and what the wake event restarts.
   */

  putreg32(1, RP23XX_CLOCKS_CLK_REF_DIV);
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

  /* Stop the crystal oscillator.  Execution stalls inside this write until a
   * power-up request from the always-on domain restarts it.
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
#ifdef RP23XX_PM_HAVE_TIMED_WAKE
  struct rp23xx_alarm_state_s saved;
  bool borrowed = false;

  /* Borrow the single always-on alarm comparator as a timed backstop, so
   * that the chip always comes back even if no GPIO event ever arrives.
   * Any application alarm is saved and re-armed on the way out.
   */

  rp23xx_rtc_savealarm(&saved);

  if (rp23xx_rtc_setalarm(rp23xx_rtc_getms() +
                          CONFIG_RP23XX_PM_SLEEP_MAX_MS, NULL, NULL) == OK)
    {
      rp23xx_rtc_alarm_pwrup(true);
      borrowed = true;
    }
  else
    {
      rp23xx_rtc_restorealarm(&saved);
    }
#endif

  /* Entering dormancy with nothing able to raise a power-up request leaves
   * the chip recoverable only by a reset, which on a deployed board means a
   * power cycle.  Degrade to standby rather than risk that.
   */

  if (g_pm_wakeup_channels == 0
#ifdef RP23XX_PM_HAVE_TIMED_WAKE
      && !borrowed
#endif
     )
    {
      _warn("no wake source armed; entering standby instead of dormant\n");
      rp23xx_pm_standby();
      return;
    }

  rp23xx_pm_dormant();

#ifdef RP23XX_PM_HAVE_TIMED_WAKE
  if (borrowed)
    {
      rp23xx_rtc_alarm_pwrup(false);
      rp23xx_rtc_cancelalarm();
      rp23xx_rtc_restorealarm(&saved);
    }
#endif
}

/****************************************************************************
 * Name: rp23xx_pm_gpio_wakeup
 ****************************************************************************/

int rp23xx_pm_gpio_wakeup(int channel, int gpio, bool edge, bool high)
{
  uint32_t regval;
  uint32_t reg;

  if (channel < 0 || channel >= RP23XX_PM_PWRUP_NCHANNELS)
    {
      return -EINVAL;
    }

  if (gpio < 0 || gpio >= RP23XX_GPIO_NUM)
    {
      return -EINVAL;
    }

  reg = RP23XX_POWMAN_PWRUP0 + channel * 4;

  /* The pad has to be input enabled for the detector to see anything, and
   * the detector lives in the always-on domain so it keeps watching after
   * the switched core has stopped.
   */

  modbits_reg32(RP23XX_PADS_BANK0_GPIO_IE, RP23XX_PADS_BANK0_GPIO_IE,
                RP23XX_PADS_BANK0_GPIO(gpio));

  /* Disable the channel while its source is changed, otherwise a spurious
   * power-up request can be latched from the old configuration.
   */

  powman_clrbits(reg, RP23XX_POWMAN_PWRUP0_ENABLE);

  regval = (uint32_t)gpio & RP23XX_POWMAN_PWRUP0_SOURCE_MASK;

  if (edge)
    {
      regval |= RP23XX_POWMAN_PWRUP0_MODE;
    }

  if (high)
    {
      regval |= RP23XX_POWMAN_PWRUP0_DIRECTION;
    }

  putreg32(POWMAN_PASSWORD | regval, reg);
  powman_setbits(reg, RP23XX_POWMAN_PWRUP0_ENABLE);

  g_pm_wakeup_channels |= 1u << channel;
  return OK;
}

/****************************************************************************
 * Name: rp23xx_pm_gpio_wakeup_disable
 ****************************************************************************/

int rp23xx_pm_gpio_wakeup_disable(int channel)
{
  if (channel < 0 || channel >= RP23XX_PM_PWRUP_NCHANNELS)
    {
      return -EINVAL;
    }

  powman_clrbits(RP23XX_POWMAN_PWRUP0 + channel * 4,
                 RP23XX_POWMAN_PWRUP0_ENABLE);

  g_pm_wakeup_channels &= ~(1u << channel);
  return OK;
}

#endif /* CONFIG_RP23XX_PM */
