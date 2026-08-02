/****************************************************************************
 * graphics/nxterm/nxterm_putc.c
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

#include <nuttx/ascii.h>

#include "nxterm.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_pendingwrap
 *
 * Description:
 *   Act on a wrap that was deferred when the last column was written.
 *
 *   A terminal that wraps the moment the last column is filled cannot write
 *   the bottom right cell of the screen without scrolling, which is exactly
 *   what a full-screen editor does when it draws its last line.  So the
 *   wrap is remembered instead and taken only if another character follows.
 *
 * Returned Value:
 *   True if the character that triggered this may now be written.
 *
 ****************************************************************************/

static bool nxterm_pendingwrap(FAR struct nxterm_state_s *priv)
{
  if (!priv->wrap)
    {
      return true;
    }

  priv->wrap = false;

#ifdef CONFIG_NXTERM_NOWRAP
  /* Wrapping is disabled:  drop everything until the line ends */

  return false;
#else
  priv->ccol = 0;
  nxterm_index(priv);
  return true;
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_putc
 *
 * Description:
 *   Render the specified character at the cursor and advance the cursor.
 *
 ****************************************************************************/

void nxterm_putc(FAR struct nxterm_state_s *priv, uint8_t ch)
{
  FAR struct nxterm_cell_s *cell;
  bool wasblank;

  switch (ch)
    {
      case '\r':
        {
          /* Carriage return moves back to the left margin.
           *
           * This used to be ignored, which is defensible for output that
           * only ever appends but breaks any line editor: readline redraws
           * a line with "\r", erase-to-end-of-line, then the whole line
           * again, and with the return dropped that redraw was appended
           * instead of replacing.
           */

          priv->ccol = 0;
          priv->wrap = false;
          return;
        }

      case '\n':
        {
          /* A line feed here is a new line:  there is no output processing
           * layer between this driver and its writers to turn one into the
           * other, so a console that only emitted the line feed would draw
           * a staircase.
           */

          priv->ccol = 0;
          priv->wrap = false;
          nxterm_index(priv);
          return;
        }

      case ASCII_BS:
      case ASCII_DEL:
        {
          /* Backspace moves left without erasing, which is what a VT100
           * does.  Callers that mean to erase send the escape sequence for
           * it -- readline emits backspace followed by erase-to-end-of-line
           * -- and a destructive backspace here would take one character
           * too many.
           */

          priv->wrap = false;
          if (priv->ccol > 0)
            {
              priv->ccol--;
            }

          return;
        }

      case '\t':
        {
          priv->wrap = false;
          priv->ccol = (priv->ccol / NXTERM_TABSIZE + 1) * NXTERM_TABSIZE;
          if (priv->ccol >= priv->cols)
            {
              priv->ccol = priv->cols - 1;
            }

          return;
        }

      default:
        break;
    }

  /* Every other control character is not something this terminal does.
   * Drawing them as glyphs is what produced the "no bitmap for code 1b"
   * complaints and the stray characters that went with them.
   */

  if (ch < ASCII_SPACE)
    {
      return;
    }

  if (!nxterm_pendingwrap(priv))
    {
      return;
    }

  /* Put the character in the cell it belongs to */

  cell     = nxterm_cellat(priv, priv->crow, priv->ccol);
  wasblank = cell->code == ' ' && cell->attr == 0;

  cell->code = ch;
  cell->attr = priv->attr;

  /* A cell that was blank already stands on the window background, so the
   * glyph can go straight down.  That keeps the ordinary case -- typing
   * onto a clear line -- at one drawing operation per character.
   */

  nxterm_paintcell(priv, (int)priv->crow, (int)priv->ccol, NULL, !wasblank);

  /* Advance, deferring the wrap until there is another character */

  if (priv->ccol + 1 >= priv->cols)
    {
      priv->wrap = true;
    }
  else
    {
      priv->ccol++;
    }
}

/****************************************************************************
 * Name: nxterm_showcursor
 *
 * Description:
 *   Draw the cursor at its current position.
 *
 *   The cursor is a block that the text shows through rather than one that
 *   hides it: the cell is simply drawn in reverse video, so a cursor parked
 *   on a character still shows the character.  A cursor that drew a glyph
 *   over the cell would look exactly as though the character had been
 *   deleted.
 *
 ****************************************************************************/

void nxterm_showcursor(FAR struct nxterm_state_s *priv)
{
  /* Already drawn somewhere else?  Take it away from there first, or the
   * old block would be left behind.
   */

  if (priv->cshown &&
      (priv->cdrawrow != priv->crow || priv->cdrawcol != priv->ccol))
    {
      nxterm_hidecursor(priv);
    }

  if (!priv->cvisible || priv->cshown)
    {
      return;
    }

  /* A deferred wrap leaves the cursor on the last column it wrote.  Show it
   * there:  moving it to the next line would be acting on a wrap that has
   * not happened and may never happen.
   */

  priv->cdrawrow = priv->crow;
  priv->cdrawcol = priv->ccol;
  priv->cshown   = true;

  nxterm_paintcell(priv, (int)priv->cdrawrow, (int)priv->cdrawcol,
                   NULL, true);
}

/****************************************************************************
 * Name: nxterm_hidecursor
 *
 * Description:
 *   Remove the cursor from the display, putting back whatever it was
 *   sitting on.
 *
 ****************************************************************************/

void nxterm_hidecursor(FAR struct nxterm_state_s *priv)
{
  int row;
  int col;

  if (!priv->cshown)
    {
      return;
    }

  row = (int)priv->cdrawrow;
  col = (int)priv->cdrawcol;

  /* Clear the flag first:  the cell is painted with the attributes it
   * really has, which is what nxterm_effattr() reports once the cursor is
   * no longer claimed to be here.
   */

  priv->cshown = false;

  if (row < (int)priv->rows && col < (int)priv->cols)
    {
      nxterm_paintcell(priv, row, col, NULL, true);
    }
}

/****************************************************************************
 * Name: nxterm_eraseline
 *
 * Description:
 *   Erase part of the cursor's line.  The cursor does not move.
 *
 * Input Parameters:
 *   priv - Driver state
 *   mode - 0: cursor to the end of the line
 *          1: the start of the line to the cursor
 *          2: the whole line
 *
 ****************************************************************************/

void nxterm_eraseline(FAR struct nxterm_state_s *priv, int mode)
{
  int first;
  int last;

  switch (mode)
    {
      case 0:
      default:
        first = (int)priv->ccol;
        last  = (int)priv->cols - 1;
        break;

      case 1:
        first = 0;
        last  = (int)priv->ccol;
        break;

      case 2:
        first = 0;
        last  = (int)priv->cols - 1;
        break;
    }

  nxterm_blank(priv, (int)priv->crow, first, last);
  nxterm_paintarea(priv, (int)priv->crow, first, (int)priv->crow, last,
                   NULL);
}

/****************************************************************************
 * Name: nxterm_erasedisplay
 *
 * Description:
 *   Erase part of the display.  The cursor does not move.
 *
 * Input Parameters:
 *   priv - Driver state
 *   mode - 0: cursor to the end of the display
 *          1: the start of the display to the cursor
 *          2: the whole display (3 is the same, there is no scrollback)
 *
 ****************************************************************************/

void nxterm_erasedisplay(FAR struct nxterm_state_s *priv, int mode)
{
  int row;

  switch (mode)
    {
      case 0:
      default:
        {
          nxterm_blank(priv, (int)priv->crow, (int)priv->ccol,
                       (int)priv->cols - 1);
          nxterm_paintarea(priv, (int)priv->crow, (int)priv->ccol,
                           (int)priv->crow, (int)priv->cols - 1, NULL);

          for (row = (int)priv->crow + 1; row < (int)priv->rows; row++)
            {
              nxterm_blank(priv, row, 0, (int)priv->cols - 1);
            }

          if (priv->crow + 1 < priv->rows)
            {
              nxterm_paintarea(priv, (int)priv->crow + 1, 0,
                               (int)priv->rows - 1, (int)priv->cols - 1,
                               NULL);
            }
        }
        break;

      case 1:
        {
          for (row = 0; row < (int)priv->crow; row++)
            {
              nxterm_blank(priv, row, 0, (int)priv->cols - 1);
            }

          nxterm_blank(priv, (int)priv->crow, 0, (int)priv->ccol);

          if (priv->crow > 0)
            {
              nxterm_paintarea(priv, 0, 0, (int)priv->crow - 1,
                               (int)priv->cols - 1, NULL);
            }

          nxterm_paintarea(priv, (int)priv->crow, 0, (int)priv->crow,
                           (int)priv->ccol, NULL);
        }
        break;

      case 2:
      case 3:
        {
          for (row = 0; row < (int)priv->rows; row++)
            {
              nxterm_blank(priv, row, 0, (int)priv->cols - 1);
            }

          nxterm_paintarea(priv, 0, 0, (int)priv->rows - 1,
                           (int)priv->cols - 1, NULL);
        }
        break;
    }
}

/****************************************************************************
 * Name: nxterm_erasechars
 *
 * Description:
 *   Blank nchars cells from the cursor, without moving anything.
 *
 ****************************************************************************/

void nxterm_erasechars(FAR struct nxterm_state_s *priv, int nchars)
{
  int last;

  if (nchars <= 0)
    {
      return;
    }

  last = (int)priv->ccol + nchars - 1;
  if (last >= (int)priv->cols)
    {
      last = (int)priv->cols - 1;
    }

  nxterm_blank(priv, (int)priv->crow, (int)priv->ccol, last);
  nxterm_paintarea(priv, (int)priv->crow, (int)priv->ccol,
                   (int)priv->crow, last, NULL);
}

/****************************************************************************
 * Name: nxterm_insertchars
 *
 * Description:
 *   Open nchars blank cells at the cursor, pushing the rest of the line to
 *   the right.  Characters pushed off the end are lost.
 *
 ****************************************************************************/

void nxterm_insertchars(FAR struct nxterm_state_s *priv, int nchars)
{
  int ncols = (int)priv->cols;
  int col;

  if (nchars <= 0)
    {
      return;
    }

  if (nchars > ncols - (int)priv->ccol)
    {
      nchars = ncols - (int)priv->ccol;
    }

  for (col = ncols - 1; col >= (int)priv->ccol + nchars; col--)
    {
      *nxterm_cellat(priv, (int)priv->crow, col) =
        *nxterm_cellat(priv, (int)priv->crow, col - nchars);
    }

  nxterm_blank(priv, (int)priv->crow, (int)priv->ccol,
               (int)priv->ccol + nchars - 1);
  nxterm_paintarea(priv, (int)priv->crow, (int)priv->ccol,
                   (int)priv->crow, ncols - 1, NULL);
}

/****************************************************************************
 * Name: nxterm_deletechars
 *
 * Description:
 *   Remove nchars cells at the cursor, pulling the rest of the line left
 *   and blanking what it uncovers at the end.
 *
 ****************************************************************************/

void nxterm_deletechars(FAR struct nxterm_state_s *priv, int nchars)
{
  int ncols = (int)priv->cols;
  int col;

  if (nchars <= 0)
    {
      return;
    }

  if (nchars > ncols - (int)priv->ccol)
    {
      nchars = ncols - (int)priv->ccol;
    }

  for (col = (int)priv->ccol; col < ncols - nchars; col++)
    {
      *nxterm_cellat(priv, (int)priv->crow, col) =
        *nxterm_cellat(priv, (int)priv->crow, col + nchars);
    }

  nxterm_blank(priv, (int)priv->crow, ncols - nchars, ncols - 1);
  nxterm_paintarea(priv, (int)priv->crow, (int)priv->ccol,
                   (int)priv->crow, ncols - 1, NULL);
}
