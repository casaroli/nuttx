# GSoC 2026 Dynamic ELF Package MVP Definition

## Purpose

This document defines the first package-layer MVP strictly on top of the
loader/runtime behavior that is already proven on hardware.

## Scope Anchor

The proposal keeps `pkg` as a thin lifecycle helper under
`nuttx-apps/system/pkg`. It is not a full distro-style manager. The first MVP
must stay limited to lifecycle orchestration on top of existing NuttX
services.

## Primary Command Surface

The MVP command surface stays limited to:

- `pkg install <name>`
- `pkg update <name>`
- `pkg list`
- `pkg rollback <name>`

Not part of the primary MVP command set:

- `pkg run`
- SAT-style dependency resolution
- full remove policy
- graphical browser or richer UI

## First Implementation Boundary

The first implementation slice should focus on the smallest end-to-end path
that still proves package lifecycle value:

1. command dispatch under `system/pkg`
2. local metadata load/store
3. integrity verification
4. staged install into immutable version directory
5. compatibility gate
6. activation via current/previous pointer metadata
7. list and rollback behavior over that state

Network transport should stay behind an abstraction boundary and may be stubbed
or handled through a local repository path first. The core milestone is safe
lifecycle behavior, not network plumbing.

## On-Device Data Model

The MVP should use the proposal-aligned layout:

- `/data/repo/index.json`
- `/data/repo/installed.json`
- `/data/pkgs/<name>/<version>/`
- `/data/pkgs/<name>/current`
- `/data/pkgs/<name>/previous`
- `/data/pkgs/<name>/.txn`
- `/data/tmp/pkg/<name>-<version>.npkg`

Version directories are immutable after staging. Activation changes pointer
metadata only.

## Minimal Metadata

The initial manifest/index data should be limited to fields needed for safe
activation:

- package name
- semantic version
- target architecture
- payload type (`elf` or `shared-lib` when needed later)
- artifact path or locator
- SHA-256 digest
- direct dependency list
- compatibility identifier or equivalent runtime fingerprint

Anything beyond this should be deferred unless the implementation proves it is
strictly necessary.

## Command Semantics

### Install

`pkg install <name>` in MVP terms means:

1. resolve the target version from local index metadata
2. acquire transaction lock
3. fetch or copy artifact into temporary path
4. verify digest
5. stage immutable payload directory
6. run compatibility checks
7. atomically switch `current` pointer
8. preserve previous active version in `previous`
9. update installed metadata and cleanup temporary state

### Update

`pkg update <name>` means the same pipeline, but only for a newer compatible
version selected from index metadata.

### List

`pkg list` reads installed metadata and reports package name, installed
versions, and current/previous pointers in a script-friendly way.

### Rollback

`pkg rollback <name>` validates that a previous version exists, performs an
atomic pointer reversal, updates installed metadata, and confirms that the
active pointer now references the last-known-good version.

### Run

`pkg run` is intentionally not part of the MVP. Runtime invocation continues to
use the already-proven NSH and loader path directly.

## Compatibility Gate

The proposal's strict check order is retained:

1. architecture equality
2. required loader/config prerequisites
3. firmware-package compatibility identifier
4. optional minimum firmware/runtime version gate

Any failure must abort before activation.

## Dependencies

Dependency handling remains intentionally narrow:

- direct dependencies only
- explicitly listed in metadata
- no recursive solver beyond deterministic direct handling
- no SAT or apt/dnf-class behavior

## Shared-Library Scope

Shared-library packaging is in-scope for the project, but it should not be the
first package-layer implementation slice. The first package helper milestone
should prove safe package lifecycle for executable payloads first. Library
payload support can be layered on after the initial transaction engine and
metadata flow are stable.

## Proposed Code Layout

Initial code ownership should follow the proposal's split:

- `pkg_main.c`
- `pkg_manifest.c`
- `pkg_hash.c`
- `pkg_store.c`
- `pkg_compat.c`
- `pkg_txn.c`
- `pkg_rollback.c`
- `pkg_log.c`

Deferred until needed by the first safe end-to-end path:

- `pkg_repo.c`
- `pkg_fetch.c`

Integration files:

- `system/pkg/Kconfig`
- `system/pkg/Make.defs`
- `system/pkg/Makefile`
- `system/pkg/CMakeLists.txt`

## Acceptance Gate For Starting Code

The implementation can start if all of the following are true:

- executable ELF loading is proven on hardware
- shared-object loading is proven on hardware
- reproducible XIAO board configs exist
- runtime notes and loader assumptions are written
- package MVP boundary is frozen in writing

## Deferred Items

These remain outside the first MVP slice even if they remain in wider project
scope later:

- remote repository sync as a hard requirement for the first milestone
- rich metadata expansion beyond activation safety
- package browser UI
- secure-boot redesign
- broad board-portability policy
- advanced shared-library version policy
