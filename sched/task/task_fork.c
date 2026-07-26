/****************************************************************************
 * sched/task/task_fork.c
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

#include <sys/wait.h>
#include <stdint.h>
#include <sched.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <nuttx/debug.h>

#include <nuttx/fork.h>
#include <nuttx/kmalloc.h>
#include <nuttx/queue.h>
#include <nuttx/semaphore.h>

#include "sched/sched.h"
#include "environ/environ.h"
#include "group/group.h"
#include "task/task.h"
#include "tls/tls.h"

/* This file is the common core of task_fork(), vfork() and fork(); it is
 * built if the architecture can provide any one of them.
 */

#if defined(CONFIG_ARCH_HAVE_TASK_FORK) || defined(CONFIG_ARCH_HAVE_VFORK) || \
    defined(CONFIG_ARCH_HAVE_FORK)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_ARCH_VFORK_STACK_BORROW
/* Written at the boundary between the part of the parent's stack lent to the
 * vfork() child and the reserve kept for the parent's own remaining frames.
 */

#  define VFORK_STACK_CANARY ((uintptr_t)0xdead0f0e0f0e0f0eull)
#endif

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

#if defined(CONFIG_ARCH_ADDRENV) && defined(CONFIG_ARCH_HAVE_FORK)
static int nxtask_fork_addrenv(FAR struct tcb_s *child);
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_ARCH_VFORK_STACK_BORROW
/****************************************************************************
 * Name: vfork_borrow_stack
 *
 * Description:
 *   Give the vfork() child the unused part of the parent's stack instead of
 *   a stack of its own.  This is required on architectures whose stack
 *   frames contain absolute stack addresses -- the windowed ABI of Xtensa,
 *   whose register-window save areas hold the spilled stack pointer of each
 *   frame, so that a relocated copy unwinds onto the parent's stack.
 *
 *   The child's region is [stack_base_ptr, parent_sp - RESERVE).  The
 *   reserve is headroom for the frames the parent still has to execute
 *   before it comes to rest in nxsem_wait():  the remainder of
 *   nxtask_setup_fork(), nxtask_start_vfork() and the block itself.  The
 *   parent is suspended for the whole lifetime of the child, so the two
 *   never run on the stack at the same time; the reserve is only about
 *   where each of them is allowed to be.
 *
 *   A canary is written at the boundary and checked when the parent resumes,
 *   so that an undersized reserve is a loud failure rather than a quiet
 *   corruption.
 *
 * Input Parameters:
 *   parent    - The parent task's TCB
 *   child     - The child task's TCB
 *   parent_sp - The parent's stack pointer at the point vfork() was called
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int vfork_borrow_stack(FAR struct tcb_s *parent,
                              FAR struct tcb_s *child,
                              uintptr_t parent_sp)
{
  uintptr_t base = (uintptr_t)parent->stack_base_ptr;
  uintptr_t top;

  DEBUGASSERT(parent_sp > base);

  if (parent_sp <= base + CONFIG_ARCH_VFORK_STACK_RESERVE)
    {
      serr("ERROR: Not enough parent stack left to borrow\n");
      return -ENOMEM;
    }

  top = parent_sp - CONFIG_ARCH_VFORK_STACK_RESERVE;

  /* The child does not own this memory:  stack_alloc_ptr is recorded so that
   * stack checking and backtraces work, but TCB_FLAG_FREE_STACK is
   * deliberately left clear so that up_release_stack() will not free the
   * parent's allocation out from under it.
   */

  child->stack_alloc_ptr = parent->stack_alloc_ptr;
  child->stack_base_ptr  = parent->stack_base_ptr;
  child->adj_stack_size  = top - base;
  child->flags          &= ~TCB_FLAG_FREE_STACK;

  *(FAR uintptr_t *)top = VFORK_STACK_CANARY;

  return OK;
}
#endif /* CONFIG_ARCH_VFORK_STACK_BORROW */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxtask_setup_fork
 *
 * Description:
 *   Allocate and initialize the child task's TCB.  This is one step in the
 *   sequence common to task_fork(), vfork() and fork(); see the comment
 *   above the prototype in include/nuttx/sched.h for the whole sequence, and
 *   include/nuttx/fork.h for what the three primitives mean.
 *
 *   Exactly two things depend on `type':
 *
 *   - the address environment.  task_fork() and vfork() join the parent's,
 *     so the child shares .data, .bss and the heap.  fork() duplicates it,
 *     so the child gets its own copy at the same virtual addresses.
 *   - the stack.  Normally the child gets a stack of its own, which the
 *     architecture-specific code then fills with a relocated copy of the
 *     parent's.  A vfork() child on an architecture that selects
 *     CONFIG_ARCH_VFORK_STACK_BORROW instead borrows the parent's.
 *
 * Input Parameters:
 *   retaddr   - Address at which the child resumes
 *   type      - One of the FORK_TYPE_* constants
 *   parent_sp - The caller's stack pointer, used when borrowing a stack
 *
 * Returned Value:
 *   Upon successful completion, nxtask_setup_fork() returns a pointer to
 *   newly allocated and initialized child task's TCB.  NULL is returned
 *   on any failure and the errno is set appropriately.
 *
 ****************************************************************************/

FAR struct tcb_s *nxtask_setup_fork(start_t retaddr, int type,
                                    uintptr_t parent_sp)
{
  FAR struct tcb_s *ptcb = this_task();
  FAR struct tcb_s *parent;
  FAR struct tcb_s *child;
  FAR char **argv;
  size_t stack_size;
  uint8_t ttype;
  int priority;
  int ret;

  DEBUGASSERT(retaddr != NULL);
  DEBUGASSERT(type == FORK_TYPE_TASK || type == FORK_TYPE_VFORK ||
              type == FORK_TYPE_FORK);

  UNUSED(parent_sp);

  /* Get the type of the fork'ed task (kernel or user) */

  if ((ptcb->flags & TCB_FLAG_TTYPE_MASK) == TCB_FLAG_TTYPE_KERNEL)
    {
      /* Fork'ed from a kernel thread */

      ttype = TCB_FLAG_TTYPE_KERNEL;
      parent = ptcb;
    }
  else
    {
      /* Fork'ed from a user task or pthread */

      ttype = TCB_FLAG_TTYPE_TASK;
      if ((ptcb->flags & TCB_FLAG_TTYPE_MASK) == TCB_FLAG_TTYPE_TASK)
        {
          parent = ptcb;
        }
      else
        {
          parent = nxsched_get_tcb(ptcb->group->tg_pid);
          if (parent == NULL)
            {
              ret = -ENOENT;
              goto errout;
            }
        }
    }

  /* Allocate a TCB for the child task. */

  child = kmm_zalloc(sizeof(struct tcb_s));
  if (!child)
    {
      serr("ERROR: Failed to allocate TCB\n");
      ret = -ENOMEM;
      goto errout;
    }

  child->flags |= TCB_FLAG_FREE_TCB;

  /* Initialize the task join */

  nxtask_joininit(child);

  /* Allocate a new task group with the same privileges as the parent */

  ret = group_allocate(child, ttype);
  if (ret < 0)
    {
      goto errout_with_tcb;
    }

#if defined(CONFIG_ARCH_ADDRENV)
  if (ttype != TCB_FLAG_TTYPE_KERNEL && type != FORK_TYPE_FORK)
    {
      /* task_fork() and vfork():  join the parent address environment,
       * exactly as pthread_create() does.  The child shares .data, .bss and
       * the heap.
       */

      ret = addrenv_join(parent, child);
      if (ret < 0)
        {
          goto errout_with_tcb;
        }
    }

  /* POSIX fork() deliberately does *not* establish the child's address
   * environment here.  Everything that follows -- the stack, the argument
   * vector, thread-local storage -- is allocated out of the parent's user
   * heap and written through the parent's mappings, because the parent's is
   * the address environment that is current.  Duplicating before that point
   * would snapshot a heap that does not yet contain the child's stack, and
   * would then fill a stack the child cannot see.  So the duplication is
   * deferred to nxtask_start_fork(), by which time the architecture code has
   * finished building the child's stack and the snapshot captures it.
   */
#else
  /* Without address environments there is only one address space, so
   * everything except the stack is shared no matter which primitive was
   * called.  POSIX fork() cannot be provided at all, and CONFIG_ARCH_HAVE_
   * FORK is not selected, so `type' can never be FORK_TYPE_FORK here.
   */

  DEBUGASSERT(type != FORK_TYPE_FORK);
