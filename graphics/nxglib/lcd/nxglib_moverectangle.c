/****************************************************************************
 * graphics/nxglib/lcd/nxglib_moverectangle.c
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
#include <debug.h>

#include <nuttx/lcd/lcd.h>
#include <nuttx/nx/nxglib.h>

#include "nxglib_bitblit.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxgl_hwscroll
 *
 * Description:
 *   Try to satisfy a move with the controller's own vertical scrolling.
 *
 *   Scrolling in hardware moves the whole display rather than a region of
 *   it, so it can only stand in for a move that scrolls the whole display:
 *   full width, purely vertical, and with the source and the place it lands
 *   between them covering every row.  Anything narrower or partial is
 *   declined and moved pixel by pixel instead.
 *
 *   The rows the scroll uncovers are left holding undefined display memory.
 *   That is the same contract the pixel move has, which leaves the vacated
 *   rows holding the old content, so a caller that repaints them after a
 *   move needs no changes to work with this.
 *
 * Input Parameters:
 *   pinfo  - The plane to move within
 *   rect   - The region to move
 *   offset - Where the upper left corner of that region is to end up
 *
 * Returned Value:
 *   True if the display was scrolled and there is nothing left to do.
 *
 ****************************************************************************/

static bool nxgl_hwscroll(FAR struct lcd_planeinfo_s *pinfo,
                          FAR const struct nxgl_rect_s *rect,
                          FAR struct nxgl_point_s *offset)
{
  FAR struct lcd_dev_s *dev = pinfo->dev;
  struct fb_videoinfo_s vinfo;
  nxgl_coord_t height;
  nxgl_coord_t lines;

  if (dev == NULL || dev->vscroll == NULL || dev->getvideoinfo == NULL)
    {
      return false;
    }

  if (dev->getvideoinfo(dev, &vinfo) < 0)
    {
      return false;
    }

  /* The move has to be straight up or down, and span the full width */

  lines = rect->pt1.y - offset->y;

  if (lines == 0 || offset->x != rect->pt1.x ||
      rect->pt1.x != 0 || rect->pt2.x != vinfo.xres - 1)
    {
      return false;
    }

  height = rect->pt2.y - rect->pt1.y + 1;

  if (lines > 0)
    {
      /* Scrolling up: the source is the bottom of the display, and it lands
       * against the top.
       */

      if (offset->y != 0 || rect->pt2.y != vinfo.yres - 1)
        {
          return false;
        }
    }
  else
    {
      /* Scrolling down: the source is the top of the display, and it lands
       * against the bottom.
       */

      if (rect->pt1.y != 0 || offset->y + height != vinfo.yres)
        {
          return false;
        }
    }

  return dev->vscroll(dev, lines) >= 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxgl_moverectangle_*bpp
 *
 * Description:
 *   Move a rectangular region from location to another in the
 *   LCD memory.  The source is expressed as a rectangle; the
 *   destination position is expressed as a point corresponding to the
 *   translation of the upper, left-hand corner.
 *
 ****************************************************************************/

void NXGL_FUNCNAME(nxgl_moverectangle, NXGLIB_SUFFIX)
(
  FAR struct lcd_planeinfo_s *pinfo,
  FAR const struct nxgl_rect_s *rect,
  FAR struct nxgl_point_s *offset)
{
  unsigned int ncols;
  unsigned int srcrow;
  unsigned int destrow;

  /* If this is a scroll of the whole display, the controller may be able to
   * do it itself for the cost of a register write.
   */

  if (nxgl_hwscroll(pinfo, rect, offset))
    {
      return;
    }

  /* Otherwise the pixels have to be read back and written out again, which a
   * write-only display cannot do.  Refusing here leaves the display stale,
   * but the alternative is calling through a NULL getrun.
   */

  if (pinfo->getrun == NULL)
    {
      gerr("ERROR: Display cannot move: no getrun and no hardware scroll\n");
      return;
    }

  /* Get the width of the rectangle to move in pixels. */

  ncols = rect->pt2.x - rect->pt1.x + 1;

  /* Case 1:  The destination position (offset) is above the displayed
   * position (rect)
   */

  if (offset->y < rect->pt1.y)
    {
      /* Copy the rectangle from top down */

      for (srcrow = rect->pt1.y, destrow = offset->y;
           srcrow <= rect->pt2.y;
           srcrow++, destrow++)
        {
          pinfo->getrun(pinfo->dev, srcrow, rect->pt1.x, pinfo->buffer,
                        ncols);
          pinfo->putrun(pinfo->dev, destrow, offset->x, pinfo->buffer,
                        ncols);
        }
    }

  /* Case 2: The destination position (offset) is below the displayed
   * position (rect)
   */

  else
    {
      unsigned int dy = rect->pt2.y - rect->pt1.y;

      /* Copy the rectangle from the bottom up */

      for (srcrow = rect->pt2.y, destrow = offset->y + dy;
           srcrow >= rect->pt1.y;
           srcrow--, destrow--)
        {
          pinfo->getrun(pinfo->dev, srcrow, rect->pt1.x, pinfo->buffer,
                        ncols);
          pinfo->putrun(pinfo->dev, destrow, offset->x, pinfo->buffer,
                        ncols);
        }
    }
}
