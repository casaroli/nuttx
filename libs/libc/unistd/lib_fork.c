/****************************************************************************
 * libs/libc/unistd/lib_fork.c
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
#include <nuttx/arch.h>
#include <nuttx/tls.h>

#include <sched.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>
#include <errno.h>
#include <nuttx/debug.h>

#if defined(CONFIG_ARCH_HAVE_TASK_FORK) || defined(CONFIG_ARCH_HAVE_VFORK) || \
    defined(CONFIG_ARCH_HAVE_FORK)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_PTHREAD_ATFORK
/****************************************************************************
 * Name:  atfork_prepare
 *
 * Description:
 *    Invoke this method in the parent process before fork starts
 *
 ****************************************************************************/

static void atfork_prepare(void)
{
  FAR struct task_info_s *info = task_get_info();
  FAR struct list_node *list = &info->ta_atfork;
  FAR struct pthread_atfork_s *entry;

  /* According to posix standard, the prepare handlers are called in reverse
   * order of registration
   * so we iterate over the func list in reverse order
   */

  nxmutex_lock(&info->ta_lock);
  list_for_every_entry_reverse(list, entry,
                               struct pthread_atfork_s, node)
    {
       if (entry->prepare != NULL)
         {
           entry->prepare();
         }
    }

  nxmutex_unlock(&info->ta_lock);
}

/****************************************************************************
 * Name:  atfork_child
 *
 * Description:
 *    Invoke this method in the child process after fork completes
 *
 ****************************************************************************/

static void atfork_child(void)
{
  FAR struct task_info_s *info = task_get_info();
  FAR struct list_node *list = &info->ta_atfork;
  FAR struct pthread_atfork_s *entry;

  /* The parent handlers are called in the order of registration */

  nxmutex_lock(&info->ta_lock);
  list_for_every_entry(list, entry,
                       struct pthread_atfork_s, node)
    {
       if (entry->child != NULL)
         {
           entry->child();
         }
    }

  nxmutex_unlock(&info->ta_lock);
}

/****************************************************************************
 * Name:  atfork_parent
 *
 * Description:
 *    Invoke this method in the parent process after fork completes
 *
 ****************************************************************************/

