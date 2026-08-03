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
#include <debug.h>

#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/kthread.h>
#include <nuttx/audio/audio.h>

#include <arch/board/board.h>

#include "arm_internal.h"
#include "rp23xx_gpio.h"
#include "rp23xx_dmac.h"
#include "rp23xx_pwm.h"
#include "rp23xx_pwm_audio.h"
#include "hardware/rp23xx_pwm.h"
#include "hardware/rp23xx_dma.h"

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

/* Two buffers of converted samples, so one can be filled while the other is
 * being played.  A single buffer leaves a gap at every boundary.
 */

#define PWM_AUDIO_NBUFFERS  2
#define PWM_AUDIO_NSAMPLES  CONFIG_RP23XX_PWM_AUDIO_BUFSAMPLES

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

  DMA_HANDLE    dma;                      /* Channel feeding the compare */
  uintptr_t     ccaddr;                   /* Address of that compare */

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
  int           threadid;                 /* Filling thread, or 0 */
};

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
                                   FAR struct pwm_audio_buf_s *buf)
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

#ifdef CONFIG_AUDIO_MULTI_SESSION
          priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK,
                          NULL);
#else
          priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK);
#endif
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
    }

  buf->nsamples = nsamples;
  return nsamples;
}

/****************************************************************************
 * Name: pwm_audio_startbuf
 *
 * Description:
 *   Point the DMA at a buffer and let it run.  Called with priv->lock held,
 *   or from the DMA callback where the channel is idle by definition.
 *
 ****************************************************************************/

static void pwm_audio_startbuf(FAR struct pwm_audio_dev_s *priv,
                               unsigned int index);

/****************************************************************************
 * Name: pwm_audio_dmacallback
 *
 * Description:
 *   One buffer has been played.  Hand the other to the DMA if it is ready
 *   and wake the filling thread for the one just released.
 *
 *   This runs in interrupt context, so it does no conversion of its own --
 *   that is the thread's job.
 *
 ****************************************************************************/

static void pwm_audio_dmacallback(DMA_HANDLE handle, uint8_t status,
                                  FAR void *arg)
{
  FAR struct pwm_audio_dev_s *priv = (FAR struct pwm_audio_dev_s *)arg;
  unsigned int next;

  if (!priv->streaming)
    {
      return;
    }

  priv->buf[priv->playing].state    = PWM_AUDIO_BUF_FREE;
  priv->buf[priv->playing].nsamples = 0;

  next = (priv->playing + 1) % PWM_AUDIO_NBUFFERS;

  if (priv->buf[next].state == PWM_AUDIO_BUF_READY)
    {
      pwm_audio_startbuf(priv, next);
    }
  else
    {
      /* Nothing prepared in time.  Hold the last level rather than letting
       * the compare register keep the final sample of the buffer just
       * finished, which would be an arbitrary DC offset.
       */

      pwm_audio_setcc(priv, (PWM_AUDIO_MID << 16) | PWM_AUDIO_MID);
    }

  nxsem_post(&priv->wake);
}

/****************************************************************************
 * Name: pwm_audio_startbuf
 ****************************************************************************/

