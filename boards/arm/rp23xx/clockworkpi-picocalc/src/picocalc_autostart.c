/****************************************************************************
 * boards/arm/rp23xx/clockworkpi-picocalc/src/picocalc_autostart.c
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

/* Bring the panel up as a terminal without anyone having to type anything.
 *
 * On a handheld the debug console is not a thing the user has.  Leaving the
 * screen blank until somebody runs two commands over SWD makes every reset
 * look like a failure, which is a poor way for a machine to behave and an
 * excellent way to lose an afternoon deciding whether the board is broken.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <syslog.h>

#include <nuttx/lib/builtin.h>
#include <nuttx/sched.h>

#include "rp23xx_pico.h"

#ifdef CONFIG_CLOCKWORKPI_PICOCALC_AUTOSTART

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The window nxterm registers once it is ready to be typed into. */

#define TERM_DEVICE "/dev/nxterm0"

/* How long to wait for that to appear, and how often to look.  Two seconds
 * is far longer than it takes; it exists so that a failure to start does not
 * leave this task looping forever.
 */

#define TERM_WAIT_MS   2000
#define TERM_POLL_MS   20

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: start_builtin
 *
 * Description:
 *   Launch a builtin application by name, with the priority and stack size
 *   it was registered with.
 *
 ****************************************************************************/

static int start_builtin(FAR const char *name)
{
  int i;

  for (i = 0; i < g_builtin_count; i++)
    {
      FAR const struct builtin_s *b = &g_builtins[i];

      if (b->name != NULL && strcmp(b->name, name) == 0)
        {
          return nxtask_create(b->name, b->priority, NULL, b->stacksize,
                               b->main, NULL, NULL);
        }
    }

  syslog(LOG_ERR, "picocalc: %s is not built in\n", name);
  return -ENOENT;
}

/****************************************************************************
 * Name: autostart_task
 *
 * Description:
 *   Start the terminal, then the keyboard bridge.
 *
 *   The order matters and so does the wait between them: kbdbridge feeds
 *   keys to whichever NX window has focus, so starting it before nxterm has
 *   created one throws those keys away.  Waiting for the terminal device to
 *   appear is the honest form of that dependency -- a fixed sleep would be a
 *   guess that goes wrong on a busy boot.
 *
 ****************************************************************************/

static int autostart_task(int argc, FAR char *argv[])
{
  struct stat buf;
  int waited;

  if (start_builtin("nxterm") < 0)
    {
      return EXIT_FAILURE;
    }

  for (waited = 0; waited < TERM_WAIT_MS; waited += TERM_POLL_MS)
    {
      if (stat(TERM_DEVICE, &buf) == 0)
        {
          break;
        }

      usleep(TERM_POLL_MS * 1000);
    }

  if (waited >= TERM_WAIT_MS)
    {
      syslog(LOG_ERR, "picocalc: %s never appeared, not starting the "
                      "keyboard bridge\n", TERM_DEVICE);
      return EXIT_FAILURE;
    }

  if (start_builtin("kbdbridge") < 0)
    {
      return EXIT_FAILURE;
    }

#ifdef CONFIG_SYSTEM_USBSH
  /* A second shell on the USB port, if one is built.
   *
   * Not having it is not a failure:  the panel is the terminal this board
   * is for, and the USB session is a convenience for whoever is developing
   * on it.  It comes last so that nothing it does can delay the panel.
   */

  start_builtin("usbsh");
#endif

  return EXIT_SUCCESS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: picocalc_autostart
 *
 * Description:
 *   Arrange for the terminal to come up on the panel by itself.
 *
 *   The work happens on its own task rather than here, because this is
 *   called from board_late_initialize(), which runs before the initial
 *   application is started and must not block: waiting for the terminal
 *   device inline would stall the boot it is waiting on.
 *
 ****************************************************************************/

void picocalc_autostart(void)
{
  int ret;

  ret = nxtask_create("picocalc_autostart", SCHED_PRIORITY_DEFAULT, NULL,
                      CONFIG_CLOCKWORKPI_PICOCALC_AUTOSTART_STACKSIZE,
                      autostart_task, NULL, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: picocalc_autostart: %d\n", ret);
    }
}

#endif /* CONFIG_CLOCKWORKPI_PICOCALC_AUTOSTART */
