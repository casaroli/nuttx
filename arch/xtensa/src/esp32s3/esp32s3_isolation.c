/****************************************************************************
 * arch/xtensa/src/esp32s3/esp32s3_isolation.c
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

/* Permission control that a protected build and a kernel build share: the
 * violation interrupt, and the peripheral permissions.  Everything here
 * concerns the WORLD0 (privileged) / WORLD1 (unprivileged) split and is
 * independent of how the user memory itself is laid out, which is what
 * separates the two build models.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/irq.h>
#include <nuttx/nuttx.h>
#include <nuttx/sched.h>

#ifdef CONFIG_ESP32S3_PAGEFAULT_ABORT
#include <signal.h>
#include <arch/irq.h>
#include <arch/xtensa/xtensa_corebits.h>
#endif

#include "chip.h"
#include "xtensa.h"
#include "esp_attr.h"
#include "esp_irq.h"
#include "esp32s3_isolation.h"
#include "esp32s3_pms.h"
#include "hardware/esp32s3_sensitive.h"
#include "hardware/esp32s3_soc.h"

#include "soc/extmem_reg.h"

#ifdef CONFIG_ESP32S3_PAGEFAULT_ABORT
#include "sched/sched.h"
#include "signal/signal.h"
#endif

#ifndef CONFIG_BUILD_FLAT

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_PAGEFAULT_ABORT

/****************************************************************************
 * Name: pms_clear_violations
 *
 * Description:
 *   Acknowledge and re-arm every PMS violation monitor.  The monitors raise
 *   a level-triggered interrupt, so the latch must be cleared (pulse the CLR
 *   bit) before returning from the ISR or the interrupt re-fires forever.
 *
 ****************************************************************************/

static void IRAM_ATTR pms_clear_violations(void)
{
  /* IRAM0 / DRAM0 / PIF monitors: pulse VIOLATE_CLR (keeping VIOLATE_EN). */

  modifyreg32(SENSITIVE_CORE_0_IRAM0_PMS_MONITOR_1_REG, 0,
              SENSITIVE_CORE_0_IRAM0_PMS_MONITOR_VIOLATE_CLR_M);
  modifyreg32(SENSITIVE_CORE_0_IRAM0_PMS_MONITOR_1_REG,
              SENSITIVE_CORE_0_IRAM0_PMS_MONITOR_VIOLATE_CLR_M, 0);

  modifyreg32(SENSITIVE_CORE_0_DRAM0_PMS_MONITOR_1_REG, 0,
              SENSITIVE_CORE_0_DRAM0_PMS_MONITOR_VIOLATE_CLR_M);
  modifyreg32(SENSITIVE_CORE_0_DRAM0_PMS_MONITOR_1_REG,
              SENSITIVE_CORE_0_DRAM0_PMS_MONITOR_VIOLATE_CLR_M, 0);

  modifyreg32(SENSITIVE_CORE_0_PIF_PMS_MONITOR_1_REG, 0,
              SENSITIVE_CORE_0_PIF_PMS_MONITOR_VIOLATE_CLR_M);
  modifyreg32(SENSITIVE_CORE_0_PIF_PMS_MONITOR_1_REG,
              SENSITIVE_CORE_0_PIF_PMS_MONITOR_VIOLATE_CLR_M, 0);

  /* Flash instruction/data cache reject monitors. */

  modifyreg32(EXTMEM_CORE0_ACS_CACHE_INT_CLR_REG, 0,
              EXTMEM_CORE0_IBUS_REJECT_INT_CLR_M |
              EXTMEM_CORE0_DBUS_REJECT_INT_CLR_M);
  modifyreg32(EXTMEM_CORE0_ACS_CACHE_INT_CLR_REG,
              EXTMEM_CORE0_IBUS_REJECT_INT_CLR_M |
              EXTMEM_CORE0_DBUS_REJECT_INT_CLR_M, 0);
}
#endif

/****************************************************************************
 * Name: pms_violation_isr
 *
 * Description:
 *   This is the common PMS interrupt handler. It will be invoked the PMS
 *   detects an access violation.
 *
 * Parameters:
 *   cpuint        - CPU interrupt index
 *   context       - Context data from the ISR
 *   arg           - Opaque pointer to the internal driver state structure.
 *
 * Returned Value:
 *   Zero (OK) is returned on success. A negated errno value is returned on
 *   failure.
 *
 ****************************************************************************/

