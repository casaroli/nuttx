/****************************************************************************
 * graphics/nxterm/nxterm.h
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

#ifndef __GRAPHICS_NXTERM_NXTERM_H
#define __GRAPHICS_NXTERM_NXTERM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <termios.h>

#include <nuttx/mutex.h>
#include <nuttx/spinlock.h>
#include <nuttx/fs/fs.h>
#include <nuttx/nx/nx.h>
#include <nuttx/nx/nxtk.h>
#include <nuttx/nx/nxfonts.h>
#include <nuttx/nx/nxterm.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* NxTerm Definitions *******************************************************/

/* Device path formats */

#define NX_DEVNAME_FORMAT  "/dev/nxterm%d"
#define NX_DEVNAME_SIZE    16

/* Semaphore protection */

#define NO_HOLDER          (INVALID_PROCESS_ID)

/* Whether the console can scroll by asking NX to move the display.
 *
 * NX_WRITEONLY says the display cannot be read back, which is not the same
 * as being unable to move its contents: a controller with hardware scrolling
 * moves them without reading anything.  Where that is available the console
 * scrolls with a move, and only falls back to repainting every glyph when
 * the display can neither be read nor scroll itself.
 */

#if !defined(CONFIG_NX_WRITEONLY) || defined(CONFIG_NX_HWSCROLL)
#  define NXTERM_HAVE_MOVE 1
#endif

/* Character cell attributes.
 *
 * Only reverse video is represented.  It is the one attribute that can be
 * rendered without a second font: the glyph cache bakes the colours into
 * every glyph it renders, so a reverse-video glyph is simply one taken from
 * a second cache connected with the foreground and background swapped.
 * Bold, underline and blink would each need a font that does not exist here
 * and are accepted and discarded by the SGR handler.
 */

#define NXTERM_ATTR_REVERSE (1 << 0)

/* The width of a tab stop, in columns */

#define NXTERM_TABSIZE     8

/* VT100 escape sequence parsing.
 *
 * The parser is a state machine rather than a table of complete sequences:
 * a CSI sequence carries a variable number of numeric parameters, so the
 * set of byte strings to be recognised is unbounded.
 */

#define VT100_MAX_PARAMS   8   /* Numeric parameters retained per sequence */

#define VT100_STATE_NONE    0  /* Not in an escape sequence */
#define VT100_STATE_ESC     1  /* Saw ESC, waiting for the sequence type */
#define VT100_STATE_CSI     2  /* Saw "ESC [", collecting parameters */
#define VT100_STATE_DISCARD 3  /* Consuming and dropping one more byte */

/* States of the escape sequence detector used to suppress the local echo
 * of escape sequences typed by the user.  Only the input side uses this;
 * escape sequences written by the application are handled by the separate
 * VT100 emulation above.
 */

#define NXTERM_ESCAPE_NONE  0 /* Not in an escape sequence */
#define NXTERM_ESCAPE_START 1 /* Saw ESC, waiting for '[', 'O', or other */
#define NXTERM_ESCAPE_CSI   2 /* Saw "ESC [", consuming CSI bytes */
#define NXTERM_ESCAPE_SS3   3 /* Saw "ESC O", waiting for the final byte */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Identifies the state of the VT100 escape sequence processing */

enum nxterm_vt100state_e
{
  VT100_NOT_CONSUMED = 0, /* Character is not part of an escape sequence */
  VT100_CONSUMED,         /* Character was consumed, sequence incomplete */
  VT100_PROCESSED         /* The full escape sequence was processed */
};

/* Describes on set of console window callbacks */

struct nxterm_state_s;
struct nxterm_operations_s
{
  int (*fill)(FAR struct nxterm_state_s *priv,
              FAR const struct nxgl_rect_s *rect,
              nxgl_mxpixel_t wcolor[CONFIG_NX_NPLANES]);
#ifdef NXTERM_HAVE_MOVE
  int (*move)(FAR struct nxterm_state_s *priv,
              FAR const struct nxgl_rect_s *rect,
              FAR const struct nxgl_point_s *offset);
#endif
  int (*bitmap)(FAR struct nxterm_state_s *priv,
                FAR const struct nxgl_rect_s *dest,
                FAR const void *src[CONFIG_NX_NPLANES],
                FAR const struct nxgl_point_s *origin,
                unsigned int stride);
};

/* One character cell of the display.
 *
 * The terminal is a rows x cols array of these.  That is what lets a
 * character be written *at* a position, replacing whatever was there, which
 * is the whole of what a full-screen program needs and what an append-only
 * list of glyphs cannot express.
 */

