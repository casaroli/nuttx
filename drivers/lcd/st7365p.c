/****************************************************************************
 * drivers/lcd/st7365p.c
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

/* Sitronix ST7365P, as fitted to the ClockworkPi PicoCalc.
 *
 * The part is close enough to an ILI9488 that ILI9488 initialisation
 * sequences drive it, and two different ones are in production use on the
 * same hardware, so both are provided as a Kconfig choice.  It differs from
 * a true ILI9488 in one way that matters a great deal here: it accepts a
 * 16 bit MCU pixel format over SPI (COLMOD 0x65, 16 bit MCU into an 18 bit
 * panel), which an ILI9488 cannot do.  That halves the pixel traffic.
 *
 * Pixels are pushed with the SPI word size set to 16 bits, so a little
 * endian RGB565 buffer goes out MSB first with no byte swapping anywhere.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/spi/spi.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/st7365p.h>

#ifdef CONFIG_LCD_ST7365P

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Command set */

#define ST7365P_SWRESET     0x01  /* Software reset */
#define ST7365P_SLPIN       0x10  /* Sleep in */
#define ST7365P_SLPOUT      0x11  /* Sleep out */
#define ST7365P_INVOFF      0x20  /* Display inversion off */
#define ST7365P_INVON       0x21  /* Display inversion on */
#define ST7365P_DISPOFF     0x28  /* Display off */
#define ST7365P_DISPON      0x29  /* Display on */
#define ST7365P_CASET       0x2a  /* Column address set */
#define ST7365P_RASET       0x2b  /* Row (page) address set */
#define ST7365P_RAMWR       0x2c  /* Memory write */
#define ST7365P_RAMRD       0x2e  /* Memory read */
#define ST7365P_VSCRDEF     0x33  /* Vertical scrolling definition */
#define ST7365P_TEON        0x35  /* Tearing effect line on */
#define ST7365P_MADCTL      0x36  /* Memory access control */
#define ST7365P_VSCRSADD    0x37  /* Vertical scrolling start address */
#define ST7365P_COLMOD      0x3a  /* Interface pixel format */
#define ST7365P_CSCON       0xf0  /* Command set control */

/* Geometry.
 *
 * The controller drives 480 rows; the PicoCalc's panel shows 320 of them.
 * The unused rows are what makes hardware scrolling possible.
 */

#define ST7365P_XRES       CONFIG_LCD_ST7365P_XRES
#define ST7365P_YRES       CONFIG_LCD_ST7365P_YRES
#define ST7365P_GRAM_YRES  CONFIG_LCD_ST7365P_GRAM_YRES

#define ST7365P_BYTESPP    2
#define ST7365P_SPI_BITS   8
#define ST7365P_COLMOD_VAL 0x65   /* 16 bit MCU, 18 bit panel */

#define ST7365P_XYRES      (ST7365P_XRES * ST7365P_YRES)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One entry of an initialisation table.  A zero command terminates it. */

struct st7365p_cmd_s
{
  uint8_t cmd;                 /* Command byte */
  uint8_t len;                 /* Number of parameter bytes that follow */
  uint8_t delay_ms;            /* Delay to observe after the command */
  const uint8_t *data;         /* Parameters, or NULL */
};

