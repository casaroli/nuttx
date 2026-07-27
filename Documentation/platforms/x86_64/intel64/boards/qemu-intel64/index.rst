============
qemu-intel64
============

This page file describes the contents of the build configurations available
for the NuttX QEMU x86_64 port.

QEMU/KVM
========

QEMU is a generic and open source machine emulator and virtual machine.  Here are
some links (which will probably be mostly outdated by the time your read this):

* Home Page: http://wiki.qemu.org/Main_Page
* Downloads: http://wiki.qemu.org/Download
* Documentation: http://wiki.qemu.org/Manual

KVM is the Linux kernel hypervisor.
It supports creations of virtual machines in Linux systems.
It is usually coupled with Qemu as its I/O supporting layer.

The qemu can be build from source or downloaded from distro repositories.

KVM is strongly preferred, because full-system emulation of x86_64 costs
roughly an order of magnitude in speed.  It is **not** required, however:
QEMU's own emulation backend (TCG) can run these configurations, provided
``-cpu max`` is used and the configuration is adjusted for the features TCG
does not implement.  See `Running QEMU without KVM (TCG)`_ below.  That is the
route to take on a Windows host, on a CI runner with no ``/dev/kvm``, or on a
non-x86 host such as an Apple Silicon Mac, where no hypervisor for x86 guests
exists at all.

Running QEMU
------------

When you created a bootable disk, use command::

    qemu-system-x86_64 -cpu host -enable-kvm -m 2G -cdrom boot.iso -nographic -serial mon:stdio

or, when option ``CONFIG_ARCH_PVHBOOT`` is set, you can use ``-kernel`` argument instead::

    qemu-system-x86_64 -cpu host -enable-kvm -m 2G -kernel nuttx.elf -nographic -serial mon:stdio

This multiplex the qemu console and COM1 to your console.

Use control-a 1 and 2 to switch between.
Use control-a x to terminate the emulation.

P.S. Make sure that you CPU supports the mandatory features. Look at Real machine
section for more information.

For testing the PCI bus and driver layers.  This QEMU configuration can be used
with the pcitest NuttX configuration::

    qemu-system-x86_64  -cpu host,+pcid,+x2apic,+tsc-deadline,+xsave,+rdrand --enable-kvm -smp 1 -m 2G -cdrom boot.iso --nographic -s -no-reboot -device edu -device pci-testdev
  
This will enable the QEMU pci-test and edu PCI test devices which test PIO, MMIO, IRQ, and DMA
functions.  Additionally it will show detailed information about the enumeration of the PCI bus.

If you want to boot using UEFI and TianoCore you will need to add a flag like this to
point at OVMF ``--bios /usr/share/edk2/ovmf/OVMF_CODE.fd``

Running QEMU without KVM (TCG)
------------------------------

Without ``-enable-kvm``, QEMU falls back to TCG, its portable emulation
backend.  The stock configurations do not boot that way, and the failure is
silent, so the two adjustments below are worth making before concluding
anything is broken.

Use ``-cpu max``
^^^^^^^^^^^^^^^^

``-cpu host`` requires KVM or HVF and is rejected outright without one.  The
replacement is ``-cpu max``, which advertises everything the TCG backend can
emulate — including **X2APIC**, which ``x86_64_check_and_enable_capability()``
requires unconditionally.

Do not fall back to the default ``qemu64`` model.  It does not advertise
X2APIC, and these configurations additionally set
``CONFIG_ARCH_X86_64_HAVE_XSAVE`` and ``CONFIG_ARCH_INTEL64_HAVE_RDRAND``;
adding those three by hand (``-cpu qemu64,+x2apic,+xsave,+rdrand``) is still
not sufficient, because an XSAVE state-area size check and
``__enable_sse_avx()`` follow.  ``-cpu max`` is the supported choice.

Turn off PCID and the TSC deadline timer
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

TCG implements neither, even under ``-cpu max``, and both are on by default in
the ``qemu-intel64`` configurations.  Each one independently prevents the
board from booting.  Move the system clock onto the HPET instead, which QEMU
does emulate at the usual ``0xfed00000`` (NuttX reads the period from the
capability register rather than assuming one)::

    kconfig-tweak --file .config --disable CONFIG_ARCH_INTEL64_HAVE_PCID
    kconfig-tweak --file .config --disable CONFIG_ARCH_INTEL64_TSC_DEADLINE
    kconfig-tweak --file .config --enable  CONFIG_ARCH_INTEL64_HPET_ALARM
    make olddefconfig

The machine must then be started with ``hpet=on``, which is not the default on
all machine types::

    qemu-system-x86_64 -machine pc,hpet=on -cpu max -m 2G \
      -kernel nuttx -nographic -no-reboot -net none

