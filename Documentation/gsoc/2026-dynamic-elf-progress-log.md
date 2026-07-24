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

#### Additional Completed

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

#### Additional Evidence

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

#### Additional Notes

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
- Added the next `nxpkg` implementation unit in `nuttx-apps`:
  metadata and local store foundation.
- Added shared internal `nxpkg` structures and helpers for:
  - manifest field validation
  - on-device repository and storage path layout
  - transaction state naming
  - basic package logging
- Clean-build validated the `nxpkg` foundation on top of the XIAO `elf`
  configuration.
- Wrote a draft XIAO dynamic-loading guide intended to preserve development
  knowledge for future tutorial cleanup.
- Added the next `nxpkg` implementation unit in `nuttx-apps`:
  local metadata-backed `install` and `list`.
- Added local JSON-backed repository/index handling and installed-state
  persistence.
- Added self-contained SHA-256 verification inside `nxpkg` so the install path
  does not depend on unresolved kernel-side crypto symbols.
- Added local compatibility gating against the current board/runtime identity.
- Clean-build validated the new `install`/`list` unit on top of the XIAO `elf`
  configuration.

#### Evidence

- `nxpkg` continues to register as a builtin command in the XIAO ELF build path.
- The XIAO ELF build still completes successfully with `CONFIG_SYSTEM_NXPKG=y`.
- The new guide records:
  - reproducible build commands
  - BOOT/RESET flashing procedure
  - expected `elf` and `sotest` success markers
  - common bring-up failures and fixes
- The XIAO ELF build still completes after the `install`/`list` unit and
  produces `nuttx.bin`.
- The local `nxpkg` command now builds with:
  - local `index.json` parsing
  - local `installed.json` persistence
  - artifact copy/stage helpers
  - pointer file updates for `current` and `previous`
  - script-friendly `nxpkg list` output

#### Notes

- `nxpkg update` and `nxpkg rollback` are still intentionally deferred.
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
  USB CDC, the remaining runtime gap for `nxpkg install` is writable package
  storage, not loader capability.

#### Next

- Provision a writable `/data` mount for the XIAO runtime path and validate
  `nxpkg list` / `nxpkg install` on target.
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
- Closed the first end-to-end target-side `nxpkg install` / `nxpkg list`
  validation on the XIAO runtime path using writable `tmpfs`-backed `/data`.
- Renamed the command and integration path from `pkg` to `nxpkg` following
  maintainer feedback, and revalidated the renamed command on hardware.
- Confirmed that the ROMFS-backed local package fixture can install the `hello`
  ELF payload on target and persist installed-state metadata.
- Fixed a real runtime stability issue in `nxpkg` by moving large metadata
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
- `nxpkg list` on target returned the same installed-state record after the
  install completed.
- After the rename rebuild, the board-side runtime path confirmed:
  - `nxpkg` appears in NSH builtin apps
  - `nxpkg` prints the expected usage line
  - the ROMFS fixture runs `nxpkg install hello`
  - `nxpkg list` still reports the installed package state on target

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
- The main runtime blocker in the first `nxpkg install` path was not JSON parsing
  itself, but package-task stack pressure. The `pkg_index_s` and installed-db
  structures were large enough to make the original task-stack allocation
  unstable on target.
- Serial automation on the XIAO CDC ACM console is sensitive to stale host-side
  file handles. pyserial-based probing was more reliable than the earlier raw
  nonblocking probe method for end-to-end scripted verification.
- The rename was carried through:
  - app path: `system/nxpkg`
  - config symbol: `CONFIG_SYSTEM_NXPKG`
  - builtin program name: `nxpkg`
  - XIAO `elf` validation config
  - ROMFS fixture generation and export tooling references

#### Next

- Close the next concrete application/module gap subset:
  executable apps that still miss `MODULE = $(CONFIG_...)`.
- Extend the export/publish path to shared-library artifacts after the
  executable install/list path is stable.
