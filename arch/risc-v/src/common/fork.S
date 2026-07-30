/****************************************************************************
 * arch/risc-v/src/common/fork.S
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

#include "riscv_fork.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Public Symbols
 ****************************************************************************/

  .file  "fork.S"
  .globl riscv_fork
#ifdef CONFIG_TASK_FORK
  .globl up_task_fork
#endif
#ifdef CONFIG_ARCH_HAVE_VFORK
  .globl up_vfork
#endif
#ifdef CONFIG_ARCH_HAVE_FORK
  .globl up_fork
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_task_fork, up_vfork, up_fork
 *
 * Description:
 *   These are the architecture-specific entry points of NuttX's three
 *   cloning primitives.  All three need exactly the same thing from
 *   assembly -- a snapshot of the caller's callee-saved registers, stack
 *   pointer and return address -- and differ only in what the C code then
 *   does with it, so they share one snapshot sequence and are distinguished
 *   by a FORK_TYPE_* constant passed to riscv_fork() in a1.
 *
 *   See include/nuttx/fork.h for what the three primitives mean.
 *
 *   The overall sequence is:
 *
 *   1) User code calls task_fork(), vfork() or fork().  Each is a libc
 *      wrapper around the matching entry point here.
 *   2) The entry point collects the context and calls riscv_fork().
 *   3) riscv_fork() calls nxtask_setup_fork(), which allocates and
 *      configures the child task's TCB.
 *   4) riscv_fork() provides any additional operating context and relocates
 *      the copied stack.
 *   5) riscv_fork() then calls nxtask_start_fork(), or nxtask_start_vfork()
 *      which additionally suspends the caller.
 *   6) which executes the child thread.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Upon successful completion, 0 is returned to the child and the process
 *   ID of the child is returned to the parent.  Otherwise, -1 is returned to
 *   the parent, no child is created, and errno is set to indicate the error.
 *
 ****************************************************************************/

#ifdef CONFIG_TASK_FORK
.type up_task_fork, function

up_task_fork:
  li          a1, FORK_TYPE_TASK
  j           riscv_fork_common

  .size  up_task_fork, .-up_task_fork
#endif

#ifdef CONFIG_ARCH_HAVE_VFORK
.type up_vfork, function

up_vfork:
  li          a1, FORK_TYPE_VFORK
  j           riscv_fork_common

  .size  up_vfork, .-up_vfork
#endif

#ifdef CONFIG_ARCH_HAVE_FORK
.type up_fork, function

up_fork:
  li          a1, FORK_TYPE_FORK
  j           riscv_fork_common

  .size  up_fork, .-up_fork
#endif

/* The shared snapshot.  a1 holds the FORK_TYPE_* selector on entry and is
 * carried through to riscv_fork(); it is not part of the snapshot, and ra
 * still holds the original caller's return address because the entry points
 * above jumped here rather than calling.
 */

.type riscv_fork_common, function

riscv_fork_common:

#ifdef CONFIG_LIB_SYSCALL
  /* When coming via system call, everything is in place already */

  tail        riscv_fork
#else
  /* Create a stack frame */

  addi        sp, sp, -FORK_SIZEOF

  /* CPU registers */
  /* Save the volatile registers */

  REGSTORE    s1, FORK_S1_OFFSET(sp)
  REGSTORE    s2, FORK_S2_OFFSET(sp)
  REGSTORE    s3, FORK_S3_OFFSET(sp)
  REGSTORE    s4, FORK_S4_OFFSET(sp)
  REGSTORE    s5, FORK_S5_OFFSET(sp)
  REGSTORE    s6, FORK_S6_OFFSET(sp)
  REGSTORE    s7, FORK_S7_OFFSET(sp)
  REGSTORE    s8, FORK_S8_OFFSET(sp)
  REGSTORE    s9, FORK_S9_OFFSET(sp)
  REGSTORE    s10, FORK_S10_OFFSET(sp)
  REGSTORE    s11, FORK_S11_OFFSET(sp)

  /* Save the frame pointer, stack pointer, and return address */

#ifdef CONFIG_RISCV_FRAMEPOINTER
  REGSTORE    fp, FORK_FP_OFFSET(sp)
#else
  REGSTORE    s0, FORK_S0_OFFSET(sp)
#endif

#ifdef RISCV_SAVE_GP
  REGSTORE    gp, FORK_GP_OFFSET(sp)
#endif

  addi        a0, sp, FORK_SIZEOF
  REGSTORE    a0, FORK_SP_OFFSET(sp) /* original SP */
  REGSTORE    x1, FORK_RA_OFFSET(sp) /* return address */

  /* Floating point registers */

#ifdef CONFIG_ARCH_FPU
  FSTORE      fs0, FORK_FS0_OFFSET(sp)
  FSTORE      fs1, FORK_FS1_OFFSET(sp)
  FSTORE      fs2, FORK_FS2_OFFSET(sp)
  FSTORE      fs3, FORK_FS3_OFFSET(sp)
  FSTORE      fs4, FORK_FS4_OFFSET(sp)
  FSTORE      fs5, FORK_FS5_OFFSET(sp)
  FSTORE      fs6, FORK_FS6_OFFSET(sp)
  FSTORE      fs7, FORK_FS7_OFFSET(sp)
  FSTORE      fs8, FORK_FS8_OFFSET(sp)
  FSTORE      fs9, FORK_FS9_OFFSET(sp)
  FSTORE      fs10, FORK_FS10_OFFSET(sp)
  FSTORE      fs11, FORK_FS11_OFFSET(sp)
#endif

  /* Then, call riscv_fork(), passing it a pointer to the stack frame.  a1
   * already holds the FORK_TYPE_* selector.
   */

  mv          a0, sp
  call        riscv_fork

  /* Release the stack frame and return the value returned by riscv_fork */

  REGLOAD     x1, FORK_RA_OFFSET(sp)
  addi        sp, sp, FORK_SIZEOF
  ret
#endif

  .size  riscv_fork_common, .-riscv_fork_common
  .end
