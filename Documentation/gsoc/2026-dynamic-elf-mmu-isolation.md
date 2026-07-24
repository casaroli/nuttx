# GSoC 2026 Dynamic ELF — MMU / Process Isolation and Recoverable-Fault Design Note

## Status

Research / design exploration. This note freezes the hardware facts and the
architecture options for real process isolation and recoverable page faults on
the ESP32-S3, so the work can be scoped separately from the `nxpkg`
package-layer deliverable.

> **On-silicon correction (2026-07-24, Unit B — supersedes the fault-mechanism
> claim below).** The go/no-go primitive was proven on the ESP32-S3-DevKitC
> WROOM-2 board, and it revised a central assumption of this note. On this
> silicon a **PMS (World Controller) permission violation is *asynchronous*** —
> it raises the level-triggered DRAM0/IRAM0 PMS-monitor interrupt, **not** a
> precise, restartable CPU exception. The precise, restartable primitive that
> demand paging / COW need instead comes from the **cache-attribute** faults
> (`LoadProhibited` / `StoreProhibited` / `InstrFetchProhibited`, EXCCAUSE
> 28/29/20). These were shown to be precise, to carry a **tracking `EXCVADDR`**,
> and — critically — to **`RFE`-restart cleanly** (the identical faulting load
> re-executed on demand with no write-buffer/prefetch corruption). So recoverable
> paging is **viable**, but sourced from the cache-MMU attribute layer, not PMS;
> and PMS-based isolation is a kill-only (async) mechanism, not a restartable
> one. Where the text below says "PMS `*Prohibited`", read "cache-attribute
> `*Prohibited`". See `2026-dynamic-elf-progress-log.md` (2026-07-24, Unit B).

> **On-silicon correction #2 (2026-07-25, Unit G go/no-go — demand paging is NOT
> viable).** A follow-up probe settled whether a *present* cache-mapped page can
> be made to fault precisely and restartably (the demand-paging primitive).
> Result: **no.** Invalidating a page's MMU entry (`SOC_MMU_INVALID`, bit 14)
> makes an access **read 0 silently — it does not fault** (confirmed by reading
> the MMU table: the "reads-0" in-window DBUS slots already carry invalid entries
> yet return 0 with no exception, for both worlds). So a not-present page yields
> no fault to trigger a fill. The only precise, restartable `*Prohibited` faults
> are for addresses with **no cache region at all** (e.g. `0x0`, `0x8000_0000`),
> which cannot be "filled" to become present. Net: **no in-window mechanism gives
> a precise, restartable present-but-gated fault** — MMU-invalid is silent, PMS
> is asynchronous. **Variant B demand paging and copy-on-write (fork) are not
> achievable on this silicon.** What survives: real isolation (WORLD0/WORLD1 +
> PMS), precise-fault detection, and guard-page / segfault-kill (deliver SIGSEGV,
> terminate just the faulting task) — all implemented and validated. A
> `BUILD_KERNEL` address environment is still possible but only with **static**
> per-process memory (no lazy stack/heap growth, no demand fill). See
> `2026-dynamic-elf-progress-log.md` (2026-07-25, Unit G).

This is a **stretch / future-work** track. It sits well above the thin
lifecycle-helper scope of `nxpkg` and should not block or expand the current
package milestones. See `2026-dynamic-elf-pkg-mvp.md` and
`2026-dynamic-elf-delivery-plan.md` for the in-scope work.

## Motivation

Dynamic ELF loading today runs in `CONFIG_BUILD_FLAT`: every loaded ELF shares
the kernel address space with no protection. The natural questions are:

1. Can we get real per-process isolation on this board (separate address spaces,
   fault containment)?
2. Can we catch and recover from page faults (demand paging, copy-on-write,
   guard pages) the way NuttX on-demand paging does on other platforms?

The short answers, verified against the ESP32-S3 Technical Reference Manual and
the ESP-IDF HAL:

- **Full `CONFIG_BUILD_KERNEL` with a general MMU: no.** The Xtensa LX7 has no
  process-isolation MMU with per-context page tables.