- Replace the temporary SHA-256 implementation with the NuttX crypto
  subsystem, following mentor feedback.

### 2026-05-12

#### Completed

- Added the first upstream-facing `nxpkg` application documentation page under
  `Documentation/applications/system/nxpkg/index.rst`.
- Kept the page scoped to the current MVP instead of the full future package
  system:
  - local-first install/list flow
  - current on-device layout
  - configuration knobs
  - host-side repository generation with `export_pkg_repo.py`
  - validated runtime flow on the XIAO board
  - explicit current limitations
- Recorded the new documentation requirement in the checklist and meeting
  follow-up notes.
- Re-validated the existing host-side export helper against the built `hello`
  ELF payload and confirmed that it emits a repository layout and `index.json`
  matching the current `nxpkg` metadata parser.
- Closed two additional application/module gap-closure passes in `apps/`:
  - corrected missing `MODULE = $(CONFIG_...)` wiring for a first set of
    already-`tristate` executable applications and tests
  - converted a second set of clear executable apps/tests from `bool` to
    `tristate` and added matching `MODULE = $(CONFIG_...)` wiring
- Fixed the module-support audit helper so it now recognizes valid Makefile
  assignment styles already used in-tree:
  - `MODULE = $(CONFIG_...)`
  - `MODULE := $(CONFIG_...)`
  - `MODULE += $(CONFIG_...)`
- Rebuild-validated the XIAO `esp32s3-xiao:elf` path after both module-support
  cleanup passes.

#### Evidence

- The new page documents:
  - the current `nxpkg` command surface
  - default repository/store paths under `/data`
  - a host-side `export_pkg_repo.py` invocation for the `hello` payload
  - the ROMFS-backed fixture script used during runtime validation
  - the observed board-side `nxpkg list` output
- The page lives under `Documentation/applications/system/`, so it is picked up
  automatically by the existing `system/index.rst` globbed toctree.
- The export helper produced:
  - `artifacts/esp32s3-xiao/hello/1.0.0/hello`
  - `index.json` with a SHA-256 digest matching the exported payload
- The module-support audit moved from:
  - `MAKEFILE_NEEDS_MODULE: 34`
  to:
  - `MAKEFILE_NEEDS_MODULE: 19`
- The follow-up XIAO build still completed successfully and regenerated
  `nuttx.bin`.
- A further nested-tool pass moved the audit again from:
  - `MAKEFILE_NEEDS_MODULE: 19`
  to:
  - `MAKEFILE_NEEDS_MODULE: 18`
- The representative XIAO build still completed successfully after that
  pass as well.

#### Notes

- This new Applications-page documentation is intentionally user-facing and
  smaller than the GSoC report/tracker notes.
- Broader package-system architecture topics, such as BASE vs USERLAND
  separation and future dependency solving, should stay in design discussion
  and later documentation rather than being overloaded into this first page.
- A significant portion of the remaining `MAKEFILE_NEEDS_MODULE` audit bucket
  is now mixed with:
  - library-like directories
  - path/config helper directories
  - multi-command layouts that need per-directory review instead of
    mechanical conversion
- The audit helper itself needed to be corrected before the second pass;
  otherwise valid `:=` and `+=` module assignments would continue to appear as
  false positives.
- The next remaining bucket is now mostly made of:
  - parent package toggles with nested runnable tools
  - pure library directories
  - bootloader-focused apps
  - mixed multi-command layouts
  These need per-directory judgment instead of more mechanical conversion.

#### Completed

- Added another small application/module-support pass focused only on nested
  runnable tools:
  - `NETUTILS_BARE_TEST` was changed from `bool` to `tristate`
  - `baretest` now uses `MODULE = $(CONFIG_NETUTILS_BARE_TEST)` instead of a
    hard-coded builtin-only assignment
  - `TFLITEMICRO_TOOL` was changed from `bool` to `tristate`
  - the TFLite Micro GNU Make path now uses
    `MODULE = $(CONFIG_TFLITEMICRO_TOOL)`
  - the TFLite Micro CMake path now passes
    `MODULE ${CONFIG_TFLITEMICRO_TOOL}` to `nuttx_add_application(...)`