static void atfork_parent(void)
{
  FAR struct task_info_s *info = task_get_info();
  FAR struct list_node *list = &info->ta_atfork;
  FAR struct pthread_atfork_s *entry;

  /* The child handlers are called in the order of registration */

  nxmutex_lock(&info->ta_lock);
  list_for_every_entry(list, entry,
                       struct pthread_atfork_s, node)
    {
      if (entry->parent != NULL)
        {
          entry->parent();
        }
    }

  nxmutex_unlock(&info->ta_lock);
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: task_fork
 *
 * Description:
 *   task_fork() clones the calling task.  The child shares the parent's
 *   .data, .bss and heap -- it joins the parent's address environment,
 *   exactly as a pthread does -- but runs on a private copy of the parent's
 *   stack, and it runs concurrently with the parent.
 *
 *   This is not POSIX and it is not fork().  It is the behaviour NuttX has
 *   always had, under an honest name:  writes by the child are visible to
 *   the parent, and vice versa.  New code should prefer pthread_create(),
 *   which is the same memory relationship spelled clearly, or posix_spawn().
 *
 *   It is a wrapper of the up_task_fork() syscall.
 *
 * Returned Value:
 *   Upon successful completion, task_fork() returns 0 to the child and
 *   returns the process ID of the child to the parent.  Otherwise, -1 is
 *   returned to the parent, no child is created, and errno is set to
 *   indicate the error.
 *
 ****************************************************************************/

#ifdef CONFIG_ARCH_HAVE_TASK_FORK
pid_t task_fork(void)
{
  pid_t pid;

#ifdef CONFIG_PTHREAD_ATFORK
  atfork_prepare();
#endif
  pid = up_task_fork();

#ifdef CONFIG_PTHREAD_ATFORK
  if (pid == 0)
    {
      atfork_child();
    }
  else
    {
      atfork_parent();
    }
#endif

  return pid;
}
#endif /* CONFIG_ARCH_HAVE_TASK_FORK */

/****************************************************************************
 * Name: vfork
 *
 * Description:
 *   The vfork() function is equivalent to fork(), except that the behavior
 *   is undefined if the process created by vfork() either modifies any data
 *   other than a variable of type pid_t used to store the return value from
 *   vfork(), or returns from the function in which vfork() was called, or
 *   calls any other function before successfully calling _exit() or one of
 *   the exec family of functions.
 *
 *   The child shares the parent's memory -- nothing is copied, which is the
 *   entire point of vfork() and the reason a caller chooses it -- and the
 *   parent is suspended until the child calls _exit() or exec().  That
 *   suspension is what makes the sharing safe, and it lives in the kernel
 *   primitive rather than here, so the parent is resumed at exec() as POSIX
 *   requires and vfork() does not depend on CONFIG_SCHED_WAITPID.
 *
 *   It is a wrapper of the up_vfork() syscall.
 *
 * Returned Value:
 *   Upon successful completion, vfork() returns 0 to the child process and
 *   returns the process ID of the child process to the parent process.
 *   Otherwise, -1 is returned to the parent, no child process is created,
 *   and errno is set to indicate the error.
 *
 ****************************************************************************/

#ifdef CONFIG_ARCH_HAVE_VFORK
pid_t vfork(void)
{
  pid_t pid;

#ifdef CONFIG_PTHREAD_ATFORK
  atfork_prepare();
#endif
  pid = up_vfork();

#ifdef CONFIG_PTHREAD_ATFORK
  if (pid == 0)
    {
      atfork_child();
    }
  else
    {
      atfork_parent();
    }
#endif

  return pid;
}
#endif /* CONFIG_ARCH_HAVE_VFORK */

/****************************************************************************
 * Name: fork
 *
 * Description:
 *   POSIX fork().  The child receives its own copy of the parent's memory,
 *   at the same virtual addresses.  It may modify anything, call anything,
 *   return from the function that called fork(), and it runs concurrently
 *   with the parent.  None of vfork()'s restrictions apply.
 *
 *   This requires an address environment that can be duplicated, so fork()
 *   exists only where CONFIG_ARCH_HAVE_FORK is selected.  Where it is not,
 *   fork() is not provided at all and calling it fails to build -- which is
 *   the point:  a build error naming the function is strictly better than
 *   silently giving the caller shared memory it did not ask for.
 *
 *   CONFIG_FORK_IS_TASK_FORK aliases fork() to task_fork() for legacy code
 *   that wants the historical NuttX behaviour under the historical name.
 *
 *   It is a wrapper of the up_fork() syscall.
 *
 * Returned Value:
 *   Upon successful completion, fork() returns 0 to the child process and
 *   returns the process ID of the child process to the parent process.
 *   Otherwise, -1 is returned to the parent, no child process is created,
 *   and errno is set to indicate the error.
 *
 ****************************************************************************/

#if defined(CONFIG_ARCH_HAVE_FORK)
pid_t fork(void)
{
  pid_t pid;

#ifdef CONFIG_PTHREAD_ATFORK
  atfork_prepare();
#endif
  pid = up_fork();

#ifdef CONFIG_PTHREAD_ATFORK
  if (pid == 0)
    {
      atfork_child();
    }
  else
    {
      atfork_parent();
    }
#endif

  return pid;
}
#elif defined(CONFIG_FORK_IS_TASK_FORK)
pid_t fork(void)
{
  return task_fork();
}
#endif

#endif /* CONFIG_ARCH_HAVE_TASK_FORK || CONFIG_ARCH_HAVE_VFORK ||
        * CONFIG_ARCH_HAVE_FORK */
