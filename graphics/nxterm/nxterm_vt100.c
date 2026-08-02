/****************************************************************************
 * graphics/nxterm/nxterm_vt100.c
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

#include <stdio.h>
#include <assert.h>

#include <nuttx/ascii.h>

#include "nxterm.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_param
 *
 * Description:
 *   One numeric parameter of the sequence being processed.
 *
 *   A parameter that is absent or zero means "use the default", which is
 *   how a VT100 spells it:  "ESC [ A", "ESC [ 0 A" and "ESC [ 1 A" all move
 *   the cursor up one line.
 *
 ****************************************************************************/

static int nxterm_param(FAR struct nxterm_state_s *priv, int index,
                        int defval)
{
  if (index >= (int)priv->nparams || priv->params[index] == 0)
    {
      return defval;
    }

  return (int)priv->params[index];
}

/****************************************************************************
 * Name: nxterm_rawparam
 *
 * Description:
 *   One numeric parameter, where zero is a value in its own right rather
 *   than a request for the default.  Erasing and SGR are specified that
 *   way:  in "ESC [ 0 J" the zero selects a mode.
 *
 ****************************************************************************/

static int nxterm_rawparam(FAR struct nxterm_state_s *priv, int index,
                           int defval)
{
  if (index >= (int)priv->nparams)
    {
      return defval;
    }

  return (int)priv->params[index];
}

/****************************************************************************
 * Name: nxterm_sgr
 *
 * Description:
 *   Select graphic rendition.
 *
 *   Reverse video is the only attribute that can be honoured, since a glyph
 *   carries its colours from the cache it was rendered by and there is no
 *   bold, italic or underlined font here.  Colour selection is accepted and
 *   discarded for the same reason -- the window has one foreground and one
 *   background.  Discarding is still the point: the parameters have to be
 *   consumed, or they would be drawn as text.
 *
 ****************************************************************************/

static void nxterm_sgr(FAR struct nxterm_state_s *priv)
{
  int nparams = priv->nparams > 0 ? (int)priv->nparams : 1;
  int i;

  for (i = 0; i < nparams; i++)
    {
      switch (nxterm_rawparam(priv, i, 0))
        {
          case 0:  /* All attributes off */
            priv->attr = 0;
            break;

          case 7:  /* Reverse video on */
            priv->attr |= NXTERM_ATTR_REVERSE;
            break;

          case 27: /* Reverse video off */
            priv->attr &= ~NXTERM_ATTR_REVERSE;
            break;

          default:
            break;
        }
    }
}

/****************************************************************************
 * Name: nxterm_decstbm
 *
 * Description:
 *   Set the scrolling region.
 *
 *   A region smaller than the display cannot be scrolled by moving the
 *   display, so everything that touches it repaints instead.  Supporting it
 *   at all matters because a program that sets a region and is then ignored
 *   scrolls the whole screen when it meant to scroll part of one.
 *
 ****************************************************************************/

static void nxterm_decstbm(FAR struct nxterm_state_s *priv)
{
  int top    = nxterm_param(priv, 0, 1) - 1;
  int bottom = nxterm_param(priv, 1, (int)priv->rows) - 1;

  if (top < 0)
    {
      top = 0;
    }

  if (bottom >= (int)priv->rows)
    {
      bottom = (int)priv->rows - 1;
    }

  /* A region has to have at least two lines to be scrollable.  An invalid
   * one is ignored, which is what the specification asks for.
   */

  if (top >= bottom)
    {
      return;
    }

  priv->strow = (uint16_t)top;
  priv->sbrow = (uint16_t)bottom;

  /* Setting the region homes the cursor */

  nxterm_home(priv);
}

/****************************************************************************
 * Name: nxterm_dsr
 *
 * Description:
 *   Device status report.  The answer is delivered on the input side, which
 *   is where a real terminal would put it:  the program that asked reads it
 *   back from the same descriptor it types on.
 *
 ****************************************************************************/