- **A constrained, novel per-process isolation model: yes** — by combining the
  cache MMU (translation) with the World Controller (privilege) and PMS
  (per-region permission), driven by reprogramming on context switch.
- **Recoverable page faults / demand paging / copy-on-write: yes** — but only if
  the fault is triggered by a **PMS permission violation** (precise, restartable),
  **not** by an invalid cache-MMU entry (asynchronous, fatal).

## Hardware Facts (verified)

### Build-mode support in-tree

- `arch/xtensa` does **not** declare `ARCH_HAVE_MMU` / `ARCH_HAVE_ADDRENV`.
  No Espressif chip does (ESP32-S3 or the RISC-V ESP32-P4). So `BUILD_KERNEL`
  with separate per-process address spaces is not available.
- `arch/xtensa` **does** `select ARCH_HAVE_MPU`, and the ESP32-S3 already
  supports `CONFIG_BUILD_PROTECTED`.
- The existing protected-mode path (`arch/xtensa/src/esp32s3/esp32s3_userspace.c`)
  **already combines both units we need**: it uses the cache MMU to map the user
  image (see the "required number of MMU pages" routine) and the World Controller
  (`wcl_set_world0_entry`, `_user_exception_vector`) plus internal-SRAM region
  configuration to keep user code out of kernel memory. The hardware primitives
  for the isolation model below are therefore already exercised in-tree.

### The cache MMU

- Maps external SPI flash and PSRAM into the CPU virtual address space at
  **64 KB page** granularity. Internal SRAM, ROM, and peripherals are at fixed
  physical addresses and are **not** translated.
- It is a **single global** translation table shared by both cores. There is
  **no ASID and no swappable page-table-base register**. Changing a mapping means
  rewriting MMU entries and invalidating cache.
- Its `MMU_MEM_CAP_EXEC/READ/WRITE` capabilities describe which *bus*
  (instruction vs data) a virtual window is wired to — **per region, not per page,
  and not a user/kernel privilege check.**
- **Accessing an invalid (unmapped) MMU entry raises an asynchronous cache-error
  interrupt that is fatal** (ESP-IDF: "Guru Meditation Error: Cache error").
  It is not a precise, restartable fault and does not reliably report a faulting
  address. **Do not use unmapped MMU entries as a page-fault trigger.**

### The World Controller (WCL)

- Provides exactly **2 worlds** (`WORLD_0`, `WORLD_1`) — a hard binary
  privileged / non-privileged split. This is the S3's "MPU-class" privilege
  mechanism. Worlds can express kernel vs. user; they **cannot** express N
  isolated processes.

### PMS / memory-protection regions

Per the ESP32-S3 memory-protection HAL:

- IRAM0 (instruction-bus SRAM): **3 split lines → 4 areas** (area 0-3),
  permissions per world and per operation (R/W/X).
- DRAM0 (data-bus SRAM): **2 split lines → 4 areas** (area 0-3), per world/op.
- RTC_FAST: **2 areas** (low/high) per world.
- Split lines are 512 B-aligned.

Consequence: only a **handful** of independently protectable regions exist.
Statically you can gate roughly "kernel region(s) + one active user region" — i.e.
about **one active user sandbox at a time**, not many simultaneous ones.

### Precise, restartable exceptions (the key enabler)

- PMS/region-protection violations raise the classic Xtensa protection causes:
  **`LoadProhibited` / `StoreProhibited` / `InstrFetchProhibited`**
  (EXCCAUSE 28 / 29 / 20).
- These are **precise, synchronous** exceptions. `EXCVADDR` holds the faulting
  data address (for load/store); the faulting instruction address is `EPC1`
  (= PC, which is also the faulting address for instruction-fetch faults).
- Architecturally they are **RFE-restartable**: a handler can service the cause
  and return to re-execute the faulting instruction. ESP-IDF halts on them only
  as *policy* (no page to fill), not because the hardware forbids resuming.
  (This restart behavior must still be proven on silicon — see Open Questions.)

## Isolation Model

Real per-process isolation on the XIAO ESP32-S3 is buildable as a combination of
three units, with responsibilities split:

