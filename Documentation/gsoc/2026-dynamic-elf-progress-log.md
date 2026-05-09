# GSoC 2026 Dynamic ELF Progress Log

## Purpose

This log keeps dated progress evidence for later weekly updates, evaluations,
and final reporting.

## Environment Snapshot

- Main working branch: `gsoc/dynamic-elf-baseline`
- Upstream remote: `apache/nuttx`
- Personal remote: `aviralgarg05/nuttx`
- Primary board: Seeed XIAO ESP32S3 Sense

## Log

### 2026-05-05

#### Completed

- Established real-hardware validation on XIAO ESP32S3 Sense.
- Validated executable Dynamic ELF loading on hardware.
- Found and fixed a XIAO-specific ELF linker-script path issue in:
  `boards/xtensa/esp32s3/esp32s3-xiao/scripts/Make.defs`
- Validated shared-object loading on hardware through `sotest`.
- Added dedicated XIAO `elf` and `sotest` board defconfigs.
- Build-validated both dedicated XIAO defconfigs from clean configure flows.
- Wrote runtime notes and loader-assumptions notes for later reporting and MVP
  scoping.
- Wrote the first package-layer MVP definition, keeping it aligned with the
  proposal's thin lifecycle-helper scope.

#### Evidence

- `elf` path executed successfully on hardware and returned to `nsh>`.
- `sotest` path executed successfully on hardware with visible output:
  - `main: Registering romdisk at /dev/ram0`
  - `main: Mounting ROMFS filesystem at target=/mnt/sotest/romfs with source=/dev/ram0`
  - `module_initialize`
  - `testfunc1: Hello, everyone!`
  - `testfunc2: Hope you are having a great day!`
  - `testfunc3: Let's talk again very soon`
  - `module_uninitialize`
- Shell remained alive after completion.

#### Notes

- USB flash flow on this board requires deliberate BOOT/RESET handling.
- Console interaction on the XIAO CDC ACM path is sensitive to serial control
  lines and line endings during scripted testing.
- Some local `.config` combinations were used only to make runtime output
  visible during validation; these should not be treated as final upstream
  board configuration without cleanup.
- The dedicated board defconfigs are now the reproducible path; the remaining
  local source change is the isolated XIAO linker-script fix.
- The first package-layer slice is now explicitly constrained to local-safe
  lifecycle behavior before broader fetch/update polish.

#### Next

- Commit the isolated XIAO linker-script fix and the dedicated board defconfigs.
- Commit the tracker docs and MVP definition as the reportable baseline.
- Start the thin `system/pkg` skeleton with integration files and command
  dispatch only.

### 2026-05-06

#### Completed

- Kept the package-layer work split into small proposal-aligned units rather
  than expanding multiple milestones at once.
- Added the next `pkg` implementation unit in `nuttx-apps`:
  metadata and local store foundation.
- Added shared internal `pkg` structures and helpers for:
  - manifest field validation
  - on-device repository and storage path layout
  - transaction state naming
  - basic package logging
- Clean-build validated the `pkg` foundation on top of the XIAO `elf`
  configuration.
- Wrote a draft XIAO dynamic-loading guide intended to preserve development
  knowledge for future tutorial cleanup.
- Added the next `pkg` implementation unit in `nuttx-apps`:
  local metadata-backed `install` and `list`.
- Added local JSON-backed repository/index handling and installed-state
  persistence.
- Added self-contained SHA-256 verification inside `pkg` so the install path
  does not depend on unresolved kernel-side crypto symbols.
- Added local compatibility gating against the current board/runtime identity.
- Clean-build validated the new `install`/`list` unit on top of the XIAO `elf`
  configuration.

#### Evidence

- `pkg` continues to register as a builtin command in the XIAO ELF build path.
- The XIAO ELF build still completes successfully with `CONFIG_SYSTEM_PKG=y`.
- The new guide records:
  - reproducible build commands
  - BOOT/RESET flashing procedure
  - expected `elf` and `sotest` success markers
  - common bring-up failures and fixes
- The XIAO ELF build still completes after the `install`/`list` unit and
  produces `nuttx.bin`.
- The local `pkg` command now builds with:
  - local `index.json` parsing
  - local `installed.json` persistence
  - artifact copy/stage helpers
  - pointer file updates for `current` and `previous`
  - script-friendly `pkg list` output

#### Notes

- `pkg update` and `pkg rollback` are still intentionally deferred.
- This unit adds only the first local executable package lifecycle path:
  `install` plus `list`.
- The first executable package lifecycle path is now implemented in code, but
  hardware runtime validation still needs a writable `/data` mount on the
  target board.
- The current install path assumes:
  - `/data/repo/index.json`
  - `/data/repo/installed.json`
  - package payload artifacts available via absolute or repo-relative paths
- Because the current XIAO `elf` path proves loader behavior through ROMFS and
  USB CDC, the remaining runtime gap for `pkg install` is writable package
  storage, not loader capability.

