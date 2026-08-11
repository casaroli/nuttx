/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_pwm_audio.c
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

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <syslog.h>
#include <debug.h>

#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/kthread.h>
#include <nuttx/audio/audio.h>
#include <nuttx/audio/pwm_audio.h>

#include <arch/board/board.h>

#include "arm_internal.h"
#include "rp23xx_gpio.h"
#include "rp23xx_dmac.h"
#include "rp23xx_pwm.h"
#include "rp23xx_pwm_audio.h"
#include "hardware/rp23xx_pwm.h"
#include "hardware/rp23xx_dma.h"
#include "hardware/rp23xx_memorymap.h"
#include "hardware/rp23xx_timer.h"

#ifdef CONFIG_RP23XX_PWM_AUDIO

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The carrier.
 *
 * The board's reconstruction filter is a single pole at 23.1 kHz, so the
 * carrier is only attenuated by 20*log10(fc / 23.1 kHz).  Ten bits puts it
 * at 146.5 kHz -- 16 dB down, and 3.3x the highest sample rate offered,
 * which is far enough for the filter to tell them apart.  Eleven bits would
 * drop the carrier to 73 kHz, only 1.7x the sample rate, and no filter this
 * simple separates those.  See the board's hardware notes, "Audio".
 */

#define PWM_AUDIO_BITS      10
#define PWM_AUDIO_TOP       ((1 << PWM_AUDIO_BITS) - 1)

/* Silence is mid-scale, not zero.  The output is AC coupled, so what the
 * speaker hears is the departure from the average; sitting at zero would
 * just charge the coupling capacitor and click.
 */

#define PWM_AUDIO_MID       (1 << (PWM_AUDIO_BITS - 1))

/* Signed 16-bit samples scale to the compare range by a shift alone */

#define PWM_AUDIO_SHIFT     (16 - PWM_AUDIO_BITS)

/* Two halves of one buffer, each with its own DMA channel, each channel's
 * CHAIN_TO pointing at the other.
 *
 * This is what makes the output continuous.  Both channels are armed and
 * only the first is ever triggered by software; when one finishes, the
 * hardware starts the other in the same cycle, with nothing in between for
 * software to be late for.  Driving one channel and restarting it from a
 * thread -- which is what this did before -- leaves the output dead from
 * the completion interrupt until the thread runs, and that was measured at
 * 1.1 ms mean and 1.2 ms worst against a 93 ms buffer:  a gap at every
 * boundary, ten times a second, which is what it sounded like.
 *
 * The interrupt at the end of each half is still there and is still what
 * paces the refill; it just no longer has the output waiting on it.
 */

#define PWM_AUDIO_NBUFFERS  2
#define PWM_AUDIO_NSAMPLES  CONFIG_RP23XX_PWM_AUDIO_BUFSAMPLES

/* Silence, as one compare register write:  both channels at mid-scale */

#define PWM_AUDIO_MIDPAIR   ((PWM_AUDIO_MID << 16) | PWM_AUDIO_MID)

/* How many buffers the upper half may keep in flight.
 *
 * This is what the player asks for with AUDIOIOC_GETBUFFERINFO, and it sets
 * how much audio is queued ahead of the hardware.  It has to cover the
 * worst case for refilling one, which here means a read from a microSD card
 * over SPI while the CPU is also converting samples.  Two shallow buffers
 * starve audibly -- half a second of sound, half a second of silence.
 */

#define PWM_AUDIO_NAPBS     8

/* Size of each buffer handed to the player, in bytes.  Kept independent of
 * the conversion buffers above:  those want to be long, to make the gap
 * between them a small fraction of the time they cover, while these only
 * need to be numerous enough to ride out a card read.
 */

#define PWM_AUDIO_APBSIZE   4096

/* How long the mid-scale ramp takes at either end of a stream */

#define PWM_AUDIO_RAMPMS    8

#define PWM_AUDIO_MAXRATE   48000
#define PWM_AUDIO_MINRATE   4000

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum pwm_audio_bufstate_e
{
  PWM_AUDIO_BUF_FREE = 0,   /* Nothing in it; the filler may have it */
  PWM_AUDIO_BUF_READY,      /* Holds samples waiting to be played */
  PWM_AUDIO_BUF_PLAYING     /* The DMA is reading it now */
};

struct pwm_audio_buf_s
{
  FAR uint32_t *samples;                  /* PWM_AUDIO_NSAMPLES words */
  unsigned int  nsamples;                 /* How many are valid */
  uint8_t       state;                    /* See enum pwm_audio_bufstate_e */
};

struct pwm_audio_dev_s
{
  struct audio_lowerhalf_s dev;           /* The upper half's view of us */

  unsigned int  slice;                    /* PWM slice being driven */
  int           pin_a;                    /* Left channel GPIO */
  int           pin_b;                    /* Right channel GPIO, or -1 */

  /* One DMA channel per half, their numbers kept alongside because
   * CHAIN_TO needs a number and the allocator only returns a handle.
   */

  DMA_HANDLE    dma[PWM_AUDIO_NBUFFERS];
  unsigned int  dmach[PWM_AUDIO_NBUFFERS];

  FAR uint32_t *ring;                     /* Both halves, one allocation */
  uintptr_t     ccaddr;                   /* Address of the compare */

  mutex_t       lock;                     /* Guards the queue and state */
  sem_t         wake;                     /* Wakes the filling thread */
  struct dq_queue_s pendq;                /* Buffers from the upper half */

  struct pwm_audio_buf_s buf[PWM_AUDIO_NBUFFERS];
  uint8_t       playing;                  /* Index the DMA is reading */

  uint32_t      samprate;                 /* Configured sample rate */
  uint8_t       nchannels;                /* 1 or 2 */
  uint8_t       bpsamp;                   /* Bits per sample; only 16 */
  uint16_t      volume;                   /* 0..1000, as the upper half uses */

  bool          streaming;                /* Between start and stop */
  bool          terminate;                /* Tell the thread to go away */
  bool          claimed;                  /* Slice ownership is held */
  bool          completed;                /* The end of stream was reported */
  int           threadid;                 /* Filling thread, or 0 */

#ifdef CONFIG_RP23XX_PWM_AUDIO_STATS
  uint32_t      pendframes;               /* Frames queued but unconverted */
  uint32_t      donetime;                 /* When a buffer ended */
  bool          donevalid;                /* ...and whether that is a time */
  bool          doneready;                /* Was the next one ready then? */
  bool          draining;                 /* The last buffer has been given */
#endif
};

#ifdef CONFIG_RP23XX_PWM_AUDIO_STATS

/* Everything measured about one stream.
 *
 * Static, and carrying its own magic number and sample rate, so that a
 * debugger can find it in the symbol table and read it while the stream is
 * still running -- which matters, because halting the CPU to look at it is
 * itself the kind of stall being measured.
 *
 * Every field is 32 bits.  The sums are in microseconds and would take
 * about seventy minutes of *accumulated* latency to wrap, which no
 * plausible run reaches.
 */

#define PWM_AUDIO_STATS_MAGIC 0x50415331   /* "PAS1" */