struct nxterm_cell_s
{
  uint8_t code;                              /* Character code, ' ' if blank */
  uint8_t attr;                              /* See NXTERM_ATTR_* */
};

/* Describes the state of one NX console driver */

struct nxterm_state_s
{
  FAR const struct nxterm_operations_s *ops; /* Window operations */
  FAR void *handle;                          /* The window handle */
  struct nxterm_window_s wndo;               /* Describes the window and font */
  mutex_t lock;                              /* Forces mutually exclusive access */
#ifdef CONFIG_DEBUG_GRAPHICS
  pid_t holder;                              /* Deadlock avoidance */
#endif
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
  bool unlinked;                             /* True:  Driver has been unlinked */
  uint8_t orefs;                             /* Open reference count */
#endif
  uint8_t minor;                             /* Device minor number */

  /* Font geometry.  The font is fixed width, which is what makes a cell
   * address reducible to a pixel address by multiplication.
   */

  uint8_t fheight;                           /* Max height of a font in pixels */
  uint8_t fwidth;                            /* Max width of a font in pixels */
  uint8_t lineheight;                        /* fheight + line separation */

  /* The character grid */

  uint16_t rows;                             /* Number of character rows */
  uint16_t cols;                             /* Number of character columns */
  FAR struct nxterm_cell_s *cells;           /* rows x cols cells */

  /* Cursor and terminal state */

  uint16_t crow;                             /* Cursor row */
  uint16_t ccol;                             /* Cursor column */
  uint16_t savedrow;                         /* Cursor row saved by DECSC */
  uint16_t savedcol;                         /* Cursor column saved by DECSC */
  uint8_t  savedattr;                        /* Attributes saved by DECSC */
  uint16_t strow;                            /* Scrolling region, first row */
  uint16_t sbrow;                            /* Scrolling region, last row */
  uint8_t  attr;                             /* Attributes for new characters */
  bool     wrap;                             /* Deferred wrap is pending */
  bool     cvisible;                         /* Cursor is not hidden by DECTCEM */
  bool     cshown;                           /* Cursor is drawn on the display */
  uint16_t cdrawrow;                         /* Row the cursor is drawn at */
  uint16_t cdrawcol;                         /* Column the cursor is drawn at */

  /* A redraw that arrived while the display lock was held.
   *
   * Guarded by the spinlock rather than by priv->lock, because the whole
   * point of it is to be recorded without waiting for priv->lock.  See
   * nxterm_redraw().
   */

  struct nxgl_rect_s dmgrect;                /* Region still to be drawn */
  bool     dmgpending;                       /* dmgrect holds something */

  /* VT100 escape sequence parsing */

  uint8_t  vtstate;                          /* See VT100_STATE_* */
  uint8_t  nparams;                          /* Number of parameters seen */
  bool     vtprivate;                        /* Sequence began "ESC [ ?" */
  uint16_t params[VT100_MAX_PARAMS];         /* The parameters themselves */

  /* Font cache data storage */

  FCACHE fcache;                             /* Font cache handle */

  /* A second cache with the colours swapped, used for reverse video.  That
   * is what draws the block cursor as something the text shows through
   * rather than something that hides it, and a cache is the only way to get
   * it: glyphs are rendered with their colours baked in.
   */

  FCACHE rcache;

  /* Keyboard input support */

#ifdef CONFIG_NXTERM_NXKBDIN
  sem_t waitsem;                             /* Supports waiting for input data */
  uint8_t nwaiters;                          /* Number of threads waiting for data */
  uint8_t head;                              /* rxbuffer head/input index */
  uint8_t tail;                              /* rxbuffer tail/output index */

  tcflag_t tc_lflag;                         /* Local modes; only ECHO is used */
  uint8_t escape;                            /* See NXTERM_ESCAPE_* */

#ifdef CONFIG_NXTERM_SIGINT
  /* The task that receives SIGINT when the interrupt character is typed.
   *
   * Set by TIOCSCTTY, which NSH issues with the pid of each foreground
   * command it spawns -- so Ctrl-C interrupts the command rather than the
   * shell that is waiting for it.  INVALID_PROCESS_ID when no task has
   * claimed the terminal, in which case the character is data like any
   * other.
   */

  pid_t pid;

  /* Set when the interrupt character has been seen and not yet shown.
   *
   * A terminal echoes "^C" so the user can see why their command stopped.
   * It cannot be echoed where it is detected: nxterm_kbdin() may be called
   * from an interrupt handler, and putting a character on the display needs
   * priv->lock, which is a mutex.  So the sighting is recorded here and the
   * next write -- in practice the shell's next prompt, in the writer's own
   * context with the lock properly held -- emits it.
   */