#endif

  /* Duplicate the parent tasks environment */

  ret = env_dup(child->group, environ);
  if (ret < 0)
    {
      goto errout_with_tcb;
    }

  /* Associate file descriptors with the new task */

  ret = group_setuptaskfiles(child, NULL, false);
  if (ret < OK)
    {
      goto errout_with_tcb;
    }

  /* Set the task name */

  argv = nxsched_get_stackargs(parent);
  nxtask_setup_name(child, argv[0]);

  /* Allocate the stack for the TCB -- or borrow the parent's */

#ifdef CONFIG_ARCH_VFORK_STACK_BORROW
  if (type == FORK_TYPE_VFORK)
    {
      ret = vfork_borrow_stack(parent, child, parent_sp);
    }
  else
#endif
    {
      stack_size = (uintptr_t)ptcb->stack_base_ptr -
                   (uintptr_t)ptcb->stack_alloc_ptr + ptcb->adj_stack_size;

      ret = up_create_stack(child, stack_size, ttype);
    }

  if (ret < OK)
    {
      goto errout_with_tcb;
    }

#if defined(CONFIG_ARCH_ADDRENV) && defined(CONFIG_ARCH_KERNEL_STACK)
  /* Allocate the kernel stack */

  if (ttype != TCB_FLAG_TTYPE_KERNEL)
    {
      ret = up_addrenv_kstackalloc(child);
      if (ret < 0)
        {
          goto errout_with_tcb;
        }
    }