How this fails when you get it wrong
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``x86_64_check_and_enable_capability()`` requires every feature the
configuration asks for, and otherwise takes a ``cli; hlt`` error path.  There
is no message.  The console stops dead after SeaBIOS's ``Booting from ROM..``
and stays empty, which is indistinguishable from a kernel hang.

A missing capability is by far the most likely cause, but note that the same
empty console results from any ``PANIC()`` raised before
``x86_64_cpu_priv_set(0)``, near the end of ``__nxstart()``: ``_assert()``
reads ``up_interrupt_context()``, which is ``movb %gs:6``, and the GS base is
still zero, so the fault double- and triple-faults the CPU.  If the capability
changes above do not help, attach a debugger and read ``RIP`` rather than
reading the console — see `Debugging with gdb`_.

Speed
^^^^^

Full-system emulation is roughly 12x native on the same host, and there is no
way around that: TCG has to emulate the MMU in software.  What is worth
knowing is which knobs actually move, measured on an Apple M4 Pro with QEMU
11.0.3:

* **The CPU model is the only QEMU flag that matters much.** ``-cpu max`` is
  required here anyway, and it is also the fastest choice, because it
  advertises ``erms``/``fsrm`` and QEMU implements ``rep movsb`` as a single
  bulk helper rather than instruction by instruction.
* **Boot the kernel directly.** ``-kernel`` skips firmware and bootloader.
  Boot to the NSH prompt takes about 0.1 s that way; booting a disk image is
  tens of seconds.
* **Do not pass a named Intel model** (``Nehalem``, ``Haswell``,
  ``Skylake-Client``, ``core2duo``) to a *Linux* guest on the same host: Linux
  then sees a Meltdown-affected CPU and enables KPTI, which costs 6.7x under
  TCG's software MMU.  NuttX does not use KPTI and is unaffected, but the trap
  is easy to hit while preparing a comparison.
* **Measured to be noise for execution speed, and not worth setting:**
  ``tb-size``, ``split-wx``, ``thread=multi``, and RAM size.  The machine type
  changes only device bring-up, which is a fraction of a second either way;
  ``microvm`` is cheaper still but has no HPET, so it is not usable here.

For reference, a full ``ostest`` under TCG takes about 73 s for
``knsh_romfs`` and about 156 s for the flat ``nsh`` configuration.  If a run
appears to take an hour, suspect the harness rather than the emulator — see
the next section.

Driving the console from a script
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Two things catch out automated runs:

* **QEMU does not exit when the test finishes.**  NSH simply returns to its
  prompt.  A harness built from fixed ``sleep`` calls therefore costs whatever
  the longest sleep is, on every run, no matter how quickly the guest
  finished.  Wait for a marker in the output (``ostest`` ends with
  ``Exiting with status``) and stop the machine then.

* **Console input cannot be written in one burst.**  The 16550 receive FIFO is
  16 bytes and under TCG the guest cannot drain it fast enough, so a command
  written all at once is silently truncated after 15 characters and NSH never
  sees the newline.  Send the line a character at a time.  Pacing on the
  guest's echo is both exact and quick — NSH redraws the whole input line on
  every keypress, so the echoed line prefix is the acknowledgement to wait
  for.  A fixed delay per character works too but is far slower for no gain.

Debugging with gdb
^^^^^^^^^^^^^^^^^^

QEMU's gdbstub works the same under TCG as under KVM, and is the only way to
diagnose the silent-hang cases above.  Start with ``-s -S`` (listen on
``:1234``, halt at reset) and attach.  On an x86_64 host the system ``gdb``
will do; on any other host you need a cross debugger, such as Homebrew's
``x86_64-elf-gdb``::

    qemu-system-x86_64 -machine pc,hpet=on -cpu max -m 2G \
      -kernel nuttx -nographic -no-reboot -net none -s -S

    x86_64-elf-gdb -ex 'target remote :1234' nuttx

Two things to avoid:

* **Do not** ``set architecture`` **before connecting.**  Forcing one makes
  gdb reject the stub's target description, and the session fails with the
  misleading ``Remote 'g' packet reply is too long``.  Connect first and let
  gdb take the architecture the stub advertises.
* **Use** ``hbreak``\ **, not** ``break``\ **, for early-boot addresses.**  A
  software breakpoint writes ``0xCC`` into memory that early boot may still
  overwrite, and it then silently never fires.

Bochs
=====

Bochs is also a generic and open source machine emulator and virtualizer.
It does very comprehensive emulation of x86 platform, even the state-of-art processors.
Here are some links (which will probably be mostly outdated by the time your read this):

* Home Page: http://bochs.sourceforge.net

The bochs can be build from source.
Unlike qemu, it does not rely on KVM to support modern hardware features,
therefore it can also be used under Windows.
When building bochs, remember to enable x86-64 support with ``--enable-x86-64``.
If you also want support for SIMD instructions, enable them with ``--enable-avx --enable-evex``.