  bool intr_echo;
#endif

  uint8_t rxbuffer[CONFIG_NXTERM_KBDBUFSIZE];

  /* The following is a list if poll structures of threads waiting for
   * driver events. The 'struct pollfd' reference for each open is also
   * retained in the f_priv field of the 'struct file'.
   */

  FAR struct pollfd *fds[CONFIG_NXTERM_NPOLLWAITERS];
#endif /* CONFIG_NXTERM_NXKBDIN */

  /* Spinlock */

  spinlock_t spinlock;
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* This is the common NX driver file operations */

extern const struct file_operations g_nxterm_drvrops;

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_cellat
 *
 * Description:
 *   The cell at a row and column.  Every access to the grid goes through
 *   here, so there is one place that knows how it is laid out.
 *
 ****************************************************************************/

static inline FAR struct nxterm_cell_s *
nxterm_cellat(FAR struct nxterm_state_s *priv, int row, int col)
{
  return &priv->cells[(unsigned int)row * priv->cols + (unsigned int)col];
}

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Common device registration/un-registration */

FAR struct nxterm_state_s *
nxterm_register(NXTERM handle,
                FAR struct nxterm_window_s *wndo,
                FAR const struct nxterm_operations_s *ops,
                int minor);
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
void nxterm_unregister(FAR struct nxterm_state_s *priv);
#endif

/* Driver methods */

#ifdef CONFIG_NXTERM_NXKBDIN
ssize_t nxterm_read(FAR struct file *filep, FAR char *buffer, size_t len);
int nxterm_poll(FAR struct file *filep, FAR struct pollfd *fds, bool setup);
#endif

/* IOCTL handlers */

void nxterm_redraw(NXTERM handle, FAR const struct nxgl_rect_s *rect,
                   bool more);
void nxterm_flushdamage(FAR struct nxterm_state_s *priv);
#ifdef CONFIG_NXTERM_NXKBDIN
void nxterm_kbdin(NXTERM handle, FAR const uint8_t *buffer, uint8_t buflen);
#endif
int nxterm_resize(NXTERM handle, FAR const struct nxgl_size_s *size);

/* VT100 Terminal emulation */

enum nxterm_vt100state_e nxterm_vt100(FAR struct nxterm_state_s *priv,
                                      char ch);

/* Grid allocation and geometry */

int nxterm_gridalloc(FAR struct nxterm_state_s *priv);
void nxterm_gridfree(FAR struct nxterm_state_s *priv);
void nxterm_gridreset(FAR struct nxterm_state_s *priv);

/* Painting.  Cell contents are the only truth; these put what the grid says
 * onto the glass.
 */

void nxterm_paintcell(FAR struct nxterm_state_s *priv, int row, int col,
                      FAR const struct nxgl_rect_s *clip, bool fillbg);
void nxterm_paintarea(FAR struct nxterm_state_s *priv,
                      int row1, int col1, int row2, int col2,
                      FAR const struct nxgl_rect_s *clip);

/* Generic text display helpers */

void nxterm_home(FAR struct nxterm_state_s *priv);
void nxterm_clear(FAR struct nxterm_state_s *priv);
void nxterm_gotoxy(FAR struct nxterm_state_s *priv, int row, int col);
void nxterm_blank(FAR struct nxterm_state_s *priv, int row,
                  int col1, int col2);
void nxterm_index(FAR struct nxterm_state_s *priv);
void nxterm_revindex(FAR struct nxterm_state_s *priv);

void nxterm_putc(FAR struct nxterm_state_s *priv, uint8_t ch);
void nxterm_showcursor(FAR struct nxterm_state_s *priv);
void nxterm_hidecursor(FAR struct nxterm_state_s *priv);

/* Editing operations used by the VT100 emulation */

void nxterm_erasedisplay(FAR struct nxterm_state_s *priv, int mode);
void nxterm_eraseline(FAR struct nxterm_state_s *priv, int mode);
void nxterm_erasechars(FAR struct nxterm_state_s *priv, int nchars);
void nxterm_insertchars(FAR struct nxterm_state_s *priv, int nchars);
void nxterm_deletechars(FAR struct nxterm_state_s *priv, int nchars);
void nxterm_insertlines(FAR struct nxterm_state_s *priv, int nlines);
void nxterm_deletelines(FAR struct nxterm_state_s *priv, int nlines);

/* Scrolling support */

void nxterm_scroll(FAR struct nxterm_state_s *priv, int nlines);
void nxterm_scrolldown(FAR struct nxterm_state_s *priv, int nlines);

#endif /* __GRAPHICS_NXTERM_NXTERM_H */