struct pwm_audio_stats_s
{
  uint32_t magic;          /* PWM_AUDIO_STATS_MAGIC once a stream has run */
  uint32_t samprate;       /* Frames per second, to read the rest in time */
  uint32_t nchannels;
  uint32_t bufframes;      /* PWM_AUDIO_NSAMPLES */

  uint32_t completions;    /* Buffers the DMA finished */
  uint32_t measured;       /* ...of which came before the stream ran out */
  uint32_t starved;        /* ...of which found nothing ready to follow */

  uint32_t occmin;         /* Low-water mark of unplayed audio, in frames */
  uint32_t occlast;        /* The most recent reading of the same */

  /* How long the thread took to put fresh audio into a half after being
   * told the half was free, in microseconds.
   *
   * This is margin, not dead air:  the other half is playing throughout,
   * started by the hardware.  It matters against the half-period, which is
   * how long there is before the chain comes back round.
   */

  uint32_t refilln;
  uint32_t refillmax;
  uint32_t refillsum;

  uint32_t starvemax;      /* The same, when nothing was waiting */
  uint32_t starvesum;

  uint32_t filln;          /* Buffers converted */
  uint32_t fillmax;        /* Longest conversion, us */
  uint32_t fillsum;
  uint32_t fillframes;     /* Frames converted, for a per-frame cost */
};

static struct pwm_audio_stats_s g_pwm_audio_stats;

#endif /* CONFIG_RP23XX_PWM_AUDIO_STATS */

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  pwm_audio_getcaps(FAR struct audio_lowerhalf_s *dev, int type,
                              FAR struct audio_caps_s *caps);
#ifdef CONFIG_AUDIO_MULTI_SESSION
static int  pwm_audio_configure(FAR struct audio_lowerhalf_s *dev,
                                FAR void *session,
                                FAR const struct audio_caps_s *caps);
static int  pwm_audio_start(FAR struct audio_lowerhalf_s *dev,
                            FAR void *session);
static int  pwm_audio_stop(FAR struct audio_lowerhalf_s *dev,
                           FAR void *session);
static int  pwm_audio_pause(FAR struct audio_lowerhalf_s *dev,
                            FAR void *session);
static int  pwm_audio_resume(FAR struct audio_lowerhalf_s *dev,
                             FAR void *session);
static int  pwm_audio_reserve(FAR struct audio_lowerhalf_s *dev,
                              FAR void **session);
static int  pwm_audio_release(FAR struct audio_lowerhalf_s *dev,
                              FAR void *session);
#else
static int  pwm_audio_configure(FAR struct audio_lowerhalf_s *dev,
                                FAR const struct audio_caps_s *caps);
static int  pwm_audio_start(FAR struct audio_lowerhalf_s *dev);
static int  pwm_audio_stop(FAR struct audio_lowerhalf_s *dev);
static int  pwm_audio_pause(FAR struct audio_lowerhalf_s *dev);
static int  pwm_audio_resume(FAR struct audio_lowerhalf_s *dev);
static int  pwm_audio_reserve(FAR struct audio_lowerhalf_s *dev);
static int  pwm_audio_release(FAR struct audio_lowerhalf_s *dev);
#endif
static int  pwm_audio_shutdown(FAR struct audio_lowerhalf_s *dev);
static int  pwm_audio_enqueue(FAR struct audio_lowerhalf_s *dev,
                              FAR struct ap_buffer_s *apb);
static int  pwm_audio_cancel(FAR struct audio_lowerhalf_s *dev,
                             FAR struct ap_buffer_s *apb);
