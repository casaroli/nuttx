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
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/fork.h>
#include <nuttx/sched.h>

#include "sched/sched.h"
#include "xtensa.h"
#include "chip_macros.h"

/* The caller's register context comes from the system call that brought us
 * into the kernel.  Without system calls there is no such context -- a plain
 * C function cannot see its caller's callee-saved registers -- and this
 * would need an assembly entry stub of the kind arm/fork.S provides.  Xtensa
 * has none, so the capabilities are gated on LIB_SYSCALL and this should
 * never be reached otherwise.
 */

#ifndef CONFIG_LIB_SYSCALL
#  error "Xtensa fork() requires CONFIG_LIB_SYSCALL"
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: xtensa_fork
 *
 * Description:
 *   The common half of up_fork() and up_vfork().  They differ only in the
 *   FORK_TYPE_* selector handed to nxtask_setup_fork(), which is where the
 *   memory semantics are decided.
 *
 *   There is no assembly counterpart to this on Xtensa, and it does not need
 *   one.  Other architectures enter through a stub that spills the caller's
 *   registers into a struct fork_s, because a C function cannot see its
 *   caller's callee-saved registers.  Here the system call that brought us
 *   into the kernel has already done better than that:
 *   _xtensa_context_save() runs SPILL_ALL_WINDOWS on every exception entry,
 *   so by the time this runs, every live register window of the calling
 *   thread has been written to its stack and the whole context is in the
 *   exception frame.  Copying the stack therefore copies a complete and
 *   self-consistent frame chain, which is what the child's first retw needs
 *   to find.  xcp.sregs is that exception frame (see xtensa_swint()).
 *
 *   Note what the windowed ABI costs elsewhere: a frame's base save area
 *   holds the *absolute* stack pointer of its caller, so a stack copy is
 *   only usable at the address it was taken from.  That is why this
 *   architecture selects CONFIG_ARCH_VFORK_STACK_BORROW -- a vfork() child
 *   borrows rather than copies -- and why it cannot offer task_fork(), whose
 *   whole model is a relocated stack copy in a shared address environment.
 *
 ****************************************************************************/

static pid_t xtensa_fork(int type)
{
  struct tcb_s *parent = this_task();
  struct tcb_s *child;
  uint32_t     *sregs  = parent->xcp.sregs;
  uintptr_t     usp;
  uintptr_t     regstop;
  uintptr_t     newsp;
  int           index;

  DEBUGASSERT(sregs != NULL);

  /* This runs as the body of a system call, so the caller's own state is not
   * simply what is in the exception frame.
   */

  index = (int)parent->xcp.nsyscalls - 1;
  DEBUGASSERT(index >= 0);

  /* The stack pointer to work from is the *user* one.  A kernel build moves
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
                            type, usp);
  if (child == NULL)
    {
      sinfo("nxtask_setup_fork failed\n");
      return (pid_t)ERROR;
    }

  /* The child resumes on the parent's stack, at the parent's stack pointer,
   * and there is nothing to copy.  Both primitives arrive here that way:  a
   * fork() child inherits the parent's stack address inside its duplicated
   * address environment, and a borrowing vfork() child shares the parent's
   * outright.
   *
   * The test is on the stack *allocation*, not on stack_base_ptr:  a
   * borrowing child's base is moved up by the up_stack_frame() calls that
   * carve off its TLS block and argument vector, so the two bases differ
   * even though the memory is the same.
   *
   * There is no relocating branch because a relocated stack cannot work on
   * this architecture at all.  A frame's base save area holds the *absolute*
   * a1 of its caller, so a copy is only usable at the address it was taken
   * from; and the save area of the frame the child resumes into sits in the
   * 16 bytes *below* usp, where SPILL_ALL_WINDOWS left it on the way into
   * the kernel -- so a copy of [usp, stacktop) would not even contain the
   * words the child's first retw reads.  That is what the earlier copying
   * version of this function did, and it is why vfork() hung:  the child's
   * first return restored a0/a1 from memory nobody had written.
   */

  DEBUGASSERT(child->stack_alloc_ptr == parent->stack_alloc_ptr);
  DEBUGASSERT((uintptr_t)parent->stack_base_ptr + parent->adj_stack_size >
              usp);

  newsp = usp;

  /* Where the child's register context is restored from.  With a kernel
   * stack it must not be the user stack: the child is resumed by the same
   * path a system call returns through, which runs in kernel context.
   */

#ifdef CONFIG_ARCH_KERNEL_STACK
  if (child->xcp.kstack != NULL)
    {
      regstop = (uintptr_t)child->xcp.ktopstk;
    }
  else
#endif
    {
      regstop = newsp;
    }

  child->xcp.regs = (uint32_t *)(regstop - XCPTCONTEXT_SIZE);

  /* Start from the parent's context, then correct what must differ */

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

  return nxtask_start_fork(child, type);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

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
  return xtensa_fork(FORK_TYPE_FORK);
}
#endif

/****************************************************************************
 * Name: up_vfork
 *
 * Description:
 *   POSIX vfork():  the child borrows the parent's memory, and its stack,
 *   and the parent is suspended until it leaves.
 *
 ****************************************************************************/

#ifdef CONFIG_ARCH_HAVE_VFORK
pid_t up_vfork(void)
{
  return xtensa_fork(FORK_TYPE_VFORK);
}
#endif