1. **World Controller (2 worlds)** — static kernel (`WORLD_0`) / user (`WORLD_1`)
   privilege boundary, set once. Protects the kernel from all user code.
   *Already implemented by `CONFIG_BUILD_PROTECTED`.*
2. **Cache MMU** — on a cross-process context switch, remap the active process's
   PSRAM window to its physical pages. Because the whole table is global with no
   ASID, this is a **bounded remap of just the user window** (K = process pages /
   64 KB), plus cache invalidation. It is *not* "rewrite the world": kernel/flash
   mappings stay put, and the remap is skipped entirely when the next task shares
   the current address environment (thread↔thread, syscalls, ISRs).
3. **PMS split lines (4 IRAM0 / 4 DRAM0 areas, per world)** — reprogrammed per
   switch to fence the active process's internal-SRAM footprint (stacks, data)
   from other processes. Permission-only: a few register writes, **no cache
   flush.**

### Simultaneous vs. serialized isolation

- **Static, simultaneous:** ~1 active user sandbox + kernel (bounded by 2 worlds
  and ~4 areas per bus).
- **With per-switch reprogramming:** N processes, **serialized** — one active at
  a time, re-fenced on every cross-process switch. This is what lifts the hard
  region-count cap. The cost per cross-process switch is: MMU window remap +
  cache invalidation + PMS split-line reprogram.

### Honest limits

- Coarse (64 KB pages) and serialized (one active isolated context).
- Suited to a **few, long-lived, not-rapidly-switched** sandboxed apps — not a
  high-frequency many-task workload (cache thrash on every cross-process tick).
- All live isolated code/data must fit the mappable PSRAM virtual window.
- None of the `up_addrenv_*` machinery exists in `arch/xtensa`; this is a
  from-scratch address-environment port coordinating MMU + WCL + PMS.

## Recoverable-Fault / Paging / COW Mechanism

The enabling trick: **never leave a page unmapped in the MMU** (that is the fatal,
asynchronous cache-error path). Instead leave it **MMU-mapped but PMS-marked
no-access** until it is "present." An access then raises a **precise
`*Prohibited` exception** with `EXCVADDR` / `EPC1`, which a handler can service and
resume via `RFE`. This mirrors exactly how a general MMU implements demand paging
and copy-on-write (present + read-only → write → precise permission fault → copy →
make writable → resume).

So on the ESP32-S3 the two hardware roles are:

- **Cache MMU** = translation / relocation (keeps the page *present*, avoiding the
  fatal cache error).
- **PMS** = the recoverable permission fault + the protection boundary.

### Mapping onto NuttX on-demand paging hooks

NuttX `CONFIG_PAGING` requires precise, restartable exceptions and a locked
(never-faulting) region for the handler, worker, and IDLE task. The ESP32-S3
realization would wire:

- `pg_miss()` ← the `LoadProhibited` / `StoreProhibited` / `InstrFetchProhibited`
  exception vector. Reads `EXCVADDR` / `EPC1`, blocks the faulting task at the head
  of the ready-to-run list, signals and priority-boosts the fill worker.
- `up_checkmapping()` ← "is this page already present / permitted?" (guards against
  duplicate fills).
- `up_allocpage()` ← choose a physical page (evict if needed); set the MMU entry
  and PMS permission.
- `up_fillpage()` ← copy/DMA the page content from backing store (SPI flash, or the
  `nxpkg` repository) into the physical page.
- `pg_callback()` → on fill completion, resume the task; `RFE` re-executes the
  faulting instruction transparently.

### Two build variants

- **Variant A — classic paging, no MMU tricks.** Paged text/data lives in internal
  SRAM; not-present pages are PMS no-access; fault → fill from flash → permit →
  resume. Closest to the documented NuttX design; does not touch the cache MMU.
  Good for running an image larger than SRAM.
- **Variant B — isolation-grade.** Pages live in PSRAM, MMU-mapped (present, so no
  fatal cache error) but PMS-gated. The same primitive then delivers demand paging
  + copy-on-write + per-process isolation together.

### High-value, lower-effort uses of the same primitive

Even without full paging, the recoverable permission fault enables:

- **Guard pages** → real stack-overflow detection (no-access page below each stack;
  overflow → precise `StoreProhibited` → kill just that task).
- **Lazy stack / heap growth.**
- **Copy-on-write `fork`** for genuine processes.

## Correction To Earlier Analysis

An earlier read concluded that copy-on-write and demand paging were impossible on
this silicon. That conclusion considered only the invalid-MMU-entry cache error
(which is indeed fatal and non-restartable). It is **superseded**: driving faults
from the **PMS `*Prohibited`** causes gives precise, restartable faults, so COW and
demand paging are on the table — subject to the constraints below.

## Constraints And Open Questions

1. **PMS granularity is the real ceiling.** Only 4 protectable areas per bus.
   Classic paging assumes many independently-present fine pages; here you must
   reprogram split lines per fault to move a coarse "present boundary," or maintain
   a small working-set window. This is coarse-grained paging, not
   4 KB-page-anywhere. The MMU's own capability bits are per-region, not per-page,
   so they cannot substitute for fine permission bits.
2. **Locked-region / IRAM-safe fill discipline is strict.** The exception vector,
   `pg_miss`, the fill worker thread, and the flash-read fill path must all live in
   always-present, always-permitted IRAM/DRAM and be cache-safe — because filling a
   page reads SPI flash, touching the very cache/flash subsystem being faulted on.
   Re-entrancy here causes deadlock or double-fault. This is the ESP-IDF
   `IRAM_ATTR` / `ESP_INTR_FLAG_IRAM` problem, amplified.
3. **Precise-restart must be proven on silicon.** Confirm `EXCVADDR` is populated
   for the data causes and that `RFE` cleanly re-runs a faulted PSRAM-backed
   load/store/fetch (write-buffer and prefetch corner cases).
4. **WCL region count vs. process count.** 2 worlds is a hard binary; the ~4 PMS
   areas per bus are what actually cap simultaneous static isolation. Confirm the
   exact split-line reprogramming cost and any lock/latch behavior that would
   prevent per-switch reconfiguration.
5. **No Xtensa addrenv/paging port exists.** The hooks (`up_checkmapping`,
   `up_allocpage`, `up_fillpage`, `pg_miss`, `up_addrenv_*`) are unimplemented for
   Xtensa, and this realization is unconventional (permission-fault-driven rather
   than translation-fault-driven).

## Suggested Sequencing (if pursued)

1. **Ship first:** bring up a `CONFIG_BUILD_PROTECTED` XIAO config so `nxpkg`-loaded
   ELFs run unprivileged and the kernel is protected. Uses only existing in-tree
   machinery; real, achievable isolation win.
2. **Prove the primitive:** demonstrate a precise `StoreProhibited` on a PMS-gated
   page that a handler services and resumes via `RFE` (e.g. a guard-page /
   stack-overflow demo). This validates the whole recoverable-fault premise cheaply.
3. **Then, as separate research units:** demand paging (Variant A), COW `fork`, and
   the full MMU + WCL + PMS per-process address-environment (Variant B).

Keep each as a small, separately-reportable unit, consistent with the project's
delivery rule.

## References

- NuttX On-Demand Paging:
  https://nuttx.apache.org/docs/latest/components/paging.html
- ESP-IDF Fatal Errors (ESP32-S3) — `*Prohibited` causes, `EXCVADDR`, fatal cache
  error:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/fatal-errors.html
- ESP-IDF Security Overview (ESP32-S3) — World Controller (2 worlds), PMS:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/security.html
- ESP-IDF memory-management API (cache MMU, 64 KB pages, external memory only):
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/mm.html
- ESP-IDF `memprot_ll` HAL (ESP32-S3) — 4 IRAM0 / 4 DRAM0 areas, 2 RTC areas,
  2 worlds:
  https://github.com/espressif/esp-idf/tree/master/components/hal/esp32s3/include/hal
- ESP32-S3 Technical Reference Manual — Cache/MMU and Permission Control chapters:
  https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf
- In-tree reference for combined MMU + WCL + region use:
  `arch/xtensa/src/esp32s3/esp32s3_userspace.c` (the `CONFIG_BUILD_PROTECTED` path)
