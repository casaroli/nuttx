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
#include <stdbool.h>
#include <stdint.h>
#include <sched.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <nuttx/debug.h>

#include <nuttx/queue.h>

#include "sched/sched.h"
#include "environ/environ.h"
#include "group/group.h"
#include "task/task.h"
#include <nuttx/tls.h>

#include "tls/tls.h"

#include <nuttx/semaphore.h>

/* fork() requires architecture-specific support as well as waipid(). */

#ifdef CONFIG_ARCH_HAVE_FORK

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxtask_setup_fork
 *
 * Description:
 *   The fork() function has the same effect as posix fork(), except that the
 *   behavior is undefined if the process created by fork() either modifies
 *   any data other than a variable of type pid_t used to store the return
 *   value from fork(), or returns from the function in which fork() was
 *   called, or calls any other function before successfully calling _exit()
 *   or one of the exec family of functions.
 *
 *   This function provides one step in the overall fork() sequence:  It
 *   Allocates and initializes the child task's TCB.  The overall sequence
 *   is:
 *
 *   1) User code calls fork().  fork() is provided in
 *      architecture-specific code.
 *   2) fork()and calls nxtask_setup_fork().
 *   3) nxtask_setup_fork() allocates and configures the child task's TCB.
 *      This consists of:
 *      - Allocation of the child task's TCB.
 *      - Initialization of file descriptors and streams
 *      - Configuration of environment variables
 *      - Allocate and initialize the stack
 *      - Setup the input parameters for the task.
 *      - Initialization of the TCB (including call to up_initial_state())
 *   4) up_fork() provides any additional operating context. up_fork must:
 *      - Initialize special values in any CPU registers that were not
 *        already configured by up_initial_state()
 *   5) up_fork() then calls nxtask_start_fork()
 *   6) nxtask_start_fork() then executes the child thread.
 *
 * Input Parameters:
 *   retaddr - Return address
 *   argsize - Location to return the argument size
 *
 * Returned Value:
 *   Upon successful completion, nxtask_setup_fork() returns a pointer to
 *   newly allocated and initialized child task's TCB.  NULL is returned
 *   on any failure and the errno is set appropriately.
 *
 ****************************************************************************/