- Rebuild-validated the XIAO `esp32s3-xiao:elf` path after the nested-tool
  changes.

#### Evidence

- The module-support audit now reports:
  - `MAKEFILE_NEEDS_MODULE: 18`
  - `READY: 376`
- The representative XIAO ELF build still completed successfully and
  regenerated `nuttx.bin` after the `baretest` and `tflm` changes.

#### Notes

- This pass deliberately targeted nested runnable tools instead of the parent
  package toggles, because that better matches the maintainer request to add
  module support for applications while leaving pure libraries and bootloader
  infrastructure alone.

#### Next

- Continue with the remaining application module-support gap closure, but keep
  the next subset limited to entries whose executable/module intent is clear.
- Prepare the next documentation/design follow-up if the dev@ discussion turns
  the broader package architecture into a separate design note.
- Extend the export/publish flow toward shared-library artifacts in line with
  the earlier mentor feedback.

### 2026-05-16

#### Completed

- Addressed the remaining actionable code-review feedback on the upstream
  `nxpkg` app PR by replacing the last unbounded metadata-side string
  comparisons with bounded helpers.
- Fixed the module-support audit helper so it no longer reports valid indented
  `MODULE = $(CONFIG_...)` assignments as false positives.
- Added another small application/module-support cleanup pass for clear
  single-symbol executable/test commands:
  - `CRYPTO_CONTROLSE`
  - `NETUTILS_CJSON_TEST`
  - `TESTING_X86_64_ABI`
  - `OPTEE_SUPPLICANT`
- Validated a representative builtin/module dual-mode flow using the XIAO ELF
  configuration and the `cachespeed` benchmark.
- Re-entered the XIAO `sotest` build path long enough to regenerate a real
  shared-library artifact (`modprint`) and validated the exporter against it.
- Replaced the temporary local SHA-256 code in `nxpkg` with the NuttX crypto
  subsystem (`crypto/sha2.h`) and rebuilt the XIAO `elf` image successfully.
- Added an explicit failure-path fixture (`bad-index.json` + `pkgfail.nsh`) and
  validated on hardware that a missing payload returns `-ENOENT` without
  corrupting the installed package state.

#### Evidence

- The upstream app PR fix was pushed on
  `aviralgarg05/nuttx-apps:gsoc/nxpkg-app-pr1`, and the remaining review
  thread on `system/nxpkg/pkg_metadata.c` was resolved after the change.
- The corrected module-support audit now reports:
  - `BOOL_NEEDS_TRISTATE: 0`
  - `MAKEFILE_NEEDS_MODULE: 12`
  - `READY: 382`
- Builtin-mode validation for the representative dual-mode app still completed
  successfully on `esp32s3-xiao:elf` with:
  - `CONFIG_BENCHMARK_CACHESPEED=y`
  - visible registration of `cachespeed` during the build
- Module-mode validation for the same app also completed successfully on
  `esp32s3-xiao:elf` with:
  - `CONFIG_BENCHMARK_CACHESPEED=m`
  - generated artifact:
    `/Users/aviralgarg/Everything/gsoc-dynamic-elf-baseline/apps/bin/cachespeed`
- The regenerated XIAO `sotest` app-side build path produced:
  - `/Users/aviralgarg/Everything/gsoc-dynamic-elf-baseline/apps/bin/modprint`
  - `/Users/aviralgarg/Everything/gsoc-dynamic-elf-baseline/apps/bin/sotest`
- Export validation for the shared-library path produced:
  - `artifacts/xtensa/esp32s3/esp32s3-xiao/modprint/1.0.0/modprint`
  - `index.json` entry with:
    - `type: shared-lib`
    - `arch: xtensa`
    - `compat: esp32s3-xiao`
