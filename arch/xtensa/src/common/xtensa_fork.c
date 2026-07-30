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

#include <arch/syscall.h>

#include <nuttx/arch.h>
#include <nuttx/compiler.h>
#include <nuttx/fork.h>
#include <nuttx/sched.h>

#include "sched/sched.h"
#include "xtensa.h"
#include "chip_macros.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The base save area:  the 16 bytes immediately *below* a frame's stack
 * pointer, holding the spilled a0-a3 of that frame's caller.  A window
 * overflow writes them there (s32e a0, <callee sp>, -16 and so on) and the
 * matching underflow reads them back, so this is the link that makes a
 * windowed frame chain walkable -- and the reason a stack copy that starts
 * at the stack pointer is a copy with its first link missing.
 */

#define BASE_SAVE_AREA   16
#define BASE_SAVE_A1     1   /* a0, a1, a2, a3 -- a1 is the caller's SP */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Everything a child is built from:  the register context of the thread that
 * called, and where that thread was.  There are two ways to come by it, and
 * they differ in more than provenance -- see up_vfork() below.
 */

struct fork_snapshot_s
{
  FAR const uint32_t *regs;  /* The caller's full register context */
  uintptr_t           usp;   /* The caller's user stack pointer */
  uintptr_t           pc;    /* Where the child resumes */
  uint32_t            a2;    /* What the child sees returned in A2 */
#ifndef CONFIG_BUILD_FLAT
  uintptr_t           ctx;   /* The caller's privilege, from its syscall */
#endif
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: xtensa_fork_rebase
 *
 * Description:
 *   Walk the frame chain of a relocated stack copy and correct it.
 *
 *   Every other architecture can copy a stack and adjust one register.  A
 *   windowed ABI cannot:  each frame's base save area holds the *absolute*
 *   stack pointer of its caller, so in a copy taken at a different address
 *   the whole chain still names the original.  The child's first `retw'
 *   would underflow onto the parent's stack and run there.
 *
 *   The chain is walkable, though, which is what makes the relocation
 *   possible at all.  From a frame at `sp', the caller's spilled a1 is one
 *   word into the base save area at [sp - 16); that names the caller's
 *   frame, and so on up to the outermost.  This adds the relocation offset
 *   to each link, which is the same job x86_64_fork.c does on the saved
 *   frame-pointer chain.
 *
 *   What is *not* corrected:  spilled a4-a15 in the extra save areas, and
 *   anything else in the copy that happens to be a stack address.  Those are
 *   data, not links, and they are the residue arm_fork.c calls a "feeble
 *   effort" -- a task_fork() or vfork() child is not entitled to them.  The
 *   frame chain is different in kind:  without it the child cannot execute
 *   one return instruction, entitled or not.
 *
 * Input Parameters:
 *   newsp    - The child's stack pointer
 *   usp      - The parent's stack pointer, which newsp is a relocation of
 *   stacktop - The top of the parent's stack; the walk ends there
 *   offset   - newsp - usp, the amount every link moves by
 *
 ****************************************************************************/

static void xtensa_fork_rebase(uintptr_t newsp, uintptr_t usp,
                               uintptr_t stacktop, intptr_t offset)
{
  uintptr_t csp = newsp;   /* The frame being fixed, in the child's copy */
  uintptr_t psp = usp;     /* The same frame, as the parent addresses it */

  while (psp < stacktop)
    {
      FAR uint32_t *save = (FAR uint32_t *)(csp - BASE_SAVE_AREA);
      uintptr_t caller = save[BASE_SAVE_A1];

      /* The chain grows towards the top of the stack and ends there.  Stop
       * on anything else rather than following it:  the outermost frame's
       * save area was never written by an overflow, so what is in it is
       * whatever the stack was coloured with.
       */

      if (caller <= psp || caller > stacktop)
        {
          break;
        }

      save[BASE_SAVE_A1] = (uint32_t)((uintptr_t)((intptr_t)caller + offset));

      psp = caller;
      csp = (uintptr_t)((intptr_t)caller + offset);
    }
}

/****************************************************************************
 * Name: xtensa_fork_stack
 *
 * Description:
 *   Give the child its stack pointer, and move the parent's frames to it if
 *   they have to move.
 *
 *   A fork() child stays on the parent's own stack memory, at the parent's
 *   addresses, inside its duplicated address environment.  It has nothing to
 *   copy.
 *
 *   A task_fork() or vfork() child gets a stack of its own.  That needs the
 *   copy, and on this architecture the copy needs xtensa_fork_rebase() to go
 *   with it.
 *
 *   The copy starts one base save area *below* the stack pointer.  That is
 *   not an off-by-one guard:  the frame the child resumes into keeps its
 *   caller's spilled a0-a3 there, put there by the SPILL_ALL_WINDOWS on the
 *   way in, and a copy of [usp, stacktop) would leave the child's first
 *   `retw' reading whatever the child's stack happened to contain.
 *
 * Input Parameters:
 *   parent - The parent task's TCB
 *   child  - The child task's TCB
 *   usp    - The parent's stack pointer
 *
 * Returned Value:
 *   The child's stack pointer.
 *
 ****************************************************************************/

static uintptr_t xtensa_fork_stack(FAR struct tcb_s *parent,
                                   FAR struct tcb_s *child,
                                   uintptr_t usp)
{
  uintptr_t stacktop;
  uintptr_t stackutil;
  uintptr_t newtop;
  uintptr_t newsp;

  stacktop = (uintptr_t)parent->stack_base_ptr + parent->adj_stack_size;
  DEBUGASSERT(stacktop > usp);

  /* Is the child on the parent's stack?  The test is on the stack
   * *allocation*:  a borrowing child's stack_base_ptr is moved up by the
   * up_stack_frame() calls that carve off its TLS block and its argument
   * vector, so the two bases differ even though the memory is the same.
   */

  if (child->stack_alloc_ptr == parent->stack_alloc_ptr)
    {
      return usp;
    }

  DEBUGASSERT(usp - BASE_SAVE_AREA >= (uintptr_t)parent->stack_base_ptr);

  stackutil = stacktop - (usp - BASE_SAVE_AREA);
  newtop    = (uintptr_t)child->stack_base_ptr + child->adj_stack_size;

  /* The copy has to fit, and the register save area goes below it when the
   * child has no kernel stack to put it on -- see xtensa_fork().
   */

  DEBUGASSERT(newtop - stackutil >
              (uintptr_t)child->stack_base_ptr + XCPTCONTEXT_SIZE);

  newsp = newtop - stackutil + BASE_SAVE_AREA;

  memcpy((FAR void *)(newsp - BASE_SAVE_AREA),
         (FAR const void *)(usp - BASE_SAVE_AREA), stackutil);

  xtensa_fork_rebase(newsp, usp, stacktop, (intptr_t)(newsp - usp));

  return newsp;
}

/****************************************************************************
 * Name: xtensa_fork
 *
 * Description:
 *   The common core of up_fork(), up_vfork() and up_task_fork().  They
 *   differ in the FORK_TYPE_* selector handed to nxtask_setup_fork(), which
 *   is where the memory semantics are decided, and in where their snapshot
 *   of the caller comes from.
 *
 * Input Parameters:
 *   type - One of the FORK_TYPE_* constants
 *   snap - The caller's context; see struct fork_snapshot_s
 *
 * Returned Value:
 *   The pid of the child, or ERROR on failure.
 *
 ****************************************************************************/

static pid_t xtensa_fork(int type, FAR const struct fork_snapshot_s *snap)
{
  FAR struct tcb_s *parent = this_task();
  FAR struct tcb_s *child;
  uintptr_t newsp;
  uintptr_t regstop;

  DEBUGASSERT(snap->regs != NULL && snap->pc != 0);

  /* Allocate and initialise a TCB for the child.  The start address is only
   * bookkeeping here: what the child actually resumes with is the register
   * context assembled below.
   */

  child = nxtask_setup_fork((start_t)snap->pc, type);
  if (child == NULL)
    {
      sinfo("nxtask_setup_fork failed\n");
      return (pid_t)ERROR;
    }

  newsp = xtensa_fork_stack(parent, child, snap->usp);

  /* Where the child's register context is restored from. */

#ifdef CONFIG_ARCH_KERNEL_STACK
  if (child->xcp.kstack != NULL)
    {
      /* On the kernel stack:  the child is resumed by the same path a system
       * call returns through, which runs in kernel context.
       */

      regstop = (uintptr_t)child->xcp.ktopstk;
    }
  else
#endif
    {
      /* There is no kernel stack, so it goes on the child's own stack, below
       * the base save area rather than at the stack pointer.  Writing
       * XCPTCONTEXT_SIZE bytes down from newsp would destroy the very words
       * the child's first `retw' reads -- which is the shape of bug that
       * cost armv7-a a prefetch abort on a null PC.  It is dead memory once
       * the context has been restored, so the child may then grow over it.
       *
       * This is only sound because a child without a kernel stack always has
       * a stack of its own:  writing here on a shared stack would land in
       * the parent's frames.  See xtensa_fork_stack().
       */

      DEBUGASSERT(child->stack_alloc_ptr != parent->stack_alloc_ptr);
      regstop = newsp - BASE_SAVE_AREA;
    }

  child->xcp.regs = (FAR uint32_t *)(regstop - XCPTCONTEXT_SIZE);

  /* Start from the parent's context, then correct what must differ */

  memcpy(child->xcp.regs, snap->regs, XCPTCONTEXT_SIZE);

  /* The child is not returning the way the parent will:  it is being
   * started.  Give it directly what its resume path would have produced --
   * the address to resume at, its privilege, its own stack, and the value
   * the call returns to it.
   *
   * The privilege matters most.  The frame copied above carries the world
   * the *exception* left in it, not the caller's; taking it would resume an
   * unprivileged process privileged.
   */

  child->xcp.regs[REG_PC] = snap->pc;
  child->xcp.regs[REG_A1] = newsp;
  child->xcp.regs[REG_A2] = snap->a2;

#ifndef CONFIG_BUILD_FLAT
  xtensa_restoreprivilege(child->xcp.regs, snap->ctx);
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

#ifdef CONFIG_LIB_SYSCALL
/****************************************************************************
 * Name: xtensa_fork_syscall
 *
 * Description:
 *   Fork from the system call that brought the caller into the kernel.
 *
 *   There is no assembly counterpart to this, and it does not need one.
 *   Other architectures enter through a stub that spills the caller's
 *   registers into a struct fork_s, because a C function cannot see its
 *   caller's callee-saved registers.  Here the system call has already done
 *   better than that:  _xtensa_context_save() runs SPILL_ALL_WINDOWS on
 *   every exception entry, so every live register window of the calling
 *   thread has been written to its stack and the whole context is in the
 *   exception frame.  Copying the stack therefore copies a complete and
 *   self-consistent frame chain.  xcp.sregs is that frame (see
 *   xtensa_swint()).
 *
 *   The child resumes where the system call would have returned, and A2 is
 *   the value the call yields -- 0, as fork() and friends return to a child.
 *
 ****************************************************************************/

static pid_t xtensa_fork_syscall(int type)
{
  FAR struct tcb_s *parent = this_task();
  struct fork_snapshot_s snap;
  int index;

  /* This runs as the body of a system call, so the caller's own state is not
   * simply what is in the exception frame.
   */

  DEBUGASSERT(parent->xcp.sregs != NULL);
  index = (int)parent->xcp.nsyscalls - 1;
  DEBUGASSERT(index >= 0);

  snap.regs = parent->xcp.sregs;
  snap.pc   = parent->xcp.syscall[index].sysreturn;
  snap.a2   = 0;
#ifndef CONFIG_BUILD_FLAT
  snap.ctx  = parent->xcp.syscall[index].int_ctx;
#endif

  /* The stack pointer to work from is the *user* one.  A kernel build moves
   * the outermost system call onto the thread's kernel stack and holds the
   * user stack pointer aside in ustkptr, so the A1 in the exception frame
   * names the kernel stack from here on -- measuring the parent's user stack
   * against it would produce a nonsense length.
   */

#ifdef CONFIG_ARCH_KERNEL_STACK
  snap.usp = parent->xcp.ustkptr != NULL ?
             (uintptr_t)parent->xcp.ustkptr : snap.regs[REG_A1];
#else
  snap.usp = snap.regs[REG_A1];
#endif

  return xtensa_fork(type, &snap);
}

#else /* CONFIG_LIB_SYSCALL */

/****************************************************************************
 * Name: xtensa_fork_direct
 *
 * Description:
 *   Fork a caller that did not come through a system call, which in a flat
 *   build is every caller:  vfork() and task_fork() are plain function
 *   calls into the same image.
 *
 *   The snapshot is taken here, by SYS_save_context.  That is not a software
 *   fallback for the entry stub this architecture lacks -- it *is* an entry
 *   stub, and one that exists in every build:  it is one of the four
 *   internal system calls defined outside CONFIG_LIB_SYSCALL in
 *   arch/xtensa/include/syscall.h and handled unconditionally in
 *   xtensa_swint().  Taking it spills every window and copies out the whole
 *   exception frame, which is exactly what a system call would have left
 *   behind.  Every voluntary context switch on this architecture already
 *   goes this way.
 *
 *   **The syscall is issued here rather than by calling
 *   up_saveusercontext(), and that is not a style choice.**  The snapshot
 *   records the stack pointer of whatever frame issues it, and the child is
 *   built to resume on a copy of the stack from that point up.  Everything
 *   this function goes on to call -- nxtask_setup_fork() and the rest --
 *   runs *below* its own frame, so with the syscall inlined here the copied
 *   region is stable for the whole operation.  Call up_saveusercontext()
 *   instead and the recorded stack pointer belongs to *that* function's
 *   frame, which is dead the moment it returns and is then reused by the
 *   fork machinery:  the child resumes on a frame that has been overwritten,
 *   and its first return goes wherever the debris points.  Observed as
 *   task_fork_test() reporting a child that "did not run", twice, the second
 *   time with a garbage pid.
 *
 *   Two things differ from the system-call path.  The child resumes at the
 *   instruction after the syscall rather than at a system call return, so
 *   the PC comes from the frame itself; and A2 is 1 rather than 0, so that
 *   the branch below tells the child apart from the parent.
 *
 * Returned Value:
 *   The pid of the child to the parent, 0 to the child, ERROR on failure.
 *
 ****************************************************************************/

static pid_t xtensa_fork_direct(int type)
{
  uint32_t regs[XCPTCONTEXT_REGS] aligned_data(16);
  struct fork_snapshot_s snap;

  if (sys_call1(SYS_save_context, (uintptr_t)regs) != 0)
    {
      /* The child, resumed from the context captured just above with A2 set
       * to 1.  It has nothing to do but leave.
       */

      return 0;
    }

  snap.regs = regs;
  snap.usp  = regs[REG_A1];
  snap.pc   = regs[REG_PC];
  snap.a2   = 1;

  return xtensa_fork(type, &snap);
}
#endif /* CONFIG_LIB_SYSCALL */

/****************************************************************************
 * Name: xtensa_fork_entry
 *
 * Description:
 *   Whichever of the two above this build has.
 *
 ****************************************************************************/

static inline pid_t xtensa_fork_entry(int type)
{
#ifdef CONFIG_LIB_SYSCALL
  return xtensa_fork_syscall(type);
#else
  return xtensa_fork_direct(type);
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_fork
 *
 * Description:
 *   POSIX fork():  the child receives its own copy of the parent's memory,
 *   at the same virtual addresses.
 *
 ****************************************************************************/

#ifdef CONFIG_ARCH_HAVE_FORK
pid_t up_fork(void)
{
  return xtensa_fork_entry(FORK_TYPE_FORK);
}
#endif

/****************************************************************************
 * Name: up_vfork
 *
 * Description:
 *   POSIX vfork():  the child borrows the parent's memory, and where the
 *   architecture allows it the parent's stack as well, and the parent is
 *   suspended until the child leaves.
 *
 ****************************************************************************/

#ifdef CONFIG_ARCH_HAVE_VFORK
pid_t up_vfork(void)
{
  return xtensa_fork_entry(FORK_TYPE_VFORK);
}
#endif

/****************************************************************************
 * Name: up_task_fork
 *
 * Description:
 *   task_fork():  the child shares the parent's memory but runs on a private
 *   copy of its stack, concurrently with the parent.  It is neither fork()
 *   nor vfork(), and the copy being at a different address is inherent to
 *   it -- which is why this is the primitive that needs
 *   xtensa_fork_rebase().
 *
 ****************************************************************************/

#ifdef CONFIG_ARCH_HAVE_TASK_FORK
pid_t up_task_fork(void)
{
  return xtensa_fork_entry(FORK_TYPE_TASK);
}
#endif