#ifdef CONFIG_NXTERM_NXKBDIN
static void nxterm_dsr(FAR struct nxterm_state_s *priv)
{
  char reply[16];
  int len;

  switch (nxterm_rawparam(priv, 0, 0))
    {
      case 5:  /* Report terminal status:  always "no malfunction" */
        {
          len = snprintf(reply, sizeof(reply), "%c[0n", ASCII_ESC);
        }
        break;

      case 6:  /* Report cursor position, in one-based coordinates */
        {
          len = snprintf(reply, sizeof(reply), "%c[%u;%uR", ASCII_ESC,
                         (unsigned int)priv->crow + 1,
                         (unsigned int)priv->ccol + 1);
        }
        break;

      default:
        return;
    }

  if (len > 0 && len < (int)sizeof(reply))
    {
      nxterm_inject(priv, reply, (size_t)len);
    }
}
#endif

/****************************************************************************
 * Name: nxterm_setmode
 *
 * Description:
 *   Set or reset a mode.  Only DECTCEM, which shows and hides the cursor,
 *   changes anything here; a full-screen program uses it to keep the cursor
 *   from flickering across the screen while it redraws.
 *
 ****************************************************************************/

static void nxterm_setmode(FAR struct nxterm_state_s *priv, bool set)
{
  if (priv->vtprivate && nxterm_rawparam(priv, 0, 0) == 25)
    {
      if (!set)
        {
          nxterm_hidecursor(priv);
        }

      priv->cvisible = set;
    }
}

/****************************************************************************
 * Name: nxterm_csidispatch
 *
 * Description:
 *   Act on a complete control sequence, whose final byte is 'final'.
 *
 ****************************************************************************/

static void nxterm_csidispatch(FAR struct nxterm_state_s *priv, char final)
{
  switch (final)
    {
      case 'A':  /* CUU:  cursor up */
        nxterm_gotoxy(priv, (int)priv->crow - nxterm_param(priv, 0, 1),
                      (int)priv->ccol);
        break;

      case 'B':  /* CUD:  cursor down */
        nxterm_gotoxy(priv, (int)priv->crow + nxterm_param(priv, 0, 1),
                      (int)priv->ccol);
        break;

      case 'C':  /* CUF:  cursor forward */
        nxterm_gotoxy(priv, (int)priv->crow,
                      (int)priv->ccol + nxterm_param(priv, 0, 1));
        break;

      case 'D':  /* CUB:  cursor back */
        nxterm_gotoxy(priv, (int)priv->crow,
                      (int)priv->ccol - nxterm_param(priv, 0, 1));
        break;

      case 'E':  /* CNL:  cursor to the start of a following line */
        nxterm_gotoxy(priv, (int)priv->crow + nxterm_param(priv, 0, 1), 0);
        break;

      case 'F':  /* CPL:  cursor to the start of a preceding line */
        nxterm_gotoxy(priv, (int)priv->crow - nxterm_param(priv, 0, 1), 0);
        break;

      case 'G':  /* CHA:  cursor to an absolute column */
      case '`':  /* HPA:  the same thing under another name */
        nxterm_gotoxy(priv, (int)priv->crow, nxterm_param(priv, 0, 1) - 1);
        break;

      case 'd':  /* VPA:  cursor to an absolute row */
        nxterm_gotoxy(priv, nxterm_param(priv, 0, 1) - 1, (int)priv->ccol);
        break;

      case 'H':  /* CUP:  cursor to an absolute position */
      case 'f':  /* HVP:  the same thing under another name */
        nxterm_gotoxy(priv, nxterm_param(priv, 0, 1) - 1,
                      nxterm_param(priv, 1, 1) - 1);
        break;

      case 'J':  /* ED:  erase in display */
        nxterm_erasedisplay(priv, nxterm_rawparam(priv, 0, 0));
        break;

      case 'K':  /* EL:  erase in line */
        nxterm_eraseline(priv, nxterm_rawparam(priv, 0, 0));
        break;

      case 'L':  /* IL:  insert lines */
        nxterm_insertlines(priv, nxterm_param(priv, 0, 1));
        break;

      case 'M':  /* DL:  delete lines */
        nxterm_deletelines(priv, nxterm_param(priv, 0, 1));
        break;

      case '@':  /* ICH:  insert characters */
        nxterm_insertchars(priv, nxterm_param(priv, 0, 1));
        break;

      case 'P':  /* DCH:  delete characters */
        nxterm_deletechars(priv, nxterm_param(priv, 0, 1));
        break;

      case 'X':  /* ECH:  erase characters */
        nxterm_erasechars(priv, nxterm_param(priv, 0, 1));
        break;

      case 'S':  /* SU:  scroll the region up */
        nxterm_scroll(priv, nxterm_param(priv, 0, 1));
        break;

      case 'T':  /* SD:  scroll the region down */
        nxterm_scrolldown(priv, nxterm_param(priv, 0, 1));
        break;

      case 'm':  /* SGR:  select graphic rendition */
        nxterm_sgr(priv);
        break;

      case 'r':  /* DECSTBM:  set the scrolling region */
        nxterm_decstbm(priv);
        break;

      case 'h':  /* SM/DECSET:  set a mode */
        nxterm_setmode(priv, true);
        break;

      case 'l':  /* RM/DECRST:  reset a mode */
        nxterm_setmode(priv, false);
        break;

#ifdef CONFIG_NXTERM_NXKBDIN
      case 'n':  /* DSR:  device status report.  Answered only where this
                  * terminal has an input side to answer on.
                  */
        nxterm_dsr(priv);
        break;
#endif

      case 's':  /* Save the cursor (the ANSI.SYS spelling) */
        priv->savedrow  = priv->crow;
        priv->savedcol  = priv->ccol;
        priv->savedattr = priv->attr;
        break;

      case 'u':  /* Restore the cursor (the ANSI.SYS spelling) */
        priv->attr = priv->savedattr;
        nxterm_gotoxy(priv, (int)priv->savedrow, (int)priv->savedcol);
        break;

      default:
        /* Recognised as a sequence and discarded.  Consuming it is the
         * point:  an unhandled sequence that fell through to the display
         * would be drawn as its own text.
         */

        break;
    }
}