static int  pwm_audio_ioctl(FAR struct audio_lowerhalf_s *dev, int cmd,
                            unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct audio_ops_s g_pwm_audio_ops =
{
  pwm_audio_getcaps,
  pwm_audio_configure,
  pwm_audio_shutdown,
  pwm_audio_start,
  pwm_audio_stop,
  pwm_audio_pause,
  pwm_audio_resume,
  NULL,                     /* allocbuffer:  the upper half's will do */
  NULL,                     /* freebuffer */
  pwm_audio_enqueue,
  pwm_audio_cancel,
  pwm_audio_ioctl,
  NULL,                     /* read */
  NULL,                     /* write */
  pwm_audio_reserve,
  pwm_audio_release
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_RP23XX_PWM_AUDIO_STATS

/****************************************************************************
 * Name: pwm_audio_now
 *
 * Description:
 *   The always-on microsecond counter, read without side effects.
 *
 *   TIMERAWL is the unlatched low word:  reading it does not latch the high
 *   word the way TIMELR does, so it is safe from an interrupt and from a
 *   thread at the same time.  It wraps every 71 minutes, which unsigned
 *   subtraction of two readings survives as long as they are closer
 *   together than that.
 *
 ****************************************************************************/

static inline uint32_t pwm_audio_now(void)
{
  return getreg32(RP23XX_TIMER0_BASE + RP23XX_TIMER_TIMERAWL_OFFSET);
}

/****************************************************************************
 * Name: pwm_audio_statsreset
 *
 * Description:
 *   Begin a fresh measurement.  Called from start(), where the sample rate
 *   is finally known.
 *
 ****************************************************************************/

static void pwm_audio_statsreset(FAR struct pwm_audio_dev_s *priv)
{
  memset(&g_pwm_audio_stats, 0, sizeof(g_pwm_audio_stats));

  g_pwm_audio_stats.magic     = PWM_AUDIO_STATS_MAGIC;
  g_pwm_audio_stats.samprate  = priv->samprate;
  g_pwm_audio_stats.nchannels = priv->nchannels;
  g_pwm_audio_stats.bufframes = PWM_AUDIO_NSAMPLES;

  g_pwm_audio_stats.occmin    = UINT32_MAX;

  /* priv->pendframes is deliberately left alone.  A player enqueues before
   * it starts -- that is the whole point of queuing ahead -- so zeroing it
   * here would discard exactly the head start being measured.  It is
   * maintained by enqueue, fill and cancel, and cleared by stop.
   */

  priv->donetime  = 0;
  priv->donevalid = false;
  priv->doneready = false;
  priv->draining  = false;
}

/****************************************************************************
 * Name: pwm_audio_statsdone
 *
 * Description:
 *   A buffer has just finished.  Record how much audio was still unplayed
 *   at that instant and start the clock on whatever comes next.
 *
 *   Runs in interrupt context, so it does nothing but read a few words and
 *   subtract.  The values it reads are single aligned words written by the
 *   thread, so a torn read is not possible; a stale one is, and does not
 *   matter for a low-water mark.
 *
 ****************************************************************************/

static void pwm_audio_statsdone(FAR struct pwm_audio_dev_s *priv)
{
  uint32_t occ = priv->pendframes;
  unsigned int i;

  g_pwm_audio_stats.completions++;

  /* Once the producer has handed over its last buffer the queue is meant to
   * empty:  occupancy falls to zero and the final completion finds nothing
   * to follow it.  That is the stream ending rather than the producer
   * failing, and counting it makes the low-water mark read zero for every
   * run that plays to the end -- which is to say, useless.
   */

  if (priv->draining)
    {
      priv->donevalid = false;
      return;
    }

  g_pwm_audio_stats.measured++;

  /* Occupancy is everything the DMA could still play without the producer
   * doing anything more:  converted frames waiting in the other buffers,
   * plus the frames queued behind them.  The buffer that just ended holds
   * nothing by definition, which is what makes this the margin rather than
   * a snapshot -- when it reaches zero the output has stopped.
   */

  /* Whether a converted buffer is ready right now is also what separates
   * the driver's own gap -- the thread being woken and rearming the DMA --
   * from the producer having failed to keep up.  They need different fixes
   * and they are indistinguishable once averaged together.
   */

  priv->doneready = false;

  for (i = 0; i < PWM_AUDIO_NBUFFERS; i++)
    {
      if (i != priv->playing && priv->buf[i].state == PWM_AUDIO_BUF_READY)
        {
          occ += priv->buf[i].nsamples;
          priv->doneready = true;
        }
    }

  g_pwm_audio_stats.occlast = occ;

  if (occ < g_pwm_audio_stats.occmin)
    {
      g_pwm_audio_stats.occmin = occ;
    }

  if (!priv->doneready)
    {
      g_pwm_audio_stats.starved++;
    }

  /* A separate flag rather than a reserved value.  Forcing the timestamp
   * away from zero -- with "| 1" -- rounds it *up*, so a rearm that takes
   * no whole microsecond subtracts to -1 and reports 4294967295 us.
   */

  priv->donetime  = pwm_audio_now();
  priv->donevalid = true;
}

/****************************************************************************
 * Name: pwm_audio_statsstart
 *
 * Description:
 *   The DMA has been pointed at the next buffer.  Close out the interval
 *   that pwm_audio_statsdone() opened.
 *
 ****************************************************************************/

static void pwm_audio_statsstart(FAR struct pwm_audio_dev_s *priv)
{
  uint32_t elapsed;

  if (!priv->donevalid)
    {
      /* The first buffer of a stream follows no completion */

      return;
    }

  elapsed = pwm_audio_now() - priv->donetime;
  priv->donevalid = false;

  if (priv->doneready)
    {
      g_pwm_audio_stats.refilln++;
      g_pwm_audio_stats.refillsum += elapsed;

      if (elapsed > g_pwm_audio_stats.refillmax)
        {
          g_pwm_audio_stats.refillmax = elapsed;
        }
    }
  else
    {
      g_pwm_audio_stats.starvesum += elapsed;

      if (elapsed > g_pwm_audio_stats.starvemax)
        {
          g_pwm_audio_stats.starvemax = elapsed;
        }
    }
}

/****************************************************************************
 * Name: pwm_audio_statsfill
 *
 * Description:
 *   Record what one call to pwm_audio_fill() cost.  This is the only CPU
 *   work in the audio path that scales with the sample rate, and it has
 *   never been measured.
 *
 ****************************************************************************/

static void pwm_audio_statsfill(uint32_t elapsed, unsigned int frames)
{
  if (frames == 0)
    {
      return;
    }

  g_pwm_audio_stats.filln++;
  g_pwm_audio_stats.fillsum    += elapsed;
  g_pwm_audio_stats.fillframes += frames;

  if (elapsed > g_pwm_audio_stats.fillmax)
    {
      g_pwm_audio_stats.fillmax = elapsed;
    }
}

/****************************************************************************
 * Name: pwm_audio_statsdump
 *
 * Description:
 *   Write the measurement to the syslog.  Everything is integer arithmetic
 *   because a floating point printf is not configured in here.
 *
 ****************************************************************************/

static void pwm_audio_statsdump(void)
{
  FAR struct pwm_audio_stats_s *s = &g_pwm_audio_stats;
  uint32_t rate = s->samprate ? s->samprate : 1;

  if (s->completions == 0)
    {
      return;
    }

  syslog(LOG_INFO,
         "pwm_audio: %" PRIu32 " buffers of %" PRIu32 " frames "
         "(%" PRIu32 " ms each) at %" PRIu32 " Hz, %" PRIu32 " measured\n",
         s->completions, s->bufframes, s->bufframes * 1000 / rate, rate,
         s->measured);

  if (s->measured == 0)
    {
      syslog(LOG_INFO, "pwm_audio: too short to measure occupancy\n");
    }
  else
    {
      syslog(LOG_INFO,
             "pwm_audio: occupancy low-water %" PRIu32 " frames = %" PRIu32
             " ms, last %" PRIu32 "\n",
             s->occmin, s->occmin * 1000 / rate, s->occlast);
    }

  syslog(LOG_INFO,
         "pwm_audio: refill n=%" PRIu32 " max %" PRIu32 " us mean %" PRIu32
         " us (of %" PRIu32 " ms available)\n",
         s->refilln, s->refillmax,
         s->refilln ? s->refillsum / s->refilln : 0,
         s->bufframes * 1000 / rate);

  syslog(LOG_INFO,
         "pwm_audio: starve n=%" PRIu32 " max %" PRIu32 " us mean %" PRIu32
         " us\n",
         s->starved, s->starvemax,
         s->starved ? s->starvesum / s->starved : 0);

  syslog(LOG_INFO,
         "pwm_audio: convert n=%" PRIu32 " max %" PRIu32 " us mean %" PRIu32
         " us, %" PRIu32 " ns/frame\n",
         s->filln, s->fillmax, s->filln ? s->fillsum / s->filln : 0,
         s->fillframes ? s->fillsum * 1000 / s->fillframes : 0);
}

#else
#  define pwm_audio_statsreset(p)
#  define pwm_audio_statsdone(p)
#  define pwm_audio_statsstart(p)
#  define pwm_audio_statsfill(e, f)
#  define pwm_audio_statsdump()
#endif /* CONFIG_RP23XX_PWM_AUDIO_STATS */

/****************************************************************************
 * Name: pwm_audio_pacing
 *
 * Description:
 *   Program a DMA pacing timer to request one transfer per sample.
 *
 *   The timer asserts at (X / Y) * sys_clk with X and Y each 16 bits, so the
 *   rate has to be expressed as a fraction in that range.  The obvious
 *   reduction does not always fit -- 44100/150000000 reduces to 147/500000,
 *   and 500000 is not a 16-bit number -- so the best representable fraction
 *   is searched for instead.  The search is cheap and only runs when the
 *   rate changes.
 *
 * Input Parameters:
 *   timer - Which pacing timer, 0 to 3
 *   rate  - Wanted sample rate in Hz
 *
 * Returned Value:
 *   The rate actually programmed, or zero if none could be represented.
 *
 ****************************************************************************/

static uint32_t pwm_audio_pacing(unsigned int timer, uint32_t rate)
{
  uint64_t sysclk = BOARD_SYS_FREQ;
  uint32_t bestx  = 0;
  uint32_t besty  = 0;
  uint64_t besterr = UINT64_MAX;
  uint32_t x;

  /* For each numerator, the denominator that comes closest is a rounded
   * division.  Sweeping X and keeping the best pair beats reducing the
   * fraction, which can leave Y too large to program.
   */

  for (x = 1; x <= 0xffff; x++)
    {
      uint64_t y = ((uint64_t)x * sysclk + rate / 2) / rate;
      uint64_t got;
      uint64_t err;

      if (y == 0)
        {
          continue;
        }

      if (y > 0xffff)
        {
          /* Larger numerators only need larger denominators from here on */

          break;
        }

      got = ((uint64_t)x * sysclk) / y;
      err = got > rate ? got - rate : rate - got;

      if (err < besterr)
        {
          besterr = err;
          bestx   = x;
          besty   = (uint32_t)y;

          if (err == 0)
            {
              break;
            }
        }
    }

  if (bestx == 0)
    {
      return 0;
    }

  putreg32((bestx << RP23XX_DMA_TIMER0_X_SHIFT) | besty,
           RP23XX_DMA_TIMER0 + timer * 4);

  return (uint32_t)(((uint64_t)bestx * sysclk) / besty);
}

/****************************************************************************
 * Name: pwm_audio_setcc
 *
 * Description:
 *   Write both channels of the slice at once.  One register, one write:
 *   channel A in the low half, channel B in the high half.
 *
 ****************************************************************************/

static inline void pwm_audio_setcc(FAR struct pwm_audio_dev_s *priv,
                                   uint32_t value)
{
  putreg32(value, priv->ccaddr);
}

/****************************************************************************
 * Name: pwm_audio_pack
 *
 * Description:
 *   Turn one signed sample pair into the word the compare register wants.
 *
 ****************************************************************************/

static inline uint32_t pwm_audio_pack(FAR struct pwm_audio_dev_s *priv,
                                      int32_t left, int32_t right)
{
  int32_t a;
  int32_t b;

  /* Volume is a plain multiply, and it costs resolution -- there are only
   * ten bits to begin with.  The board has a potentiometer for coarse
   * control, so this is meant for trimming rather than for turning down.
   */

  if (priv->volume != 1000)
    {
      left  = (left  * (int32_t)priv->volume) / 1000;
      right = (right * (int32_t)priv->volume) / 1000;
    }

  a = PWM_AUDIO_MID + (left  >> PWM_AUDIO_SHIFT);
  b = PWM_AUDIO_MID + (right >> PWM_AUDIO_SHIFT);

  if (a < 0)
    {
      a = 0;
    }
  else if (a > PWM_AUDIO_TOP)
    {
      a = PWM_AUDIO_TOP;
    }

  if (b < 0)
    {
      b = 0;
    }
  else if (b > PWM_AUDIO_TOP)
    {
      b = PWM_AUDIO_TOP;
    }

  return (uint32_t)a | ((uint32_t)b << 16);
}

/****************************************************************************
 * Name: pwm_audio_giveback
 *
 * Description:
 *   Return spent buffers to the upper half.
 *
 *   MUST be called with priv->lock released.  The upper half's callback
 *   runs a long way -- it posts to the client's message queue and can
 *   re-enter this driver -- and calling it from under the lock is how a
 *   driver deadlocks against its own client.
 *
 ****************************************************************************/

static void pwm_audio_giveback(FAR struct pwm_audio_dev_s *priv,
                               FAR struct dq_queue_s *doneq)
{
  FAR struct ap_buffer_s *apb;

  while ((apb = (FAR struct ap_buffer_s *)dq_remfirst(doneq)) != NULL)
    {
#ifdef CONFIG_AUDIO_MULTI_SESSION
      priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK, NULL);
#else
      priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK);
#endif
    }
}

