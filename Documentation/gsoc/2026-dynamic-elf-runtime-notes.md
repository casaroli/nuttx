# GSoC 2026 Dynamic ELF Runtime Notes

## Purpose

This note captures the repeatable hardware-side workflow used to validate the
Dynamic ELF and `sotest` milestones on the Seeed XIAO ESP32S3 Sense.

## Board And Host

- Board: Seeed XIAO ESP32S3 Sense
- Active NuttX branch: `gsoc/dynamic-elf-baseline`
- USB path used during testing: CDC ACM over the board's native USB connector

## Reproducible Build Commands

### XIAO ELF

```sh
./tools/configure.sh -E esp32s3-xiao:elf
make olddefconfig
make -j8
```

### XIAO SOTEST

```sh
./tools/configure.sh -E esp32s3-xiao:sotest
make olddefconfig
make -j8
```

## Flash Procedure

When `esptool.py` cannot talk to the board directly, use ROM download mode:

1. Unplug the board.
2. Hold the tiny `BOOT` button.
3. Replug the board while still holding `BOOT`.
4. Release `BOOT` after about one to two seconds.
5. Flash the generated `nuttx.bin`.

After the image is written:

1. Make sure `BOOT` is released.
2. Tap `RESET` once.
3. Wait a few seconds for the board to leave download mode and re-enumerate.

## Runtime Console Notes

- The board can re-enumerate with a different `/dev/cu.usbmodem*` path after
  flashing or reset.
- USB CDC console interaction on this board is sensitive to serial control
  lines and line endings during scripted testing.
- During automated testing, carriage return (`\\r`) worked more reliably than
  line feed (`\\n`) for shell commands.

## Expected Success Markers

### ELF

The exact banner text may vary slightly, but a successful run should show the
`elf` command executing several test payloads and then returning to `nsh>`.
Observed test coverage in the validated hardware run included:

- `errno`
- `hello`
- `signal`
- `struct`
- `hello++2`
- `hello++3`
- `mutex`
- `pthread`
- `task`

The important success condition is that the test run completes and the board
returns cleanly to `nsh>` without obvious instability.

### SOTEST

The following markers were observed on the successful hardware run:

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

The shell should remain alive after the command completes.

## Temporary Versus Real Changes

- Real source change: XIAO linker-script path fix in
  `boards/xtensa/esp32s3/esp32s3-xiao/scripts/Make.defs`
- Reproducibility changes: dedicated XIAO `elf` and `sotest` board defconfigs
- Temporary experiments that should not be treated as final design:
  alternate syslog routing combinations used only to make output visible during
  bring-up
