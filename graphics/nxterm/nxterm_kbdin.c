/****************************************************************************
 * graphics/nxterm/nxterm_kbdin.c
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

#include <inttypes.h>
#include <ctype.h>
#include <fcntl.h>
#include <sched.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <nuttx/ascii.h>
#include <nuttx/debug.h>

#include <nuttx/signal.h>
#include <nuttx/spinlock.h>

#include "nxterm.h"

#ifdef CONFIG_NXTERM_NXKBDIN

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_pollnotify
 ****************************************************************************/

static void nxterm_pollnotify(FAR struct nxterm_state_s *priv,
                              pollevent_t eventset)
{
  irqstate_t flags;

  /* This function may be called from an interrupt handler */

  flags = spin_lock_irqsave_nopreempt(&priv->spinlock);
  poll_notify(priv->fds, CONFIG_NXTERM_NPOLLWAITERS, eventset);
  spin_unlock_irqrestore_nopreempt(&priv->spinlock, flags);
}

/****************************************************************************
 * Name: nxterm_echo
 *
 * Description:
 *   Render one character of keyboard input back to the window, the way a
 *   serial driver echoes what it receives.  Without this the terminal
 *   shows nothing at all until the line is submitted:  readline() draws
 *   only the *corrections* it makes to a line (erase-to-end-of-line after
 *   an insertion, a full redraw after an edit) and relies on the driver
 *   to have echoed the character itself, which is why it looks fine on a
 *   serial console and dead here.
 *
 *   Escape sequences typed by the user are swallowed rather than drawn:
 *   they are commands to the line editor, not text, and rendering them
 *   literally would leave "[A" style droppings on the screen.
 *
 *   The caller must hold priv->lock, and must have hidden the cursor.
 *
 ****************************************************************************/