Running Bochs
-------------

First edit/check the ``.bochsrc``
You can create one in the top-level NuttX directory or bochs will use the one in your $HOME.
Remember to change the CPU model to one with mandatory features and enable the COM port.

* Find and edit (You might adjust the IPS as you machine perform)::

    cpu: model=broadwell_ult, count=1, ips=50000000, reset_on_triple_fault=0, ignore_bad_msrs=0, msrs="msrs.def"
    ata0-master: type=cdrom, path="<PATH TO boot.iso>", status=inserted

* Add::

    com1: enabled=1, mode=file, dev=com1.out

* In the top-level NuttX directory::

    bochs

The emulator will drop into debugger mode.
Enter ``c`` to start the emulation.
COM port output will be in the com1.out file.

Real machine
============

This port can work on real x86-64 machine with a proper CPU.
The mandatory CPU features are:

* TSC DEADLINE or APIC timer or HPET
* PCID, if ``CONFIG_ARCH_INTEL64_HAVE_PCID`` is set — it is on by default in
  the ``qemu-intel64`` configurations, but the port runs without it
* X2APIC — this one is required unconditionally
* legacy serial port support or PCI serial card (AX99100 only supported now)

WARNING: IF you use TSC DEADLINE, make sure that your CPU's TSC DEADLINE timer
is not buggy!

Toolchains
==========

Currently, only the Linux GCC toolchain is tested.
While building on a modern x86_64 PC, the default system GCC can be used.

Configurations
==============

Common Configuration Notes
--------------------------

1. Each Qemu-intel64 configuration is maintained in a sub-directory
   and can be selected as follow::

     tools/configure.sh qemu-intel64:<subdir>

   Where ``<subdir>`` is one of the configuration sub-directories described in
   the following paragraph.

2. These configurations use the mconf-based configuration tool.  To
   change a configurations using that tool, you should:

   a. Build and install the kconfig-mconf tool.  See nuttx/README.txt
      see additional README.txt files in the NuttX tools repository.

   b. Execute ``make menuconfig`` in nuttx/ in order to start the
      reconfiguration process.

3. By default, all configurations assume the Linux.  This is easily
   reconfigured::

     CONFIG_HOST_LINUX=y

Configuration Sub-Directories
-----------------------------

nsh
---

This configuration provides a basic NuttShell configuration (NSH) with
the default console on legacy UART0 port (base=0x3f8)

nsh_pci
-------

This configuration provides a basic NuttShell configuration (NSH) with
the default console on PCI serial port (AX99100 based card).

nsh_pci_smp
-----------

This is a configuration to run NuttX in SMP mode on hardware with
a PCI serial port card (AX99100).

ostest
------

The "standard" NuttX examples/ostest configuration with
the default console on legacy UART0 port (base=0x3f8)

jumbo
-----

This is a QEMU configuration that enables many NuttX features.

Basic command to run the image without additional PCI devices attached::

  qemu-system-x86_64 -m 2G -cpu host -smp 4 -enable-kvm \
  -kernel nuttx -nographic -serial mon:stdio

Command to run the image with some xHCI devices attached::

  qemu-system-x86_64 -m 4G -smp 4 -cpu host -enable-kvm \
  -kernel nuttx -serial mon:stdio -chardev pty,id=ch1 \
  -device qemu-xhci -device usb-mouse -device usb-kbd

Command to run the image with e1000 NIC device with TAP::

  qemu-system-x86_64 -m 2G -smp 4 -cpu host -enable-kvm \
  -kernel nuttx -nographic -serial mon:stdio \
  -device e1000,netdev=mynet0 \
  -netdev tap,id=mynet0,ifname=tap0,script=no,downscript=no

knsh_romfs
----------

This is similar to the ``nsh`` configuration except that NuttX
is built as a kernel-mode, monolithic module, and the user applications
are built separately. It uses ROMFS to load the user-space applications.
This is intended to run on QEMU with COM serial port support.

Steps to build kernel image with user-space apps in ROMFS::
    
    ./tools/configure.sh qemu-intel64/knsh_romfs
    make -j
    make export -j
    pushd ../apps
    ./tools/mkimport.sh -z -x ../nuttx/nuttx-export-*.tar.gz
    make import -j
    ./tools/mkromfsimg.sh
    mv boot_romfsimg.h ../nuttx/arch/x86_64/src/board/romfs_boot.c
    popd
    make -j

knsh_romfs_pci
--------------

This is similar to the ``knsh_romfs`` configuration except that it is intended
to run on a bare metal Intel hardware with PCI serial port support.

lvgl
----

LVGL demo example that demonstrates x86_64 framebuffer feature.

fb
---

Configuration that enables NuttX framebuffer examples.
