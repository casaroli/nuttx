# GSoC 2026 Dynamic ELF Checklist

## Purpose

This document tracks the project strictly against the approved Dynamic ELF
proposal. It is intended to be updated as the work moves forward, but scope
changes must remain aligned with the proposal and with maintainer guidance.

## Current Status

Date: 2026-05-05

- Board in active use: Seeed XIAO ESP32S3 Sense
- Host workflow in active use: Linux-style NuttX flow, tested from local setup
- Real-hardware executable ELF loading: done
- Real-hardware shared-object loading via `sotest`: done
- Reproducible XIAO `elf` config: build-validated
- Reproducible XIAO `sotest` config: build-validated
- Package-layer implementation: not started

## Scope Rules

### In Scope

- Dynamic ELF executable loading on real hardware
- Shared object loading and unloading
- ROMFS-backed test artifact flow as needed for loader validation
- Board-specific fixes required to make the validated path work
- A minimal package-management MVP only after loader behavior is proven
- Reproducibility, documentation, and reportable progress tracking

### Out of Scope Until Later

- Network repository sync logic
- Full install/update/remove UX
- Rollback/version pinning
- Rich metadata formats beyond MVP needs
- Demo-only features that do not help the proposal milestone
- Unrelated board cleanup not needed for Dynamic ELF work

## Phase Checklist

### Phase 0: Workspace And Reporting Discipline

- [x] Create isolated GSoC worktree and branch
- [x] Keep upstream and local work separated from older long-lived branches
- [x] Record board, branch, and runtime decisions
- [x] Start a versioned progress log for later reports
- [x] Keep tracker updated after every milestone or blocker
- [ ] Keep commits small and scoped by purpose

### Phase 1: Executable ELF Baseline

- [x] Validate the control path conceptually before touching package logic
- [x] Bring up NuttX on a real ESP32-S3 board
- [x] Establish reliable flash and console workflow
- [x] Enable ELF loader prerequisites on the board configuration
- [x] Build an ELF-enabled XIAO image successfully
- [x] Flash the ELF-enabled image successfully
- [x] Run the `elf` example from NSH on hardware
- [x] Verify return to shell after test execution
- [x] Verify no obvious post-test instability

### Phase 2: Shared Object Baseline

- [x] Enable `sotest` prerequisites on the XIAO configuration
- [x] Build `modprint` and `sotest` shared objects successfully
- [x] Build and flash a `sotest`-capable XIAO image
- [x] Verify `sotest` starts from NSH on hardware
- [x] Verify ROMFS registration and mount path
- [x] Verify `dlopen` flow for dependent modules
- [x] Verify `dlsym` lookups for test functions and messages
- [x] Verify test function execution output
- [x] Verify cleanup path including module uninitialization
- [x] Verify shell remains alive after `sotest`

### Phase 3: Reproducibility And Upstreamability

- [x] Convert the proven local XIAO ELF settings into a reproducible board config
- [x] Decide whether XIAO needs a dedicated `elf` config in-tree
- [x] Decide whether XIAO needs a dedicated `sotest` config in-tree
- [x] Separate true source fixes from temporary local test config edits
- [x] Keep the XIAO linker-script fix as an isolated upstreamable change
- [x] Write down exact flash and runtime steps for repeat testing
- [x] Capture expected success output for `elf`
- [x] Capture expected success output for `sotest`

### Phase 4: Loader Assumptions Freeze

- [x] Write a short loader/runtime assumptions note
- [x] Record how applications are stored and discovered in the current proof path
- [x] Record which symbol-table path is currently required
- [x] Record what the package layer may assume about startup and cleanup
- [x] Record remaining runtime risks and open questions

### Phase 5: Package MVP Definition

- [x] Define the first package-layer MVP strictly on top of the proven loader path
- [x] Keep the MVP limited to essential lifecycle operations
- [x] Decide the minimal manifest/metadata needs
- [x] Decide where package payloads live on device
- [x] Define what "install" means in MVP terms
- [x] Define what "run" means in MVP terms
- [x] Explicitly defer update/rollback/network features if not required for MVP

### Phase 6: Package MVP Implementation

- [ ] Create the thin package helper or command boundary
- [ ] Support loading a packaged executable artifact
- [ ] Support loading a packaged shared-library artifact if required by MVP
- [ ] Validate error handling for missing payloads or invalid modules
- [ ] Validate that successful load/run paths are reproducible on hardware

### Phase 7: Evidence, Reporting, And Demo Readiness

- [ ] Keep milestone outputs recorded in the progress log
- [ ] Keep a list of tested commands and expected output
- [ ] Keep a list of hardware-specific steps and caveats
- [ ] Prepare a concise weekly report summary format
- [ ] Prepare a concise "current status / next step / blocker" format

## Immediate Next Steps

1. Commit the isolated XIAO linker-script fix together with the dedicated XIAO
   `elf` and `sotest` board configs.
2. Commit the GSoC tracker docs, runtime notes, loader assumptions, and package
   MVP definition as the reporting baseline.
3. Start the thin `system/pkg` skeleton in `nuttx-apps` with command dispatch,
   Kconfig, Makefile, and CMake integration only.
4. Implement the first local-only install/list path before touching update and
   rollback execution paths.

## Milestone Exit Criteria

### Executable ELF Milestone

- `elf` runs on the target board from NSH
- The board returns to `nsh>` after execution
- No immediate loader instability remains unexplained

### Shared Object Milestone

- `sotest` runs on the target board from NSH
- ROMFS registration and mount succeed
- Module initialization and function call outputs appear
- Cleanup succeeds and the board returns to `nsh>`

### Package MVP Entry Gate

- Executable ELF milestone complete
- Shared object milestone complete
- Reproducible board configuration path documented
- Loader assumptions written down
