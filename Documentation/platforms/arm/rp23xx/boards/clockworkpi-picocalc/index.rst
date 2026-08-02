====================
ClockworkPi PicoCalc
====================

.. tags:: chip:rp2350, chip:rp2350b, lcd, keyboard, audio

The `ClockworkPi PicoCalc <https://www.clockworkpi.com/picocalc>`_ is a
pocket-sized computer kit built around a socket that takes a Raspberry Pi Pico
form-factor module.  It supplies a 320x320 SPI panel, a 57-key QWERTY keyboard,
two speakers and a headphone jack, a microSD slot, and a pair of 18650 cells
with charging.

The module supplies only the processor.  This port targets a **Pimoroni Pico
Plus 2 W** -- an RP2350B with 16MB of flash and 8MB of PSRAM -- because the
panel is driven from a framebuffer that does not fit in the RP2350's own SRAM.

.. note::

   The keyboard, the two backlights and the battery are **not** wired to the
   Pico socket.  They belong to an STM32F103 co-processor which the RP2350
   reaches over I2C, and which fronts an AXP2101 PMU that the RP2350 cannot
   address at all.  Anything that reads a key or a battery voltage goes
   through that co-processor.

Features
========

* Socket for a Raspberry Pi Pico form-factor module
* 4.0 inch 320x320 IPS panel, Sitronix ST7365P, 4-line SPI
* 57-key QWERTY keyboard on an STM32F103R8T6 co-processor
* Dual 1.5W speakers and a 3.5mm headphone jack with insertion detect
* microSD slot, 1-bit SPI mode
* 2x 18650 cells, charged and measured by an X-Powers AXP2101
* Software-controlled panel and keyboard backlights
* 8MB of PSRAM on the mainboard, in addition to any on the module

Peripherals
===========

======================  ==========  ====================================
Peripheral              Bus         GPIO
======================  ==========  ====================================
Panel (ST7365P)         SPI1        SCK 10, MOSI 11, MISO 12, CS 13,
                                    D/C 14, RST 15
microSD                 SPI0        MISO 16, CS 17, SCK 18, MOSI 19,
                                    card detect 22 (active low)
Keyboard co-processor   I2C1        SDA 6, SCL 7
Co-processor attention  --          9 (input, from STM32 PC10)
Co-processor wake       --          8 (output, to STM32 PC11)
Audio                   PWM         left 26, right 27
======================  ==========  ====================================

The panel's Data/Command pin is **not** the SPI RX pin, which the RP23XX SPI
driver would otherwise claim; ``CONFIG_RP23XX_SPI1_DC_GPIO`` exists for that.

Keyboard
========

The co-processor is reached at I2C address ``0x1F`` with the register protocol
the stock ClockworkPi firmware speaks.  It has no interrupt line in that mode,
so the driver polls; that is not merely a convenience, because the
co-processor re-initialises its own I2C slave if neither of its callbacks has
run for 2500ms, and a host that goes quiet gets a link that resets underneath
it.

A second address, ``0x1E``, is served by a replacement firmware and returns a
block of queued events in one transaction while driving the attention line on
GPIO 9, so the bus stays quiet while nothing is being typed.  The driver
probes ``0x1E`` at start-up and falls back to ``0x1F`` when it NACKs, so both
firmwares work with no configuration.

The keyboard is registered at ``/dev/kbd0`` through the ``INPUT_KEYBOARD``
upper half, so it is read like any other keyboard.  The arrow keys produce no
character and are reported as special keys carrying ``KEYCODE_UP`` and
friends.

Battery
=======

``/dev/batt0`` is a battery gauge backed by the co-processor's cached summary
of the AXP2101: charge state, capacity, and battery, VBUS and VSYS voltages.
The AXP2101 is not on any bus the RP2350 can reach, so this is the only route
to it.

Configurations
==============

All configurations below are selected with the following command in the
``nuttx`` directory (consult the main
:doc:`RP23XX documentation <../../index>`):

.. code:: console

   $ ./tools/configure.sh clockworkpi-picocalc:<configname>

nsh
---

Basic NuttShell configuration, console on UART0 at 115200 bps.  Nothing that
belongs to the mainboard is enabled, so this is the configuration to reach for
when bringing up a new module.

rtt
---

NuttShell with the console on SEGGER RTT over SWD, which needs no pins and no
serial adapter.  The PicoCalc's UART0 pins are not brought out to anything
convenient, so this is how the board was brought up.

lcd
---

The ``rtt`` console plus the panel, a framebuffer and ``examples/fb``.

lvglterm
--------

The ``lcd`` configuration plus LVGL and ``examples/lvglterm``: a terminal
drawn with LVGL, driven by the keyboard.

wifi
----

The ``nxterm`` configuration plus the CYW43439 on the Pico module: ``wlan0``
in station mode, WAPI, a DHCP client, a DNS client and ``ping``.

This configuration needs the CYW43439 firmware blob, which is not in the
NuttX tree.  ``CONFIG_CYW43439_FIRMWARE_BIN_PATH`` defaults to a path under
``${PICO_SDK_PATH}``, so set that in the environment before
``configure.sh``:

.. code:: console

   $ export PICO_SDK_PATH=/path/to/pico-sdk
   $ ./tools/configure.sh clockworkpi-picocalc:wifi

That is why the radio is a configuration of its own rather than part of
``nxterm`` -- the terminal builds with nothing but the ARM toolchain, and
should keep doing so.

**No credentials are stored.**  ``CONFIG_NETINIT_WAPI_SSID`` and
``CONFIG_NETINIT_WAPI_PASSPHRASE`` are deliberately empty, so associate at
runtime:

.. code:: console

   nsh> wapi mode wlan0 2
   nsh> wapi psk wlan0 "my-passphrase" 3 2
   nsh> wapi essid wlan0 my-ssid 1
   nsh> renew wlan0
   nsh> ifconfig

Set them in the configuration instead only if you are content for a
passphrase to sit in a defconfig, which is a file that tends to get
committed.

``CONFIG_IEEE80211_BROADCOM_DEFAULT_COUNTRY`` is ``"XX"``, the worldwide
setting.  Set it to your own country to get the channels your regulator
allows.

nxterm
------

A full-screen NX terminal on the panel with the shell behind it, the keyboard
fed in through ``system/kbdbridge``, the battery gauge, audio and the microSD
card.  This is the configuration that makes the board a usable handheld, and
it starts on the panel at boot.

The panel scrolls in hardware: the ST7365P's own vertical scroll is used
instead of repainting the display, which is what makes the terminal usable at
this resolution.