struct st7365p_dev_s
{
  struct lcd_dev_s dev;        /* Publicly visible device structure */
  FAR struct spi_dev_s *spi;   /* The SPI device the panel is wired to */
  uint8_t power;               /* Current power setting */
  uint16_t voffset;            /* Hardware scroll offset, in GRAM rows */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void st7365p_select(FAR struct spi_dev_s *spi, int bits);
static void st7365p_deselect(FAR struct spi_dev_s *spi);
static void st7365p_sendcmd(FAR struct st7365p_dev_s *dev, uint8_t cmd);
static void st7365p_senddata(FAR struct st7365p_dev_s *dev,
                             FAR const uint8_t *data, size_t len);
static uint16_t st7365p_setarea(FAR struct st7365p_dev_s *dev,
                                uint16_t x0, uint16_t y0,
                                uint16_t x1, uint16_t y1);
static void st7365p_wrram(FAR struct st7365p_dev_s *dev,
                          FAR const uint8_t *buff, size_t size,
                          size_t skip, size_t count);
static void st7365p_fill(FAR struct st7365p_dev_s *dev, uint16_t color);

/* LCD data transfer methods */

static int st7365p_putrun(FAR struct lcd_dev_s *dev,
                          fb_coord_t row, fb_coord_t col,
                          FAR const uint8_t *buffer, size_t npixels);
static int st7365p_putarea(FAR struct lcd_dev_s *dev,
                           fb_coord_t row_start, fb_coord_t row_end,
                           fb_coord_t col_start, fb_coord_t col_end,
                           FAR const uint8_t *buffer, fb_coord_t stride);

/* LCD configuration */

static int st7365p_getvideoinfo(FAR struct lcd_dev_s *dev,
                                FAR struct fb_videoinfo_s *vinfo);
static int st7365p_getplaneinfo(FAR struct lcd_dev_s *dev,
                                unsigned int planeno,
                                FAR struct lcd_planeinfo_s *pinfo);

/* LCD specific controls */

static int st7365p_getpower(FAR struct lcd_dev_s *dev);
static int st7365p_setpower(FAR struct lcd_dev_s *dev, int power);
static int st7365p_getcontrast(FAR struct lcd_dev_s *dev);
static int st7365p_setcontrast(FAR struct lcd_dev_s *dev,
                               unsigned int contrast);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* A single row of pixels, used by the LCD framework for the run buffer. */

static uint16_t g_runbuffer[ST7365P_XRES];

/* This must be defined before g_planeinfo, which points back at it. */

static struct st7365p_dev_s g_lcddev;

static struct lcd_planeinfo_s g_planeinfo =
{
  .putrun  = st7365p_putrun,
  .putarea = st7365p_putarea,
  .buffer  = (FAR uint8_t *)g_runbuffer,
  .bpp     = 16,
  .dev     = (FAR struct lcd_dev_s *)&g_lcddev,
};

/* Initialisation sequences *************************************************/

#ifdef CONFIG_LCD_ST7365P_INIT_ST7796

/* The ST7796 style sequence, framed by the 0xF0 command set unlock.  This is
 * what PicoMite, FUZIX and the NES port all converged on, and it is the one
 * demonstrated to work with the 16 bit COLMOD.  Its command values are
 * shared with drivers/lcd/st7796.c, which is the same silicon family.
 */

static const uint8_t g_cscon_on1[]  =
{
  0xc3
};

static const uint8_t g_cscon_on2[]  =
{
  0x96
};

static const uint8_t g_cscon_off1[] =
{
  0x3c
};

static const uint8_t g_cscon_off2[] =
{
  0x69
};

static const uint8_t g_madctl[]     =
{
  0x48
};

static const uint8_t g_colmod[]     =
{
  ST7365P_COLMOD_VAL
};

static const uint8_t g_b4[]         =
{
  0x00
};

static const uint8_t g_b7[]         =
{
  0xc6
};

static const uint8_t g_b9[]         =
{
  0x02, 0xe0
};

static const uint8_t g_c0[]         =
{
  0x80, 0x06
};

static const uint8_t g_c1[]         =
{
  0x15
};

static const uint8_t g_c2[]         =
{
  0xa7
};

static const uint8_t g_c5[]         =
{
  0x04
};

static const uint8_t g_e8[]         =
{
  0x40, 0x8a, 0x00, 0x00, 0x29, 0x19, 0xaa, 0x33
};

static const uint8_t g_gamma_p[]    =
{
  0xf0, 0x06, 0x0f, 0x05, 0x04, 0x20, 0x37,
  0x33, 0x4c, 0x37, 0x13, 0x14, 0x2b, 0x31
};

static const uint8_t g_gamma_n[]    =
{
  0xf0, 0x11, 0x1b, 0x11, 0x0f, 0x0a, 0x37,
  0x43, 0x4c, 0x37, 0x13, 0x13, 0x2c, 0x32
};

static const uint8_t g_teon[]       =
{
  0x00
};

static const struct st7365p_cmd_s g_initcmds[] =
{
  { ST7365P_CSCON,   1,   0, g_cscon_on1  },
  { ST7365P_CSCON,   1,   0, g_cscon_on2  },
  { ST7365P_MADCTL,  1,   0, g_madctl     },
  { ST7365P_COLMOD,  1,   0, g_colmod     },
  { 0xb4,            1,   0, g_b4         },
  { 0xb7,            1,   0, g_b7         },
  { 0xb9,            2,   0, g_b9         },
  { 0xc0,            2,   0, g_c0         },
  { 0xc1,            1,   0, g_c1         },
  { 0xc2,            1,   0, g_c2         },
  { 0xc5,            1,   0, g_c5         },
  { 0xe8,            8,   0, g_e8         },
  { 0xe0,           14,   0, g_gamma_p    },
  { 0xe1,           14,   0, g_gamma_n    },
  { ST7365P_CSCON,   1,   0, g_cscon_off1 },
  { ST7365P_CSCON,   1,   0, g_cscon_off2 },
  { ST7365P_TEON,    1,   0, g_teon       },
  { ST7365P_SLPOUT,  0, 120, NULL         },
  { ST7365P_DISPON,  0,  20, NULL         },
  { 0,               0,   0, NULL         }
};

#else /* CONFIG_LCD_ST7365P_INIT_ILI9488 */

/* The classic ILI9488 vendor sequence, as used by the ClockworkPi lcdspi
 * driver.  No command set unlock, different gamma and power values.
 */

static const uint8_t g_gamma_p[]    =
{
  0x00, 0x03, 0x09, 0x08, 0x16, 0x0a, 0x3f, 0x78,
  0x4c, 0x09, 0x0a, 0x08, 0x16, 0x1a, 0x0f
};

static const uint8_t g_gamma_n[]    =
{
  0x00, 0x16, 0x19, 0x03, 0x0f, 0x05, 0x32, 0x45,
  0x46, 0x04, 0x0e, 0x0d, 0x35, 0x37, 0x0f
};

static const uint8_t g_c0[]         =
{
  0x17, 0x15
};

static const uint8_t g_c1[]         =
{
  0x41
};

static const uint8_t g_c5[]         =
{
  0x00, 0x12, 0x80
};

static const uint8_t g_madctl[]     =
{
  0x48
};

static const uint8_t g_colmod[]     =
{
  ST7365P_COLMOD_VAL
};

static const uint8_t g_b0[]         =
{
  0x00
};

static const uint8_t g_b1[]         =
{
  0xa0
};

static const uint8_t g_b4[]         =
{
  0x02
};

static const uint8_t g_b6[]         =
{
  0x02, 0x02, 0x3b
};

static const uint8_t g_b7[]         =
{
  0xc6
};

static const uint8_t g_e9[]         =
{
  0x00
};

static const uint8_t g_f7[]         =
{
  0xa9, 0x51, 0x2c, 0x82
};

static const struct st7365p_cmd_s g_initcmds[] =
{
  { 0xe0,           15,   0, g_gamma_p },
  { 0xe1,           15,   0, g_gamma_n },
  { 0xc0,            2,   0, g_c0      },
  { 0xc1,            1,   0, g_c1      },
  { 0xc5,            3,   0, g_c5      },
  { ST7365P_MADCTL,  1,   0, g_madctl  },
  { ST7365P_COLMOD,  1,   0, g_colmod  },
  { 0xb0,            1,   0, g_b0      },
  { 0xb1,            1,   0, g_b1      },
  { 0xb4,            1,   0, g_b4      },
  { 0xb6,            3,   0, g_b6      },
  { 0xb7,            1,   0, g_b7      },
  { 0xe9,            1,   0, g_e9      },
  { 0xf7,            4,   0, g_f7      },
  { ST7365P_SLPOUT,  0, 120, NULL      },
  { ST7365P_DISPON,  0, 120, NULL      },
  { 0,               0,   0, NULL      }
};

#endif

/* Description of this LCD */

static const struct fb_videoinfo_s g_videoinfo =
{
  .fmt     = FB_FMT_RGB16_565,
  .xres    = ST7365P_XRES,
  .yres    = ST7365P_YRES,
  .nplanes = 1,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: st7365p_select
 *
 * Description:
 *   Lock the bus, select the panel and configure the transfer.
 *
 *   The panel is written at CONFIG_LCD_ST7365P_FREQUENCY but can only be
 *   read at CONFIG_LCD_ST7365P_READ_FREQUENCY, which is markedly slower;
 *   every implementation of this panel drops the clock for reads.
 *
 ****************************************************************************/

static void st7365p_select(FAR struct spi_dev_s *spi, int bits)
{
  SPI_LOCK(spi, true);
  SPI_SELECT(spi, SPIDEV_DISPLAY(0), true);

  SPI_SETMODE(spi, CONFIG_LCD_ST7365P_SPIMODE);
  SPI_SETBITS(spi, bits);
  SPI_SETFREQUENCY(spi, CONFIG_LCD_ST7365P_FREQUENCY);
}

/****************************************************************************
 * Name: st7365p_deselect
 ****************************************************************************/

static void st7365p_deselect(FAR struct spi_dev_s *spi)
{
  SPI_SELECT(spi, SPIDEV_DISPLAY(0), false);
  SPI_LOCK(spi, false);
}

/****************************************************************************
 * Name: st7365p_sendcmd
 *
 * Description:
 *   Send a single command byte with the Data/Command line low.
 *
 ****************************************************************************/

static void st7365p_sendcmd(FAR struct st7365p_dev_s *dev, uint8_t cmd)
{
  st7365p_select(dev->spi, ST7365P_SPI_BITS);
  SPI_CMDDATA(dev->spi, SPIDEV_DISPLAY(0), true);
  SPI_SEND(dev->spi, cmd);
  SPI_CMDDATA(dev->spi, SPIDEV_DISPLAY(0), false);
  st7365p_deselect(dev->spi);
}

/****************************************************************************
 * Name: st7365p_senddata
 *
 * Description:
 *   Send command parameters with the Data/Command line high.
 *
 ****************************************************************************/

static void st7365p_senddata(FAR struct st7365p_dev_s *dev,
                             FAR const uint8_t *data, size_t len)
{
  if (data == NULL || len == 0)
    {
      return;
    }

  st7365p_select(dev->spi, ST7365P_SPI_BITS);
  SPI_SNDBLOCK(dev->spi, data, len);
  st7365p_deselect(dev->spi);
}

/****************************************************************************
 * Name: st7365p_sendtable
 *
 * Description:
 *   Run one of the initialisation tables.
 *
 ****************************************************************************/

static void st7365p_sendtable(FAR struct st7365p_dev_s *dev,
                              FAR const struct st7365p_cmd_s *table)
{
  FAR const struct st7365p_cmd_s *e;

  for (e = table; e->cmd != 0; e++)
    {
      st7365p_sendcmd(dev, e->cmd);
      st7365p_senddata(dev, e->data, e->len);

      if (e->delay_ms != 0)
        {
          up_mdelay(e->delay_ms);
        }
    }
}

/****************************************************************************
 * Name: st7365p_setarea
 *
 * Description:
 *   Set the active drawing window, and return the number of rows it covers.
 *
 *   The current hardware scroll offset is folded in here, so every caller
 *   keeps addressing rows 0..yres-1 and stays correct whether or not the
 *   display has been scrolled.
 *
 *   Once the display has scrolled, a window can run off the end of GRAM,
 *   and that cannot be expressed in one pair of address registers: RAMWR
 *   wraps back to the top of the window, not to the top of GRAM, so the
 *   rows past the end would overwrite the ones already written.  Such a
 *   window is therefore truncated at the wrap and the number of rows
 *   actually programmed is returned, which is always at least one.  A
 *   caller with rows left over issues them as a second transfer starting
 *   where this one stopped, by which point the offset has wrapped to the
 *   top of GRAM.
 *
 ****************************************************************************/

static uint16_t st7365p_setarea(FAR struct st7365p_dev_s *dev,
                                uint16_t x0, uint16_t y0,
                                uint16_t x1, uint16_t y1)
{
  uint8_t buf[4];
  uint16_t g0;
  uint16_t g1;
  uint16_t nrows;

  g0    = (y0 + dev->voffset) % ST7365P_GRAM_YRES;
  nrows = y1 - y0 + 1;

  if (g0 + nrows > ST7365P_GRAM_YRES)
    {
      nrows = ST7365P_GRAM_YRES - g0;
    }

  g1 = g0 + nrows - 1;

  /* Column address */

  buf[0] = x0 >> 8;
  buf[1] = x0 & 0xff;
  buf[2] = x1 >> 8;
  buf[3] = x1 & 0xff;
  st7365p_sendcmd(dev, ST7365P_CASET);
  st7365p_senddata(dev, buf, 4);

  /* Row address */

  buf[0] = g0 >> 8;
  buf[1] = g0 & 0xff;
  buf[2] = g1 >> 8;
  buf[3] = g1 & 0xff;
  st7365p_sendcmd(dev, ST7365P_RASET);
  st7365p_senddata(dev, buf, 4);

  return nrows;
}

/****************************************************************************
 * Name: st7365p_wrram
 *
 * Description:
 *   Write pixel data to GRAM.
 *
 *   The SPI word size is set to 16 bits for the pixel payload.  The PL022
 *   shifts a 16 bit word out most significant bit first, which is exactly
 *   the order the panel wants, so a little endian RGB565 buffer needs no
 *   byte swapping.  Note the word count passed to SPI_SNDBLOCK is therefore
 *   in pixels, not bytes.
 *
 ****************************************************************************/

static void st7365p_wrram(FAR struct st7365p_dev_s *dev,
                          FAR const uint8_t *buff, size_t size,
                          size_t skip, size_t count)
{
  size_t i;

  st7365p_sendcmd(dev, ST7365P_RAMWR);

  st7365p_select(dev->spi, ST7365P_BYTESPP * ST7365P_SPI_BITS);

  for (i = 0; i < count; i++)
    {
      SPI_SNDBLOCK(dev->spi, buff + (i * (size + skip)),
                   size / ST7365P_BYTESPP);
    }

  st7365p_deselect(dev->spi);
}

/****************************************************************************
 * Name: st7365p_fill
 *
 * Description:
 *   Fill the whole of GRAM, including the rows the panel does not show, with
 *   a solid colour.
 *
 *   Clearing all GRAM_YRES rows rather than just the visible YRES matters:
 *   the off-screen rows are what hardware scrolling brings into view, and
 *   they hold random values out of reset.
 *
 ****************************************************************************/

static void st7365p_fill(FAR struct st7365p_dev_s *dev, uint16_t color)
{
  uint16_t save;
  uint16_t row;
  int i;

  for (i = 0; i < ST7365P_XRES; i++)
    {
      g_runbuffer[i] = color;
    }

  /* Address raw GRAM rows, not display rows. */

  save = dev->voffset;
  dev->voffset = 0;

  for (row = 0; row < ST7365P_GRAM_YRES; row++)
    {
      st7365p_setarea(dev, 0, row, ST7365P_XRES - 1, row);
      st7365p_wrram(dev, (FAR const uint8_t *)g_runbuffer,
                    ST7365P_XRES * ST7365P_BYTESPP, 0, 1);
    }

  dev->voffset = save;
}

/****************************************************************************
 * Name: st7365p_putrun
 ****************************************************************************/

static int st7365p_putrun(FAR struct lcd_dev_s *dev,
                          fb_coord_t row, fb_coord_t col,
                          FAR const uint8_t *buffer, size_t npixels)
{
  FAR struct st7365p_dev_s *priv = (FAR struct st7365p_dev_s *)dev;

  ginfo("row: %d col: %d npixels: %zu\n", row, col, npixels);
  DEBUGASSERT(buffer && ((uintptr_t)buffer & 1) == 0);

  st7365p_setarea(priv, col, row, col + npixels - 1, row);
  st7365p_wrram(priv, buffer, npixels * ST7365P_BYTESPP, 0, 1);

  return OK;
}

/****************************************************************************
 * Name: st7365p_putarea
 ****************************************************************************/

static int st7365p_putarea(FAR struct lcd_dev_s *dev,
                           fb_coord_t row_start, fb_coord_t row_end,
                           fb_coord_t col_start, fb_coord_t col_end,
                           FAR const uint8_t *buffer, fb_coord_t stride)
{
  FAR struct st7365p_dev_s *priv = (FAR struct st7365p_dev_s *)dev;
  size_t cols = col_end - col_start + 1;
  size_t row_size = cols * ST7365P_BYTESPP;
  fb_coord_t row;
  uint16_t nrows;

  ginfo("row_start: %d row_end: %d col_start: %d col_end: %d\n",
        row_start, row_end, col_start, col_end);

  DEBUGASSERT(buffer && ((uintptr_t)buffer & 1) == 0);

  /* Once the display has been scrolled the area can straddle the end of
   * GRAM, which takes two transfers.  setarea() reports how many rows the
   * window it programmed actually covers, so this loop runs once in the
   * common case and twice across the wrap.
   */

  row = row_start;
  while (row <= row_end)
    {
      nrows = st7365p_setarea(priv, col_start, row, col_end, row_end);

      /* When the stride matches the row, this chunk is contiguous and can go
       * out as one transfer.  Otherwise fall back to row by row.
       */

      if (stride == row_size)
        {
          st7365p_wrram(priv, buffer, nrows * row_size, 0, 1);
        }
      else
        {
          st7365p_wrram(priv, buffer, row_size, stride - row_size, nrows);
        }

      buffer += nrows * stride;
      row    += nrows;
    }

  return OK;
}

/****************************************************************************
 * Name: st7365p_getvideoinfo
 ****************************************************************************/

static int st7365p_getvideoinfo(FAR struct lcd_dev_s *dev,
                                FAR struct fb_videoinfo_s *vinfo)
{
  DEBUGASSERT(dev && vinfo);

  ginfo("fmt: %d xres: %d yres: %d nplanes: %d\n",
        g_videoinfo.fmt, g_videoinfo.xres,
        g_videoinfo.yres, g_videoinfo.nplanes);

  memcpy(vinfo, &g_videoinfo, sizeof(struct fb_videoinfo_s));
  return OK;
}

/****************************************************************************
 * Name: st7365p_getplaneinfo
 ****************************************************************************/

static int st7365p_getplaneinfo(FAR struct lcd_dev_s *dev,
                                unsigned int planeno,
                                FAR struct lcd_planeinfo_s *pinfo)
{
  DEBUGASSERT(dev && pinfo && planeno == 0);

  ginfo("planeno: %d bpp: %d\n", planeno, g_planeinfo.bpp);

  memcpy(pinfo, &g_planeinfo, sizeof(struct lcd_planeinfo_s));
  pinfo->dev = dev;
  return OK;
}

/****************************************************************************
 * Name: st7365p_getpower
 ****************************************************************************/

static int st7365p_getpower(FAR struct lcd_dev_s *dev)
{
  FAR struct st7365p_dev_s *priv = (FAR struct st7365p_dev_s *)dev;

  ginfo("power: %d\n", priv->power);
  return priv->power;
}

/****************************************************************************
 * Name: st7365p_setpower
 *
 * Description:
 *   Turn the panel on or off.  Anything non-zero is "on"; the backlight
 *   itself is not on a pin of this controller, so intermediate levels are
 *   the board's business.
 *
 ****************************************************************************/

static int st7365p_setpower(FAR struct lcd_dev_s *dev, int power)
{
  FAR struct st7365p_dev_s *priv = (FAR struct st7365p_dev_s *)dev;

  ginfo("power: %d\n", power);
  DEBUGASSERT((unsigned)power <= CONFIG_LCD_MAXPOWER);

  if (power > 0)
    {
      st7365p_sendcmd(priv, ST7365P_SLPOUT);
      up_mdelay(120);
      st7365p_sendcmd(priv, ST7365P_DISPON);
    }
  else
    {
      st7365p_sendcmd(priv, ST7365P_DISPOFF);
      st7365p_sendcmd(priv, ST7365P_SLPIN);
    }

  priv->power = power;
  return OK;
}

/****************************************************************************
 * Name: st7365p_getcontrast
 ****************************************************************************/

static int st7365p_getcontrast(FAR struct lcd_dev_s *dev)
{
  ginfo("Not implemented\n");
  return -ENOSYS;
}

/****************************************************************************
 * Name: st7365p_setcontrast
 ****************************************************************************/

static int st7365p_setcontrast(FAR struct lcd_dev_s *dev,
                               unsigned int contrast)
{
  ginfo("contrast: %d\n", contrast);
  return -ENOSYS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: st7365p_lcdinitialize
 ****************************************************************************/

FAR struct lcd_dev_s *st7365p_lcdinitialize(FAR struct spi_dev_s *spi)
{
  FAR struct st7365p_dev_s *priv = &g_lcddev;

  ginfo("Initializing ST7365P\n");

  priv->dev.getvideoinfo = st7365p_getvideoinfo;
  priv->dev.getplaneinfo = st7365p_getplaneinfo;
  priv->dev.getpower     = st7365p_getpower;
  priv->dev.setpower     = st7365p_setpower;
  priv->dev.getcontrast  = st7365p_getcontrast;
  priv->dev.setcontrast  = st7365p_setcontrast;
#ifdef CONFIG_LCD_ST7365P_HWSCROLL
  priv->dev.vscroll      = st7365p_scroll;
#endif
  priv->spi              = spi;
  priv->power            = 0;
  priv->voffset          = 0;

  /* The board has already driven the hardware reset line by this point. */

  st7365p_sendcmd(priv, ST7365P_SWRESET);
  up_mdelay(120);

  st7365p_sendtable(priv, g_initcmds);

#ifdef CONFIG_LCD_ST7365P_INVCOLOR
  st7365p_sendcmd(priv, ST7365P_INVON);
#else
  st7365p_sendcmd(priv, ST7365P_INVOFF);
#endif

#ifdef CONFIG_LCD_ST7365P_HWSCROLL
  /* Define the whole of GRAM as the scrolling area: no fixed region at the
   * top or the bottom.  FUZIX's driver for this panel does the same.  Using
   * the visible height as the scroll area instead, as some implementations
   * do, defines a window that cannot actually scroll.
   */

    {
      uint8_t vscr[6];

      vscr[0] = 0;
      vscr[1] = 0;
      vscr[2] = ST7365P_GRAM_YRES >> 8;
      vscr[3] = ST7365P_GRAM_YRES & 0xff;
      vscr[4] = 0;
      vscr[5] = 0;

      st7365p_sendcmd(priv, ST7365P_VSCRDEF);
      st7365p_senddata(priv, vscr, 6);
    }
#endif

  /* Clear all of GRAM, including the off-screen rows. */

  st7365p_fill(priv, 0x0000);

  return &priv->dev;
}

/****************************************************************************
 * Name: st7365p_scroll
 ****************************************************************************/

int st7365p_scroll(FAR struct lcd_dev_s *dev, int lines)
{
#ifdef CONFIG_LCD_ST7365P_HWSCROLL
  FAR struct st7365p_dev_s *priv = (FAR struct st7365p_dev_s *)dev;
  uint8_t buf[2];
  int off;

  off = (int)priv->voffset + lines;

  /* C's % keeps the sign of the dividend, so normalise by hand. */

  off %= ST7365P_GRAM_YRES;
  if (off < 0)
    {
      off += ST7365P_GRAM_YRES;
    }

  priv->voffset = (uint16_t)off;

  buf[0] = priv->voffset >> 8;
  buf[1] = priv->voffset & 0xff;

  st7365p_sendcmd(priv, ST7365P_VSCRSADD);
  st7365p_senddata(priv, buf, 2);

  return OK;
#else
  return -ENOSYS;
#endif
}

#endif /* CONFIG_LCD_ST7365P */
