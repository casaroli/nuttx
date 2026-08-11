/****************************************************************************
 * arch/arm64/src/bcm2712/bcm2712_lowputc.S
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
 ****************************************************************************
 *
 *    DESCRIPTION
 *       Wrapper for early printk
 *
 ***************************************************************************/

#include <nuttx/config.h>

#include "arm64_macro.inc"

/****************************************************************************
 * Public Symbols
 ****************************************************************************/

    .file    "bcm2712_lowputc.S"

/****************************************************************************
 * Assembly Macros
 ****************************************************************************/

 /* The console UART, taken from CONFIG_UART0_BASE so the early console and
  * the PL011 driver cannot disagree.  Two are reachable on a Raspberry Pi 5:
  *
  *   0x10_7d001000  uart10 on the BCM2712, the 3-pin debug connector.  This
  *                  connector is muxed with SWD, so a debugger takes it away.
  *   0x1f_00030000  the RP1 UART on GPIO14/15, which the firmware brings up
  *                  and leaves mapped given enable_rp1_uart=1 and
  *                  pciex4_reset=0.  No PCIe or RP1 driver is needed;  it is
  *                  a plain PL011 at a fixed address.  This leaves the debug
  *                  connector free for SWD.
  *
  * The reference clock is 44236800 Hz.  That is not the 9216000 Hz the device
  * tree states for clk_uart:  the firmware leaves IBRD at 24 for a 115200
  * console, and 115200 * 16 * 24 is 44236800.  CONFIG_UART0_CLK_FREQ carries
  * the measured value, because the PL011 driver recomputes the divisor from
  * it once the console is opened.
  */

 #define UART_BASE_ADDRESS   CONFIG_UART0_BASE

 /* PL011 registers, as offsets from the base */

 #define UART_DR             0x00
 #define UART_FR             0x18

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* PL011 UART initialization
 */

GTEXT(arm64_earlyprintinit)
SECTION_FUNC(text, arm64_earlyprintinit)
    /* Deliberately empty.
     *
     * The firmware and BL31 both print on this UART before they hand over,
     * so it arrives already enabled and already at the console baud rate.
     * Reprogramming it here only creates a way to get it wrong:  the divisor
     * depends on the reference clock, and the value in the device tree does
     * not agree with what the firmware actually programmed.
     *
     * Leaving it alone means the first character out of __start is at
     * whatever rate the boot log was already using, which is what anyone
     * watching is set to.
     */

    ret

/* PL011 UART wait UART to be ready to transmit
 * xb: register which contains the UART base address
 * c: scratch register number
 */

/* Wait for the transmitter, but not forever.
 *
 * A UART that is not there reads as all ones, so BUSY never clears and an
 * unbounded loop hangs the kernel on its first character -- before the
 * console exists to say so.  That happened with the RP1 UART, and cost a
 * debugger session to find.  Give up after a bounded number of reads and
 * carry on;  losing early output is better than losing the boot.
 */

.macro early_uart_ready xb, wt
    mov   x14, #0x100000
1:
    ldrh  \wt, [\xb, #UART_FR]   /* <- UARTFR (Flag register) */
    tst   \wt, #0x8              /* Check BUSY bit */
    b.eq  2f                     /* Ready */
    subs  x14, x14, #1
    b.ne  1b                     /* Keep waiting until the budget runs out */
2:
.endm

/* PL011 UART transmit character
 * xb: register which contains the UART base address
 * wt: register which contains the character to transmit
 */

.macro early_uart_transmit xb, wt
    strb  \wt, [\xb, #UART_DR]             /* -> UARTDR (Data Register) */
.endm

/* Print a character on the UART - this function is called by C
 * x0: character to print
 */

GTEXT(arm64_lowputc)
SECTION_FUNC(text, arm64_lowputc)
    ldr   x15, =UART_BASE_ADDRESS
    early_uart_ready x15, w2
    early_uart_transmit x15, w0
    ret
