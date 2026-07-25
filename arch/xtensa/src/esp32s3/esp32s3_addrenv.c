/****************************************************************************
 * arch/xtensa/src/esp32s3/esp32s3_addrenv.c
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
#include <string.h>

#include <nuttx/addrenv.h>
#include <nuttx/arch.h>
#include <nuttx/pgalloc.h>

#include "esp32s3_addrenv.h"

#ifdef CONFIG_ARCH_ADDRENV

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: alloc_region
 *
 * Description:
 *   Allocate and wipe the physical page-pool (PSRAM) pages that back a
 *   single user region, recording them in the caller's page array.  On the
 *   ESP32-S3 the physical pages are only recorded here; the global cache-MMU
 *   table is (re)programmed lazily in up_addrenv_select().
 *
 * Input Parameters:
 *   pages    - Destination page array (physical addresses)
 *   maxpages - Capacity of the page array
 *   size     - Region size in bytes
 *   count    - Receives the number of pages actually allocated
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.  On failure
 *   *count reflects the pages allocated so far so the caller can free them.
 *
 ****************************************************************************/

static int alloc_region(uintptr_t *pages, unsigned int maxpages, size_t size,
                        uint16_t *count)
{
  unsigned int npages = MM_NPAGES(size);
  unsigned int i;

  *count = 0;

  if (npages > maxpages)
    {
      berr("ERROR: region needs %u pages, only %u available\n",
           npages, maxpages);
      return -E2BIG;
    }

  for (i = 0; i < npages; i++)
    {
      uintptr_t paddr = mm_pgalloc(1);
      if (paddr == 0)
        {
          *count = i;
          return -ENOMEM;
        }

      esp32s3_pgwipe(paddr);
      pages[i] = paddr;
    }

  *count = npages;
  return OK;
}

/****************************************************************************
 * Name: free_region
 *
 * Description:
 *   Return every page recorded in a region's page array to the page pool.
 *
 ****************************************************************************/

static void free_region(uintptr_t *pages, uint16_t *count)
{
  uint16_t i;

  for (i = 0; i < *count; i++)
    {
      if (pages[i] != 0)
        {
          mm_pgfree(pages[i], 1);
          pages[i] = 0;
        }
    }

  *count = 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_addrenv_create
 *
 * Description:
 *   This function is called when a new task is created in order to
 *   instantiate an address environment for the new task group.  Physical
 *   pages for .text, .data/.bss and the heap are allocated from the PSRAM
 *   page pool and recorded in 'addrenv'.  The reserved OS region (heap MM
 *   bookkeeping and signal delivery support) occupies the first page of the
 *   data window.
 *
 ****************************************************************************/

int up_addrenv_create(size_t textsize, size_t datasize, size_t heapsize,
                      arch_addrenv_t *addrenv)
{
  size_t datatotal;
  int    ret;

  DEBUGASSERT(addrenv);
  DEBUGASSERT(MM_ISALIGNED(CONFIG_ARCH_TEXT_VBASE));
  DEBUGASSERT(MM_ISALIGNED(CONFIG_ARCH_DATA_VBASE));
  DEBUGASSERT(MM_ISALIGNED(CONFIG_ARCH_HEAP_VBASE));

  /* Start from a clean slate */

  memset(addrenv, 0, sizeof(arch_addrenv_t));

  /* The data window carries the OS reserve at its base, followed by the
   * task's .data/.bss.  vdata is therefore reported past the reserve.
   */

  datatotal = MM_PGALIGNUP(ARCH_DATA_RESERVE_SIZE) + datasize;

  addrenv->textvbase = CONFIG_ARCH_TEXT_VBASE;
  addrenv->datavbase = CONFIG_ARCH_DATA_VBASE +
                       MM_PGALIGNUP(ARCH_DATA_RESERVE_SIZE);
  addrenv->heapvbase = CONFIG_ARCH_HEAP_VBASE;
  addrenv->heapsize  = heapsize;

  /* Allocate the backing pages for each region */

  ret = alloc_region(addrenv->textpages, CONFIG_ARCH_TEXT_NPAGES, textsize,
                     &addrenv->ntext);
  if (ret < 0)
    {
      goto errout;
    }

  ret = alloc_region(addrenv->datapages, CONFIG_ARCH_DATA_NPAGES, datatotal,
                     &addrenv->ndata);
  if (ret < 0)
    {
      goto errout;
    }

  ret = alloc_region(addrenv->heappages, CONFIG_ARCH_HEAP_NPAGES, heapsize,
                     &addrenv->nheap);
  if (ret < 0)
    {
      goto errout;
    }

  return OK;

errout:
  up_addrenv_destroy(addrenv);
  return ret;
}

/****************************************************************************
 * Name: up_addrenv_destroy
 *
 * Description:
 *   This function is called when a task group is finally deleted.  Return
 *   all of the group's physical pages to the page pool.
 *
 ****************************************************************************/

int up_addrenv_destroy(arch_addrenv_t *addrenv)
{
  DEBUGASSERT(addrenv);

  free_region(addrenv->textpages, &addrenv->ntext);
  free_region(addrenv->datapages, &addrenv->ndata);
  free_region(addrenv->heappages, &addrenv->nheap);

  memset(addrenv, 0, sizeof(arch_addrenv_t));
  return OK;
}

/****************************************************************************
 * Name: up_addrenv_vtext
 *
 * Description:
 *   Return the virtual address associated with the newly created .text
 *   address environment.
 *
 ****************************************************************************/

int up_addrenv_vtext(arch_addrenv_t *addrenv, void **vtext)
{
  DEBUGASSERT(addrenv && vtext);
  *vtext = (void *)addrenv->textvbase;
  return OK;
}

/****************************************************************************
 * Name: up_addrenv_vdata
 *
 * Description:
 *   Return the virtual address associated with the newly created .bss/.data
 *   address environment.
 *
 ****************************************************************************/

int up_addrenv_vdata(arch_addrenv_t *addrenv, uintptr_t textsize,
                     void **vdata)
{
  DEBUGASSERT(addrenv && vdata);
  *vdata = (void *)addrenv->datavbase;
  return OK;
}

/****************************************************************************
 * Name: up_addrenv_vheap
 *
 * Description:
 *   Return the heap virtual address associated with the newly created
 *   address environment.
 *
 ****************************************************************************/

int up_addrenv_vheap(const arch_addrenv_t *addrenv, void **vheap)
{
  DEBUGASSERT(addrenv && vheap);
  *vheap = (void *)addrenv->heapvbase;
  return OK;
}

/****************************************************************************
 * Name: up_addrenv_heapsize
 *
 * Description:
 *   Return the size of the initial heap allocation.
 *
 ****************************************************************************/

ssize_t up_addrenv_heapsize(const arch_addrenv_t *addrenv)
{
  DEBUGASSERT(addrenv);
  return (ssize_t)addrenv->heapsize;
}

#endif /* CONFIG_ARCH_ADDRENV */