#endif

  /* Get the priority of the parent task */

#ifdef CONFIG_PRIORITY_INHERITANCE
  priority = ptcb->base_priority;   /* "Normal," unboosted priority */
#else
  priority = ptcb->sched_priority;  /* Current priority */
#endif

  /* Initialize the task control block.  This calls up_initial_state() */

  sinfo("Child priority=%d start=%p\n", priority, retaddr);
  ret = nxtask_setup_scheduler(child, priority, retaddr,
                               ptcb->entry.main, ttype);
  if (ret < OK)
    {
      goto errout_with_tcb;
    }

  /* Setup thread local storage */

  ret = tls_dup_info(child, parent);
  if (ret < OK)
    {
      goto errout_with_tcb;
    }

  /* Setup to pass parameters to the new task */

  ret = nxtask_setup_stackargs(child, argv[0], &argv[1]);
  if (ret < OK)
    {
      goto errout_with_tcb;
    }

  /* Now we have enough in place that we can join the group */

  group_initialize(child);
  sinfo("parent=%p, returning child=%p\n", parent, child);
  return child;

errout_with_tcb:
  nxsched_release_tcb((FAR struct tcb_s *)child, ttype);
errout:
  set_errno(-ret);
  return NULL;
}

/****************************************************************************
 * Name: nxtask_start_fork
 *
 * Description:
 *   The last step of all three primitives:  finish the child and run it.
 *   The architecture-specific code calls this once it has built the child's
 *   register context and stack.
 *
 *   For POSIX fork() this is also where the parent's address environment is
 *   duplicated -- see the note in nxtask_setup_fork() for why it cannot
 *   happen earlier.  For vfork() this additionally suspends the caller.
 *
 * Input Parameters:
 *   child - The tcb_s struct instance created by nxtask_setup_fork()
 *   type  - One of the FORK_TYPE_* constants
 *
 * Returned Value:
 *   The process ID of the child, or ERROR on failure.
 *
 ****************************************************************************/

