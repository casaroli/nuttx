# GSoC 2026 Dynamic ELF Delivery Plan

## Purpose

This note keeps the implementation aligned with the proposal timeline while
forcing the work into small, reviewable units instead of one large drop.

## Rule

Do not batch multiple milestone classes into one change unless they are tightly
coupled and impossible to review separately.

## Current Status Against Proposal Timeline

Already completed ahead of package-layer coding:

- Dynamic ELF hardware baseline
- Shared-object hardware baseline
- Reproducible XIAO `elf` and `sotest` board configs
- Runtime notes, loader assumptions, and package MVP definition

## Delivery Sequence

### Unit 1: XIAO Baseline Cleanup

Scope:

- isolated XIAO linker-script source fix
- dedicated XIAO `elf` defconfig
- dedicated XIAO `sotest` defconfig
- GSoC tracking and runtime documentation

Why separate:

- this closes the loader baseline cleanly
- easy to review and report
- no package-layer code mixed in

### Unit 2: `system/pkg` Skeleton

Scope:

- `apps/system/pkg/`
- `Kconfig`
- `Make.defs`
- `Makefile`
- `CMakeLists.txt`
- `pkg_main.c` command dispatch shell
- usage/help text
- no real lifecycle logic yet

Why separate:

- proves build integration only
- keeps CLI and file layout review independent from lifecycle logic

### Unit 3: Metadata And Local Store Foundation

Scope:

- manifest/index structure definitions
- installed-state metadata handling
- local filesystem layout creation
- transaction file and pointer file helpers

Why separate:

- this is the first real lifecycle substrate
- it should be reviewed before command behavior depends on it

### Unit 4: Install And List Path

Scope:

- local-only install pipeline
- integrity verification
- compatibility gate
- staged activation
- `pkg list`

Why separate:

- gives the first end-to-end package milestone
- easier to validate before update/rollback complexity is added

### Unit 5: Update And Rollback

Scope:

- update selection
- current/previous pointer handling
- rollback execution
- interrupted-transaction safety checks

Why separate:

- rollback semantics deserve isolated review
- aligns with the proposal's safety emphasis

### Unit 6: Shared-Library Packaging Path

Scope:

- direct dependency metadata
- packaged `.so` handling
- one dependent ELF app flow

Why separate:

- maps directly to the shared-library objective
- should land only after executable package lifecycle is stable

## Timeline Mapping

Proposal-aligned flow:

- Weeks 1-2: ELF/shared-object baseline plus metadata shape
- Weeks 3-4: board/filesystem readiness and reproducibility
- Weeks 5-6: thin lifecycle helper command surface
- Weeks 7-8: shared-library reuse and direct dependencies
- Weeks 9-10: rollback hardening and failure injection
- Weeks 11-12: hardware validation, cleanup, documentation

Current interpretation for this repo:

- baseline work is already complete
- next coding step starts at the `thin lifecycle helper` stage
- shared-library packaging stays after executable package lifecycle
- hardening and cleanup stay as later explicit units, not mixed into the first
  package PRs

## Reporting Format

For every delivery unit, record:

- scope
- commands/tests run
- evidence
- blocker if any
- next unit

## Immediate Next Unit

Start `Unit 2: system/pkg skeleton`, and do not mix it with metadata/store or
rollback logic in the same commit.
