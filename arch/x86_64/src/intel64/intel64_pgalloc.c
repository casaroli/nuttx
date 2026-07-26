/****************************************************************************
 * arch/x86_64/src/intel64/intel64_pgalloc.c
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

#include <nuttx/arch.h>
#include <nuttx/config.h>

#include <assert.h>
#include <nuttx/debug.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Additional checks for CONFIG_ARCH_PGPOOL_MAPPING */

#ifdef CONFIG_ARCH_PGPOOL_MAPPING
#  if CONFIG_ARCH_PGPOOL_VBASE != (CONFIG_ARCH_PGPOOL_PBASE + X86_64_LOAD_OFFSET)
#    error invalid PGPOOL configuration
#  endif
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_allocate_pgheap
 *
 * Description:
 *   If there is a page allocator in the configuration, then this function
 *   must be provided by the platform-specific code.  The OS initialization
 *   logic will call this function early in the initialization sequence to
 *   get the page heap information needed to configure the page allocator.
 *
 ****************************************************************************/

void up_allocate_pgheap(void **heap_start, size_t *heap_size)
{
  DEBUGASSERT(heap_start && heap_size);

  /* mm_pginitialize() wants the *physical* base of the pool:  the pages
   * mm_pgalloc() hands out are physical addresses, which is what every
   * consumer here assumes -- they all pass them through x86_64_pgvaddr()
   * before touching them.  Give it a virtual base and that translation
   * silently returns 0, and the first x86_64_pgwipe() memsets NULL.
   */

#ifndef CONFIG_ARCH_PGPOOL_MAPPING
  /* pgheap at the end of RAM */

  *heap_start = (void *)X86_64_PGPOOL_BASE;
  *heap_size  = (size_t)X86_64_PGPOOL_SIZE;
#else
  /* pgheap defined with Kconfig options */

  *heap_start = (void *)CONFIG_ARCH_PGPOOL_PBASE;
  *heap_size  = (size_t)CONFIG_ARCH_PGPOOL_SIZE;
#endif
}
