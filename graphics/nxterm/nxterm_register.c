/****************************************************************************
 * graphics/nxterm/nxterm_register.c
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

#include <sys/types.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <nuttx/debug.h>

#include <nuttx/kmalloc.h>
#include <nuttx/fs/fs.h>
#include <nuttx/spinlock.h>

#include "nxterm.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_gridalloc
 *
 * Description:
 *   Size the character grid to the window and the font, and allocate it.
 *
 *   The grid replaces the list of positioned glyphs this driver used to
 *   keep, and it is the smaller of the two:  one character code and one
 *   attribute byte per cell, against a glyph pointer and a pixel coordinate
 *   per character written.  What it buys is that a cell can be addressed,
 *   which is what absolute cursor positioning needs and what a list that
 *   only ever grew at one end could not offer.
 *
 *   CONFIG_NXTERM_MXCHARS is not consulted.  It described how many
 *   characters the old list would remember before it began dropping them;
 *   a grid remembers exactly what fits on the display, no more and no
 *   fewer.
 *
 * Returned Value:
 *   Zero on success, a negated errno on failure.
 *
 ****************************************************************************/

int nxterm_gridalloc(FAR struct nxterm_state_s *priv)
{
  unsigned int ncells;
  uint16_t rows;
  uint16_t cols;

  DEBUGASSERT(priv->fwidth > 0 && priv->lineheight > 0);

  cols = priv->wndo.wsize.w / priv->fwidth;
  rows = priv->wndo.wsize.h / priv->lineheight;

  if (cols < 1)
    {
      cols = 1;
    }

  if (rows < 1)
    {
      rows = 1;
    }

  ncells = (unsigned int)rows * (unsigned int)cols;

  /* Reuse the allocation when the geometry has not actually changed */

  if (priv->cells != NULL && priv->rows == rows && priv->cols == cols)
    {
      return OK;
    }

  kmm_free(priv->cells);

  priv->cells = (FAR struct nxterm_cell_s *)
    kmm_malloc(ncells * sizeof(struct nxterm_cell_s));

  if (priv->cells == NULL)
    {
      gerr("ERROR: Failed to allocate a %u x %u character grid\n",
           rows, cols);

      priv->rows = 0;
      priv->cols = 0;
      return -ENOMEM;
    }

  priv->rows = rows;
  priv->cols = cols;
  return OK;
}

/****************************************************************************
 * Name: nxterm_gridfree
 ****************************************************************************/

void nxterm_gridfree(FAR struct nxterm_state_s *priv)
{
  kmm_free(priv->cells);

  priv->cells = NULL;
  priv->rows  = 0;
  priv->cols  = 0;
}

/****************************************************************************
 * Name: nxterm_gridreset
 *
 * Description:
 *   Blank the grid and put the terminal back in its initial state.  The
 *   display itself is not touched; the caller repaints or clears it.
 *
 ****************************************************************************/

void nxterm_gridreset(FAR struct nxterm_state_s *priv)
{
  int row;

  for (row = 0; row < (int)priv->rows; row++)
    {
      nxterm_blank(priv, row, 0, (int)priv->cols - 1);
    }

  priv->crow      = 0;
  priv->ccol      = 0;
  priv->savedrow  = 0;
  priv->savedcol  = 0;
  priv->savedattr = 0;
  priv->strow     = 0;
  priv->sbrow     = priv->rows > 0 ? priv->rows - 1 : 0;
  priv->attr      = 0;
  priv->wrap      = false;
  priv->cvisible  = true;
  priv->cshown    = false;
  priv->cdrawrow  = 0;
  priv->cdrawcol  = 0;
  priv->vtstate   = VT100_STATE_NONE;
  priv->nparams   = 0;
  priv->vtprivate = false;
}

/****************************************************************************
 * Name: nxterm_allocate
 ****************************************************************************/