pid_t nxtask_start_fork(FAR struct tcb_s *child, int type)
{
  pid_t pid;

  sinfo("Starting Child TCB=%p type=%d\n", child, type);
  DEBUGASSERT(child);

#if defined(CONFIG_ARCH_ADDRENV) && defined(CONFIG_ARCH_HAVE_FORK)
  if (type == FORK_TYPE_FORK &&
      (child->flags & TCB_FLAG_TTYPE_MASK) != TCB_FLAG_TTYPE_KERNEL)
    {
      int ret = nxtask_fork_addrenv(child);
      if (ret < 0)
        {
          nxtask_abort_fork(child, -ret);
          return (pid_t)ERROR;
        }
    }
#endif

  /* Get the assigned pid before we start the task */

  pid = child->pid;

#ifdef CONFIG_ARCH_HAVE_VFORK
  if (type == FORK_TYPE_VFORK)
    {
      return nxtask_start_vfork(child);
    }
#endif

  /* Activate the task */

  nxtask_activate(child);

  return pid;
}

#if defined(CONFIG_ARCH_ADDRENV) && defined(CONFIG_ARCH_HAVE_FORK)
/****************************************************************************
 * Name: nxtask_fork_addrenv
 *
 * Description:
 *   Give a POSIX fork() child its own copy of the parent's memory.
 *
 *   This runs late, after the architecture code has allocated the child's
 *   stack out of the parent's user heap and filled it with a copy of the
 *   parent's, and it runs with the *parent's* address environment still
 *   current.  That ordering is the whole point:  the snapshot taken here
 *   therefore contains the child's stack, with its contents, at the virtual
 *   address the child will actually run on.  Duplicating any earlier would
 *   hand the child a stack it could not see.
 *
 *   Having taken the snapshot, the parent no longer needs its own copy of
 *   that allocation -- the child owns the duplicate, at the same address, in
 *   its own address environment -- so it is returned to the parent's heap.
 *   The child's copy remains marked allocated in the child's heap, and
 *   up_release_stack() will free it there when the child exits.
 *
 * Input Parameters:
 *   child - The child's TCB
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int nxtask_fork_addrenv(FAR struct tcb_s *child)
{
  FAR void *stack = child->stack_alloc_ptr;
  int ret;

  ret = addrenv_fork(this_task(), child);
  if (ret < 0)
    {
      return ret;
    }

  /* Release the parent's copy of the child's stack.  Note that this must
   * come after the snapshot, and that it does not disturb the child:  the
   * child's heap metadata was copied while the allocation was still live.
   */

  if (stack != NULL && (child->flags & TCB_FLAG_FREE_STACK) != 0)
    {
      kumm_free(stack);
    }

  return OK;
}
#endif /* CONFIG_ARCH_ADDRENV && CONFIG_ARCH_HAVE_FORK */

#ifdef CONFIG_ARCH_HAVE_VFORK
/****************************************************************************
 * Name: nxtask_start_vfork
 *
 * Description:
 *   Start execution of a vfork() child and suspend the caller until the
 *   child calls _exit() or one of the exec family of functions.
 *
 *   The suspension lives here, in the kernel primitive, rather than in a
 *   libc waitpid() as it once did.  Two things follow from that.  The parent
 *   is released when the child's TCB is torn down (see
 *   nxtask_vfork_resume()), which for an exec()ing child is immediately
 *   after exec_swap() has handed the child's pid to the program it loaded --
 *   so the parent resumes at exec(), holding a pid that names the running
 *   program, as POSIX requires.  And vfork() no longer depends on
 *   CONFIG_SCHED_WAITPID.
 *
 * Input Parameters:
 *   child - The tcb_s struct instance created by nxtask_setup_fork()
 *
 * Returned Value:
 *   The process ID of the child.
 *
 ****************************************************************************/