- The crypto-subsystem migration rebuilt cleanly on `esp32s3-xiao:elf`, and
  the regenerated `nuttx.bin` was flashed and exercised on the board.
- The success-path runtime flow still completed after the hashing change:
  - `elf`
  - `source /mnt/elf/romfs/pkgtest.nsh`
  - `nxpkg list`
- The failure-path runtime flow now returns the expected error when the payload
  is missing:
  - fixture copy:
    `/mnt/elf/romfs/bad-index.json -> /etc/nxpkg/index.json`
  - command:
    `nxpkg install hello-missing`
  - observed result:
    `nxpkg: error: install failed for 'hello-missing': -2`
  - `nxpkg list` still shows only the previously installed `hello` package
    after the failed install attempt.

#### Blockers or Risks

- The remaining module-support audit bucket is no longer mechanical. The 12
  remaining entries are mostly parent-package toggles, pure libraries,
  bootloader-focused apps, or mixed multi-command layouts that need manual
  judgment.
- Reusing the same shared `apps/` output tree across `elf` and `sotest`
  configure/build paths can leave stale objects behind; the baseline XIAO
  `elf` tree must be rebuilt cleanly after temporary `sotest` artifact
  generation before claiming the final state is restored.
- The remaining module-support audit bucket is still the hardest manual-review
  set; none of the 12 entries look safe for a bulk mechanical conversion.
- The current target validation still covers a missing-payload failure path,
  but not yet richer invalid-module cases such as a structurally broken ELF or
  mismatched shared-library payload.

#### Next

- Continue the remaining application/module gap closure only for entries whose
  executable/module intent is unambiguous.
- Extend the package/runtime validation path toward packaged shared-library
  install/use now that the local executable success/failure slices are both
  covered.

### 2026-07-24

Start of the MMU/address-environment stretch track (see
`2026-dynamic-elf-mmu-isolation.md`). Hardware: ESP32-S3-DevKitC-1 v1.1 with an
ESP32-S3-WROOM-2 (N32R8V) module — 32 MB octal flash + 8 MB octal PSRAM.

#### Completed

- **Unit A — expose the ESP32-S3 MMU/WCL/PMS primitives as a callable arch
  API.** Lifted the cache-MMU, World-Controller and PMS (memory-protection)
  helpers out of the file-static scope of `esp32s3_userspace.c` into three new
  reviewable modules — `esp32s3_mmu.{c,h}`, `esp32s3_wcl.{c,h}`,
  `esp32s3_pms.{c,h}` — leaving the protected-mode layout *policy* in
  `esp32s3_userspace.c`. Pure refactor, no behavior change. This is the
  primitive layer the later `up_addrenv_*` select/remap work builds on.
- **Brought BUILD_PROTECTED up on the WROOM-2 board.** Root-caused (with GDB
  over the built-in USB-JTAG) a WROOM-2-specific early-boot crash: the protected
  kernel linker `boards/xtensa/esp32s3/common/scripts/kernel-space.ld` placed
  the octal-flash (OPI) init helpers in mapped flash, so calling them while the
  flash mapping is reconfigured in `__start` faulted (illegal instruction).
  Fixed by IRAM-residing those functions (mirrors the flat sections script).
  WROOM-1 quad-flash configs never hit this, which is why `knsh` "worked" there.

#### Evidence

- `esp32s3-devkit:knsh` (retargeted to WROOM2N32R8V + octal flash + a
  config-matched ESP-IDF second-stage bootloader) boots to an interactive
  `nsh>`; `free` shows the separate `Kmem`/`Umem` kernel/user heaps (protected
  isolation active) and `ostest` passes.
- Unit A additionally validated by byte-identical on-silicon behavior against
  the pristine (pre-refactor) build.

#### Blockers or Risks

- Protected mode on ESP32-S3 requires the LEGACY app format + a second-stage
  bootloader; the config-matched bootloader needs an ESP-IDF build environment.

#### Next

