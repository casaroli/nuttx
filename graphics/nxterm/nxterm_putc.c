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

#include <nuttx/ascii.h>

#include "nxterm.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_putc
 *
 * Description:
 *   Render the specified character at the current display position.
 *
 ****************************************************************************/

void nxterm_putc(FAR struct nxterm_state_s *priv, uint8_t ch)
{
  FAR const struct nxterm_bitmap_s *bm;
  int lineheight;

  /* Carriage return moves back to the left margin.
   *
   * This used to be ignored, which is defensible for output that only ever
   * appends but breaks any line editor: readline redraws a line with
   * "\r", erase-to-end-of-line, then the whole line again, and with the
   * return dropped that redraw was appended instead of replacing.
   */

  if (ch == '\r')
    {
      priv->fpos.x = priv->spwidth;
      return;
    }

  /* Handle backspace (treating both BS and DEL as backspace) */

  if (ch == ASCII_BS || ch == ASCII_DEL)
    {
      nxterm_backspace(priv);
      return;
    }

  /* Will another character fit on this line? */

  if (priv->fpos.x + priv->fwidth > priv->wndo.wsize.w)
    {
#ifndef CONFIG_NXTERM_NOWRAP
      /* No.. move to the next line */

      nxterm_newline(priv);

      /* If we were about to output a newline character, then don't */

      if (ch == '\n')
        {
          return;
        }
#else
      /* No.. Ignore all further characters until a newline is encountered */

      if (ch != '\n')
        {
          return;
        }
#endif
    }

  /* If it is a newline character, then just perform the logical newline
   * operation.
   */

  if (ch == '\n')
    {
      nxterm_newline(priv);
      return;
    }

  /* Check if we need to scroll up */

  lineheight = (priv->fheight + CONFIG_NXTERM_LINESEPARATION);
  while (priv->fpos.y >= priv->wndo.wsize.h - lineheight)
    {
      nxterm_scroll(priv, lineheight);
    }

  /* Find the glyph associated with the character and render it onto the
   * display.
   */

  bm = nxterm_addchar(priv, ch);
  if (bm)
    {
      nxterm_fillchar(priv, NULL, bm);
    }
}

/****************************************************************************
 * Name: nxterm_showcursor
 *
 * Description:
 *   Render the cursor character at the current display position.
 *
 ****************************************************************************/

void nxterm_showcursor(FAR struct nxterm_state_s *priv)
{
  int lineheight;

  /* Will another character fit on this line? */

  if (priv->fpos.x + priv->fwidth > priv->wndo.wsize.w)
    {
#ifndef CONFIG_NXTERM_NOWRAP
      /* No.. move to the next line */

      nxterm_newline(priv);
#else
      return;
#endif
    }

  /* Check if we need to scroll up */

  lineheight = (priv->fheight + CONFIG_NXTERM_LINESEPARATION);
  while (priv->fpos.y >= priv->wndo.wsize.h - lineheight)
    {
      nxterm_scroll(priv, lineheight);
    }

  priv->cursor.pos.x = priv->fpos.x;
  priv->cursor.pos.y = priv->fpos.y;

  /* Draw the cursor as a block that the text shows through.
   *
   * A cursor that simply renders a glyph over the cell hides whatever is
   * underneath, which is invisible while the cursor can only sit past the
   * last character and very obvious once a line editor can park it on one --
   * the character appears to have been deleted.
   *
   * So fill the cell with the foreground colour and, if a character lives
   * there, draw it back in reverse video on top.  On an empty cell that
   * leaves a plain block, which is what a cursor at the end of a line should
   * look like anyway.
   */

    {
      struct nxgl_rect_s bounds;
      int i;

      bounds.pt1.x = priv->cursor.pos.x;
      bounds.pt1.y = priv->cursor.pos.y;
      bounds.pt2.x = priv->cursor.pos.x + priv->fwidth - 1;
      bounds.pt2.y = priv->cursor.pos.y + priv->fheight - 1;

      priv->ops->fill(priv, &bounds, priv->wndo.fcolor);

      for (i = priv->nchars - 1; i >= 0; i--)
        {
          FAR struct nxterm_bitmap_s *bm = &priv->bm[i];

          if (bm->pos.x == priv->cursor.pos.x &&
              bm->pos.y == priv->cursor.pos.y)
            {
              nxterm_reversechar(priv, bm);
              break;
            }
        }
    }
}

/****************************************************************************
 * Name: nxterm_hidecursor
 *
 * Description:
 *   Render the cursor cursor character from the display.
 *
 ****************************************************************************/

void nxterm_hidecursor(FAR struct nxterm_state_s *priv)
{
  int i;

  nxterm_hidechar(priv, &priv->cursor);

  /* Put back whatever the cursor was covering.
   *
   * Hiding the cursor clears its whole cell, which was harmless while the
   * cursor could only ever sit past the last character.  A line editor moves
   * it back over text, and without this the character underneath is erased
   * for good the next time anything writes -- it looks exactly as though the
   * left arrow deleted it.
   *
   * Searched from the end because the most recently written glyph at a
   * position is the one currently displayed there.
   */

  for (i = priv->nchars - 1; i >= 0; i--)
    {
      FAR struct nxterm_bitmap_s *bm = &priv->bm[i];

      if (bm->pos.x == priv->cursor.pos.x &&
          bm->pos.y == priv->cursor.pos.y)
        {
          nxterm_fillchar(priv, NULL, bm);
          break;
        }
    }
}