/****************************************************************************
 * Name: pwm_audio_fill
 *
 * Description:
 *   Convert queued audio into one buffer of compare values, returning
 *   buffers to the upper half as they are used up.
 *
 *   Called with priv->lock held.
 *
 * Returned Value:
 *   The number of samples placed in the buffer, which is zero when there is
 *   nothing queued.
 *
 ****************************************************************************/

static unsigned int pwm_audio_fill(FAR struct pwm_audio_dev_s *priv,
                                   FAR struct pwm_audio_buf_s *buf,
                                   FAR struct dq_queue_s *doneq)
{
  unsigned int nsamples = 0;
  unsigned int framesz;

  framesz = priv->nchannels * sizeof(int16_t);

  while (nsamples < PWM_AUDIO_NSAMPLES)
    {
      FAR struct ap_buffer_s *apb;
      FAR const int16_t *pcm;
      unsigned int avail;

      apb = (FAR struct ap_buffer_s *)dq_peek(&priv->pendq);
      if (apb == NULL)
        {
          break;
        }

      avail = (apb->nbytes - apb->curbyte) / framesz;
      if (avail == 0)
        {
          /* Spent.  Hand it back and take the next one. */

          dq_remfirst(&priv->pendq);

          /* Do not hand it back from here.  The upper half is called with
           * priv->lock released -- see pwm_audio_giveback().
           */

          dq_addlast(&apb->dq_entry, doneq);
          continue;
        }

      if (avail > PWM_AUDIO_NSAMPLES - nsamples)
        {
          avail = PWM_AUDIO_NSAMPLES - nsamples;
        }

      pcm = (FAR const int16_t *)(apb->samp + apb->curbyte);

      if (priv->nchannels == 2)
        {
          unsigned int i;

          for (i = 0; i < avail; i++)
            {
              buf->samples[nsamples + i] =
                pwm_audio_pack(priv, pcm[2 * i], pcm[2 * i + 1]);
            }
        }
      else
        {
          unsigned int i;

          /* Mono goes to both channels, so a single speaker is not silent */

          for (i = 0; i < avail; i++)
            {
              buf->samples[nsamples + i] =
                pwm_audio_pack(priv, pcm[i], pcm[i]);
            }
        }

      nsamples      += avail;
      apb->curbyte  += avail * framesz;

#ifdef CONFIG_RP23XX_PWM_AUDIO_STATS
      priv->pendframes -= avail;
#endif
    }

  buf->nsamples = nsamples;
  return nsamples;
}

static void pwm_audio_dmacallback(DMA_HANDLE handle, uint8_t status,
                                  FAR void *arg);

/****************************************************************************
 * Name: pwm_audio_dmactrl
 *
 * Description:
 *   The control word for one half's channel.
 *
 *   CHAIN_TO names the *other* channel, which is the whole mechanism:  when
 *   this one runs out, the hardware starts that one immediately.
 *
 ****************************************************************************/

static uint32_t pwm_audio_dmactrl(FAR struct pwm_audio_dev_s *priv,
                                  unsigned int index)
{
  unsigned int other = index ^ 1;

  /* INCR_WRITE stays clear:  the compare register is a single address.
   * INCR_READ must be set, or the DMA re-reads one sample forever, which
   * is a constant duty cycle -- DC, and therefore silence.
   */

  return RP23XX_DMA_CTRL_TRIG_EN |
         RP23XX_DMA_CTRL_TRIG_DATA_SIZE_SIZE_WORD |
         RP23XX_DMA_CTRL_TRIG_INCR_READ |
         (priv->dmach[other] << RP23XX_DMA_CTRL_TRIG_CHAIN_TO_SHIFT) |
         (RP23XX_DMA_TREQ_TIMER(CONFIG_RP23XX_PWM_AUDIO_DMA_TIMER) <<
          RP23XX_DMA_CTRL_TRIG_TREQ_SEL_SHIFT);
}