static int IRAM_ATTR pms_violation_isr(int cpuint, void *context, void *arg)
{
#ifdef CONFIG_ESP32S3_PAGEFAULT_ABORT
  uint32_t *regs = (uint32_t *)context;

  /* Acknowledge and re-arm the monitors first so the level-triggered
   * interrupt does not immediately re-fire while we handle it.
   */

  pms_clear_violations();

  /* An ESP32-S3 PMS permission violation is asynchronous (unlike the precise
   * cache-attribute faults).  If the interruptee was an unprivileged (user)
   * WORLD1 task -- its saved PS carries the User Mode bit -- terminate only
   * that task with SIGSEGV instead of the whole system.  The IRQ dispatch
   * return path applies the up_schedule_sigaction() redirect.
   */

  if (regs != NULL && (regs[REG_PS] & PS_UM) != 0)
    {
      struct tcb_s *tcb = this_task();
      siginfo_t     info;

      _alert("SIGSEGV (PMS) task %s: PC=%08x\n",
             get_task_name(tcb), (unsigned)regs[REG_PC]);

      info.si_signo           = SIGSEGV;
      info.si_code            = SI_USER;
      info.si_errno           = 0;
      info.si_value.sival_ptr = NULL;

      nxsig_tcbdispatch(tcb, &info, false);
      return OK;
    }
#endif

  /* Privileged (WORLD0) violation, or abort disabled: not survivable. */

  PANIC();

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_isolation_revoke_peripherals
 ****************************************************************************/

void esp32s3_isolation_revoke_peripherals(void)
{
  /* Revoke User access permission to every peripheral */

  esp32s3_pms_configure_peripheral(PMS_UART1, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_I2C, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_MISC, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_IO_MUX, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_RTC, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_FE, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_FE2, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_GPIO, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_G0SPI_0, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_G0SPI_1, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_UART, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_SYSTIMER, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_TIMERGROUP1, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_TIMERGROUP, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_BB, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_LEDC, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_RMT, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_UHCI0, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_I2C_EXT0, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_BT, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_PWR, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_WIFIMAC, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_RWBT, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_I2S1, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_CAN, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_APB_CTRL, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_SPI_2, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_WORLD_CONTROLLER, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_DIO, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_AD, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_CACHE_CONFIG, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_DMA_COPY, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_INTERRUPT, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_SENSITIVE, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_SYSTEM, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_BT_PWR, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_APB_ADC, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_CRYPTO_DMA, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_CRYPTO_PERI, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_USB_WRAP, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_USB_DEVICE, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_I2S0, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_HINF, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_PWM0, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_BACKUP, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_SLC, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_PCNT, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_SLCHOST, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_UART2, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_PWM1, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_SDIO_HOST, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_I2C_EXT1, PMS_WORLD_1,
                                   PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_SPI_3, PMS_WORLD_1, PMS_ACCESS_NONE);
  esp32s3_pms_configure_peripheral(PMS_USB, PMS_WORLD_1, PMS_ACCESS_NONE);
}

/****************************************************************************
 * Name: esp32s3_pmsirqinitialize
 ****************************************************************************/

void esp32s3_pmsirqinitialize(void)
{
  VERIFY(esp_setup_irq(ESP32S3_PERIPH_CORE_0_IRAM0_PMS_MONITOR_VIOLATE,
                       1, ESP_IRQ_TRIGGER_LEVEL, pms_violation_isr, NULL));
  VERIFY(esp_setup_irq(ESP32S3_PERIPH_CORE_0_DRAM0_PMS_MONITOR_VIOLATE,
                       1, ESP_IRQ_TRIGGER_LEVEL, pms_violation_isr, NULL));
  VERIFY(esp_setup_irq(ESP32S3_PERIPH_CACHE_CORE0_ACS,
                       1, ESP_IRQ_TRIGGER_LEVEL, pms_violation_isr, NULL));
  VERIFY(esp_setup_irq(ESP32S3_PERIPH_CORE_0_PIF_PMS_MONITOR_VIOLATE,
                       1, ESP_IRQ_TRIGGER_LEVEL, pms_violation_isr, NULL));

  up_enable_irq(ESP32S3_IRQ_CORE_0_IRAM0_PMS_MONITOR_VIOLATE);
  up_enable_irq(ESP32S3_IRQ_CORE_0_DRAM0_PMS_MONITOR_VIOLATE);
  up_enable_irq(ESP32S3_IRQ_CACHE_CORE0_ACS);
  up_enable_irq(ESP32S3_IRQ_CORE_0_PIF_PMS_MONITOR_VIOLATE);
}

#endif /* !CONFIG_BUILD_FLAT */
