# Dynamic Loading on XIAO ESP32S3 Sense

## Purpose

This document captures the practical steps and lessons learned while bringing
up Dynamic ELF loading and shared-object loading on the Seeed XIAO ESP32S3
Sense during the GSoC 2026 work.

It is written as development-facing documentation first, but it is structured
so it can later be turned into a cleaner tutorial for new users.

## Board And Scope

- Board: Seeed XIAO ESP32S3 Sense
- Runtime goals already validated:
  - executable Dynamic ELF loading
  - shared-object loading through `sotest`
- Operator interface:
  - NSH over USB CDC

## What This Does And Does Not Cover

Covered:

- how to build the validated XIAO `elf` and `sotest` targets
- how to flash them on the board
- how to verify successful runtime behavior
- board-specific caveats seen during validation

Not covered:

- package lifecycle helper usage
- network repository sync
- richer shared-library packaging policies

## Source-Level Prerequisite

The XIAO board path needed a linker-script fix for the ELF test path:

- old path: `binfmt/libelf/gnu-elf.ld`
- current path: `libs/libc/elf/gnu-elf.ld`

This change lives in:

- [Make.defs](/Users/aviralgarg/Everything/gsoc-dynamic-elf-baseline/nuttx/boards/xtensa/esp32s3/esp32s3-xiao/scripts/Make.defs)

Without this fix, the XIAO ELF build path does not match the current tree.

## Reproducible Board Configs

Validated board configs:

- [xiao:elf](/Users/aviralgarg/Everything/gsoc-dynamic-elf-baseline/nuttx/boards/xtensa/esp32s3/esp32s3-xiao/configs/elf/defconfig)
- [xiao:sotest](/Users/aviralgarg/Everything/gsoc-dynamic-elf-baseline/nuttx/boards/xtensa/esp32s3/esp32s3-xiao/configs/sotest/defconfig)

These are the preferred starting points instead of ad hoc local `.config`
edits.

## Build Steps

### ELF

```sh
./tools/configure.sh -E esp32s3-xiao:elf
make olddefconfig
make -j8
```

### Shared Objects (`sotest`)

```sh
./tools/configure.sh -E esp32s3-xiao:sotest
make olddefconfig
make -j8
```

Expected output artifact:

- `nuttx.bin`

For the shared-object test path, expected app-side artifacts include:

- `apps/bin/modprint`
- `apps/bin/sotest`

## Flash Procedure

If direct flashing does not work, use ROM downloader mode:

1. Unplug the board.
2. Hold the tiny `BOOT` button.
3. Replug the board while still holding `BOOT`.
4. Release `BOOT` after about one to two seconds.
5. Flash the generated `nuttx.bin`.

After flashing:

1. Make sure `BOOT` is released.
2. Tap `RESET` once.
3. Wait a few seconds for the board to re-enumerate.

## Console Behavior

Observed console path:

- USB CDC ACM over native USB

Important caveats:

- the `/dev/cu.usbmodem*` device node can change after reset or reflashing
- scripted interaction was sensitive to serial control lines
- carriage return worked more reliably than plain line feed in automation

## How To Verify ELF Loading

Success criteria:

- `elf` runs from NSH
- it executes multiple ELF payloads
- the system returns to `nsh>` cleanly

Observed payload coverage in the successful hardware run:

- `errno`
- `hello`
- `signal`
- `struct`
- `hello++2`
- `hello++3`
- `mutex`
- `pthread`
- `task`

The most important check is not exact banner text. The important check is that
the full test path completes and the board remains healthy afterward.

## How To Verify Shared-Object Loading

Success criteria:

- `sotest` starts from NSH
- ROMFS registers and mounts
- module initialization runs
- symbol lookups resolve
- test function calls execute
- module cleanup runs
- the board returns to `nsh>`

Observed success markers:

```text
main: Registering romdisk at /dev/ram0
main: Mounting ROMFS filesystem at target=/mnt/sotest/romfs with source=/dev/ram0
module_initialize
testfunc1: Hello, everyone!
   caller: Hello to you too!
testfunc2: Hope you are having a great day!
   caller: Not so bad so far.
testfunc3: Let's talk again very soon
   caller: Yes, don't be a stranger!
module_uninitialize
```

## Common Problems Seen During Bring-Up

### 1. Flash Fails With No Serial Data

Typical symptom:

- `esptool.py` reports that it failed to connect or received no serial data

Most likely cause:

- the board is not in download mode

Fix:

- repeat the BOOT-hold replug flow

### 2. Board Stays In Downloader Mode

Typical symptom:

- serial output shows `DOWNLOAD(USB/UART0)` and `waiting for download`

Fix:

- release `BOOT`
- tap `RESET`

### 3. Console Appears Silent After Flash

Possible causes:

- board is still in downloader mode
- device node changed
- current USB CDC endpoint needs a fresh reconnect

What to check:

- re-enumerated `/dev/cu.usbmodem*` path
- one clean unplug/replug
- one explicit `RESET`

### 4. ELF Build Fails On XIAO Path

Most likely cause:

- board is still using the outdated linker-script path

Fix:

- use the current XIAO board script state with the linker-script path under
  `libs/libc/elf/gnu-elf.ld`

## Why This Matters For The Package Work

The package-layer work depends on exactly these loader properties:

- executable ELF payloads can be loaded from storage
- shared libraries can be loaded and resolved
- the board can return cleanly to NSH after the operation

Because those facts are now proven on hardware, later `pkg` work can build on
measured behavior instead of assumptions.
