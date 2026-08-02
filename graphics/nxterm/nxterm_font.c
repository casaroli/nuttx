/****************************************************************************
 * graphics/nxterm/nxterm_font.c
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

#include <string.h>
#include <assert.h>
#include <errno.h>
#include <nuttx/debug.h>

#include "nxterm.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_effattr
 *
 * Description:
 *   The attributes a cell is to be drawn with, which is what the cell holds
 *   except where the cursor is sitting on it.
 *
 *   Carrying the cursor here rather than as a separate object is what makes
 *   a redraw arriving from the NX server -- which knows nothing about a
 *   cursor -- put the block back where it belongs.
 *
 ****************************************************************************/

static uint8_t nxterm_effattr(FAR struct nxterm_state_s *priv,
                              int row, int col)
{
  uint8_t attr = nxterm_cellat(priv, row, col)->attr;

  if (priv->cshown && row == (int)priv->cdrawrow &&
      col == (int)priv->cdrawcol)
    {
      attr ^= NXTERM_ATTR_REVERSE;
    }

  return attr;
}

/****************************************************************************
 * Name: nxterm_cellbounds
 *
 * Description:
 *   The pixel rectangle covered by a run of cells on one row.
 *
 ****************************************************************************/

static void nxterm_cellbounds(FAR struct nxterm_state_s *priv,
                              int row, int col1, int col2,
                              FAR struct nxgl_rect_s *rect)
{
  rect->pt1.x = (nxgl_coord_t)(col1 * priv->fwidth);
  rect->pt1.y = (nxgl_coord_t)(row * priv->lineheight);
  rect->pt2.x = (nxgl_coord_t)((col2 + 1) * priv->fwidth - 1);
  rect->pt2.y = (nxgl_coord_t)((row + 1) * priv->lineheight - 1);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_paintcell
 *
 * Description:
 *   Draw one cell of the grid.
 *
 *   'fillbg' says whether the cell's background has to be laid down first.
 *   It can be skipped when the caller knows the cell already stands on the
 *   window background -- either because it has just filled a larger region
 *   containing this cell, or because the cell being overwritten was blank.
 *   That is not a micro-optimisation: skipping it is what keeps ordinary
 *   typing at one drawing operation per character, which is what it cost
 *   before there was a grid at all.
 *
 *   A cell in reverse video is always filled, since its background is not
 *   the window background.
 *
 * Input Parameters:
 *   priv   - Driver state
 *   row    - The cell row
 *   col    - The cell column
 *   clip   - Restrict drawing to this rectangle, or NULL for no clipping
 *   fillbg - Lay down the cell background before the glyph
 *
 ****************************************************************************/

void nxterm_paintcell(FAR struct nxterm_state_s *priv, int row, int col,
                      FAR const struct nxgl_rect_s *clip, bool fillbg)
{
  FAR const struct nxfonts_glyph_s *glyph;
  FAR struct nxterm_cell_s *cell;
  struct nxgl_rect_s bounds;
  struct nxgl_rect_s rect;
  bool reverse;
  uint8_t attr;
  int ret;

  DEBUGASSERT(row >= 0 && row < (int)priv->rows);
  DEBUGASSERT(col >= 0 && col < (int)priv->cols);

  cell    = nxterm_cellat(priv, row, col);
  attr    = nxterm_effattr(priv, row, col);
  reverse = (attr & NXTERM_ATTR_REVERSE) != 0;

  nxterm_cellbounds(priv, row, col, col, &bounds);

  if (clip != NULL)
    {
      nxgl_rectintersect(&rect, clip, &bounds);
      if (nxgl_nullrect(&rect))
        {
          return;
        }
    }
  else
    {
      nxgl_rectcopy(&rect, &bounds);
    }

  /* Lay down the background where one is needed */

  if (reverse)
    {
      ret = priv->ops->fill(priv, &rect, priv->wndo.fcolor);
      if (ret < 0)
        {
          gerr("ERROR: fill failed: %d\n", get_errno());
        }
    }
  else if (fillbg)
    {
      ret = priv->ops->fill(priv, &rect, priv->wndo.wcolor);
      if (ret < 0)
        {
          gerr("ERROR: fill failed: %d\n", get_errno());
        }
    }

  /* A blank cell is the background and nothing else */

  if (cell->code == ' ')
    {
      return;
    }

  /* Find (or render) the glyph.  The colours are baked into it, so reverse
   * video comes from the second cache rather than from anything done here.
   */

  glyph = nxf_cache_getglyph(reverse ? priv->rcache : priv->fcache,
                             cell->code);
  if (glyph == NULL)
    {
      /* There is no bitmap for this code.  It has already been drawn as a
       * blank cell, which is what a missing glyph should look like.
       */

      return;
    }

  /* The glyph may be narrower or shorter than the cell.  Clip to what it
   * actually covers so the rest of the cell keeps the background just
   * drawn.
   */

  bounds.pt2.x = bounds.pt1.x + glyph->width - 1;
  bounds.pt2.y = bounds.pt1.y + glyph->height - 1;

  if (clip != NULL)
    {
      nxgl_rectintersect(&rect, clip, &bounds);
    }
  else
    {
      nxgl_rectcopy(&rect, &bounds);
    }

  if (!nxgl_nullrect(&rect))
    {
      FAR const void *src = (FAR const void *)glyph->bitmap;

      ret = priv->ops->bitmap(priv, &rect, &src, &bounds.pt1,
                              (unsigned int)glyph->stride);
      if (ret < 0)
        {
          gerr("ERROR: bitmap failed: %d\n", get_errno());
        }
    }
}

/****************************************************************************
 * Name: nxterm_paintarea
 *
 * Description:
 *   Draw a rectangular range of cells, inclusive of both corners.
 *
 *   The background of the whole range is laid down in one operation and the
 *   glyphs are then drawn over it, which costs one fill however many cells
 *   are involved.
 *
 * Input Parameters:
 *   priv - Driver state
 *   row1, col1 - The first cell of the range
 *   row2, col2 - The last cell of the range
 *   clip - Restrict drawing to this rectangle, or NULL for no clipping
 *
 ****************************************************************************/

void nxterm_paintarea(FAR struct nxterm_state_s *priv,
                      int row1, int col1, int row2, int col2,
                      FAR const struct nxgl_rect_s *clip)
{
  struct nxgl_rect_s bounds;
  struct nxgl_rect_s rect;
  int row;
  int col;
  int ret;

  if (row1 < 0)
    {
      row1 = 0;
    }

  if (col1 < 0)
    {
      col1 = 0;
    }

  if (row2 >= (int)priv->rows)
    {
      row2 = (int)priv->rows - 1;
    }

  if (col2 >= (int)priv->cols)
    {
      col2 = (int)priv->cols - 1;
    }

  if (row1 > row2 || col1 > col2)
    {
      return;
    }

  /* The bounding box of the whole range */

  nxterm_cellbounds(priv, row1, col1, col2, &bounds);
  bounds.pt2.y = (nxgl_coord_t)((row2 + 1) * priv->lineheight - 1);

  if (clip != NULL)
    {
      nxgl_rectintersect(&rect, clip, &bounds);
      if (nxgl_nullrect(&rect))
        {
          return;
        }
    }
  else
    {
      nxgl_rectcopy(&rect, &bounds);
    }

  ret = priv->ops->fill(priv, &rect, priv->wndo.wcolor);
  if (ret < 0)
    {
      gerr("ERROR: fill failed: %d\n", get_errno());
    }

  /* Then the glyphs, on a background that has already been laid down */

  for (row = row1; row <= row2; row++)
    {
      for (col = col1; col <= col2; col++)
        {
          nxterm_paintcell(priv, row, col, &rect, false);
        }
    }
}

/****************************************************************************
 * Name: nxterm_blank
 *
 * Description:
 *   Return a run of cells on one row to blank, in the grid only.  The
 *   caller is responsible for painting what it has changed.
 *
 ****************************************************************************/

void nxterm_blank(FAR struct nxterm_state_s *priv, int row,
                  int col1, int col2)
{
  FAR struct nxterm_cell_s *cell;
  int col;

  if (row < 0 || row >= (int)priv->rows)
    {
      return;
    }

  if (col1 < 0)
    {
      col1 = 0;
    }

  if (col2 >= (int)priv->cols)
    {
      col2 = (int)priv->cols - 1;
    }

  for (col = col1; col <= col2; col++)
    {
      cell       = nxterm_cellat(priv, row, col);
      cell->code = ' ';
      cell->attr = 0;
    }
}

/****************************************************************************
 * Name: nxterm_home
 *
 * Description:
 *   Set the cursor to the top-left corner of the display.
 *
 ****************************************************************************/

void nxterm_home(FAR struct nxterm_state_s *priv)
{
  nxterm_gotoxy(priv, 0, 0);
}

/****************************************************************************
 * Name: nxterm_gotoxy
 *
 * Description:
 *   Move the cursor to an absolute position, clamped to the display.  This
 *   is the operation the whole grid exists for.
 *
 ****************************************************************************/

void nxterm_gotoxy(FAR struct nxterm_state_s *priv, int row, int col)
{
  if (row < 0)
    {
      row = 0;
    }
  else if (row >= (int)priv->rows)
    {
      row = (int)priv->rows - 1;
    }

  if (col < 0)
    {
      col = 0;
    }
  else if (col >= (int)priv->cols)
    {
      col = (int)priv->cols - 1;
    }

  priv->crow = (uint16_t)row;
  priv->ccol = (uint16_t)col;

  /* Any deliberate cursor movement cancels a wrap that had not happened
   * yet.
   */

  priv->wrap = false;
}