- Unit B: prove the precise-fault → `RFE`-restart primitive on silicon (guard
  page / stack-overflow demo) — the go/no-go gate for the address-environment
  work — then the BUILD_KERNEL units.

### 2026-07-24 — Unit B (recoverable-fault primitive)

#### Completed

- Added the recoverable-fault path (`CONFIG_ESP32S3_PAGEFAULT`, default off):
  `xtensa_user()` (esp32s3_user.c) routes the precise Load/Store/InstrFetch
  Prohibited causes (EXCCAUSE 28/29/20) to a new dispatcher
  `esp32s3_pagefault_dispatch()` (esp32s3_pagefault.{c,h}); on "serviced" it
  returns the register frame so the exception vector's `RFE` re-executes the
  faulting instruction. Non-invasive when off (all three config combinations
  build; `knsh` default is unchanged).
- Added `apps/examples/pffault`, a WORLD1 user task that reads/writes an
  arbitrary address, as the on-target fault-injection harness.
- **Proved the go/no-go gate on silicon** and, in doing so, corrected a key
  assumption: on the ESP32-S3, **PMS (World Controller) memory-protection
  violations are asynchronous** (the level-triggered DRAM0/IRAM0 PMS-monitor
  interrupt), **not** precise restartable exceptions. The precise, restartable
  primitive the address-environment work needs comes instead from the
  **cache-attribute** faults (EXCCAUSE 28/29/20). This re-sources demand
  paging from PMS to the cache-MMU attribute layer, but the primitive itself
  is proven.

#### Evidence

- Address sweep (`pffault`): a WORLD1 load of an out-of-cache-region address
  (`0x0`, `0x4`, `0x80000000`) raises a **precise LoadProhibited (EXCCAUSE 28)
  whose EXCVADDR tracks the accessed address exactly**; PMS-protected regions
  instead take the async PMS-monitor path; in-cache-window unmapped pages read
  0 without faulting.
- RFE-restart proof (`CONFIG_ESP32S3_PAGEFAULT_SELFTEST`, `pffault r
  0x80000000`): the dispatcher returns "serviced" without fixing the address,
  and the console shows the **identical** faulting instruction re-executed
  three times (`restart #1/#2/#3 ... PC=4211cd0f` unchanged) — RFE cleanly
  restarts a faulted precise access — then steps past it and the task resumes;
  `nsh` stays interactive afterward.
- No regression: `esp32s3-devkit:knsh` (WROOM-2) with the feature enabled
  boots to `nsh>` and `ostest` completes with `ostest_main: Exiting with
  status 0` (0 assertion failures, 0 dispatcher faults during the run).
- A genuine unhandled user fault (`pffault r 0x0`) is reported by the
  dispatcher (`EXCCAUSE=28 EXCVADDR=00000000 task=pffault`) and then declined
  to the existing panic path.

#### Blockers or Risks

- Per-task abort (terminate just the faulting WORLD1 task instead of a
  whole-system panic) is not yet implemented; today an unhandled user fault
  still panics. The async PMS-monitor ISR currently `PANIC()`s and its panic
  path crashes in `up_saveusercontext` — both are follow-on work.
- Demand paging (Unit G) must *create* the fault via the cache access-attribute
  mechanism (in-window unmapped pages silently read 0), then service by
  restoring the attribute + mapping a PSRAM page — the core physics are proven,
  that specific mechanism is the remaining build.

#### Next

- Wire the corrected two-path design: async-PMS abort-task isolation, and the
  cache-attribute recoverable path as the basis for `up_addrenv_*` (Units C–F)
  and Variant B demand paging (Unit G).

### 2026-07-25 — Per-task abort (SIGSEGV) + async PMS ISR fix

#### Completed