/****************************************************************************
 * Name: pwm_audio_arm
 *
 * Description:
 *   Load one half's channel and leave it waiting to be chained to.
 *
 *   The control word goes to AL1_CTRL rather than CTRL_TRIG.  Writing
 *   CTRL_TRIG would start the transfer there and then; writing the alias
 *   arms the channel and leaves the other channel's CHAIN_TO as the only
 *   thing that can start it.  That is what keeps the two halves in step
 *   instead of both running at once.
 *
 *   Always the full half:  a partly filled half would leave the chain with
 *   a shorter transfer than the audio it represents.  The filling thread
 *   pads with mid-scale instead.
 *
 ****************************************************************************/

static void pwm_audio_arm(FAR struct pwm_audio_dev_s *priv,
                          unsigned int index)
{
  unsigned int ch = priv->dmach[index];

  putreg32((uintptr_t)priv->buf[index].samples, RP23XX_DMA_READ_ADDR(ch));
  putreg32(priv->ccaddr, RP23XX_DMA_WRITE_ADDR(ch));
  putreg32(PWM_AUDIO_NSAMPLES, RP23XX_DMA_TRANS_COUNT(ch));
  putreg32(pwm_audio_dmactrl(priv, index), RP23XX_DMA_AL1_CTRL(ch));

  /* The shared interrupt handler takes the callback away on every call, so
   * it has to be put back each time.  rp23xx_dmastart() would do that but
   * would also trigger the channel, which is exactly what must not happen
   * here.
   */

  rp23xx_dmacallback(priv->dma[index], pwm_audio_dmacallback, priv);
}

/****************************************************************************
 * Name: pwm_audio_dmacallback
 *
 * Description:
 *   One half has been played and the other is already running, started by
 *   the hardware.  Re-arm this one straight away so the chain always has a
 *   loaded target, and wake the thread to put fresh audio in it.
 *
 *   Re-arming from here is safe now that rp23xx_dmac_interrupt() clears the
 *   callback *before* invoking it.  It is also necessary:  a channel left
 *   with a zero transfer count would be chained to anyway, and would
 *   complete instantly and chain onwards, which is an interrupt storm
 *   rather than a silence.
 *
 *   The half is not being read while this runs -- the other half is -- so
 *   the thread has a whole half-period to refill it.
 *
 ****************************************************************************/

static void pwm_audio_dmacallback(DMA_HANDLE handle, uint8_t status,
                                  FAR void *arg)
{
  FAR struct pwm_audio_dev_s *priv = (FAR struct pwm_audio_dev_s *)arg;
  unsigned int index;

  if (!priv->streaming)
    {
      return;
    }

  index = (handle == priv->dma[0]) ? 0 : 1;

  /* Measured while priv->playing still names the half that just ended */

  priv->playing = index;
  pwm_audio_statsdone(priv);

  priv->buf[index].state    = PWM_AUDIO_BUF_FREE;
  priv->buf[index].nsamples = 0;

  pwm_audio_arm(priv, index);

  nxsem_post(&priv->wake);
}

/****************************************************************************
 * Name: pwm_audio_dmago
 *
 * Description:
 *   Arm both halves and set the pair running.  Only half zero is triggered;
 *   from then on each half is started by the other finishing.
 *
 ****************************************************************************/

static void pwm_audio_dmago(FAR struct pwm_audio_dev_s *priv)
{
  unsigned int i;

  for (i = 0; i < PWM_AUDIO_NBUFFERS; i++)
    {
      pwm_audio_arm(priv, i);
    }

  priv->playing = 0;

  /* MULTI_CHAN_TRIGGER starts a channel without rewriting its control
   * word, which matters because writing CTRL_TRIG here would also be the
   * moment the chain configuration took effect.
   */

  putreg32(1 << priv->dmach[0], RP23XX_DMA_MULTI_CHAN_TRIGGER);
}

/****************************************************************************
 * Name: pwm_audio_dmahalt
 ****************************************************************************/

static void pwm_audio_dmahalt(FAR struct pwm_audio_dev_s *priv)
{
  unsigned int i;

  for (i = 0; i < PWM_AUDIO_NBUFFERS; i++)
    {
      /* Point each channel's CHAIN_TO at itself and clear EN, both through
       * the non-trigger alias, before aborting anything.
       *
       * Aborting a channel still chained to the other one triggers that
       * other one on the way out, which restarts the pair -- so the halt
       * has to break the ring first.  CHAIN_TO disabled means "equal to
       * this channel", not zero:  writing a plain 0 would leave channel
       * one chained to channel zero.
       */

      putreg32(priv->dmach[i] << RP23XX_DMA_CTRL_TRIG_CHAIN_TO_SHIFT,
               RP23XX_DMA_AL1_CTRL(priv->dmach[i]));
    }

  for (i = 0; i < PWM_AUDIO_NBUFFERS; i++)
    {
      rp23xx_dmastop(priv->dma[i]);
    }
}

/****************************************************************************
 * Name: pwm_audio_worker
 *
 * Description:
 *   Convert queued audio into whichever buffer is free, and get the DMA
 *   going again if it ran dry.
 *
 ****************************************************************************/

static int pwm_audio_worker(int argc, FAR char *argv[])
{
  FAR struct pwm_audio_dev_s *priv;
  unsigned int i;

  priv = (FAR struct pwm_audio_dev_s *)
         ((uintptr_t)strtoul(argv[1], NULL, 16));

  while (!priv->terminate)
    {
      struct dq_queue_s doneq;
      bool complete = false;

      nxsem_wait_uninterruptible(&priv->wake);

      dq_init(&doneq);
      nxmutex_lock(&priv->lock);

      if (!priv->streaming)
        {
          nxmutex_unlock(&priv->lock);
          continue;
        }

      for (i = 0; i < PWM_AUDIO_NBUFFERS; i++)
        {
          unsigned int nfilled;
          unsigned int k;
#ifdef CONFIG_RP23XX_PWM_AUDIO_STATS
          uint32_t started;
#endif

          if (priv->buf[i].state != PWM_AUDIO_BUF_FREE)
            {
              continue;
            }

#ifdef CONFIG_RP23XX_PWM_AUDIO_STATS
          started = pwm_audio_now();
#endif

          nfilled = pwm_audio_fill(priv, &priv->buf[i], &doneq);

          /* Whatever is not audio has to be silence.
           *
           * The DMA plays this half either way -- it never stops between
           * start and stop -- so a short fill would otherwise replay the
           * tail of what was there before, which is a periodic buzz and a
           * good deal worse than a dropout.  Silence is mid-scale, not
           * zero:  the output is AC coupled.
           */

          for (k = nfilled; k < PWM_AUDIO_NSAMPLES; k++)
            {
              priv->buf[i].samples[k] = PWM_AUDIO_MIDPAIR;
            }

#ifdef CONFIG_RP23XX_PWM_AUDIO_STATS
          pwm_audio_statsfill(pwm_audio_now() - started, nfilled);
#endif

          priv->buf[i].state = PWM_AUDIO_BUF_READY;

          /* The half is loaded and the chain will reach it on its own.
           * This is where the refill deadline was met, so it is where the
           * margin against the half-period is worth measuring.
           */

          pwm_audio_statsstart(priv);

          if (nfilled == 0 && dq_empty(&priv->pendq) && !priv->completed)
            {
              /* Out of audio:  the stream has ended.  The DMA keeps
               * running on silence -- stopping it is the client's call,
               * and a sink that shuts down between tracks would click.
               */

              priv->completed = true;
              complete = true;
            }
        }

      nxmutex_unlock(&priv->lock);

      pwm_audio_giveback(priv, &doneq);

      if (complete)
        {
#ifdef CONFIG_AUDIO_MULTI_SESSION
          priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_COMPLETE,
                          NULL, OK, NULL);
#else
          priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_COMPLETE,
                          NULL, OK);
#endif
        }
    }

  return 0;
}

