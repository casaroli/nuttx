/****************************************************************************
 * graphics/nxterm/nxterm_scroll.c
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

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <nuttx/debug.h>

#include <nuttx/nx/nx.h>
#include <nuttx/nx/nxfonts.h>

#include "nxterm.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_movedisplay
 *
 * Description:
 *   Shift the whole display vertically by asking NX to move it.
 *
 *   THIS IS THE ONLY CALLER OF priv->ops->move, AND IT MUST STAY THAT WAY.
 *
 *   A move is claimed by the LCD hardware-scroll path only when it is full
 *   width, purely vertical, and the source together with where it lands
 *   covers every row of the display.  A move that does not meet that
 *   description is declined, and on a display that cannot be read back
 *   there is then nothing left to try: nxglib_moverectangle() logs an error
 *   and returns having drawn nothing.  The grid would go on believing the
 *   move had happened, and the display would disagree with it for good.
 *
 *   Nothing else in this driver may call move for that reason.  Every other
 *   rearrangement of the grid -- inserting a line, deleting a line,
 *   scrolling a region smaller than the display -- repaints the cells it
 *   changed instead.  Repainting is always correct, where a move is correct
 *   only in this one shape.
 *
 *   The rectangle is constructed here rather than passed in, so that the
 *   one shape that is safe is the only one that can be requested.
 *
 * Input Parameters:
 *   priv   - Driver state
 *   npixels - Rows of pixels to move by; positive moves the contents up.
 *
 ****************************************************************************/

#ifdef NXTERM_HAVE_MOVE
static void nxterm_movedisplay(FAR struct nxterm_state_s *priv, int npixels)
{
  struct nxgl_rect_s rect;
  struct nxgl_point_s offset;
  int ret;

  DEBUGASSERT(npixels != 0);

  rect.pt1.x = 0;
  rect.pt2.x = priv->wndo.wsize.w - 1;

  if (npixels > 0)
    {
      /* Moving up:  everything below the vacated band lands against the top
       * of the display.
       */

      rect.pt1.y = (nxgl_coord_t)npixels;
      rect.pt2.y = priv->wndo.wsize.h - 1;
    }
  else
    {
      /* Moving down:  everything above the vacated band lands against the
       * bottom of the display.
       */

      rect.pt1.y = 0;
      rect.pt2.y = priv->wndo.wsize.h - 1 + (nxgl_coord_t)npixels;
    }

  /* The offset nx_move() takes is how far to move the rectangle, not where
   * to put it.  nxbe_clipmovesrc() adds it to the source corner to get the
   * destination, and -- worth knowing -- skips the move altogether when it
   * is zero, so a destination expressed here as an absolute position would
   * be silently dropped rather than merely drawn in the wrong place.
   */

  offset.x = 0;
  offset.y = (nxgl_coord_t)-npixels;

  ret = priv->ops->move(priv, &rect, &offset);
  if (ret < 0)
    {
      gerr("ERROR: Move failed: %d\n", get_errno());
    }
}
#endif

/****************************************************************************
 * Name: nxterm_repaintrows
 *
 * Description:
 *   Draw a band of whole rows again from the grid.
 *
 *   'tobottom' extends the band to the foot of the window rather than
 *   stopping at the end of the last row.  A window whose height is not a
 *   whole number of rows has a strip below the grid that no cell describes,
 *   and a move shifts that strip along with everything else -- so whenever
 *   a band reaches the last row it has to be cleared down to the glass, or
 *   the strip keeps whatever the move dragged into it.
 *
 ****************************************************************************/

static void nxterm_repaintrows(FAR struct nxterm_state_s *priv,
                               int row1, int row2, bool tobottom)
{
  struct nxgl_rect_s band;
  int row;
  int col;
  int ret;

  if (row1 < 0)
    {
      row1 = 0;
    }

  if (row2 >= (int)priv->rows)
    {
      row2 = (int)priv->rows - 1;
    }

  if (row1 > row2)
    {
      return;
    }

  band.pt1.x = 0;
  band.pt2.x = priv->wndo.wsize.w - 1;
  band.pt1.y = (nxgl_coord_t)(row1 * priv->lineheight);
  band.pt2.y = tobottom ?
    priv->wndo.wsize.h - 1 :
    (nxgl_coord_t)((row2 + 1) * priv->lineheight - 1);

  ret = priv->ops->fill(priv, &band, priv->wndo.wcolor);
  if (ret < 0)
    {
      gerr("ERROR: Fill failed: %d\n", get_errno());
    }

  for (row = row1; row <= row2; row++)
    {
      for (col = 0; col < (int)priv->cols; col++)
        {
          nxterm_paintcell(priv, row, col, &band, false);
        }
    }
}

