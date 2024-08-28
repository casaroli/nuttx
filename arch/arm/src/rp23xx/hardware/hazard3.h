/*
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HARDWARE_HAZARD3_
#define _HARDWARE_HAZARD3_

#include "pico.h"
#include "hardware/riscv.h"
#include "hardware/regs/rvcsr.h"

/** \file hardware/hazard3.h
 *  \defgroup hardware_hazard3 hardware_hazard3
 *
 * \brief Accessors for Hazard3-specific RISC-V CSRs, and intrinsics for Hazard3 custom instructions
 *
 */

// Feature detection macros for Hazard3 custom extensions
#if PICO_RP2350
#define __hazard3_extension_xh3power
#define __hazard3_extension_xh3bextm
#define __hazard3_extension_xh3irq
#define __hazard3_extension_xh3pmpm
#endif

#ifdef __ASSEMBLER__

// Assembly language instruction macros for Hazard3 custom instructions

// h3.bextm: Extract up to 8 consecutive bits from register rs1, with the
// first bit indexed by rs2, and bit count configured by an immediate value.
// R-format instruction. Pseudocode:
//
//     rd = (rs1 >> rs2[4:0]) & ~(-1 << nbits)

.macro h3.bextm rd rs1 rs2 nbits
.if (\nbits < 1) || (\nbits > 8)
.err
.endif
#ifdef __hazard3_extension_xh3bextm
    .insn r 0x0b, 0x4, (((\nbits - 1) & 0x7 ) << 1), \rd, \rs1, \rs2
#else
    srl  \rd, \rs1, \rs2
    andi \rd, \rd, ((1 << \nbits) - 1)
#endif
.endm

// h3.bextmi: Extract up to 8 consecutive bits from register rs1, with the
// first bit index and the number of bits both configured by immediate
// values. I-format instruction. Pseudocode:
//
//     rd = (rs1 >> shamt) & ~(-1 << nbits)

.macro h3.bextmi rd rs1 shamt nbits
.if (\nbits < 1) || (\nbits > 8)
.err
.endif
.if (\shamt < 0) || (\shamt > 31)
.err
.endif
#ifdef __hazard3_extension_xh3bextm
    .insn i 0x0b, 0x4, \rd, \rs1, (\shamt & 0x1f) | (((\nbits - 1) & 0x7 ) << 6)
#else
    srli \rd, \rs1, \shamt
    andi \rd, \rd, ((1 << \nbits) - 1)
#endif
.endm

// h3.block: enter an idle state until another processor in the same
// multiprocessor complex executes an h3.unblock instruction, or the
// processor is interrupted. Fall through immediately if an h3.unblock has
// been received since the last execution of an h3.block on this processor.
// On RP2350, processors also have their own h3.unblock signals reflected
// back to them.

.macro h3.block
#ifdef __hazard3_extension_xh3power
    slt x0, x0, x0
#else
    nop
#endif
.endm

// h3.unblock: signal other processors in the same multiprocessor complex to
// exit the idle state entered by an h3.block instruction. On RP2350, this
// signal is also reflected back to the processor that executed the
// h3.unblock, which will cause that processor's next h3.block to fall
// through immediately.

.macro h3.unblock
#ifdef __hazard3_extension_xh3power
    slt x0, x0, x1
#else
    nop
#endif
.endm

#else // !__ASSEMBLER__

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __hazard3_extension_xh3irq
#define hazard3_irqarray_read(csr, index) (riscv_read_set_csr(csr, (index)) >> 16)
#else
#define hazard3_irqarray_read(csr, index) static_assert(false, "Not supported: Xh3irq extension")
#endif

#ifdef __hazard3_extension_xh3irq
#define hazard3_irqarray_write(csr, index, data) (riscv_write_csr(csr, (index) | ((uint32_t)(data) << 16)))
#else
#define hazard3_irqarray_write(csr, index, data) static_assert(false, "Not supported: Xh3irq extension")
#endif

#ifdef __hazard3_extension_xh3irq
#define hazard3_irqarray_set(csr, index, data) (riscv_set_csr(csr, (index) | ((uint32_t)(data) << 16)))
#else
#define hazard3_irqarray_set(csr, index, data) static_assert(false, "Not supported: Xh3irq extension")
#endif

#ifdef __hazard3_extension_xh3irq
#define hazard3_irqarray_clear(csr, index, data) (riscv_clear_csr(csr, (index) | ((uint32_t)(data) << 16)))
#else
#define hazard3_irqarray_clear(csr, index, data) static_assert(false, "Not supported: Xh3irq extension")
#endif


// nbits must be a constant expression
#ifdef __hazard3_extension_xh3bextm
#define __hazard3_bextm(nbits, rs1, rs2) ({\
    uint32_t __h3_bextm_rd; \
    asm (".insn r 0x0b, 0, %3, %0, %1, %2"\
        : "=r" (__h3_bextm_rd) \
        : "r" (rs1), "r" (rs2), "i" ((((nbits) - 1) & 0x7) << 1)\
    ); \
    __h3_bextm_rd; \
})
#else
#define __hazard3_bextm(nbits, rs1, rs2) (((rs1) >> ((rs2) & 0x1f)) & (0xffu >> (7 - (((nbits) - 1) & 0x7))))
#endif

// nbits and shamt must be constant expressions
#ifdef __hazard3_extension_xh3bextm
#define __hazard3_bextmi(nbits, rs1, shamt) ({\
    uint32_t __h3_bextmi_rd; \
    asm (".insn i 0x0b, 0x4, %0, %1, %2"\
        : "=r" (__h3_bextmi_rd) \
        : "r" (rs1), "i" ((((nbits) - 1) & 0x7) << 6 | ((shamt) & 0x1f)) \
    ); \
    __h3_bextmi_rd; \
})
#else
#define __hazard3_bextm(nbits, rs1, rs2) (((rs1) >> ((shamt) & 0x1f)) & (0xffu >> (7 - (((nbits) - 1) & 0x7))))
#endif

#ifdef __hazard3_extension_xh3power
#define __hazard3_block() asm volatile ("slt x0, x0, x0" : : : "memory")
#else
#define __hazard3_block() do {} while (0)
#endif

#ifdef __hazard3_extension_xh3power
#define __hazard3_unblock() asm volatile ("slt x0, x0, x1" : : : "memory")
#else
#define __hazard3_unblock() do {} while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif // !__ASSEMBLER__

#endif