/****************************************************************************
 * Name: nxterm_csi
 *
 * Description:
 *   Collect one byte of a control sequence.
 *
 ****************************************************************************/

static enum nxterm_vt100state_e nxterm_csi(FAR struct nxterm_state_s *priv,
                                           char ch)
{
  /* A private sequence is marked by a single byte between the bracket and
   * the parameters.
   */

  if (priv->nparams == 0 && (ch == '?' || ch == '>' || ch == '<' ||
                             ch == '='))
    {
      priv->vtprivate = true;
      return VT100_CONSUMED;
    }

  if (ch >= '0' && ch <= '9')
    {
      if (priv->nparams == 0)
        {
          priv->nparams   = 1;
          priv->params[0] = 0;
        }

      if (priv->nparams <= VT100_MAX_PARAMS)
        {
          FAR uint16_t *param = &priv->params[priv->nparams - 1];

          /* Saturate rather than wrap.  A parameter larger than the display
           * is clamped by whatever acts on it, and a wrapped one would not
           * be.
           */

          if (*param < 10000)
            {
              *param = *param * 10 + (uint16_t)(ch - '0');
            }
        }

      return VT100_CONSUMED;
    }

  if (ch == ';')
    {
      if (priv->nparams == 0)
        {
          priv->nparams   = 1;
          priv->params[0] = 0;
        }

      /* Parameters past the last slot are dropped rather than allowed to
       * overwrite the ones already collected.  A sequence with more than
       * VT100_MAX_PARAMS of them carries nothing this emulation reads.
       */

      if (priv->nparams < VT100_MAX_PARAMS)
        {
          priv->params[priv->nparams] = 0;
          priv->nparams++;
        }

      return VT100_CONSUMED;
    }

  /* Intermediate bytes carry no information this emulation uses */

  if ((unsigned char)ch >= 0x20 && (unsigned char)ch <= 0x2f)
    {
      return VT100_CONSUMED;
    }

  /* A final byte ends the sequence */

  if ((unsigned char)ch >= 0x40 && (unsigned char)ch <= 0x7e)
    {
      nxterm_csidispatch(priv, ch);
      priv->vtstate = VT100_STATE_NONE;
      return VT100_PROCESSED;
    }

  /* Anything else is a control character that has arrived in the middle of
   * a sequence.  Abandon the sequence and let the character be handled on
   * its own:  a malformed sequence must not swallow the output that
   * follows it.
   */

  priv->vtstate = VT100_STATE_NONE;
  return VT100_NOT_CONSUMED;
}