FAR struct nxterm_state_s *
  nxterm_register(NXTERM handle, FAR struct nxterm_window_s *wndo,
                  FAR const struct nxterm_operations_s *ops, int minor)
{
  FAR struct nxterm_state_s *priv;
  FAR const struct nx_font_s *fontset;
  char devname[NX_DEVNAME_SIZE];
  NXHANDLE hfont;
  int ret;

  DEBUGASSERT(handle && wndo && ops && (unsigned)minor < 256);

  /* Allocate the driver structure */

  priv = (FAR struct nxterm_state_s *)
    kmm_zalloc(sizeof(struct nxterm_state_s));
  if (!priv)
    {
      gerr("ERROR: Failed to allocate the NX driver structure\n");
      return NULL;
    }

  /* Initialize the driver structure */

  priv->ops     = ops;
  priv->handle  = handle;
  priv->minor   = minor;
  memcpy(&priv->wndo, wndo, sizeof(struct nxterm_window_s));

  nxmutex_init(&priv->lock);
#ifdef CONFIG_DEBUG_GRAPHICS
  priv->holder  = NO_HOLDER;
#endif

#ifdef CONFIG_NXTERM_NXKBDIN
  nxsem_init(&priv->waitsem, 0, 0);

  /* Echo keyboard input by default, as the serial driver does.  ICANON is
   * deliberately left clear:  reads from this driver are always
   * character-at-a-time and there is no line-editing logic behind them,
   * so claiming canonical mode would be a lie that readline() acts on.
   */

  priv->tc_lflag = ECHO;
#ifdef CONFIG_NXTERM_SIGINT
  priv->pid      = INVALID_PROCESS_ID;
  priv->intr_echo = false;
#endif
  priv->escape   = NXTERM_ESCAPE_NONE;
#endif

  spin_lock_init(&priv->spinlock);

  /* Connect to the font cache for the configured font characteristics */

  priv->fcache = nxf_cache_connect(wndo->fontid, wndo->fcolor[0],
                                   wndo->wcolor[0], CONFIG_NXTERM_BPP,
                                   CONFIG_NXTERM_CACHESIZE);
  if (priv->fcache == NULL)
    {
      gerr("ERROR: Failed to connect to font cache for font ID %d: %d\n",
           wndo->fontid, get_errno());
      goto errout;
    }

  /* Get the handle of the font managed by the font cache */

  /* The reverse-video cache, for the character under the cursor.  Failing to
   * get one is not fatal: the cursor simply falls back to a plain block.
   */

  priv->rcache = nxf_cache_connect(wndo->fontid, wndo->wcolor[0],
                                   wndo->fcolor[0], CONFIG_NXTERM_BPP,
                                   CONFIG_NXTERM_CACHESIZE);

  hfont = nxf_cache_getfonthandle(priv->fcache);
  if (hfont == NULL)
    {
      gerr("ERROR: Failed to get handlr for font ID %d: %d\n",
           wndo->fontid, get_errno());
      goto errout;
    }

  /* Get information about the font set being used and save this in the
   * state structure
   */

  fontset           = nxf_getfontset(hfont);
  priv->fheight     = fontset->mxheight;
  priv->fwidth      = fontset->mxwidth;
  priv->lineheight  = fontset->mxheight + CONFIG_NXTERM_LINESEPARATION;

  /* Set up the character grid */

  ret = nxterm_gridalloc(priv);
  if (ret < 0)
    {
      goto errout;
    }

  nxterm_gridreset(priv);

  /* Clear the display */

  nxterm_clear(priv);

  /* Show the cursor */

  nxterm_showcursor(priv);

  /* Register the driver */

  snprintf(devname, sizeof(devname), NX_DEVNAME_FORMAT, minor);
  ret = register_driver(devname, &g_nxterm_drvrops, 0600, priv);
  if (ret < 0)
    {
      gerr("ERROR: Failed to register %s\n", devname);
    }

  return (NXTERM)priv;

errout:
  nxterm_gridfree(priv);
  nxmutex_destroy(&priv->lock);
#ifdef CONFIG_NXTERM_NXKBDIN
  nxsem_destroy(&priv->waitsem);
#endif
  if (priv->rcache != NULL)
    {
      nxf_cache_disconnect(priv->rcache);
    }

  if (priv->fcache != NULL)
    {
      nxf_cache_disconnect(priv->fcache);
    }

  kmm_free(priv);
  return NULL;
}
