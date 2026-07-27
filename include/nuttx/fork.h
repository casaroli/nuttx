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

/* Which primitive a clone of the calling task implements.  Passed to
 * nxtask_setup_fork(), which is where the memory semantics are decided.
 * Macros rather than an enumeration:  the architecture entry points are
 * assembly and load them into a register.
 *
 *   FORK_TYPE_TASK   task_fork():  shares memory, private stack copy, both
 *                    run.  Not POSIX.
 *   FORK_TYPE_VFORK  vfork():  shares memory, parent suspended until the
 *                    child _exit()s or exec()s.
 *   FORK_TYPE_FORK   fork():  child gets its own copy of the parent's memory
 *                    at the same virtual addresses, both run.
 */

#define FORK_TYPE_TASK   0
#define FORK_TYPE_VFORK  1
#define FORK_TYPE_FORK   2

#endif /* __INCLUDE_NUTTX_FORK_H */