/****************************************************************************
 * Name: pwm_audio_hwstart
 *
 * Description:
 *   Bring the slice up at the carrier frequency, idling at mid-scale.
 *
 ****************************************************************************/

static void pwm_audio_hwstart(FAR struct pwm_audio_dev_s *priv)
{
  /* Divisor of 1.0:  the carrier is sys_clk / (TOP + 1) */

  putreg32(1 << 4, RP23XX_PWM_DIV(priv->slice));
  putreg32(PWM_AUDIO_TOP, RP23XX_PWM_TOP(priv->slice));

  pwm_audio_setcc(priv, (PWM_AUDIO_MID << 16) | PWM_AUDIO_MID);

  rp23xx_gpio_set_function(priv->pin_a, RP23XX_GPIO_FUNC_PWM);
  if (priv->pin_b >= 0)
    {
      rp23xx_gpio_set_function(priv->pin_b, RP23XX_GPIO_FUNC_PWM);
    }

  putreg32(RP23XX_PWM_CSR_EN, RP23XX_PWM_CSR(priv->slice));
}

/****************************************************************************
 * Name: pwm_audio_hwstop
 ****************************************************************************/

static void pwm_audio_hwstop(FAR struct pwm_audio_dev_s *priv)
{
  pwm_audio_setcc(priv, (PWM_AUDIO_MID << 16) | PWM_AUDIO_MID);
  putreg32(0, RP23XX_PWM_CSR(priv->slice));
}

/****************************************************************************
 * Name: pwm_audio_getcaps
 ****************************************************************************/

static int pwm_audio_getcaps(FAR struct audio_lowerhalf_s *dev, int type,
                             FAR struct audio_caps_s *caps)
{
  audinfo("type=%d ac_type=%d\n", type, caps->ac_type);

  caps->ac_format.hw  = 0;
  caps->ac_controls.w = 0;

  switch (caps->ac_type)
    {
      case AUDIO_TYPE_QUERY:
        {
          caps->ac_channels = 2;

          switch (caps->ac_subtype)
            {
              case AUDIO_TYPE_QUERY:
                caps->ac_controls.b[0] = AUDIO_TYPE_OUTPUT;
                caps->ac_format.hw     = 1 << (AUDIO_FMT_PCM - 1);
                break;

              default:
                caps->ac_controls.b[0] = AUDIO_SUBFMT_END;
                break;
            }
        }
        break;

      case AUDIO_TYPE_OUTPUT:
        {
          caps->ac_channels = 2;

          switch (caps->ac_subtype)
            {
              case AUDIO_TYPE_QUERY:

                /* Any rate the pacing timer can express, which is all of
                 * them to well under a tenth of a percent.
                 */

                caps->ac_controls.hw[0] = AUDIO_SAMP_RATE_8K |
                                          AUDIO_SAMP_RATE_11K |
                                          AUDIO_SAMP_RATE_16K |
                                          AUDIO_SAMP_RATE_22K |
                                          AUDIO_SAMP_RATE_32K |
                                          AUDIO_SAMP_RATE_44K |
                                          AUDIO_SAMP_RATE_48K;
                caps->ac_controls.b[2]  = 16;
                break;

              default:
                break;
            }
        }
        break;

      case AUDIO_TYPE_FEATURE:
        {
          if (caps->ac_subtype == AUDIO_TYPE_QUERY)
            {
              caps->ac_controls.b[0] = AUDIO_FU_VOLUME;
            }
        }
        break;

      default:
        caps->ac_subtype = 0;
        caps->ac_channels = 0;
        break;
    }

  return caps->ac_len;
}

/****************************************************************************
 * Name: pwm_audio_configure
 ****************************************************************************/

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int pwm_audio_configure(FAR struct audio_lowerhalf_s *dev,
                               FAR void *session,
                               FAR const struct audio_caps_s *caps)
#else
static int pwm_audio_configure(FAR struct audio_lowerhalf_s *dev,
                               FAR const struct audio_caps_s *caps)
#endif
{
  FAR struct pwm_audio_dev_s *priv = (FAR struct pwm_audio_dev_s *)dev;
  uint32_t got;
  int ret = OK;

  switch (caps->ac_type)
    {
      case AUDIO_TYPE_FEATURE:
        {
          if (caps->ac_format.hw == AUDIO_FU_VOLUME)
            {
              priv->volume = caps->ac_controls.hw[0];
              if (priv->volume > 1000)
                {
                  priv->volume = 1000;
                }
            }
          else
            {
              ret = -ENOTTY;
            }
        }
        break;

      case AUDIO_TYPE_OUTPUT:
        {
          if (caps->ac_controls.b[2] != 16)
            {
              auderr("ERROR: only 16-bit samples are supported, not %d\n",
                     caps->ac_controls.b[2]);
              return -ERANGE;
            }

          if (caps->ac_channels != 1 && caps->ac_channels != 2)
            {
              auderr("ERROR: %d channels\n", caps->ac_channels);
              return -ERANGE;
            }

          priv->samprate  = caps->ac_controls.hw[0];
          priv->nchannels = caps->ac_channels;
          priv->bpsamp    = caps->ac_controls.b[2];

          if (priv->samprate > PWM_AUDIO_MAXRATE ||
              priv->samprate < PWM_AUDIO_MINRATE)
            {
              auderr("ERROR: %" PRIu32 " Hz is outside %d..%d\n",
                     priv->samprate, PWM_AUDIO_MINRATE, PWM_AUDIO_MAXRATE);
              return -ERANGE;
            }

          got = pwm_audio_pacing(CONFIG_RP23XX_PWM_AUDIO_DMA_TIMER,
                                 priv->samprate);
          if (got == 0)
            {
              auderr("ERROR: no pacing fraction for %" PRIu32 " Hz\n",
                     priv->samprate);
              return -ERANGE;
            }

          audinfo("%" PRIu32 " Hz requested, %" PRIu32 " Hz programmed, "
                  "%d channels\n", priv->samprate, got, priv->nchannels);
        }
        break;

      default:
        ret = -ENOTTY;
        break;
    }

  return ret;
}