FAR struct tcb_s *nxtask_setup_fork(start_t retaddr, bool share,
                                    uintptr_t usp)
{
  FAR struct tcb_s *ptcb = this_task();
  FAR struct tcb_s *parent;
  FAR struct tcb_s *child;
  FAR char **argv;
  size_t stack_size;
  uint8_t ttype;
  int priority;
  int ret;
  bool inherited = false;
#if defined(CONFIG_ARCH_ADDRENV) && defined(CONFIG_ARCH_HAVE_ADDRENV_FORK)
  FAR struct addrenv_s *oldenv = NULL;
  bool selected = false;
#endif

  DEBUGASSERT(retaddr != NULL);

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

#if defined(CONFIG_ARCH_ADDRENV) && defined(CONFIG_ARCH_HAVE_ADDRENV_FORK)
  if (ttype != TCB_FLAG_TTYPE_KERNEL && !share)
    {
      /* Give the child its own copy of the parent's memory, at the same
       * virtual addresses.  This is what fork() is supposed to mean, and it
       * is what lets the child use the parent's stack: everything in that
       * stack that points into itself stays valid only because the copy
       * lives at the same address.
       *
       * This happens before anything is allocated on the child's behalf,
       * and the child's environment is then made current for the rest of
       * the setup.  Otherwise those allocations would come out of whichever
       * heap happened to be instantiated -- the parent's -- and the child
       * would carry pointers to blocks that do not exist in its own heap,
       * to be freed out of it when the child exits.
       *
       * Everything the setup reads from the parent (environ, the argument
       * vector) is legible under the child's environment too, at the same
       * address and with the same contents, precisely because it is a copy.
       */

      ret = addrenv_fork(parent, child);
      if (ret < 0)
        {
          goto errout_with_tcb;
        }

      ret = addrenv_select(child->addrenv_own, &oldenv);
      if (ret < 0)
        {
          goto errout_with_tcb;
        }

      selected = true;
    }
#endif

  /* Allocate a new task group with the same privileges as the parent */

  ret = group_allocate(child, ttype);
  if (ret < 0)
    {
      goto errout_with_tcb;
    }

#if defined(CONFIG_ARCH_ADDRENV)
  if (ttype != TCB_FLAG_TTYPE_KERNEL && child->addrenv_own == NULL)
    {
      /* Without the ability to duplicate an address environment the child
       * can only share the parent's, which is weaker than fork() -- the two
       * processes see each other's writes.
       */

      ret = addrenv_join(parent, child);
      if (ret < 0)
        {
          goto errout_with_tcb;
        }
    }
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

  /* Allocate the stack for the TCB */

  stack_size = (uintptr_t)ptcb->stack_base_ptr -
               (uintptr_t)ptcb->stack_alloc_ptr + ptcb->adj_stack_size;

#ifdef CONFIG_ARCH_HAVE_VFORK
  if (share)
    {
      /* vfork():  the child borrows the parent's stack.  POSIX describes
       * exactly this -- the child runs in the parent's context until it
       * leaves through _exit() or exec(), which is why the parent must be
       * suspended for the duration.  Giving it a copy at some other address
       * would not be vfork(), and on architectures whose frame chain holds
       * absolute pointers into the stack (Xtensa's windowed ABI, for one) a
       * relocated copy is not even usable: the child's first return unwinds
       * onto the parent's stack anyway, having corrupted its own.
       *
       * The stack belongs to the parent, so TCB_FLAG_FREE_STACK is left
       * clear and the child does not release it.
       *
       * Note the temporary adj_stack_size:  up_initial_state() lays the
       * child's initial register frame down at stack_base_ptr +
       * adj_stack_size, and that must not land on the parent's live frames.
       * Reporting the stack as ending at the parent's current stack pointer
       * puts the frame just below them -- in the region the child is about
       * to use anyway.  The full extent is restored once that is done.
       */

      child->stack_alloc_ptr = ptcb->stack_alloc_ptr;
      child->stack_base_ptr  = ptcb->stack_base_ptr;
      inherited              = true;

      if (usp > (uintptr_t)ptcb->stack_base_ptr &&
          usp <= (uintptr_t)ptcb->stack_base_ptr + ptcb->adj_stack_size)
        {
          child->adj_stack_size = usp - (uintptr_t)ptcb->stack_base_ptr;
        }
      else
        {
          child->adj_stack_size = ptcb->adj_stack_size;
        }
    }
  else
#endif
#if defined(CONFIG_ARCH_ADDRENV) && defined(CONFIG_ARCH_HAVE_ADDRENV_FORK)
  if (ttype != TCB_FLAG_TTYPE_KERNEL)
    {
      /* The child's address environment is a copy of the parent's, so the
       * parent's stack already exists in it, at the same address and with
       * the same contents -- including the allocator bookkeeping that owns
       * it.  Take those addresses rather than allocating a second stack,
       * which would be both a waste and, since it would land somewhere
       * else, useless to a copied frame chain.  The child frees it on exit
       * out of its own heap, exactly as the parent will out of the parent's.
       */

      child->stack_alloc_ptr = ptcb->stack_alloc_ptr;
      child->stack_base_ptr  = ptcb->stack_base_ptr;
      child->adj_stack_size  = ptcb->adj_stack_size;
      inherited              = true;
    }
  else
#endif
    {
      ret = up_create_stack(child, stack_size, ttype);
      if (ret < OK)
        {
          goto errout_with_tcb;
        }
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

  if (inherited)
    {
      FAR struct tls_info_s *info;

      /* The child's environment is already current (see above), so this
       * writes to the child's stack, not the parent's.
       */

      ret = nxtask_setup_scheduler(child, priority, retaddr,
                                   ptcb->entry.main, ttype);
      if (ret < OK)
        {
          goto errout_with_tcb;
        }

      /* The child took the parent's stack -- borrowed it (vfork) or got its
       * own copy of it at the same address (fork) -- so thread-local storage
       * and the argument vector are already there, at the same addresses.
       * Building them again would carve a second TLS frame off a stack that
       * already has one, and would write the argument vector over the
       * parent's outermost frame.  Only the fields that must differ from the
       * parent's are corrected.
       */

      info = (FAR struct tls_info_s *)child->stack_alloc_ptr;
      info->tl_task = child->group->tg_info;
      info->tl_tid  = child->pid;
    }
  else
    {
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
    }

#ifdef CONFIG_ARCH_HAVE_VFORK
  if (share)
    {
      /* The initial frame is placed; let the child see the whole stack it is
       * borrowing, so that the architecture code below resumes it exactly
       * where the parent was.
       */

      child->adj_stack_size = ptcb->adj_stack_size;
    }
#endif

  /* Now we have enough in place that we can join the group */

  group_initialize(child);

#if defined(CONFIG_ARCH_ADDRENV) && defined(CONFIG_ARCH_HAVE_ADDRENV_FORK)
  if (selected)
    {
      addrenv_restore(oldenv);
    }
#endif

  sinfo("parent=%p, returning child=%p\n", parent, child);
  return child;

errout_with_tcb:
#if defined(CONFIG_ARCH_ADDRENV) && defined(CONFIG_ARCH_HAVE_ADDRENV_FORK)
  if (selected)
    {
      addrenv_restore(oldenv);
    }
#endif

  nxsched_release_tcb((FAR struct tcb_s *)child, ttype);
errout:
  set_errno(-ret);
  return NULL;
}

/****************************************************************************
 * Name: nxtask_start_fork
 *
 * Description:
 *   The fork() function has the same effect as fork(), except that the
 *   behavior is undefined if the process created by fork() either modifies
 *   any data other than a variable of type pid_t used to store the return
 *   value from fork(), or returns from the function in which fork() was
 *   called, or calls any other function before successfully calling _exit()
 *   or one of the exec family of functions.
 *
 *   This function provides one step in the overall fork() sequence:  It
 *   starts execution of the previously initialized TCB.  The overall
 *   sequence is:
 *
 *   1) User code calls fork()
 *   2) Architecture-specific code provides fork()and calls
 *      nxtask_setup_fork().
 *   3) nxtask_setup_fork() allocates and configures the child task's TCB.
 *      This consists of:
 *      - Allocation of the child task's TCB.
 *      - Initialization of file descriptors and streams
 *      - Configuration of environment variables
 *      - Allocate and initialize the stack
 *      - Setup the input parameters for the task.
 *      - Initialization of the TCB (including call to up_initial_state())
 *   4) fork() provides any additional operating context. fork must:
 *      - Initialize special values in any CPU registers that were not
 *        already configured by up_initial_state()
 *   5) fork() then calls nxtask_start_fork()
 *   6) nxtask_start_fork() then executes the child thread.
 *
 * Input Parameters:
 *   child - The tcb_s struct instance that created by
 *           nxtask_setup_fork() method
 *   wait_child - whether need to wait until the child is running finished
 *
 * Returned Value:
 *   Upon successful completion, fork() returns 0 to the child process and
 *   returns the process ID of the child process to the parent process.
 *   Otherwise, -1 is returned to the parent, no child process is created,
 *   and errno is set to indicate the error.
 *
 ****************************************************************************/

pid_t nxtask_start_fork(FAR struct tcb_s *child)
{
  pid_t pid;

  sinfo("Starting Child TCB=%p\n", child);
  DEBUGASSERT(child);

  /* Get the assigned pid before we start the task */

  pid = child->pid;

  /* Activate the task */

  nxtask_activate(child);

  return pid;
}

/****************************************************************************
 * Name: nxtask_start_vfork
 *
 * Description:
 *   Start a vfork() child and suspend the caller until it leaves through
 *   _exit() or exec().
 *
 *   The suspension belongs here, not in the caller.  A vfork() child borrows
 *   the parent's stack, so the parent must not execute a single instruction
 *   between the child being started and the child finishing -- anything it
 *   ran would use frames the child has already taken.  Suspending later,
 *   in libc, is too late: the parent returns through several frames first.
 *
 *   The semaphore lives in this frame, which is above the stack pointer the
 *   child starts from, so the child cannot disturb it.  Its address is left
 *   in the child's group for nxtask_vfork_release() to find.
 *
 ****************************************************************************/

pid_t nxtask_start_vfork(FAR struct tcb_s *child)
{
  sem_t  sem;
  pid_t  pid;

  DEBUGASSERT(child != NULL && child->group != NULL);

  nxsem_init(&sem, 0, 0);

  child->group->tg_vforksem = &sem;

  pid = nxtask_start_fork(child);
  if (pid > 0)
    {
      nxsem_wait_uninterruptible(&sem);
    }

  nxsem_destroy(&sem);
  return pid;
}

/****************************************************************************
 * Name: nxtask_vfork_release
 *
 * Description:
 *   Release the parent of a vfork() child, if this task is one.
 *
 ****************************************************************************/

void nxtask_vfork_release(FAR struct tcb_s *tcb)
{
  FAR sem_t *sem;

  if (tcb == NULL || tcb->group == NULL)
    {
      return;
    }

  sem = tcb->group->tg_vforksem;
  if (sem != NULL)
    {
      /* Clear it first:  the parent destroys the semaphore as soon as it
       * wakes, and this must not be posted twice.
       */

      tcb->group->tg_vforksem = NULL;
      nxsem_post(sem);
    }
}

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

#endif /* CONFIG_ARCH_HAVE_FORK */