static void pwm_audio_startbuf(FAR struct pwm_audio_dev_s *priv,
                               unsigned int index)
{
  FAR struct pwm_audio_buf_s *buf = &priv->buf[index];
  dma_config_t config;

  config.dreq   = RP23XX_DMA_TREQ_TIMER(CONFIG_RP23XX_PWM_AUDIO_DMA_TIMER);
  config.size   = RP23XX_DMA_SIZE_WORD;
  config.noincr = true;

  priv->playing = index;
  buf->state    = PWM_AUDIO_BUF_PLAYING;

  rp23xx_txdmasetup(priv->dma, priv->ccaddr, (uintptr_t)buf->samples,
                    buf->nsamples * sizeof(uint32_t), config);
  rp23xx_dmastart(priv->dma, pwm_audio_dmacallback, priv);
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
      nxsem_wait_uninterruptible(&priv->wake);

      nxmutex_lock(&priv->lock);

      if (!priv->streaming)
        {
          nxmutex_unlock(&priv->lock);
          continue;
        }

      for (i = 0; i < PWM_AUDIO_NBUFFERS; i++)
        {
          if (priv->buf[i].state == PWM_AUDIO_BUF_FREE &&
              pwm_audio_fill(priv, &priv->buf[i]) > 0)
            {
              priv->buf[i].state = PWM_AUDIO_BUF_READY;
            }
        }

      /* If nothing is playing, start whatever is ready.  This is both the
       * first start and the recovery from having run dry.
       */

      if (priv->buf[priv->playing].state != PWM_AUDIO_BUF_PLAYING)
        {
          for (i = 0; i < PWM_AUDIO_NBUFFERS; i++)
            {
              if (priv->buf[i].state == PWM_AUDIO_BUF_READY)
                {
                  pwm_audio_startbuf(priv, i);
                  break;
                }
            }

          if (i == PWM_AUDIO_NBUFFERS && dq_empty(&priv->pendq))
            {
              /* Out of audio and out of buffers:  the stream has ended */

#ifdef CONFIG_AUDIO_MULTI_SESSION
              priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_COMPLETE,
                              NULL, OK, NULL);
#else
              priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_COMPLETE,
                              NULL, OK);
#endif
            }
        }

      nxmutex_unlock(&priv->lock);
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
  priv->playing   = 0;
  priv->buf[0].state = PWM_AUDIO_BUF_FREE;
  priv->buf[1].state = PWM_AUDIO_BUF_FREE;

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

  nxmutex_lock(&priv->lock);

  priv->streaming = false;
  rp23xx_dmastop(priv->dma);
  pwm_audio_hwstop(priv);

  /* Give back anything still queued, or the upper half waits forever */

  while ((apb = (FAR struct ap_buffer_s *)dq_remfirst(&priv->pendq)) != NULL)
    {
#ifdef CONFIG_AUDIO_MULTI_SESSION
      priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK, NULL);
#else
      priv->dev.upper(priv->dev.priv, AUDIO_CALLBACK_DEQUEUE, apb, OK);
#endif
    }

  priv->buf[0].state = PWM_AUDIO_BUF_FREE;
  priv->buf[1].state = PWM_AUDIO_BUF_FREE;

  nxmutex_unlock(&priv->lock);
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
  rp23xx_dmastop(priv->dma);
  pwm_audio_setcc(priv, (PWM_AUDIO_MID << 16) | PWM_AUDIO_MID);
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
  nxmutex_unlock(&priv->lock);

  return OK;
}

/****************************************************************************
 * Name: pwm_audio_ioctl
 ****************************************************************************/

static int pwm_audio_ioctl(FAR struct audio_lowerhalf_s *dev, int cmd,
                           unsigned long arg)
{
  int ret = -ENOTTY;

#ifdef CONFIG_AUDIO_DRIVER_SPECIFIC_BUFFERS
  FAR struct ap_buffer_info_s *bufinfo;

  if (cmd == AUDIOIOC_GETBUFFERINFO)
    {
      bufinfo              = (FAR struct ap_buffer_info_s *)arg;
      bufinfo->buffer_size = CONFIG_RP23XX_PWM_AUDIO_BUFSAMPLES * 4;
      bufinfo->nbuffers    = PWM_AUDIO_NBUFFERS;
      ret                  = OK;
    }
#endif

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

  priv->dma = rp23xx_dmachannel();
  if (priv->dma == NULL)
    {
      rp23xx_pwm_release(priv->slice);
      nxmutex_unlock(&priv->lock);
      return -EBUSY;
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

  pwm_audio_shutdown(dev);

  nxmutex_lock(&priv->lock);

  if (priv->claimed)
    {
      if (priv->dma != NULL)
        {
          rp23xx_dmafree(priv->dma);
          priv->dma = NULL;
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

  for (i = 0; i < PWM_AUDIO_NBUFFERS; i++)
    {
      priv->buf[i].samples = kmm_malloc(PWM_AUDIO_NSAMPLES *
                                        sizeof(uint32_t));
      if (priv->buf[i].samples == NULL)
        {
          auderr("ERROR: cannot allocate the sample buffers\n");
          goto errout;
        }

      priv->buf[i].state = PWM_AUDIO_BUF_FREE;
    }

  return &priv->dev;

errout:
  for (i = 0; i < PWM_AUDIO_NBUFFERS; i++)
    {
      kmm_free(priv->buf[i].samples);
    }

  nxmutex_destroy(&priv->lock);
  nxsem_destroy(&priv->wake);
  kmm_free(priv);
  return NULL;
}

#endif /* CONFIG_RP23XX_PWM_AUDIO */
