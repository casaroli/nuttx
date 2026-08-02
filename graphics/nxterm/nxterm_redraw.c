/****************************************************************************
 * graphics/nxterm/nxterm_redraw.c
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
#include <assert.h>
#include <errno.h>
#include <nuttx/debug.h>

#include <nuttx/spinlock.h>
#include <nuttx/nx/nx.h>
#include <nuttx/nx/nxglib.h>

#include "nxterm.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_dodraw
 *
 * Description:
 *   Draw a rectangular region again.  The caller must hold priv->lock.
 *
 ****************************************************************************/

static void nxterm_dodraw(FAR struct nxterm_state_s *priv,
                          FAR const struct nxgl_rect_s *rect)
{
  int ret;
  int i;

  /* Fill the rectangular region with the window background color */

  ret = priv->ops->fill(priv, rect, priv->wndo.wcolor);
  if (ret < 0)
    {
      gerr("ERROR: fill failed: %d\n", get_errno());
    }

  /* Then redraw each character on the display (Only the characters within
   * the rectangle will actually be redrawn).
   */

  for (i = 0; i < priv->nchars; i++)
    {
      nxterm_fillchar(priv, rect, &priv->bm[i]);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_flushdamage
 *
 * Description:
 *   Draw again anything a redraw request had to leave for later.  Called
 *   with priv->lock held, by whoever is about to release it.
 *
 ****************************************************************************/

void nxterm_flushdamage(FAR struct nxterm_state_s *priv)
{
  struct nxgl_rect_s rect;
  irqstate_t flags;
  bool pending;

  flags   = spin_lock_irqsave_nopreempt(&priv->spinlock);
  pending = priv->dmgpending;

  if (pending)
    {
      nxgl_rectcopy(&rect, &priv->dmgrect);
      priv->dmgpending = false;
    }

  spin_unlock_irqrestore_nopreempt(&priv->spinlock, flags);

  if (pending)
    {
      nxterm_dodraw(priv, &rect);
    }
}

/****************************************************************************
 * Name: nxterm_redraw
 *
 * Description:
 *   Re-draw a portion of the NX console.  This function should be called
 *   from the appropriate window callback logic.
 *
 *   THIS MUST NOT WAIT FOR priv->lock.
 *
 *   It runs on the client's NX listener thread, which is the only thread
 *   that empties the server's messages to this client.  A writer holds
 *   priv->lock for the whole of a write and blocks inside it whenever the
 *   server's queue is full.  If the listener then waits for that lock, all
 *   three parties are stuck:  the writer waits for the server to consume a
 *   drawing message, the server waits to hand this client a message, and
 *   the listener that would take it waits for the writer.  The terminal
 *   stops dead and only a reset clears it.
 *
 *   So the lock is taken only if it is free.  When it is not, the damaged
 *   region is remembered and drawn by whoever is holding the lock, on its
 *   way out.  Nothing is lost and the listener keeps draining.
 *
 * Input Parameters:
 *   handle - A handle previously returned by nx_register, nxtk_register, or
 *     nxtool_register.
 *   rect - The rectangle that needs to be re-drawn (in window relative
 *          coordinates)
 *   more - true:  More re-draw requests will follow
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void nxterm_redraw(NXTERM handle,
                   FAR const struct nxgl_rect_s *rect,
                   bool more)
{
  FAR struct nxterm_state_s *priv;
  irqstate_t flags;

  DEBUGASSERT(handle && rect);
  ginfo("rect={(%d,%d),(%d,%d)} more=%s\n",
        rect->pt1.x, rect->pt1.y, rect->pt2.x, rect->pt2.y,
        more ? "true" : "false");

  /* Recover our private state structure */

  priv = (FAR struct nxterm_state_s *)handle;

  if (nxmutex_trylock(&priv->lock) < 0)
    {
      /* Busy.  Leave the region for the holder to draw and get back to
       * emptying the server's queue.
       */

      flags = spin_lock_irqsave_nopreempt(&priv->spinlock);

      if (priv->dmgpending)
        {
          nxgl_rectunion(&priv->dmgrect, &priv->dmgrect, rect);
        }
      else
        {
          nxgl_rectcopy(&priv->dmgrect, rect);
          priv->dmgpending = true;
        }

      spin_unlock_irqrestore_nopreempt(&priv->spinlock, flags);
      return;
    }

  nxterm_dodraw(priv, rect);

  /* Anything left over from an earlier request that could not be served */

  nxterm_flushdamage(priv);
  nxmutex_unlock(&priv->lock);
}
