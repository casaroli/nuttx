/****************************************************************************
 * arch/xtensa/src/common/xtensa_fork.c
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

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/sched.h>

#include "sched/sched.h"
#include "xtensa.h"
#include "chip_macros.h"

/* This implementation reads the caller's register context out of the system
 * call that brought it into the kernel.  Without system calls there is no
 * such context -- a plain C function cannot see its caller's callee-saved
 * registers -- and forking would need an assembly entry stub of the kind
 * arm/fork.S provides.  Xtensa has none, so ARCH_HAVE_FORK is gated on
 * LIB_SYSCALL and this should never be reached otherwise.
 */

#ifndef CONFIG_LIB_SYSCALL
#  error "Xtensa fork() requires CONFIG_LIB_SYSCALL"
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: xtensa_fork
 *
 * Description:
 *   The common half of up_fork() and up_vfork().  The two differ only in
 *   what the child gets for memory, which is 'share':
 *
 *     share == false (fork)   the child receives its own copy of the
 *                             parent's memory at the same virtual
 *                             addresses, and may do anything it likes with
 *                             it.  Needs an address environment that can be
 *                             duplicated.
 *     share == true  (vfork)  the child borrows the parent's memory and the
 *                             parent is suspended until it leaves through
 *                             _exit() or exec().  Needs no MMU.
 *
 *   There is no assembly counterpart to this on Xtensa, and it does not
 *   need one.  Other architectures enter fork through a stub that spills
 *   the caller's registers into a struct fork_s, because a C function
 *   cannot see its caller's callee-saved registers.  Here the system call
 *   that brought us into the kernel has already done better than that:
 *   _xtensa_context_save() runs SPILL_ALL_WINDOWS on every exception entry,
 *   so by the time this runs, every live register window of the calling
 *   thread has been written to its stack and the whole context is in the
 *   exception frame.  Copying the stack therefore copies a complete and
 *   self-consistent frame chain, which is what the child's first retw needs
 *   to find.  xcp.sregs is that exception frame (see xtensa_swint()).
 *
 *   NOTE: the child shares the parent's address environment, because that
 *   is what nxtask_setup_fork() -> addrenv_join() gives it.  On a kernel
 *   build that makes this closer to a thread than to a process: the copied
 *   stack is private, everything else is shared.  Giving the child its own
 *   pages is a separate piece of work.
 *
 * Returned Value:
 *   Upon successful completion, up_fork() returns 0 to the child and the
 *   pid of the child to the parent.  On failure it returns ERROR to the
 *   parent and no child is created.
 *
 ****************************************************************************/

static pid_t xtensa_fork(bool share)
{
  struct tcb_s *parent = this_task();
  struct tcb_s *child;
  uint32_t     *sregs  = parent->xcp.sregs;
  uintptr_t     usp;
  uintptr_t     stacktop;
  uintptr_t     stackutil;
  uintptr_t     newtop;
  uintptr_t     newsp;
  int           index;

  DEBUGASSERT(sregs != NULL);

  /* This runs as the body of a system call, so the caller's own state is not
   * simply what is in the exception frame.
   */

  index = (int)parent->xcp.nsyscalls - 1;
  DEBUGASSERT(index >= 0);

  /* The stack pointer to copy from is the *user* one.  A kernel build moves
   * the outermost system call onto the thread's kernel stack and holds the
   * user stack pointer aside in ustkptr, so the A1 in the exception frame
   * names the kernel stack from here on -- measuring the parent's user stack
   * against it would produce a nonsense length.
   */

#ifdef CONFIG_ARCH_KERNEL_STACK
  usp = parent->xcp.ustkptr != NULL ?
        (uintptr_t)parent->xcp.ustkptr : sregs[REG_A1];
#else
  usp = sregs[REG_A1];
#endif

  /* Allocate and initialise a TCB for the child.  The start address is only
   * bookkeeping here: what the child actually resumes with is the register
   * context assembled below.
   */

  child = nxtask_setup_fork((start_t)parent->xcp.syscall[index].sysreturn,
                            share, usp);
  if (child == NULL)
    {
      sinfo("nxtask_setup_fork failed\n");
      return (pid_t)ERROR;
    }

  /* Copy the part of the parent's stack that is in use: everything from its
   * user stack pointer to the top, which includes the register windows the
   * exception entry spilled there.
   */

  stacktop = (uintptr_t)parent->stack_base_ptr + parent->adj_stack_size;
  DEBUGASSERT(stacktop > usp);
  stackutil = stacktop - usp;

  newtop = (uintptr_t)child->stack_base_ptr + child->adj_stack_size;
  newsp  = newtop - stackutil;

  /* When the child has its own memory this is a copy onto itself -- the
   * child's stack is the same address, already filled by the address
   * environment duplication -- and when it shares, this is the copy that
   * gives it a private stack.
   */

  if (newsp != usp)
    {
      memcpy((void *)newsp, (const void *)usp, stackutil);
    }

  /* Where the child's register context is restored from.  With a kernel
   * stack it must not be the user stack: the child is resumed by the same
   * path a system call returns through, which runs in kernel context.
   */

#ifdef CONFIG_ARCH_KERNEL_STACK
  if (child->xcp.kstack != NULL)
    {
      stacktop = (uintptr_t)child->xcp.ktopstk;
    }
  else
#endif
    {
      stacktop = newsp;
    }

  child->xcp.regs = (uint32_t *)(stacktop - XCPTCONTEXT_SIZE);

  /* Start from the parent's context, then correct the two things that must
   * differ: the stack it runs on, and what fork() returns.
   */

  memcpy(child->xcp.regs, sregs, XCPTCONTEXT_SIZE);

  /* The child is not returning from a system call, it is being started, so
   * it cannot use the syscall return path.  Give it directly what that path
   * would have produced: the caller's resume address, its privilege, its own
   * stack, and 0 as the value fork() returns.
   *
   * The privilege matters most.  The frame copied above carries the world
   * the *exception* left in it, not the caller's; taking it would resume an
   * unprivileged process privileged.
   */

  child->xcp.regs[REG_PC]  = parent->xcp.syscall[index].sysreturn;
  child->xcp.regs[REG_A1]  = newsp;
  child->xcp.regs[REG_A2]  = 0;

#ifndef CONFIG_BUILD_FLAT
  xtensa_restoreprivilege(child->xcp.regs,
                          parent->xcp.syscall[index].int_ctx);
#endif

#ifdef CONFIG_ARCH_KERNEL_STACK
  /* The parent is inside a system call, so its saved user stack pointer is
   * held aside in ustkptr and A1 names the kernel stack.  The child is not
   * inside that call and must not inherit it.
   */

  child->xcp.ustkptr = NULL;
#endif

  /* And start the child.  On failure nxtask_start_fork() discards the TCB
   * through nxtask_abort_fork().
   */

  return share ? nxtask_start_vfork(child) : nxtask_start_fork(child);
}

/****************************************************************************
 * Name: up_fork
 *
 * Description:
 *   POSIX fork():  the child receives its own copy of the parent's memory.
 *
 ****************************************************************************/

#ifdef CONFIG_ARCH_HAVE_FORK
pid_t up_fork(void)
{
  return xtensa_fork(false);
}
#endif

/****************************************************************************
 * Name: up_vfork
 *
 * Description:
 *   POSIX vfork():  the child borrows the parent's memory.  Never copies,
 *   whether or not this configuration could.
 *
 ****************************************************************************/

#ifdef CONFIG_ARCH_HAVE_VFORK
pid_t up_vfork(void)
{
  return xtensa_fork(true);
}
#endif
