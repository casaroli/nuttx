/****************************************************************************
 * include/nuttx/fork.h
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

#ifndef __INCLUDE_NUTTX_FORK_H
#define __INCLUDE_NUTTX_FORK_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* NuttX has three primitives that clone the calling task in place, returning
 * twice.  They differ in exactly two respects -- what happens to memory, and
 * what happens to the parent -- and every other part of the machinery is
 * common.  These constants select between them.
 *
 *   FORK_TYPE_TASK   task_fork()
 *     The child joins the parent's address environment:  .data, .bss and the
 *     heap are shared, exactly as they are for a pthread.  The child runs on
 *     a private copy of the parent's stack and runs concurrently with the
 *     parent.  This is not POSIX; it is the historical behaviour that NuttX
 *     used to publish under the name fork().
 *
 *   FORK_TYPE_VFORK  vfork()
 *     The child joins the parent's address environment, and the parent is
 *     suspended until the child calls _exit() or one of the exec family of
 *     functions.  The suspension is what makes the sharing safe.  Where
 *     CONFIG_ARCH_VFORK_STACK_BORROW is selected the child also borrows the
 *     parent's stack rather than copying it.
 *
 *   FORK_TYPE_FORK   fork()
 *     The child receives a duplicate of the parent's address environment --
 *     its own pages, holding a copy of the parent's contents, mapped at the
 *     same virtual addresses -- and runs concurrently with the parent.  This
 *     is POSIX fork(), and it requires an address environment that can be
 *     duplicated.
 *
 * These are plain macros rather than an enumeration because the
 * architecture-specific assembly entry points load them into a register
 * before branching into the common snapshot sequence.
 */

#define FORK_TYPE_TASK   0
#define FORK_TYPE_VFORK  1
#define FORK_TYPE_FORK   2

#endif /* __INCLUDE_NUTTX_FORK_H */
