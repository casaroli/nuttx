/************************************************************************************
 * arch/mips/src/mips32/fork.S
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
 ************************************************************************************/

/************************************************************************************
 * Included Files
 ************************************************************************************/

#include <nuttx/config.h>

#include "mips_fork.h"

/************************************************************************************
 * Pre-processor Definitions
 ************************************************************************************/

/************************************************************************************
 * Public Symbols
 ************************************************************************************/

	.file	"fork.S"
	.globl	mips_fork

/************************************************************************************
 * Public Functions
 ************************************************************************************/

/************************************************************************************
 * Name: up_task_fork, up_vfork, up_fork
 *
 * Description:
 *   The up_fork() function is the base of fork() function that provided in
 *   libc, and fork() is implemented as a wrapper of up_fork() function.
 *   The fork() function has the same effect as posix fork(), except that the behavior is
 *   undefined if the process created by fork() either modifies any data other than
 *   a variable of type pid_t used to store the return value from fork(), or returns
 *   from the function in which fork() was called, or calls any other function before
 *   successfully calling _exit() or one of the exec family of functions.
 *
 *   This thin layer implements fork by simply calling up_fork() with the fork()
 *   context as an argument.  The overall sequence is:
 *
 *   1) User code calls fork().  fork() collects context information and
 *      transfers control up up_fork().
 *   2) mips_fork() and calls nxtask_setup_fork().
 *   3) nxtask_setup_fork() allocates and configures the child task's TCB.  This
 *      consists of:
 *      - Allocation of the child task's TCB.
 *      - Initialization of file descriptors and streams
 *      - Configuration of environment variables
 *      - Allocate and initialize the stack
 *      - Setup the input parameters for the task.
 *      - Initialization of the TCB (including call to up_initial_state())
 *   4) mips_fork() provides any additional operating context. mips_fork must:
 *      - Initialize special values in any CPU registers that were not
 *        already configured by up_initial_state()
 *   5) mips_fork() then calls nxtask_start_fork()
 *   6) nxtask_start_fork() then executes the child thread.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Upon successful completion, fork() returns 0 to the child process and returns
 *   the process ID of the child process to the parent process. Otherwise, -1 is
 *   returned to the parent, no child process is created, and errno is set to
 *   indicate the error.
 *
 ************************************************************************************/

	.text
	.align  2
	.set	nomips16
#ifdef CONFIG_MIPS_MICROMIPS
	.set	micromips
#endif

#ifdef CONFIG_TASK_FORK
	.globl	up_task_fork
	.type	up_task_fork, function
	.ent	up_task_fork
up_task_fork:
	li		$a1, FORK_TYPE_TASK
	b		mips_fork_common
	nop
	.end	up_task_fork
	.size	up_task_fork, .-up_task_fork
#endif

#ifdef CONFIG_ARCH_HAVE_VFORK
	.globl	up_vfork
	.type	up_vfork, function
	.ent	up_vfork
up_vfork:
	li		$a1, FORK_TYPE_VFORK
	b		mips_fork_common
	nop
	.end	up_vfork
	.size	up_vfork, .-up_vfork
#endif

#ifdef CONFIG_ARCH_HAVE_FORK
	.globl	up_fork
	.type	up_fork, function
	.ent	up_fork
up_fork:
	li		$a1, FORK_TYPE_FORK
	b		mips_fork_common
	nop
	.end	up_fork
	.size	up_fork, .-up_fork
#endif

	/* The shared snapshot.  $a1 holds the FORK_TYPE_* selector on entry and
	 * is carried through to mips_fork(); it is not part of the snapshot, and
	 * $ra still holds the original caller's return address because the entry
	 * points above branched here rather than calling.
	 */

	.type	mips_fork_common, function
	.ent	mips_fork_common

mips_fork_common:
	/* Create a stack frame */

	move	$t0, $sp					/* Save the value of the stack on entry */
	addiu	$sp, $sp, -FORK_SIZEOF		/* Allocate the structure on the stack */

	/* CPU registers */
	/* Save the saved registers */

	sw		$s0, FORK_S0_OFFSET($sp)
	sw		$s1, FORK_S1_OFFSET($sp)
	sw		$s2, FORK_S2_OFFSET($sp)
	sw		$s3, FORK_S3_OFFSET($sp)
	sw		$s4, FORK_S4_OFFSET($sp)
	sw		$s5, FORK_S5_OFFSET($sp)
	sw		$s6, FORK_S6_OFFSET($sp)
	sw		$s7, FORK_S7_OFFSET($sp)

#ifdef CONFIG_MIPS32_FRAMEPOINTER
	sw		$fp, FORK_FP_OFFSET($sp)
#else
	sw		$s8, FORK_S8_OFFSET($sp)
#endif

	/* Save the global pointer, stack pointer, and return address */

	sw		$t0, FORK_SP_OFFSET($sp)
	sw		$ra, FORK_RA_OFFSET($sp)
#ifdef MIPS32_SAVE_GP
	sw		$gp, FORK_GP_OFFSET($sp)
#endif

	/* Floating point registers (not yet) */

	/* Then, call mips_fork(), passing it a pointer to the stack structure.
	 * $a1 already holds the FORK_TYPE_* selector.
	 */

	move	$a0, $sp
	jal		mips_fork
	nop

	/* Release the stack data and return the value returned by mips_fork */

	lw		$ra, FORK_RA_OFFSET($sp)
	addiu	$sp, $sp, FORK_SIZEOF
	j		$ra

	.end	mips_fork_common
	.size	mips_fork_common, .-mips_fork_common
