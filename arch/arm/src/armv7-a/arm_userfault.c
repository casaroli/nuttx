/****************************************************************************
 * arch/arm/src/armv7-a/arm_userfault.c
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

#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>

#include <nuttx/arch.h>
#include <nuttx/debug.h>
#include <nuttx/irq.h>
#include <nuttx/sched.h>

#include "arm.h"
#include "arm_internal.h"
#include "sched/sched.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm_userfault_recover
 *
 * Description:
 *   Handle a fault that was taken from user mode by terminating just the
 *   task that caused it, leaving the rest of the system running.  This is
 *   what a BUILD_PROTECTED or BUILD_KERNEL configuration exists for:  the
 *   hardware refuses the access, and the kernel must not then turn a
 *   contained application bug into a system-wide outage.
 *
 *   Do not confuse this with the user task check in _assert().  That one is
 *   unreachable here:  a memory fault is delivered as an exception, so
 *   up_interrupt_context() is already true by the time _assert() runs and
 *   the panic path is taken no matter who faulted.  The decision has to be
 *   made here, before PANIC() is called.
 *
 *   arm64 does the same thing in arm64_fatal_handler() and risc-v in
 *   riscv_fault_handler().
 *
 * Input Parameters:
 *   regs - The register save area built by the exception vector.  It is
 *     rewritten in place when the fault is recoverable.
 *
 * Returned Value:
 *   true if the fault was recovered and the caller should return normally,
 *   in which case the interrupted task resumes in _exit(SIGSEGV) instead of
 *   at the instruction that faulted.  false if there is no user task that
 *   can safely be killed and the caller must panic.
 *
 ****************************************************************************/

bool arm_userfault_recover(uint32_t *regs)
{
  struct tcb_s *tcb = this_task();
  struct tcb_s *ptcb;

  /* The saved CPSR check alone is sufficient:  a kernel thread, the body of
   * a system call and an interrupt handler all run in SYS mode, so only a
   * fault taken from user code has PSR_MODE_USR here.  The other two checks
   * are kept as defensive redundancy, matching arm64 and risc-v.
   */

  if ((tcb->flags & TCB_FLAG_TTYPE_MASK) == TCB_FLAG_TTYPE_KERNEL ||
      (tcb->flags & TCB_FLAG_SYSCALL) != 0 ||
      (regs[REG_CPSR] & PSR_MODE_MASK) != PSR_MODE_USR)
    {
      return false;
    }

  ptcb = nxsched_get_tcb(tcb->group->tg_pid);

  _alert("Segmentation fault in %s (PID %d: %s)\n",
         ptcb != NULL ? get_task_name(ptcb) : "<gone>",
         tcb->pid, get_task_name(tcb));

  tcb->flags |= TCB_FLAG_FORCED_CANCEL;

  /* Do not return to the faulting instruction; it would only fault again.
   * Return to _exit(SIGSEGV) instead, and in a privileged mode because
   * _exit is kernel code.  That mode is SYS and not SVC:  SYS shares r13
   * and r14 with USR, which is the mode the kernel runs every task in, and
   * it is what arm_syscall() switches to when it redirects a system call to
   * dispatch_syscall().  This is the exact same redirect.
   */

  /* The instruction set state comes from the SPSR, never from bit 0 of the
   * new PC, so it has to be set from the symbol here.  Without this a Thumb
   * kernel resumed from a fault that was taken in ARM state -- a call to an
   * even address, which is what the execute probe does -- decodes _exit as
   * ARM and runs off into the literal pools.
   */

  regs[REG_PC]    = (uint32_t)_exit & ~1;
  regs[REG_R0]    = SIGSEGV;
  regs[REG_CPSR] &= ~(PSR_MODE_MASK | PSR_T_BIT);
  regs[REG_CPSR] |= PSR_MODE_SYS;

  if (((uint32_t)_exit & 1) != 0)
    {
      regs[REG_CPSR] |= PSR_T_BIT;
    }

#ifdef CONFIG_ARCH_KERNEL_STACK
  /* Continue on the kernel stack if this task has one.  It is unused:  the
   * fault came from user code, so no system call is in progress and nothing
   * on it needs to be preserved.  This is what arm_syscall() does with the
   * first system call of a nesting, and what risc-v does here.
   */

  if (tcb->xcp.kstack != NULL)
    {
      regs[REG_SP] = (uint32_t)tcb->xcp.kstack + ARCH_KERNEL_STACKSIZE;
    }
#endif

  /* Without a kernel stack REG_SP is left alone and _exit runs on the user
   * stack the task faulted on.  That is no new exposure:  the exception
   * vectors push this very frame onto that stack (srsdb writes the SYS mode
   * stack, and SYS shares r13 with USR), and in such a configuration
   * dispatch_syscall already runs this task's system calls there -- an
   * ordinary exit() included.
   */

  return true;
}
