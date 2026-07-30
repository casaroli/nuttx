================
MPS2 AN521 Board
================

This board configuration will use QEMU to emulate generic ARM v8-M series
hardware platform and provides support for these devices:

- ARM Generic Timer
- CMSDK UART controller

Getting Started
===============

1. Configuring NuttX and compile (Single Core)::

     $ ./tools/configure.sh -l mps2-an521:nsh
     $ make

Running with qemu::

     $ qemu-system-arm -M mps2-an521 -nographic -chardev stdio,id=con,mux=on \
     -serial chardev:con -mon chardev=con,mode=readline -kernel ./nuttx

Debugging with QEMU
===================

The nuttx ELF image can be debugged with QEMU.

1. To debug the nuttx (ELF) with symbols, make sure the following change have
   applied to defconfig::

     CONFIG_DEBUG_SYMBOLS=y

2. Run QEMU (at shell terminal 1)::

     qemu-system-arm -M mps2-an521 -nographic -chardev stdio,id=con,mux=on \
     -serial chardev:con -mon chardev=con,mode=readline -kernel ./nuttx -S -s

3. Run gdb with TUI, connect to QEMU, load nuttx and continue (at shell terminal 2)::

     $ arm-none-eabi-gdb -tui --eval-command='target remote localhost:1234' nuttx

Configurations
==============

nsh
---

Basic NuttShell configuration, flat build, console on CMSDK UART0.

knsh
----

The same shell, built as a **protected** build (``CONFIG_BUILD_PROTECTED``):
the kernel and the application are two separately linked blobs and the
Cortex-M33's MPU keeps unprivileged code out of the kernel's half of both
banks.  Applications reach the kernel through system calls.

The AN521 has only two banks -- SSRAM1 at ``0x10000000``, used for code, and
SSRAM2_3 at ``0x38000000``, used as RAM -- so both halves are carved out of
them, 2MB each, by ``scripts/memory.ld``.  The user half of RAM must end
exactly at ``PRIMARY_RAM_END`` (``0x38400000``), because that is what
``arch/arm/src/mps/mps_allocateheap.c`` bounds the user heap with.

Both blobs have to be given to QEMU.  The kernel is the ``-kernel`` image; the
user blob is loaded by the generic loader, which places it from its own ELF
headers::

     $ qemu-system-arm -M mps2-an521 -nographic -kernel ./nuttx \
       -device loader,file=./nuttx_user.elf

Note that ``-nographic`` alone is used here rather than the muxed
``-chardev``/``-mon`` invocation shown above.  That invocation puts the QEMU
monitor on the same stdio as the console, which is convenient interactively but
sends piped input to the monitor instead of to the guest.