static void nxterm_echo(FAR struct nxterm_state_s *priv, char ch)
{
  if (ch == ASCII_ESC)
    {
      /* Start (or restart) tracking a possible escape sequence */

      priv->escape = NXTERM_ESCAPE_START;
      return;
    }
  else if (priv->escape == NXTERM_ESCAPE_START)
    {
      /* The first byte after ESC selects the sequence type */

      if (ch == ASCII_LBRACKET)
        {
          priv->escape = NXTERM_ESCAPE_CSI;
          return;
        }
      else if (ch == ASCII_O)
        {
          priv->escape = NXTERM_ESCAPE_SS3;
          return;
        }

      /* Neither CSI nor SS3 -- an unrecognized two-byte "ESC x" sequence.
       * Fall through and echo 'x' normally.
       */

      priv->escape = NXTERM_ESCAPE_NONE;
    }
  else if (priv->escape == NXTERM_ESCAPE_CSI)
    {
      /* Consuming CSI parameter/intermediate bytes (0x20-0x3f); the
       * sequence ends with exactly one final byte in 0x40-0x7e.
       */

      if (ch >= 0x40 && ch <= 0x7e)
        {
          priv->escape = NXTERM_ESCAPE_NONE;
        }

      return;
    }
  else if (priv->escape == NXTERM_ESCAPE_SS3)
    {
      /* The byte following "ESC O" is always the final (and only) byte */

      priv->escape = NXTERM_ESCAPE_NONE;
      return;
    }

  /* Echo printable characters, plus the newline that ends a line.  Every
   * other control character belongs to the line editor:  backspace, for
   * one, is echoed by readline() itself as BS + erase-to-end-of-line, and
   * echoing it here as well would erase one character too many.
   */

  if (!iscntrl(ch & 0xff) || ch == '\n')
    {
      nxterm_putc(priv, (uint8_t)ch);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_read
 *
 * Description:
 *   The optional NxTerm read method
 *
 ****************************************************************************/

ssize_t nxterm_read(FAR struct file *filep, FAR char *buffer, size_t len)
{
  FAR struct nxterm_state_s *priv;
  irqstate_t flags;
  ssize_t nread;
  char ch;
  int ret;

  /* Recover our private state structure */

  DEBUGASSERT(filep->f_priv);
  priv = (FAR struct nxterm_state_s *)filep->f_priv;

  /* Loop until something is read.
   *
   * The keyboard ring is guarded by the spinlock, not by priv->lock:
   * priv->lock guards the *display*, and rendering a single glyph to a
   * panel is slow.  If delivering a keystroke had to wait for it, then
   * nxterm_kbdin() -- which runs on the client's NX listener thread --
   * would stall for the whole of a redraw, and with it every other event
   * that thread is responsible for.  Typing then appears to freeze until
   * the drawing catches up.  Keeping the two locks apart lets input be
   * queued at any time, whatever the display is doing.
   */

  for (nread = 0; nread < len; )
    {
      flags = spin_lock_irqsave_nopreempt(&priv->spinlock);

      /* Get the next byte from the buffer */

      if (priv->head == priv->tail)
        {
          /* The circular buffer is empty. Did we read anything? */

          if (nread > 0)
            {
              /* Yes.. break out to return what we have.  */

              spin_unlock_irqrestore_nopreempt(&priv->spinlock, flags);
              break;
            }

          /* If the driver was opened with O_NONBLOCK option, then
           * don't wait. Just return EGAIN.
           */

          if (filep->f_oflags & O_NONBLOCK)
            {
              spin_unlock_irqrestore_nopreempt(&priv->spinlock, flags);
              nread = -EAGAIN;
              break;
            }

          /* Otherwise, wait for something to be written to the circular
           * buffer.  The number of waiters is incremented first, while
           * the ring is still locked, so that nxterm_kbdin() cannot
           * decide there is nobody to wake between our test of the ring
           * and our wait on the semaphore.
           */

          priv->nwaiters++;
          spin_unlock_irqrestore_nopreempt(&priv->spinlock, flags);

          ret = nxsem_wait(&priv->waitsem);

          flags = spin_lock_irqsave_nopreempt(&priv->spinlock);
          priv->nwaiters--;
          spin_unlock_irqrestore_nopreempt(&priv->spinlock, flags);

          if (ret < 0)
            {
              /* Were we awakened by a signal?  Did we read anything before
               * we received the signal?
               */

              if (ret != -EINTR || nread >= 0)
                {
                  /* Yes.. return the error. */

                  nread = ret;
                }

              break;
            }
        }
      else
        {
          /* The circular buffer is not empty, get the next byte from the
           * tail index.
           */

          ch = priv->rxbuffer[priv->tail];

          if (++priv->tail >= CONFIG_NXTERM_KBDBUFSIZE)
            {
              priv->tail = 0;
            }

          spin_unlock_irqrestore_nopreempt(&priv->spinlock, flags);

          /* Add the character to the user buffer */

          buffer[nread] = ch;
          nread++;
        }
    }

  /* Echo what was read.  This is done here, after the ring has been
   * drained and released, rather than character by character inside the
   * loop above:  it needs the display lock, and holding that while the
   * ring is being served is exactly the stall described above.  The
   * cursor is hidden once for the whole run rather than once per
   * character, since showing it again in between would draw and erase it
   * at every intermediate position for nothing.
   */

  if (nread > 0 && (priv->tc_lflag & ECHO) != 0)
    {
      ret = nxmutex_lock(&priv->lock);
      if (ret < 0)
        {
          gerr("ERROR: nxmutex_lock failed\n");
        }
      else
        {
          ssize_t i;

          nxterm_hidecursor(priv);
          for (i = 0; i < nread; i++)
            {
              nxterm_echo(priv, buffer[i]);
            }

          nxterm_showcursor(priv);
          nxmutex_unlock(&priv->lock);
        }
    }

  /* Notify all poll/select waiters that they can write to the FIFO */

  if (nread > 0)
    {
      nxterm_pollnotify(priv, POLLOUT);
    }

  /* Return the number of characters actually read */

  return nread;
}

/****************************************************************************
 * Name: nxterm_poll
 ****************************************************************************/

int nxterm_poll(FAR struct file *filep, FAR struct pollfd *fds, bool setup)
{
  FAR struct inode *inode = filep->f_inode;
  FAR struct nxterm_state_s *priv;
  pollevent_t eventset;
  spinlock_t flags;
  int ret = OK;
  int i;

  /* 'ret' is initialised because neither the setup nor the teardown path
   * assigns it, and only the error paths did.  Returning a stack value from
   * a successful setup makes poll() believe the setup failed -- so it never
   * tears the registration down, and priv->fds[] is left pointing at a
   * struct pollfd that goes out of scope with its caller.  The next
   * poll_notify() then calls fds->cb through whatever is in that memory
   * now.  That is a jump to a wild address, not a crash at the pointer, so
   * it lands far from here.
   */

  /* Some sanity checking */

  DEBUGASSERT(inode->i_private);
  priv = inode->i_private;

  /* Get exclusive access to the driver structure */

  flags = spin_lock_irqsave_nopreempt(&priv->spinlock);

  /* Are we setting up the poll?  Or tearing it down? */

  if (setup)
    {
      /* This is a request to set up the poll.  Find an available
       * slot for the poll structure reference
       */

      for (i = 0; i < CONFIG_NXTERM_NPOLLWAITERS; i++)
        {
          /* Find an available slot */

          if (!priv->fds[i])
            {
              /* Bind the poll structure and this slot */

              priv->fds[i] = fds;
              fds->priv    = &priv->fds[i];
              break;
            }
        }

      if (i >= CONFIG_NXTERM_NPOLLWAITERS)
        {
          gerr("ERROR: Too many poll waiters\n");

          fds->priv = NULL;
          ret       = -EBUSY;
          goto errout;
        }

      /* Should immediately notify on any of the requested events?
       * This driver is always available for transmission.
       */

      eventset = POLLOUT;

      /* Check if the receive buffer is empty */

      if (priv->head != priv->tail)
        {
          eventset |= POLLIN;
        }

      poll_notify(&fds, 1, eventset);
    }
  else if (fds->priv)
    {
      /* This is a request to tear down the poll. */

      FAR struct pollfd **slot = (FAR struct pollfd **)fds->priv;

#ifdef CONFIG_DEBUG_GRAPHICS
      if (!slot)
        {
          gerr("ERROR: No slot\n");

          ret = -EIO;
          goto errout;
        }
#endif

      /* Remove all memory of the poll setup */

      *slot     = NULL;
      fds->priv = NULL;
    }

errout:
  spin_unlock_irqrestore_nopreempt(&priv->spinlock, flags);
  return ret;
}

/****************************************************************************
 * Name: nxterm_kbdin
 *
 * Description:
 *  This function should be driven by the window kbdin callback function
 *  (see nx.h).  When the NxTerm is the top window and keyboard input is
 *  received on the top window, that window callback should be directed to
 *  this function.  This function will buffer the keyboard data and makE
 *  it available to the NxTerm as stdin.
 *
 *  If CONFIG_NXTERM_NXKBDIN is not selected, then the NxTerm will
 *  receive its input from stdin (/dev/console).  This works great but
 *  cannot be shared between different windows.  Chaos will ensue if you
 *  try to support multiple NxTerm windows without CONFIG_NXTERM_NXKBDIN
 *
 * Input Parameters:
 *   handle - A handle previously returned by nx_register, nxtk_register, or
 *     nxtool_register.
 *   buffer   - The array of characters
 *   buflen  - The number of characters that are available in buffer[]
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void nxterm_kbdin(NXTERM handle, FAR const uint8_t *buffer, uint8_t buflen)
{
  FAR struct nxterm_state_s *priv;
  irqstate_t flags;
  ssize_t nwritten;
  int nexthead;
  uint8_t nwaiters;
  char ch;

  ginfo("buflen=%" PRId8 "\n", buflen);
  DEBUGASSERT(handle);

  /* Get the reference to the driver structure from the handle */

  priv = (FAR struct nxterm_state_s *)handle;

  /* Get exclusive access to the keyboard ring.
   *
   * This is the spinlock and not priv->lock, both because this function
   * is documented to be callable from an interrupt handler -- where a
   * mutex cannot be taken at all -- and because priv->lock belongs to the
   * display.  Waiting for a redraw to finish before a keystroke could
   * even be queued is what made typing appear to freeze.
   */

  flags = spin_lock_irqsave_nopreempt(&priv->spinlock);

  /* Loop until all of the bytes have been written */

  for (nwritten = 0; nwritten < buflen; nwritten++)
    {
      /* Add the next character */

      ch = buffer[nwritten];

#ifdef CONFIG_NXTERM_SIGINT
      /* The interrupt character is a signal, not data.
       *
       * Delivered to whoever claimed the terminal with TIOCSCTTY -- for NSH
       * that is the foreground command -- and swallowed rather than queued,
       * because a shell that both interrupted the command and then read a
       * stray 0x03 at its prompt would be wrong twice.
       *
       * With nobody claiming the terminal the character is left alone and
       * behaves as it always did: a program reading raw input still sees it.
       */

      if (ch == CONFIG_NXTERM_SIGINT_CHAR && priv->pid > 0)
        {
          priv->intr_echo = true;

          spin_unlock_irqrestore_nopreempt(&priv->spinlock, flags);
          nxsig_kill(priv->pid, SIGINT);
          flags = spin_lock_irqsave_nopreempt(&priv->spinlock);
          continue;
        }
#endif

      /* Calculate the write index AFTER the next byte is add to the ring
       * buffer
       */

      nexthead = priv->head + 1;
      if (nexthead >= CONFIG_NXTERM_KBDBUFSIZE)
        {
          nexthead = 0;
        }

      /* Would the next write overflow the circular buffer? */

      if (nexthead == priv->tail)
        {
          /* Yes... Return an indication that nothing was saved in
           * the buffer.
           */

          gerr("ERROR: Keyboard data overrun\n");
          break;
        }

      /* No... copy the byte */

      priv->rxbuffer[priv->head] = ch;
      priv->head = nexthead;
    }

  nwaiters = priv->nwaiters;
  spin_unlock_irqrestore_nopreempt(&priv->spinlock, flags);

  /* Was anything written?  The notifications are issued with the ring
   * released:  nxterm_pollnotify() takes the same spinlock, and posting a
   * semaphore can reschedule, neither of which belongs inside it.
   */

  if (nwritten > 0)
    {
      int i;

      /* Notify all poll/select waiters that they can read from the FIFO */

      nxterm_pollnotify(priv, POLLIN);

      for (i = 0; i < nwaiters; i++)
        {
          /* Notify all of the waiting readers that more data is
           * available
           */

          nxsem_post(&priv->waitsem);
        }
    }
}

#endif /* CONFIG_NXTERM_NXKBDIN */
