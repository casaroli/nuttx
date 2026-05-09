# GSoC 2026 Dynamic ELF Meeting Follow-Up

## Date

2026-05-08

## Purpose

This note captures the high-confidence outcomes from the mentor meeting and
turns them into concrete engineering work items that remain aligned with the
approved Dynamic ELF proposal.

## High-Confidence Meeting Direction

- Keep the work incremental and reportable.
- Continue from the already-proven loader/runtime foundation.
- Keep the package-manager path simple first:
  - executable apps first
  - shared-library/module support later
- Keep the checklist and progress report updated during development.

## Direct Mentor Follow-Up Items

Alan explicitly requested:

1. Add menuconfig module support for applications:
   convert eligible app Kconfig entries from `bool` to `tristate` when Dynamic
   ELF/module loading is supported.
2. Add a host-side script to export binaries to a server/package repository.
3. Add library support as a later extra step.

Halysson additionally recommended:

- Replace the temporary local SHA-256 implementation in `pkg` with the NuttX
  cryptographic subsystem.

## Interpretation For The Current Project

### Already Proved

- Executable Dynamic ELF loading works on XIAO ESP32S3 Sense.
- Shared-object loading works on the same board via `sotest`.
- The project can now build package/lifecycle work on a real validated runtime
  foundation rather than assumptions.

### Immediate Package Focus

The current package path should remain centered on executable applications
before expanding into more complex module/library packaging cases.

This means:

- close the current `pkg install` / `pkg list` target validation path
- keep the local-first repository/index flow
- avoid jumping to advanced dependency or remote sync logic yet

## Implementation Units From The Meeting

### Unit A: Application Module Support Audit And Gap Closure

Goal:

- ensure menuconfig-exposed applications can be configured as builtin (`y`) or
  loadable module (`m`) wherever that path is valid

Planned work:

- audit app directories with `MAINSRC`
- identify executable applications that still miss `MODULE = $(CONFIG_...)`
- identify any remaining `bool` application entries that should be `tristate`
- validate at least representative builtin/module flows on the Dynamic ELF path

Notes:

- pure libraries should remain `bool`
- this is an application audit, not a blind whole-tree conversion

### Unit B: Export/Publish Script

Goal:

- generate a reproducible package repository from built artifacts

Planned work:

- scan `apps/bin`
- pick packageable artifacts
- compute integrity metadata
- emit/update repository `index.json`
- place outputs into a server-friendly directory layout

Initial implementation can stay local-directory based. Transport-specific steps
such as `scp`, `rsync`, or FTP can come after the repository layout is stable.

### Unit C: Library Support

Goal:

- support packaging of shared-library/module artifacts in addition to simple
  executable apps

Planned work:

- extend manifest schema with package type distinctions
- extend export script to publish library/module payloads
- extend install path to place and activate these artifacts correctly
- validate runtime loading through the existing `dlopen`/`dlsym` proof path

This unit should remain later than the simple executable install/list path.

### Unit D: Crypto Cleanup

Goal:

- replace the temporary local SHA-256 implementation with the NuttX crypto
  subsystem once the current target-side install issue is isolated

Reasoning:

- the current custom SHA-256 path was pragmatic prototype code
- the subsystem-based path is the intended final direction

## Current Priority Order

1. Close the target-side `pkg install` / `pkg list` validation path.
2. Start the module-support audit/gap closure work.
3. Add the export/publish script.
4. Add library support after executable packaging is stable.
5. Replace the temporary SHA-256 implementation with the crypto subsystem.

## Deliverable Rule

Each of the above units should remain:

- small
- reviewable
- separately reportable
- aligned with the proposal rather than expanded into a general package-manager
  redesign
