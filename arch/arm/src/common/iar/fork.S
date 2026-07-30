/****************************************************************************
 * arch/arm/src/common/iar/fork.S
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

#include "arm_fork.h"

	MODULE arm_fork_common
	SECTION .text:CODE:NOROOT(2), "ax"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Public Symbols
 ****************************************************************************/

#ifdef CONFIG_TASK_FORK
	PUBLIC	up_task_fork
#endif
#ifdef CONFIG_ARCH_HAVE_VFORK
	PUBLIC	up_vfork
#endif
#ifdef CONFIG_ARCH_HAVE_FORK
	PUBLIC	up_fork
#endif
	EXTERN	arm_fork

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
 *   by a FORK_TYPE_* constant passed to arm_fork() in r1.
 *
 *   See include/nuttx/fork.h for what the three primitives mean.
 *
 *   The overall sequence is:
 *
 *   1) User code calls task_fork(), vfork() or fork().  Each is a libc
 *      wrapper around the matching entry point here.
 *   2) The entry point collects the context and calls arm_fork().
 *   3) arm_fork() calls nxtask_setup_fork(), which allocates and configures
 *      the child task's TCB.  This consists of:
 *      - Allocation of the child task's TCB.
 *      - Initialization of file descriptors and streams
 *      - Configuration of environment variables
 *      - Establishing the child's address environment
 *      - Allocating the stack, or inheriting the parent's for fork()
 *      - Setup the input parameters for the task.
 *      - Initialization of the TCB (including call to up_initial_state())
 *   4) arm_fork() provides any additional operating context. arm_fork must:
 *      - Initialize special values in any CPU registers that were not
 *        already configured by up_initial_state()
 *      - Relocate the copied stack, unless the child shares the parent's
 *   5) arm_fork() then calls nxtask_start_fork() or nxtask_start_vfork()
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

	THUMB

#ifdef CONFIG_TASK_FORK
up_task_fork:
	movs		r2, #FORK_TYPE_TASK
	b		arm_fork_common
#endif

#ifdef CONFIG_ARCH_HAVE_VFORK
up_vfork:
	movs		r2, #FORK_TYPE_VFORK
	b		arm_fork_common
#endif

#ifdef CONFIG_ARCH_HAVE_FORK
up_fork:
	movs		r2, #FORK_TYPE_FORK
	b		arm_fork_common
#endif

/* The shared snapshot.  r2 holds the FORK_TYPE_* selector on entry; it is
 * call-clobbered and is not part of the snapshot, and lr still holds the
 * original caller's return address because the entry points above branched
 * here rather than calling.
 */

arm_fork_common:
	/* Create a stack frame */

	mov		r0, sp			/* Save the value of the stack on entry */
	sub		sp, sp, #FORK_SIZEOF	/* Allocate the structure on the stack */

	/* CPU registers */
	/* Save the volatile registers */

	str		r4, [sp, #FORK_R4_OFFSET]
	str		r5, [sp, #FORK_R5_OFFSET]
	str		r6, [sp, #FORK_R6_OFFSET]
	str		r7, [sp, #FORK_R7_OFFSET]
	str		r8, [sp, #FORK_R8_OFFSET]
	str		r9, [sp, #FORK_R9_OFFSET]
	str		r10, [sp, #FORK_R10_OFFSET]

	/* Save the frame pointer, stack pointer, and return address */

	str		r11, [sp, #FORK_FP_OFFSET] /* fp not defined. use r11 */
	str		r0, [sp, #FORK_SP_OFFSET]
	str		lr, [sp, #FORK_LR_OFFSET]

	/* Floating point registers (not yet) */

	/* Then, call arm_fork(), passing it a pointer to the stack structure
	 * and the selector that says which primitive was called.
	 */

	mov		r0, sp
	mov		r1, r2
	bl		arm_fork

	/* Release the stack data and return the value returned by arm_fork */

	ldr		lr, [sp, #FORK_LR_OFFSET]
	add		sp, sp, #FORK_SIZEOF
	bx		lr

	END