/****************************************************************************
 * Name: pwm_audio_start
 ****************************************************************************/

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int pwm_audio_start(FAR struct audio_lowerhalf_s *dev,
                           FAR void *session)
#else
static int pwm_audio_start(FAR struct audio_lowerhalf_s *dev)
#endif
{
  FAR struct pwm_audio_dev_s *priv = (FAR struct pwm_audio_dev_s *)dev;
  FAR char *argv[2];
  char arg1[32];
  unsigned int i;
  unsigned int k;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->streaming)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  pwm_audio_hwstart(priv);

  priv->streaming = true;
  priv->completed = false;
  priv->playing   = 0;

  /* Both halves start as silence, so that whatever the first chain reaches
   * is mid-scale rather than the contents of a fresh allocation.
   */

  for (i = 0; i < PWM_AUDIO_NBUFFERS; i++)
    {
      for (k = 0; k < PWM_AUDIO_NSAMPLES; k++)
        {
          priv->buf[i].samples[k] = PWM_AUDIO_MIDPAIR;
        }

      priv->buf[i].nsamples = 0;
      priv->buf[i].state    = PWM_AUDIO_BUF_FREE;
    }

  pwm_audio_statsreset(priv);

  /* Set the pair running now, before anything has been converted.  It
   * plays silence until the thread catches up, which is what a sink does;
   * waiting for audio would put the gap back at the start of every stream.
   */

  pwm_audio_dmago(priv);

  if (priv->threadid <= 0)
    {
      priv->terminate = false;

      snprintf(arg1, sizeof(arg1), "%p", priv);
      argv[0] = arg1;
      argv[1] = NULL;

      ret = kthread_create("pwm_audio", CONFIG_RP23XX_PWM_AUDIO_PRIORITY,
                           CONFIG_RP23XX_PWM_AUDIO_STACKSIZE,
                           pwm_audio_worker, argv);
      if (ret < 0)
        {
          auderr("ERROR: failed to start the filling thread: %d\n", ret);
          priv->streaming = false;
          pwm_audio_hwstop(priv);
          nxmutex_unlock(&priv->lock);
          return ret;
        }

      priv->threadid = ret;
    }

  nxmutex_unlock(&priv->lock);

  /* Let the thread fill and start the first buffer */

  nxsem_post(&priv->wake);
  return OK;
}

/****************************************************************************
 * Name: pwm_audio_stop
 ****************************************************************************/

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int pwm_audio_stop(FAR struct audio_lowerhalf_s *dev,
                          FAR void *session)
#else
static int pwm_audio_stop(FAR struct audio_lowerhalf_s *dev)
#endif
{
  FAR struct pwm_audio_dev_s *priv = (FAR struct pwm_audio_dev_s *)dev;
  FAR struct ap_buffer_s *apb;
  struct dq_queue_s doneq;

  nxmutex_lock(&priv->lock);

  priv->streaming = false;
  pwm_audio_dmahalt(priv);
  pwm_audio_hwstop(priv);

  /* Take anything still queued off the list now, but hand it back after
   * the lock is released -- see pwm_audio_giveback().
   */

  dq_init(&doneq);

  while ((apb = (FAR struct ap_buffer_s *)dq_remfirst(&priv->pendq)) != NULL)
    {
      dq_addlast(&apb->dq_entry, &doneq);
    }

  priv->buf[0].state = PWM_AUDIO_BUF_FREE;
  priv->buf[1].state = PWM_AUDIO_BUF_FREE;

#ifdef CONFIG_RP23XX_PWM_AUDIO_STATS
  priv->pendframes = 0;
#endif

  nxmutex_unlock(&priv->lock);

  pwm_audio_giveback(priv, &doneq);

  pwm_audio_statsdump();
  return OK;
}

/****************************************************************************
 * Name: pwm_audio_pause
 ****************************************************************************/

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int pwm_audio_pause(FAR struct audio_lowerhalf_s *dev,
                           FAR void *session)
#else
static int pwm_audio_pause(FAR struct audio_lowerhalf_s *dev)
#endif
{
  FAR struct pwm_audio_dev_s *priv = (FAR struct pwm_audio_dev_s *)dev;

  nxmutex_lock(&priv->lock);
  pwm_audio_dmahalt(priv);
  pwm_audio_setcc(priv, PWM_AUDIO_MIDPAIR);
  nxmutex_unlock(&priv->lock);

  return OK;
}

/****************************************************************************
 * Name: pwm_audio_resume
 ****************************************************************************/

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int pwm_audio_resume(FAR struct audio_lowerhalf_s *dev,
                            FAR void *session)
#else
static int pwm_audio_resume(FAR struct audio_lowerhalf_s *dev)
#endif
{
  FAR struct pwm_audio_dev_s *priv = (FAR struct pwm_audio_dev_s *)dev;

  nxmutex_lock(&priv->lock);

  if (priv->streaming)
    {
      /* Both channels were disarmed by pause, so the chain has to be set
       * going again rather than merely woken.
       */

      pwm_audio_dmago(priv);
    }

  nxmutex_unlock(&priv->lock);

  nxsem_post(&priv->wake);
  return OK;
}

/****************************************************************************
 * Name: pwm_audio_enqueue
 ****************************************************************************/

static int pwm_audio_enqueue(FAR struct audio_lowerhalf_s *dev,
                             FAR struct ap_buffer_s *apb)
{
  FAR struct pwm_audio_dev_s *priv = (FAR struct pwm_audio_dev_s *)dev;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  apb->curbyte = 0;
  dq_addlast(&apb->dq_entry, &priv->pendq);

#ifdef CONFIG_RP23XX_PWM_AUDIO_STATS
  priv->pendframes += apb->nbytes / (priv->nchannels * sizeof(int16_t));

  if ((apb->flags & AUDIO_APB_FINAL) != 0)
    {
      priv->draining = true;
    }
#endif

  nxmutex_unlock(&priv->lock);

  nxsem_post(&priv->wake);
  return OK;
}

/****************************************************************************
 * Name: pwm_audio_cancel
 ****************************************************************************/

static int pwm_audio_cancel(FAR struct audio_lowerhalf_s *dev,
                            FAR struct ap_buffer_s *apb)
{
  FAR struct pwm_audio_dev_s *priv = (FAR struct pwm_audio_dev_s *)dev;

  nxmutex_lock(&priv->lock);
  dq_rem(&apb->dq_entry, &priv->pendq);

#ifdef CONFIG_RP23XX_PWM_AUDIO_STATS
  priv->pendframes -= (apb->nbytes - apb->curbyte) /
                      (priv->nchannels * sizeof(int16_t));
#endif

  nxmutex_unlock(&priv->lock);

  return OK;
}

/****************************************************************************
 * Name: pwm_audio_ioctl
 ****************************************************************************/