/****************************************************************************
 * Name: nxterm_escape
 *
 * Description:
 *   Act on the byte after an escape, which says what kind of sequence this
 *   is.
 *
 ****************************************************************************/

static enum nxterm_vt100state_e
nxterm_escape(FAR struct nxterm_state_s *priv, char ch)
{
  priv->vtstate = VT100_STATE_NONE;

  switch (ch)
    {
      case '[':  /* CSI:  a control sequence with parameters follows */
        priv->vtstate   = VT100_STATE_CSI;
        priv->nparams   = 0;
        priv->vtprivate = false;
        return VT100_CONSUMED;

      case '(':  /* Character set designation, whose argument is discarded */
      case ')':
      case '*':
      case '+':
      case '#':
        priv->vtstate = VT100_STATE_DISCARD;
        return VT100_CONSUMED;

      case '7':  /* DECSC:  save the cursor */
        priv->savedrow  = priv->crow;
        priv->savedcol  = priv->ccol;
        priv->savedattr = priv->attr;
        break;

      case '8':  /* DECRC:  restore the cursor */
      case 'u':  /* Not a VT100 sequence, but termcurses ends its window
                  * size probe with it and means DECRC by it.
                  */
        priv->attr = priv->savedattr;
        nxterm_gotoxy(priv, (int)priv->savedrow, (int)priv->savedcol);
        break;

      case 'D':  /* IND:  index */
        priv->wrap = false;
        nxterm_index(priv);
        break;

      case 'M':  /* RI:  reverse index */
        priv->wrap = false;
        nxterm_revindex(priv);
        break;

      case 'E':  /* NEL:  next line */
        priv->wrap = false;
        priv->ccol = 0;
        nxterm_index(priv);
        break;

      case 'c':  /* RIS:  reset to the initial state */
        nxterm_gridreset(priv);
        nxterm_clear(priv);
        break;

      default:
        /* Everything else -- keypad modes among them -- is recognised as a
         * two byte sequence and discarded.
         */

        break;
    }

  return VT100_PROCESSED;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_vt100
 *
 * Description:
 *   Offer one byte of the output stream to the terminal emulation.
 *
 * Input Parameters:
 *   priv - Driver data structure
 *   ch - The newly received character
 *
 * Returned Value:
 *   state - See enum nxterm_vt100state_e;
 *
 ****************************************************************************/

enum nxterm_vt100state_e nxterm_vt100(FAR struct nxterm_state_s *priv,
                                      char ch)
{
  DEBUGASSERT(priv != NULL);

  switch (priv->vtstate)
    {
      case VT100_STATE_NONE:
      default:
        {
          if (ch != ASCII_ESC)
            {
              return VT100_NOT_CONSUMED;
            }

          priv->vtstate = VT100_STATE_ESC;
          return VT100_CONSUMED;
        }

      case VT100_STATE_ESC:
        return nxterm_escape(priv, ch);

      case VT100_STATE_CSI:
        return nxterm_csi(priv, ch);

      case VT100_STATE_DISCARD:
        {
          priv->vtstate = VT100_STATE_NONE;
          return VT100_PROCESSED;
        }
    }
}
