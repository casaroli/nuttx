# GSoC 2026 Dynamic ELF — BUILD_KERNEL + eager fork() handoff

Self-contained handoff for continuing the Xtensa/ESP32-S3 address-environment
port toward a working `CONFIG_BUILD_KERNEL` with ELF execution and an
**eager-copy `fork()`**. Read this first, then the design note
`2026-dynamic-elf-mmu-isolation.md` and the delivery plan.

## Goal

Bring true `CONFIG_BUILD_KERNEL` (per-process address environments, real
processes) to the ESP32-S3 and implement `fork()` on it. Copy-on-write and
demand paging are **proven infeasible** on this silicon (no synchronous,
restartable write fault — see the progress log), but `fork()` does **not**
require them: it is implemented **eagerly** (allocate the child's pages and
copy the parent's regions up front, clone the address environment). Target is
a small number of long-lived, statically-sized isolated processes.

## Status (2026-07-25)

**DONE — the entire `up_addrenv_*` API is implemented and the BUILD_KERNEL
kernel image compiles and links** (first time on esp32s3), with all addrenv
code inside it.

**NEXT — the BUILD_KERNEL user init-image.** The link stops at
`nuttx_user.elf`; the current `CONFIG_PASS1_BUILDIR` still builds the
*protected* userspace blob, which is the wrong model for a kernel build.

## Hardware / build environment

- Board: **ESP32-S3-DevKitC-1** with an **ESP32-S3-WROOM-2 (N32R8V)** module —
  32 MB octal flash + 8 MB octal PSRAM. Not the XIAO the older docs assume.
- Toolchain / kconfig / esptool live under `<workspace>/.tools/`; `source
  .tools/env.sh`. genromfs was compiled from source into `.tools/venv/bin`
  (no Homebrew formula). Full details in the auto-memory
  `nuttx-esp32s3-build-env.md`.
- Serial: CP2102 UART bridge at `/dev/cu.usbserial-2140` (flash + console,
  115200); native USB-JTAG at `/dev/cu.usbmodem*` (OpenOCD
  `board/esp32s3-builtin.cfg`).
- `CONFIG_MM_PGSIZE` was extended to allow 32768/65536 (commit `e6a105169e`);
  the addrenv uses **65536** so one `mm_pgalloc()` page == one 64 KB cache-MMU
  page (naturally 64 KB-aligned by the granule allocator).

## The ESP32-S3 address-environment model (how it differs from RISC-V)

There is **no general paging MMU and no page-table-base register**. External
PSRAM is reached through one **global** cache-MMU remap table with **separate
instruction-bus (.text) and data-bus (.data/.bss/heap) windows** at 64 KB
granularity. Consequences baked into the port:

- `struct arch_addrenv_s` (in `arch/xtensa/include/arch.h`) is **not**
  page-table-based. It holds `textvbase/datavbase/heapvbase/heapsize` plus
  **physical-PSRAM-page arrays** `textpages[]/datapages[]/heappages[]` (one
  64 KB page per entry) and `ntext/ndata/nheap` counts.
- "Mapping" a page just **records** it in the addrenv array. The global table
  is (re)programmed only in **`up_addrenv_select`**.
- `up_addrenv_select` = the novel core: fast-path return if the environment is
  already resident; else suspend the data cache, rewrite the IBUS (.text) and
  DBUS (.data/heap) window entries to this group's pages via
  `esp32s3_mmu_map_ibus/dbus(SOC_MMU_ACCESS_SPIRAM, vaddr, page, 1)` (one page
  at a time — `mm_pgalloc` pages are not contiguous), invalidate the
  instruction cache, resume. Instruction fetch keeps running from the
  unchanged flash mapping via ICACHE, so `select` is safe to execute from
  flash (no IRAM placement needed).
- `find_page(addrenv, vaddr)` = classify the window, index the array.
- `page_vaddr`/`pa_to_va` = fixed page-pool offset translation
  (`esp32s3_pgvaddr`), because the pgpool is permanently kernel-mapped.

### Locked design decisions

1. **User pages live in octal PSRAM**, via the cache-MMU windows.
2. **Isolation = the MMU remap alone, not per-select PMS.** All processes
   share the same window VAs; only one environment is resident at a time, so a
   running process sees only its own pages. PMS (WORLD1 ↔ cache-window access,
   WORLD0 ↔ everything) is therefore set **once at boot**; `select` does not
   touch it. (This deviates from the original plan's "reprogram PMS split
   lines in select", which was for the abandoned demand-paging model.)
3. **User stacks come from the process heap** — `CONFIG_ARCH_STACK_DYNAMIC`
   and `CONFIG_ARCH_KERNEL_STACK` are off, so no ustack/kstack allocators are
   needed. `up_addrenv_mprot` has no in-tree callers and is omitted.
4. **`fork()` is eager** — `up_addrenv_clone` is a plain descriptor memcpy
   (threads of a group *share* the environment); the process-duplicating
   `fork()` is a separate higher-level op (see below).

### ⚠ Deferred isolation-hardening item (do not lose)

`up_addrenv_select` only remaps the pages a group actually uses. Window
entries **above** a group's page count still point at the previously-resident
group's pages, so a misbehaving task that touches its window beyond its own
allocation could reach stale mappings. A well-behaved task never does, and the
guard-page/SIGSEGV abort (`CONFIG_ESP32S3_PAGEFAULT_ABORT`) is the interim
backstop, but **full isolation needs the unused window entries invalidated**.
Deferred to on-target bring-up because invalidating cache-MMU entries is
sharp-edged (an invalid in-window entry reads 0 silently, it does not fault).
There is a `TODO(Unit F hardening)` comment at the exact spot in
`esp32s3_addrenv.c`.

## What is committed (branch `gsoc/dynamic-elf-baseline`)

Newest first (this thread's work):

- `4f34a9853f` xtensa: enable the syscall privilege path for BUILD_KERNEL
  (chip_macros.h WCL privilege macros `BUILD_PROTECTED`→`!BUILD_FLAT`;
  xtensa_swint.c SYS_signal_handler → `ARCH_DATA_RESERVE->ar_sigtramp` under
  KERNEL, like riscv_swint)
- `ff683866ef` fix `up_addrenv_pa_to_va` return type (`void *`, not uintptr_t)
- `f32134db35` Unit E part 3 — clone/attach/detach
- `69e905cfd7` Unit E part 2 — **up_addrenv_select** cache-MMU remap
- `ffb8544ac7` Unit E part 1 — create/destroy/vtext/vdata/vheap/heapsize +
  find_page/page_vaddr/user_vaddr/page_wipe/pa_to_va/va_to_pa +
  esp32s3_addrenv.h helpers
- `e6a105169e` mm/pgalloc — 32 KB / 64 KB page-size support
- `9c1c4cc487` Unit D — `struct arch_addrenv_s`
- `bb61a86d0e` Unit C — advertise MMU/addrenv capability (BUILD_KERNEL
  selectable; flat/protected byte-identical)
- `9d3867af55` doc — clarify eager fork() is not blocked

Files: `arch/xtensa/src/esp32s3/esp32s3_addrenv.{c,h}`,
`esp32s3_addrenv_utils.c`, `Make.defs` (addrenv gated `ARCH_ADDRENV`;
mmu/pms/wcl gated `BUILD_PROTECTED||ARCH_ADDRENV`); `arch/xtensa/include/arch.h`;
`arch/xtensa/Kconfig`; `arch/xtensa/src/esp32s3/chip_macros.h`;
`arch/xtensa/src/common/xtensa_swint.c`; `include/nuttx/pgalloc.h`; `mm/Kconfig`.

All units are nxstyle-clean and build-gated so existing flat/protected configs
are unchanged. (Note: the addrenv `.c` files cannot be syntax-checked standalone
because the real `esp32s3_mmu.h` pulls the esp-hal header chain; they were
stub-compiled during development and are now validated by the real BUILD_KERNEL
build.)

## Reproducing the BUILD_KERNEL build probe

Not yet saved as a board defconfig (it does not fully link — the user image
fails). From `nuttx/`, `source ../.tools/env.sh`, then:

```sh
./tools/configure.sh esp32s3-devkit:knsh
kconfig-tweak --disable CONFIG_BUILD_PROTECTED
kconfig-tweak --enable  CONFIG_BUILD_KERNEL
kconfig-tweak --enable  CONFIG_ARCH_USE_MMU
kconfig-tweak --enable  CONFIG_ARCH_ADDRENV
kconfig-tweak --enable  CONFIG_SCHED_LPWORK
kconfig-tweak --enable  CONFIG_MM_PGALLOC
kconfig-tweak --set-val CONFIG_MM_PGSIZE 65536
kconfig-tweak --enable  CONFIG_ARCH_PGPOOL_MAPPING
kconfig-tweak --set-val CONFIG_ARCH_TEXT_VBASE   0x42800000   # IBUS window
kconfig-tweak --set-val CONFIG_ARCH_TEXT_NPAGES  8
kconfig-tweak --set-val CONFIG_ARCH_DATA_VBASE   0x3d000000   # DBUS window
kconfig-tweak --set-val CONFIG_ARCH_DATA_NPAGES  8
kconfig-tweak --set-val CONFIG_ARCH_HEAP_VBASE   0x3d200000   # DBUS window
kconfig-tweak --set-val CONFIG_ARCH_HEAP_NPAGES  16
kconfig-tweak --set-val CONFIG_ARCH_PGPOOL_PBASE 0x400000     # PSRAM offset
kconfig-tweak --set-val CONFIG_ARCH_PGPOOL_VBASE 0x3c400000   # kernel PSRAM VA
kconfig-tweak --set-val CONFIG_ARCH_PGPOOL_SIZE  4194304      # DECIMAL (int)
make olddefconfig && make -j8
```

The kernel image builds; the link then fails at `nuttx_user.elf`.

**Gotchas:** `ARCH_PGPOOL_SIZE` is Kconfig type `int` → must be **decimal**,
not hex. The memory map above is a **first guess** and must be validated
on-target — the user windows must not collide with the kernel's own flash
rodata / text mappings or the kernel PSRAM (pgpool) window in the same
IBUS/DBUS ranges (DBUS `0x3C000000-0x3E000000`, IBUS `0x42000000-0x44000000`;
the kernel's dynamic PSRAM map starts at `esp32s3_spiram.c`'s
`g_mapped_vaddr_start`). Cache-MMU paddr for PSRAM is a **0-based offset**, so
setting `ARCH_PGPOOL_PBASE` to the pool's PSRAM offset makes a page value equal
its MMU paddr directly.

## Remaining work

### Next unit — BUILD_KERNEL user init-image (the blocker to first boot)

The link fails because `CONFIG_PASS1_BUILDIR=boards/xtensa/esp32s3/common/kernel`
builds the **protected** `esp32s3_userspace.c` descriptor (undefined
`nsh_main` / `up_signal_handler` / `g_mmheap`, no `user_start` entry). That
model is protected-only. BUILD_KERNEL wants the init process as a **separate
user ELF** loaded at runtime. Options to work through:
- an init-process build dir that links a real user program at
  `CONFIG_ARCH_TEXT_VBASE` against the user libc, entry `user_start`/`_start`;
- `CONFIG_INIT_FILEPATH` pointing at an ELF in a mounted FS (ROMFS/embedded),
  loaded by binfmt's ELF loader through the addrenv path;
- verify the syscall trap, `crt0.c` user entry (already present), and the
  user linker layout land text at the IBUS window and data at the DBUS window.

### Boot wiring (Unit F, after the image builds)

At startup: map all PSRAM (as `esp32s3_spiram.c` does,
`cache_dbus_mmu_set(SOC_MMU_ACCESS_SPIRAM, …, paddr=0, …)`) →
`mm_pginitialize(ARCH_PGPOOL_VBASE, ARCH_PGPOOL_SIZE)` for the pgpool → set PMS
**once** (WORLD1 gets the user cache windows, WORLD0 everything) → hand the
init task an address environment. Then bring up to `nsh` under BUILD_KERNEL via
OpenOCD/GDB (expect iterative EPC1 root-causing, as in the protected WROOM-2
bring-up).

### Eager fork() (final task)

On a booting BUILD_KERNEL: implement the process-duplicating `fork()` as
`up_addrenv_create(child)` sized to the parent's regions → copy each parent
page into the child's page through the kernel `page_vaddr` mappings (memcpy) →
set the child's saved return frame (return 0 in the child, child PID in the
parent). No COW, no fault handling. Costs full RAM duplication + copy time per
fork; no lazy stack/heap growth — acceptable for the static-sandbox target.

## References

- Design note: `2026-dynamic-elf-mmu-isolation.md`
- Delivery plan / checklist: `2026-dynamic-elf-delivery-plan.md`,
  `2026-dynamic-elf-checklist.md`
- Silicon findings (why COW/demand-paging are dead): the 2026-07-24/25 entries
  in `2026-dynamic-elf-progress-log.md`
- RISC-V template being mirrored: `arch/risc-v/src/common/riscv_addrenv*.c`,
  `arch/risc-v/include/arch.h`
