# GSoC 2026 Dynamic ELF Loader Assumptions

## Purpose

This note freezes the loader/runtime assumptions that are now justified by
measured hardware behavior, so the package-layer MVP can be scoped without
guesswork.

## Proven Runtime Facts

- Executable ELF loading works on the target board.
- Shared-object loading works on the target board through `sotest`.
- Both proof paths were validated from `nsh` on real hardware, not only from
  build artifacts.

## Artifact Discovery And Storage

- The current proof path uses ROMFS-backed payload discovery.
- Loader-visible artifacts are produced during the normal `apps` build and
  packaged into the test image path used by `elf` and `sotest`.
- The package-layer MVP should assume a simple local payload source first; it
  does not need network fetch or remote sync to demonstrate value.

## Symbol And Linker Assumptions

- The XIAO board needs the current linker script path:
  `libs/libc/elf/gnu-elf.ld`
- The older path under `binfmt/libelf/gnu-elf.ld` is not valid for the current
  tree and breaks the XIAO ELF test build.
- Shared-object loading currently depends on the OS symbol-table path being
  configured for the board/runtime.

## Startup And Cleanup Assumptions

- A successful runtime path begins from a healthy `nsh` session.
- The board must complete ROMFS registration and mount before the payload test
  can execute.
- A successful load/run path must return cleanly to `nsh>`.
- Module initialization and module cleanup are observable and should remain
  visible success criteria for future package-layer milestones.

## Scope Constraints For The Package MVP

- The MVP should build on local, already-proven loader behavior.
- The MVP should not assume repository sync, update logic, rollback, or rich
  metadata as prerequisites.
- The MVP should treat executable package launch as the first-class path.
- Shared-library packaging should only be pulled into MVP behavior if it is
  necessary for the selected demo flow.

## Remaining Risks And Open Questions

- USB CDC console behavior on this board is workable but finicky during
  scripted testing.
- Flashing still depends on deliberate BOOT/RESET handling.
- The eventual package-layer UX should avoid coupling core logic to fragile
  console-output assumptions.
- If future demo goals require richer UI, LCD, or camera behavior, those should
  remain secondary until package lifecycle behavior is stable.