#### Next

- Provision a writable `/data` mount for the XIAO runtime path and validate
  `pkg list` / `pkg install` on target.
- Keep update/rollback execution for the following unit, not the same one.

### 2026-05-08

#### Completed

- Captured the mentor meeting follow-up in a dedicated versioned note.
- Converted the handwritten/meeting direction into phased engineering units:
  - application module-support audit and gap closure
  - host-side export/publish script
  - later library support
  - later crypto-subsystem migration
- Added a host-side audit helper in `apps/tools/audit_module_support.py` to
  classify app-side module readiness based on:
  - `MAINSRC`
  - `MODULE = $(CONFIG_...)`
  - backing Kconfig symbol type (`bool` vs `tristate`)
- Converted the first real application subset from `bool` to `tristate` where
  Makefiles already supported `MODULE = $(CONFIG_...)`.
- Added a host-side repository export helper in
  `apps/tools/export_pkg_repo.py`.
- Closed the first end-to-end target-side `pkg install` / `pkg list`
  validation on the XIAO runtime path using writable `tmpfs`-backed `/data`.
- Confirmed that the ROMFS-backed local package fixture can install the `hello`
  ELF payload on target and persist installed-state metadata.
- Fixed a real runtime stability issue in `pkg` by moving large metadata
  structures out of the task stack and onto heap allocations.
- Fixed a post-success logging bug in `pkg_install()` where the final success
  log referenced manifest data after its backing index buffer had been freed.

#### Evidence

- The new meeting follow-up note preserves the explicit mentor asks:
  - menuconfig module support for applications
  - script to export binaries to server
  - library support as an extra step
- The audit helper gives a repeatable way to identify remaining application
  Kconfig/Makefile gaps rather than guessing from manual tree scans.
- Initial audit result shows that applications already using
  `MODULE = $(CONFIG_...)` are largely backed by `tristate` symbols already,
  so the remaining work is a targeted gap-closure pass rather than a blind
  whole-tree conversion.
- After the first conversion pass, the `BOOL_NEEDS_TRISTATE` bucket dropped to
  zero in the audit helper output.
- The export helper was validated against the built `hello` ELF payload and
  emitted:
  - copied repository artifact under `artifacts/<compat>/<name>/<version>/`
  - package `index.json`
  - SHA-256 value matching the current local fixture/runtime path
- Final target-side runtime validation on the XIAO produced:
  - `pkg: info: layout prepared`
  - `pkg: info: loading index from /data/repo/index.json`
  - `pkg: info: index read complete (213 bytes)`
  - `pkg: info: cJSON_Parse returned success`
  - `pkg: info: parsed manifest hello 1.0.0`
  - `pkg: info: selected hello version 1.0.0`
  - `pkg: info: artifact source /mnt/elf/romfs/hello`
  - `pkg: info: artifact copied to staging`
  - `pkg: info: sha256 computed: 5f66871b19ec24d7d685ce78660fc8039cebb179fb00821a6affde3513ec7e8e`
  - `pkg: info: sha256 verified`
  - `pkg: info: payload staged at /data/pkgs/hello/1.0.0/hello`
  - `pkg: info: manifest written`
  - `pkg: info: compatibility check passed`
  - `pkg: info: installed metadata updated`
  - `pkg: info: installed hello version 1.0.0`
  - `hello current=1.0.0 previous=- type=elf arch=xtensa compat=esp32s3-xiao versions=1.0.0`
- `pkg list` on target returned the same installed-state record after the
  install completed.

#### Notes

- This meeting follow-up changes the order of the next units slightly:
  install/list runtime closure still comes first, but the application-module
  audit is now an explicit workstream rather than an implicit cleanup item.
- Pure libraries should not be mass-converted to `tristate`; the mentor ask
  applies to packageable applications.
- The temporary local SHA-256 implementation should be treated as prototype
  code and replaced later by the NuttX crypto subsystem.
- The current validation build in the XIAO worktree is affected by an existing
  local tree inconsistency around the top-level `chip/` path, so build
  verification for this specific pass should not be interpreted as a failure of
  the `bool -> tristate` conversion itself.
- The main runtime blocker in the first `pkg install` path was not JSON parsing
  itself, but package-task stack pressure. The `pkg_index_s` and installed-db
  structures were large enough to make the original task-stack allocation
  unstable on target.
- Serial automation on the XIAO CDC ACM console is sensitive to stale host-side
  file handles. pyserial-based probing was more reliable than the earlier raw
  nonblocking probe method for end-to-end scripted verification.

#### Next

- Close the next concrete application/module gap subset:
  executable apps that still miss `MODULE = $(CONFIG_...)`.
- Extend the export/publish path to shared-library artifacts after the
  executable install/list path is stable.
- Replace the temporary SHA-256 implementation with the NuttX crypto
  subsystem, following mentor feedback.

## Update Format

For future entries, use:

- Date
- Completed
- Evidence
- Blockers or Risks
- Next