- Turned the proven fault primitive into a working feature: an unprivileged
  (WORLD1) task that faults is now terminated on its own instead of panicking
  the whole system. Two paths, both gated by `CONFIG_ESP32S3_PAGEFAULT_ABORT`
  (default on; selects `SIG_DEFAULT` + `SIG_SIGKILL_ACTION`):
  - **Synchronous** cache-attribute faults (EXCCAUSE 28/29/20): `xtensa_user()`
    calls `esp32s3_pagefault_abort()`, which delivers a fatal SIGSEGV and
    returns the signal-trampoline frame (no kernel stack required).
  - **Asynchronous** PMS-monitor violations: `pms_violation_isr()` now
    acknowledges/re-arms every monitor latch and delivers SIGSEGV to the
    interrupted WORLD1 task, replacing the previous unconditional `PANIC()`
    (whose panic path also crashed in `up_saveusercontext`).
  - Kernel-mode (WORLD0) faults still panic.

#### Evidence (ESP32-S3-DevKitC WROOM-2)

- `pffault r 0x0` / `pffault w 0x0` (NULL deref, EXCCAUSE 28/29) and
  `pffault r 0x3fc98000` / `pffault w 0x3fc98000` (user access to kernel DRAM,
  async PMS) each terminate only the pffault task; `nsh` stays interactive.
- Repeatable and clean: `free` unchanged after 10+ aborts (no leak), `ps`
  shows no lingering pffault tasks, and no monitor re-fire / reboot loop.
- No regression: `ostest` exits with status 0; the synchronous abort, the async
  abort, and the RFE-restart self-test all coexist.

#### Blockers or Risks

- The async-PMS abort assumes the interrupted task is the violator (true for a
  level-triggered violation taken with interrupts enabled); it confirms
  user-vs-kernel from the interruptee's saved PS before acting.

#### Next

- The BUILD_KERNEL address-environment arc (Kconfig plumbing, `arch_addrenv_s`,
  `up_addrenv_*`) and Variant B demand paging on the proven cache-attribute
  fault primitive.

### 2026-07-25 — Unit G go/no-go: demand paging is not viable on ESP32-S3

#### Completed

- Answered the last open physics question for the address-environment arc:
  can a *present* cache-mapped page be made to fault precisely and restartably
  (the demand-paging primitive)? **No.**
- A read-only probe of the flash MMU table showed that the in-window DBUS
  addresses observed to "read 0" already carry **invalid** entries
  (`SOC_MMU_INVALID`, 0x4000) and yet return 0 with **no exception** — for both
  the kernel and user worlds. So a not-present page is read silently; there is
  no fault to trigger a demand fill.
- The only precise, restartable `*Prohibited` faults are for addresses outside
  any cache region (e.g. `0x0`, `0x80000000`), which cannot be filled to become
  present. Combined with the earlier finding that PMS gating is asynchronous,
  **no in-window mechanism yields a precise, restartable present-but-gated
  fault**. Variant B demand paging (Unit G) and COW/fork (Unit H) are therefore
  **not achievable** on this silicon.

#### Evidence

- MMU-table probe (board late-init): `va=0x3c000000 -> valid`;
  `0x3c800000 / 0x3d000000 / 0x3d800000 -> INVALID (0x4000)`, each reading 0
  with no fault and a clean boot to `nsh`.
- (An earlier probe that actively invalidated an entry with cache
  suspend/resume + an exception-context MMU restore wedged the board into a
  reboot loop; that was the unsafe cache manipulation / re-entrancy -- the
  design note's flagged highest-risk area -- not the invalid access itself,
  which is benign. That throwaway probe was reverted.)

#### Blockers or Risks

- Demand paging / COW are off the table. A `BUILD_KERNEL` address environment
  remains possible but only with **static** per-process memory (no lazy
  stack/heap growth, no demand fill), which bounds it to a few static sandboxes.

#### Next

- Revise the deliverable to: isolation + precise-fault detection + guard-page /
  segfault-kill abort (done), plus optionally a static-memory `BUILD_KERNEL`
  address environment (Units C-F, scoped to no demand paging). Units G/H are
  documented as blocked.

## Update Format

For future entries, use:

- Date
- Completed
- Evidence
- Blockers or Risks
- Next