/****************************************************************************
 * Name: nxterm_scrollregion
 *
 * Description:
 *   Scroll the scrolling region by whole lines and repaint what changed.
 *
 *   The display is moved rather than redrawn only when the scrolling region
 *   is the whole display, which is the one case a move can express.  That
 *   is also the common case -- it is what an ordinary line of output does --
 *   so the acceleration is kept for the traffic that matters.
 *
 * Input Parameters:
 *   priv   - Driver state
 *   nlines - Lines to scroll by; positive scrolls the contents up.
 *
 ****************************************************************************/

static void nxterm_scrollregion(FAR struct nxterm_state_s *priv, int nlines)
{
  unsigned int nregion;
  unsigned int nmoved;
  bool wholedisplay;
  int row;
  int up;

  if (nlines == 0)
    {
      return;
    }

  up      = nlines > 0 ? nlines : -nlines;
  nregion = (unsigned int)(priv->sbrow - priv->strow + 1);

  /* Is the region the whole display?  That is the one shape a move can
   * express, and it is also the common case -- an ordinary line of output
   * scrolls exactly this way -- so the acceleration is kept for the traffic
   * that matters.
   */

  wholedisplay = priv->strow == 0 && priv->sbrow == priv->rows - 1;

  /* Scrolling by at least the height of the region leaves nothing of it */

  if ((unsigned int)up >= nregion)
    {
      for (row = (int)priv->strow; row <= (int)priv->sbrow; row++)
        {
          nxterm_blank(priv, row, 0, (int)priv->cols - 1);
        }

      nxterm_repaintrows(priv, (int)priv->strow, (int)priv->sbrow,
                         wholedisplay);
      return;
    }

  nmoved = (nregion - (unsigned int)up) * priv->cols;

  if (nlines > 0)
    {
      /* Up:  the region's later rows move over its earlier ones */

      memmove(nxterm_cellat(priv, (int)priv->strow, 0),
              nxterm_cellat(priv, (int)priv->strow + up, 0),
              nmoved * sizeof(struct nxterm_cell_s));

      for (row = (int)priv->sbrow - up + 1; row <= (int)priv->sbrow; row++)
        {
          nxterm_blank(priv, row, 0, (int)priv->cols - 1);
        }
    }
  else
    {
      /* Down:  the region's earlier rows move over its later ones.  memmove
       * handles the overlap in either direction.
       */

      memmove(nxterm_cellat(priv, (int)priv->strow + up, 0),
              nxterm_cellat(priv, (int)priv->strow, 0),
              nmoved * sizeof(struct nxterm_cell_s));

      for (row = (int)priv->strow; row < (int)priv->strow + up; row++)
        {
          nxterm_blank(priv, row, 0, (int)priv->cols - 1);
        }
    }

#ifdef NXTERM_HAVE_MOVE
  if (wholedisplay)
    {
      nxterm_movedisplay(priv, nlines * (int)priv->lineheight);

      /* Then draw again the rows the move did not leave correct.
       *
       * That is the rows just blanked, and everything below them:  where
       * the window is not a whole number of rows tall, the band the move
       * vacated begins lower than those rows do, so the space between
       * holds content the move shifted there and the grid no longer
       * claims.
       */

      if (nlines > 0)
        {
          nxterm_repaintrows(priv, (int)priv->rows - up,
                             (int)priv->rows - 1, true);
        }
      else
        {
          nxterm_repaintrows(priv, 0, up - 1, false);
        }

      return;
    }
#endif

  /* Otherwise repaint the region.  This is the slow path and the safe one:
   * see nxterm_movedisplay() for why a partial move is not an alternative.
   */

  nxterm_repaintrows(priv, (int)priv->strow, (int)priv->sbrow,
                     wholedisplay);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_scroll
 *
 * Description:
 *   Scroll the scrolling region up by nlines, blanking the lines it
 *   uncovers at the bottom.
 *
 ****************************************************************************/

void nxterm_scroll(FAR struct nxterm_state_s *priv, int nlines)
{
  if (nlines > 0)
    {
      nxterm_scrollregion(priv, nlines);
    }
}

/****************************************************************************
 * Name: nxterm_scrolldown
 *
 * Description:
 *   Scroll the scrolling region down by nlines, blanking the lines it
 *   uncovers at the top.
 *
 ****************************************************************************/

void nxterm_scrolldown(FAR struct nxterm_state_s *priv, int nlines)
{
  if (nlines > 0)
    {
      nxterm_scrollregion(priv, -nlines);
    }
}

/****************************************************************************
 * Name: nxterm_index
 *
 * Description:
 *   Move the cursor down one line, scrolling the region when it is already
 *   on the last line of it.  This is what a line feed does.
 *
 ****************************************************************************/

void nxterm_index(FAR struct nxterm_state_s *priv)
{
  if (priv->crow == priv->sbrow)
    {
      nxterm_scroll(priv, 1);
    }
  else if (priv->crow + 1 < priv->rows)
    {
      priv->crow++;
    }
}

/****************************************************************************
 * Name: nxterm_revindex
 *
 * Description:
 *   Move the cursor up one line, scrolling the region down when it is
 *   already on the first line of it.
 *
 ****************************************************************************/

void nxterm_revindex(FAR struct nxterm_state_s *priv)
{
  if (priv->crow == priv->strow)
    {
      nxterm_scrolldown(priv, 1);
    }
  else if (priv->crow > 0)
    {
      priv->crow--;
    }
}

/****************************************************************************
 * Name: nxterm_insertlines
 *
 * Description:
 *   Open nlines blank lines at the cursor, pushing the rest of the
 *   scrolling region down.  Lines pushed past the end of the region are
 *   lost.
 *
 *   This is a scroll of the part of the region at and below the cursor, and
 *   it is done by rearranging the grid and repainting.  It is never a move:
 *   the region it affects is not the whole display, so a move would be
 *   declined and would silently draw nothing.
 *
 ****************************************************************************/

void nxterm_insertlines(FAR struct nxterm_state_s *priv, int nlines)
{
  int first = (int)priv->crow;
  int last  = (int)priv->sbrow;
  int row;

  /* The cursor outside the scrolling region makes this a no-op */

  if (priv->crow < priv->strow || priv->crow > priv->sbrow || nlines <= 0)
    {
      return;
    }

  if (nlines > last - first + 1)
    {
      nlines = last - first + 1;
    }

  for (row = last; row >= first + nlines; row--)
    {
      memcpy(nxterm_cellat(priv, row, 0),
             nxterm_cellat(priv, row - nlines, 0),
             priv->cols * sizeof(struct nxterm_cell_s));
    }

  for (row = first; row < first + nlines; row++)
    {
      nxterm_blank(priv, row, 0, (int)priv->cols - 1);
    }

  nxterm_repaintrows(priv, first, last, last == (int)priv->rows - 1);
}

/****************************************************************************
 * Name: nxterm_deletelines
 *
 * Description:
 *   Remove nlines at the cursor, pulling the rest of the scrolling region
 *   up and blanking the lines uncovered at the end of the region.
 *
 *   Repainted rather than moved, for the reason given in
 *   nxterm_insertlines().
 *
 ****************************************************************************/

void nxterm_deletelines(FAR struct nxterm_state_s *priv, int nlines)
{
  int first = (int)priv->crow;
  int last  = (int)priv->sbrow;
  int row;

  if (priv->crow < priv->strow || priv->crow > priv->sbrow || nlines <= 0)
    {
      return;
    }

  if (nlines > last - first + 1)
    {
      nlines = last - first + 1;
    }

  for (row = first; row <= last - nlines; row++)
    {
      memcpy(nxterm_cellat(priv, row, 0),
             nxterm_cellat(priv, row + nlines, 0),
             priv->cols * sizeof(struct nxterm_cell_s));
    }

  for (row = last - nlines + 1; row <= last; row++)
    {
      nxterm_blank(priv, row, 0, (int)priv->cols - 1);
    }

  nxterm_repaintrows(priv, first, last, last == (int)priv->rows - 1);
}