static int pwm_audio_ioctl(FAR struct audio_lowerhalf_s *dev, int cmd,
                           unsigned long arg)
{
  FAR struct ap_buffer_info_s *bufinfo;
  int ret = -ENOTTY;

  switch (cmd)
    {
      /* Report the buffer geometry.
       *
       * Answered whether or not CONFIG_AUDIO_DRIVER_SPECIFIC_BUFFERS is
       * set, and that is not optional:  the upper half takes its idea of
       * how many buffers may exist from the answer to this, and refusing
       * it leaves that count at zero.  Every allocation then succeeds
       * while allocating nothing, and a player reports only that it
       * "could not allocate buffer 0" -- which says nothing about the
       * ioctl that actually declined.
       */

      case AUDIOIOC_GETBUFFERINFO:
        {
          bufinfo              = (FAR struct ap_buffer_info_s *)arg;
          bufinfo->buffer_size = PWM_AUDIO_APBSIZE;
          bufinfo->nbuffers    = PWM_AUDIO_NAPBS;
          ret                  = OK;
        }
        break;

#ifdef CONFIG_RP23XX_PWM_AUDIO_STATS
      /* Let the producer see how the output is coping.
       *
       * Without this the only way to read these counters is a debugger,
       * and halting the CPU stops the refill thread -- which manufactures
       * exactly the underruns the counters are there to report.
       */

      case AUDIOIOC_PWMAUDIOSTATUS:
        {
          FAR struct pwm_audio_status_s *status =
            (FAR struct pwm_audio_status_s *)arg;

          status->samprate  = g_pwm_audio_stats.samprate;
          status->bufframes = g_pwm_audio_stats.bufframes;
          status->buffers   = g_pwm_audio_stats.completions;
          status->underruns = g_pwm_audio_stats.starved;
          status->occupancy = g_pwm_audio_stats.occlast;
          status->occlow    = g_pwm_audio_stats.occmin;
          status->refillmax = g_pwm_audio_stats.refillmax;
          ret               = OK;
        }
        break;
#endif

      default:
        break;
    }

  return ret;
}

/****************************************************************************
 * Name: pwm_audio_shutdown
 ****************************************************************************/

static int pwm_audio_shutdown(FAR struct audio_lowerhalf_s *dev)
{
  FAR struct pwm_audio_dev_s *priv = (FAR struct pwm_audio_dev_s *)dev;

#ifdef CONFIG_AUDIO_MULTI_SESSION
  pwm_audio_stop(dev, NULL);
#else
  pwm_audio_stop(dev);
#endif

  if (priv->threadid > 0)
    {
      priv->terminate = true;
      nxsem_post(&priv->wake);
      priv->threadid = 0;
    }

  return OK;
}

/****************************************************************************
 * Name: pwm_audio_reserve
 *
 * Description:
 *   Claim the slice.  This is where the plain PWM device and this one are
 *   kept apart:  they are wired to the same slice, which is what lets both
 *   channels share a counter, and only one of them may program it.
 *
 ****************************************************************************/

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int pwm_audio_reserve(FAR struct audio_lowerhalf_s *dev,
                             FAR void **session)
#else
static int pwm_audio_reserve(FAR struct audio_lowerhalf_s *dev)
#endif
{
  FAR struct pwm_audio_dev_s *priv = (FAR struct pwm_audio_dev_s *)dev;
  unsigned int i;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->claimed)
    {
      nxmutex_unlock(&priv->lock);
      return -EBUSY;
    }

  ret = rp23xx_pwm_claim(priv->slice, "pwm_audio");
  if (ret < 0)
    {
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  /* Two channels, and both are needed:  one alone cannot be chained to
   * anything and the output stops between halves.
   */

  for (i = 0; i < PWM_AUDIO_NBUFFERS; i++)
    {
      priv->dma[i] = rp23xx_dmachannel();
      if (priv->dma[i] == NULL)
        {
          while (i-- > 0)
            {
              rp23xx_dmafree(priv->dma[i]);
              priv->dma[i] = NULL;
            }

          rp23xx_pwm_release(priv->slice);
          nxmutex_unlock(&priv->lock);
          return -EBUSY;
        }

      /* CHAIN_TO takes a channel number, and the allocator only hands back
       * an opaque handle.  The register base recovers it.
       */

      priv->dmach[i] = (rp23xx_dma_register(priv->dma[i], 0) -
                        RP23XX_DMA_BASE) / 0x40;
    }

  priv->claimed = true;

#ifdef CONFIG_AUDIO_MULTI_SESSION
  if (session != NULL)
    {
      *session = NULL;
    }
#endif

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: pwm_audio_release
 ****************************************************************************/

#ifdef CONFIG_AUDIO_MULTI_SESSION
static int pwm_audio_release(FAR struct audio_lowerhalf_s *dev,
                             FAR void *session)
#else
static int pwm_audio_release(FAR struct audio_lowerhalf_s *dev)
#endif
{
  FAR struct pwm_audio_dev_s *priv = (FAR struct pwm_audio_dev_s *)dev;
  unsigned int i;

  pwm_audio_shutdown(dev);

  nxmutex_lock(&priv->lock);

  if (priv->claimed)
    {
      for (i = 0; i < PWM_AUDIO_NBUFFERS; i++)
        {
          if (priv->dma[i] != NULL)
            {
              rp23xx_dmafree(priv->dma[i]);
              priv->dma[i] = NULL;
            }
        }

      rp23xx_pwm_release(priv->slice);
      priv->claimed = false;
    }

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rp23xx_pwm_audio_initialize
 ****************************************************************************/

FAR struct audio_lowerhalf_s *
rp23xx_pwm_audio_initialize(unsigned int slice, int pin_a, int pin_b)
{
  FAR struct pwm_audio_dev_s *priv;
  unsigned int i;

  priv = kmm_zalloc(sizeof(struct pwm_audio_dev_s));
  if (priv == NULL)
    {
      auderr("ERROR: cannot allocate the device\n");
      return NULL;
    }

  priv->dev.ops = &g_pwm_audio_ops;
  priv->slice   = slice;
  priv->pin_a   = pin_a;
  priv->pin_b   = pin_b;
  priv->ccaddr  = RP23XX_PWM_CC(slice);
  priv->volume  = 1000;
  priv->samprate  = 44100;
  priv->nchannels = 2;
  priv->bpsamp    = 16;

  nxmutex_init(&priv->lock);
  nxsem_init(&priv->wake, 0, 0);
  dq_init(&priv->pendq);

  /* One allocation for both halves, so that the two DMA channels walk a
   * single contiguous region and the play position is meaningful across
   * the pair rather than only within a half.
   */

  priv->ring = kmm_malloc(PWM_AUDIO_NBUFFERS * PWM_AUDIO_NSAMPLES *
                          sizeof(uint32_t));
  if (priv->ring == NULL)
    {
      auderr("ERROR: cannot allocate the sample buffers\n");
      nxmutex_destroy(&priv->lock);
      nxsem_destroy(&priv->wake);
      kmm_free(priv);
      return NULL;
    }

  for (i = 0; i < PWM_AUDIO_NBUFFERS; i++)
    {
      priv->buf[i].samples = priv->ring + i * PWM_AUDIO_NSAMPLES;
      priv->buf[i].state   = PWM_AUDIO_BUF_FREE;
    }

  return &priv->dev;
}

#endif /* CONFIG_RP23XX_PWM_AUDIO */
