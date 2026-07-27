/****************************************************************************
 * arch/arm/src/armv8-m/arm_userfault.c
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

#include <arch/irq.h>

#include "exc_return.h"
#include "nvic.h"
#include "psr.h"
#include "arm_internal.h"
#include "sched/sched.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm_userfault_recover
 *
 * Description:
 *   Handle a fault that was taken from unprivileged thread mode by
 *   terminating just the task that caused it, leaving the rest of the system
 *   running.  This is what a BUILD_PROTECTED configuration exists for:  the
 *   MPU refuses the access, and the kernel must not then turn a contained
 *   application bug into a system-wide outage.
 *
 *   Do not confuse this with the user task check in _assert().  That one is
 *   unreachable here:  a memory fault is delivered as an exception, so
 *   up_interrupt_context() is already true by the time _assert() runs and
 *   the panic path is taken no matter who faulted.  The decision has to be
 *   made here, before PANIC() is called.
 *
 *   arm64 does the same thing in arm64_fatal_handler(), risc-v in
 *   riscv_fault_handler() and armv7-a in its own arm_userfault_recover().
 *
 * Input Parameters:
 *   regs - The register save area built by exception_common().  It is
 *     rewritten in place when the fault is recoverable.
 *
 * Returned Value:
 *   true if the fault was recovered and the caller should return OK, in
 *   which case the interrupted task resumes in _exit(SIGSEGV) instead of at
 *   the instruction that faulted.  false if there is no user task that can
 *   safely be killed and the caller must panic.
 *
 ****************************************************************************/

bool arm_userfault_recover(uint32_t *regs)
{
  struct tcb_s *tcb = this_task();
  struct tcb_s *ptcb;

  /* Two pieces of saved state have to agree before this can be called a user
   * fault, because on Cortex-M neither one is sufficient alone:
   *
   *   EXC_RETURN bit 3 says the interrupted context was thread mode.  If it
   *   is clear the fault came from another exception handler -- a nested
   *   fault -- and CONTROL below says nothing about it.
   *
   *   CONTROL.nPRIV says that thread mode was unprivileged.  The bit only
   *   governs thread mode, which is why it is read together with the one
   *   above; handler mode is privileged whatever it holds.
   *
   * regs[REG_CONTROL] is the value exception_common() captured on entry, not
   * the live register:  handler mode reads SPSEL as zero and the handler may
   * have set FPCA since, so the saved copy is the one that describes the
   * interrupted task.
   *
   * The TCB flags are defensive redundancy, matching arm64 and risc-v.  A
   * kernel thread never runs with nPRIV set, and a task inside a system call
   * has been switched to privileged mode by arm_svcall(), so either one
   * already implies the state test above has failed.
   */

  if ((tcb->flags & TCB_FLAG_TTYPE_MASK) == TCB_FLAG_TTYPE_KERNEL ||
      (tcb->flags & TCB_FLAG_SYSCALL) != 0 ||
      (regs[REG_EXC_RETURN] & EXC_RETURN_THREAD_MODE) == 0 ||
      (regs[REG_CONTROL] & CONTROL_NPRIV) == 0)
    {
      return false;
    }

  ptcb = nxsched_get_tcb(tcb->group->tg_pid);

  _alert("Segmentation fault in %s (PID %d: %s)\n",
         ptcb != NULL ? get_task_name(ptcb) : "<gone>",
         tcb->pid, get_task_name(tcb));

  tcb->flags |= TCB_FLAG_FORCED_CANCEL;

  /* Do not return to the faulting instruction; it would only fault again.
   * Return to _exit(SIGSEGV) instead, privileged, because _exit is kernel
   * code.  Clearing CONTROL.nPRIV is exactly what arm_svcall() does when it
   * redirects a system call to arm_dispatch_syscall(), and exception_common()
   * writes this word back to CONTROL just before the exception return.
   *
   * The stacked PC carries no thumb bit -- the hardware takes the execution
   * state from the stacked xPSR -- so mask it off as arm_svcall() does.
   */

  regs[REG_PC]       = (uint32_t)_exit & ~1;
  regs[REG_R0]       = SIGSEGV;
  regs[REG_CONTROL] &= ~CONTROL_NPRIV;

  /* Force a sane execution state.  The task resumes at a fresh function
   * entry, so any IT/ICI continuation left over from the faulting
   * instruction must not apply to it, and the T bit must be set -- a
   * UsageFault INVSTATE is reached precisely by clearing it, and returning
   * with it still clear would fault again forever.  Bit 9 is preserved: the
   * hardware uses it to undo the alignment padding it inserted when it
   * pushed this frame.
   */

  regs[REG_XPSR] &= ~(ARMV8M_XPSR_ICIIT1_MASK | ARMV8M_XPSR_ICIIT2_MASK);
  regs[REG_XPSR] |= ARMV8M_XPSR_T;

  /* Clear the sticky fault status.  Unlike the armv7-a DFSR/IFSR these
   * registers accumulate until written, and the system is about to carry on
   * running, so leaving them set would report this fault again in the next
   * one's dump.  Both are write-one-to-clear.
   */

  putreg32(0xffffffff, NVIC_CFAULTS);
  putreg32(NVIC_HFAULTS_VECTTBL | NVIC_HFAULTS_FORCED | NVIC_HFAULTS_DEBUGEVT,
           NVIC_HFAULTS);

  /* EXC_RETURN is deliberately left alone.  arm_svcall() overwrites it with
   * EXC_RETURN_THREAD, but it may not be overwritten here:  exception_common
   * decided at entry whether to push s16-s31 based on the EXC_RETURN it was
   * called with, and it pops them based on this word.  Changing the frame
   * type between the two would desynchronise that.  Nothing needs changing
   * anyway -- bit 3 was checked above, and bit 2 selects the same stack the
   * task faulted on.
   *
   * That stack is where _exit will run, and it is the user stack when
   * threads use the PSP.  No new exposure:  the exception frame is already
   * there, and in a protected build arm_svcall() leaves the stack alone too,
   * so the kernel already runs this task's system calls -- an ordinary exit()
   * included -- on it.  There is nothing to switch to in any case; Cortex-M
   * has no CONFIG_ARCH_ADDRENV and therefore no per-task kernel stack.
   */

  return true;
}