pid_t nxtask_start_vfork(FAR struct tcb_s *child)
{
  struct vfork_s vfork;
  pid_t pid;
  int ret;
#ifdef CONFIG_ARCH_VFORK_STACK_BORROW
  uintptr_t canary = (uintptr_t)child->stack_base_ptr +
                     child->adj_stack_size;
#endif

  sinfo("Starting vfork Child TCB=%p\n", child);
  DEBUGASSERT(child);

  /* The rendezvous lives in this frame.  We are about to block in it and
   * will not leave until the child has posted, so it outlives every use.
   */

  nxsem_init(&vfork.sem, 0, 0);
  vfork.released  = false;
  child->vfork_rel = &vfork;

  pid = child->pid;

  nxtask_activate(child);

  /* Wait for the child to _exit() or exec().  This is not a cancellation
   * point and must not be interrupted by a signal:  the child may be running
   * on our stack, so returning early would corrupt it.
   */

  do
    {
      ret = nxsem_wait_uninterruptible(&vfork.sem);
    }
  while (ret == -EINTR);

  nxsem_destroy(&vfork.sem);

#ifdef CONFIG_ARCH_VFORK_STACK_BORROW
  /* The child ran on the unused part of our stack, below a reserve that our
   * own remaining frames had to fit into.  Check the canary that
   * vfork_borrow_stack() left at the boundary:  if the reserve was too small
   * then we and the child have been using the same memory, and the only
   * honest thing to do is say so rather than return into a mangled frame.
   */

  if (*(FAR uintptr_t *)canary != VFORK_STACK_CANARY)
    {
      serr("ERROR: vfork() child overran CONFIG_ARCH_VFORK_STACK_RESERVE\n");
      PANIC();
    }
#endif

  return pid;
}

/****************************************************************************
 * Name: nxtask_vfork_resume
 *
 * Description:
 *   Release the vfork() parent suspended on this child, if there is one.
 *
 *   This is called from nxsched_release_tcb(), which is the last point in
 *   the child's life:  by then the child is off the ready-to-run list and
 *   has stopped touching a borrowed stack, and an exec()ing child has
 *   already activated the program it loaded under its own pid.  Calling it
 *   any earlier -- from nxtask_exithook(), say -- would let the parent
 *   resume while the child was still executing its exit path on the parent's
 *   stack.
 *
 *   nxsched_release_tcb() is also reached from nxtask_abort_fork(), so a
 *   fork that fails after the rendezvous was established releases the parent
 *   too.
 *
 * Input Parameters:
 *   child - The TCB being torn down
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void nxtask_vfork_resume(FAR struct tcb_s *child)
{
  FAR struct vfork_s *vfork = child->vfork_rel;

  if (vfork != NULL && !vfork->released)
    {
      vfork->released  = true;
      child->vfork_rel = NULL;
      nxsem_post(&vfork->sem);
    }
}
#endif /* CONFIG_ARCH_HAVE_VFORK */

/****************************************************************************
 * Name: nxtask_abort_fork
 *
 * Description:
 *   Recover from any errors after nxtask_setup_fork() was called.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void nxtask_abort_fork(FAR struct tcb_s *child, int errcode)
{
  /* The TCB was added to the active task list by nxtask_setup_scheduler() */

  dq_rem((FAR dq_entry_t *)child, list_inactivetasks());

  /* Release the TCB */

  nxsched_release_tcb(child, child->flags & TCB_FLAG_TTYPE_MASK);
  set_errno(errcode);
}

#endif /* CONFIG_ARCH_HAVE_TASK_FORK || CONFIG_ARCH_HAVE_VFORK ||
        * CONFIG_ARCH_HAVE_FORK */
