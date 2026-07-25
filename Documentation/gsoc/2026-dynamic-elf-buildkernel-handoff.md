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

**DONE — the entire `up_addrenv_*` API is implemented, and a complete
BUILD_KERNEL image builds end to end**: the kernel links, the user programs
build as fully linked ELFs, and the ROMFS image carrying them is linked into
the kernel. Verified on the host; nothing has been run on silicon yet.

The user init-image, previously the blocker, is resolved. A kernel build does
**not** use the two-pass `CONFIG_PASS1_BUILDIR` model at all — that builds the
*protected* userspace blob, which is the wrong shape. `CONFIG_BUILD_2PASS` is
simply off, and the init process is a separate ELF loaded at runtime from a
ROMFS image, as on `rv-virt:knsh_romfs` and `canmv230:knsh`. Getting there
needed five pieces, all committed:

- the kernel-mode trampolines (`up_task_start`, `up_signal_dispatch`,
  `xtensa_dispatch_syscall`) were gated on `BUILD_PROTECTED` — now
  `!BUILD_FLAT`, as on RISC-V;
- `up_allocate_pgheap()` and `pgalloc()` for the ESP32-S3 (new
  `esp32s3_pgalloc.c`);
- `up_addrenv_mprot()`, which the ELF loader requires — necessarily a no-op
  here, see below;
- `ARCH_HAVE_ELF_EXECUTABLE`, a `-r` that is now conditional, and `crt0.o` in
  the export package, so a user program links as an `ET_EXEC`;
- the board's `scripts/gnu-elf.ld` and boot ROMFS plumbing.

An `apps/bin/init` (NSH) comes out as `ET_EXEC` with `.text` at `0x42800000`
(entry `0x42800008`) and `.rodata`/`.data`/`.bss` from `0x3d010000`, in two
PT_LOAD segments matching the two cache-MMU windows, and its 369 664-byte
ROMFS image lands in the kernel's `.flash.rodata`.

**★ IT BOOTS.** On 2026-07-25 the ESP32-S3 reached an interactive NSH under
`CONFIG_BUILD_KERNEL`, with `/system/bin/init` running as a user ELF process
in its own address environment out of PSRAM:

```
load_absmodule: Successfully loaded module /system/bin/init
exec_module: Initialize the user heap (heapsize=1048576)

NuttShell (NSH)
nsh> uname -a
NuttX 0.0.0 ... xtensa esp32s3-devkit
nsh> free
      total       used       free  ...  name
     386080       9608     376472  ...  Kmem
    4194304    1245184    2949120  ...  Page
```

`Kmem` is the internal DRAM kernel heap; `Page` is the PSRAM pool, with
1 245 184 bytes (19 × 64 KB) held by init's text, data and heap.

**NEXT — the kernel stack.** A *second* user process cannot be spawned yet;
the cause is diagnosed exactly and is the first item under "Remaining work".

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
   needed. `up_addrenv_mprot` **is** required after all — the ELF loader calls
   it — but it can only return `OK`: a cache-MMU entry has no permission bits,
   so a mapped user page is always readable and writable by its owner and a
   process can write to its own `.text`. Isolation between groups is
   unaffected; it comes from the window remap, not from page permissions.
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

- `4b862af4cb` esp32s3-devkit: user-program layout and boot ROMFS for kernel
  builds (`scripts/gnu-elf.ld` linking `.text`/`.data` at the window
  addresses; `src/romfs.h`, `src/romfs_stub.c`, the romdisk registration in
  `esp32s3_bringup.c`, and the `src/Make.defs` selection that compiles the
  placeholder away instead of swapping it out)
- `ff6cf241bb` xtensa/esp32s3: accept `up_addrenv_mprot`
- `f3c29741ef` xtensa: support fully linked ELF programs
  (`ARCH_HAVE_ELF_EXECUTABLE`; `-r` conditional on
  `CONFIG_BINFMT_ELF_RELOCATABLE`; `crt0.o` in the export package)
- `3029abe27c` xtensa/esp32s3: page pool and heap growth for BUILD_KERNEL
  (`esp32s3_pgalloc.c` — `up_allocate_pgheap()` and `pgalloc()` — plus
  `esp32s3_addrenv_mapnew()`)
- `6bf42e73eb` xtensa: build the kernel-mode trampolines for BUILD_KERNEL
  (`BUILD_PROTECTED` → `!BUILD_FLAT` in `common/Make.defs`)
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

## Reproducing the BUILD_KERNEL build

Not yet saved as a board defconfig — it builds, but the memory map is
unvalidated and the boot wiring is missing, so it cannot boot yet. A
`savedefconfig` of the working config is worth regenerating before it is
committed as, say, `esp32s3-devkit:kernel`.

From `nuttx/`, `source ../.tools/env.sh`, then:

```sh
./tools/configure.sh esp32s3-devkit:knsh
kconfig-tweak --disable CONFIG_BUILD_PROTECTED
kconfig-tweak --disable CONFIG_BUILD_2PASS      # no protected userspace blob
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

# The init process and the other user programs: separate ELFs in a ROMFS
# image, mounted at /system/bin and exec'd from there.

kconfig-tweak --enable  CONFIG_ELF
kconfig-tweak --enable  CONFIG_BINFMT_ELF_EXECUTABLE
kconfig-tweak --disable CONFIG_BINFMT_ELF_RELOCATABLE
kconfig-tweak --enable  CONFIG_FS_ROMFS
kconfig-tweak --enable  CONFIG_LIBC_EXECFUNCS
kconfig-tweak --enable  CONFIG_LIBC_ENVPATH

# CONFIG_ARCH_KERNEL_STACK defaults to CONFIG_LIBC_EXECFUNCS, so enabling the
# latter switches it on -- and this port assumes it off (user stacks come
# from the process heap; the Unit B.1 SIGSEGV delivery depends on there being
# no kernel stack).  Nothing implements up_addrenv_kstackalloc() on Xtensa,
# so leaving it on does not link.  Order matters: disable it after
# LIBC_EXECFUNCS.

kconfig-tweak --disable CONFIG_ARCH_KERNEL_STACK
kconfig-tweak --enable  CONFIG_SCHED_WAITPID
kconfig-tweak --enable  CONFIG_SCHED_HAVE_PARENT
kconfig-tweak --enable  CONFIG_INIT_MOUNT
kconfig-tweak --set-str CONFIG_INIT_MOUNT_TARGET "/system/bin"
kconfig-tweak --set-val CONFIG_INIT_MOUNT_FLAGS  0x1
kconfig-tweak --set-str CONFIG_INIT_FILEPATH     "/system/bin/init"
kconfig-tweak --set-str CONFIG_PATH_INITIAL      "/system/bin"
kconfig-tweak --enable  CONFIG_SYSTEM_NSH
kconfig-tweak --set-str CONFIG_SYSTEM_NSH_PROGNAME "init"
kconfig-tweak --enable  CONFIG_NSH_FILE_APPS

# Real hardware: WROOM-2 octal flash + octal PSRAM, and a single self-
# contained image.  ESP32S3_APP_FORMAT_LEGACY is only "default y if
# BUILD_PROTECTED"; a kernel build is one image, so turning it off selects
# SIMPLE_BOOT and the image flashes whole at 0x0 with no ESP-IDF bootloader.
# Note this board wants STR sampling, not FLASH_SAMPLE_MODE_DTR, which
# esp32s3_spiflash.c rejects outright for octal mode.

kconfig-tweak --disable CONFIG_ARCH_CHIP_ESP32S3WROOM1N4
kconfig-tweak --enable  CONFIG_ARCH_CHIP_ESP32S3WROOM2N32R8V
kconfig-tweak --disable CONFIG_ESP32S3_FLASH_MODE_DIO
kconfig-tweak --enable  CONFIG_ESP32S3_FLASH_MODE_OCT
kconfig-tweak --enable  CONFIG_ESP32S3_SPI_FLASH_USE_32BIT_ADDRESS
kconfig-tweak --disable CONFIG_ESP32S3_APP_FORMAT_LEGACY
kconfig-tweak --enable  CONFIG_ESP32S3_SPIFLASH
kconfig-tweak --enable  CONFIG_ESP32S3_SPIRAM
kconfig-tweak --enable  CONFIG_ESP32S3_SPIRAM_MODE_OCT

# The page pool's physical base is the PSRAM *offset* of its virtual base,
# and PSRAM lands at 0x3c0a0000 on this image -- see below.

kconfig-tweak --set-val CONFIG_ARCH_PGPOOL_PBASE 0x360000

# The 2048-byte defaults are too small once NSH is a user process.

kconfig-tweak --set-val CONFIG_INIT_STACKSIZE 8192
kconfig-tweak --set-val CONFIG_POSIX_SPAWN_DEFAULT_STACKSIZE 8192
kconfig-tweak --set-val CONFIG_ELF_STACKSIZE 8192

make olddefconfig && make -j8
```

Flash the whole image at 0x0 (kill OpenOCD first if it is attached, it holds
the target halted):

```sh
esptool.py -c esp32s3 -p /dev/cu.usbserial-2140 -b 460800 \
           write_flash 0x0 nuttx.bin
```

**Changing any of the flash/PSRAM options needs `make clean`.** The
esp-hal-3rdparty objects do not depend on `sdkconfig.h`, so an incremental
build silently keeps the old settings.

`INIT_MOUNT_SOURCE` and `INIT_MOUNT_FSTYPE` default to `/dev/ram0` and
`romfs`, which is what the board's romdisk registration provides.

That first `make` links the kernel against the empty ROMFS placeholder. Build
the user programs against it and link the real image in:

```sh
make export
cd ../apps
./tools/mkimport.sh -z -x ../nuttx/nuttx-export-*.tar.gz
make import
./tools/mkromfsimg.sh ../nuttx/arch/xtensa/src/board/board/romfs_boot.c
cd ../nuttx && make -j8
```

`romfs_boot.c` is a generated artifact and is deliberately not committed; the
path above resolves through the board symlinks to
`boards/xtensa/esp32s3/esp32s3-devkit/src/`. Check the result with
`xtensa-esp32s3-elf-nm nuttx | grep romfs_img` — the symbol must be `D`, not
absent, and `.flash.rodata` should grow by the image size.

**Note `zsh` does not word-split unquoted variables**, so a `for` loop over
tweak strings silently passes each one as a single argument and applies
nothing. Run them one per line, and check `.config` afterwards.

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

### Next unit — the kernel stack (precisely diagnosed on target)

Spawning a **second** user process crashes in `_window_overflow12`, called
from `addrenv_select()` (`sched/addrenv/addrenv.c:355`), storing to
`0xffffffd0` — a frame pointer of zero, minus the `s32e` offset.

The cause is a design decision that turns out to be wrong. With
`CONFIG_ARCH_KERNEL_STACK=n` the kernel runs on the **user** stack, and
`addrenv_select()` reprograms the data-bus window part way through its own
execution: the stack disappears from under the kernel's feet, and the next
register-window spill writes into whatever the new process has there. A
kernel build needs a stack that does not move when the address environment
does.

So: implement `up_addrenv_kstackalloc()` and `up_addrenv_kstackfree()`
(mirror `riscv_addrenv_kstack.c`) and enable `CONFIG_ARCH_KERNEL_STACK`.
Note it *defaults* to `CONFIG_LIBC_EXECFUNCS`, so it turns itself on — it
was only forced off earlier because nothing implemented the allocator. The
earlier "user stacks come from the process heap, kernel stack off" decision
recorded in this document is superseded for that reason.

### The memory map — measured, still worth tidying

The map is no longer guesswork — it was read off the hardware over JTAG. The
cache-MMU table base is `0x600c5000`, entry `i` at `+i*4`, each entry being
`physical_page | 0x8000` (PSRAM) with `0x4000` marking it invalid. On the
booting image:

- PSRAM is mapped at **`0x3c0a0000`-`0x3c8a0000`**, i.e. entries **10-137**
  (`mmu_valid_space()` picks the first entry after the flash mappings, which
  end at index 9). Entry 10 reads `0x8000` — PSRAM page 0. So the page pool
  at VA `0x3c400000` is PSRAM offset `0x360000`, which is what
  `CONFIG_ARCH_PGPOOL_PBASE` must be.
- User `.text` at IBUS `0x42800000` is entry **128**, which reads `0x8040`
  (PSRAM page 64) after `up_addrenv_select()` — while its neighbour entry
  129 still reads `0x8077`, PSRAM page 119, the kernel's own mapping.

**That is the collision this section warned about, confirmed:** entry 128
lies inside the kernel's PSRAM window, so mapping user text there destroys
the kernel's view of PSRAM page 118. It is harmless only by luck — the pool
(PSRAM `0x360000`-`0x760000`, pages 54-117) stops just short of it. The
windows should still be moved to indices above 137, or the pool and the
kernel window shrunk to make room, before anything else depends on this.

The rest of this section is the reasoning that led there, kept because it
explains why the layout has to be chosen deliberately. The ESP32-S3 has
**one** 512-entry cache-MMU table **shared by both buses**: the entry index is
`(vaddr & SOC_MMU_VADDR_MASK) >> 16` with `SOC_MMU_VADDR_MASK == 0x1FFFFFF`,
and `ext_mem_defs.h` carries a `_Static_assert` that the IRAM0 and DRAM0
linear addresses are the same. So IBUS `0x42000000 + X` and DBUS
`0x3C000000 + X` are the *same table entry* — which is exactly how the kernel
sees its own flash as code at `0x42xxxxxx` and as rodata at `0x3Cxxxxxx`.

Two consequences:

1. A user `.text` page at IBUS `0x42800000` (index 128) is also visible as
   data at DBUS `0x3C800000`, and a user data page at DBUS `0x3D000000`
   (index 256) is visible as code at IBUS `0x43000000`. That is inherent to
   the chip; note it rather than fight it.
2. **The current guessed map is unsafe.** `esp32s3_spiram.c` maps PSRAM at
   `mmu_valid_space()`, i.e. immediately after the last used flash entry, and
   8 MB of PSRAM is 128 entries. Starting from a low index that easily reaches
   index 128 — the guessed `ARCH_TEXT_VBASE`. The guessed
   `ARCH_PGPOOL_VBASE = 0x3c400000` (index 64) may or may not fall inside what
   SPIRAM actually mapped, since that placement is computed at runtime.

So the pgpool and the user windows must be placed **deliberately**, not left
to `esp32s3_spiram.c`'s dynamic choice, and `CONFIG_ARCH_PGPOOL_VBASE` must be
a compile-time constant that provably lands inside the kernel's PSRAM
mapping. Decide the partition of the 512 entries (kernel flash / kernel PSRAM
pgpool / user text / user data / user heap) first, then make the boot code
enforce it. Note also that enabling PSRAM on this WROOM-2 config needs
`CONFIG_ESP32S3_SPIRAM` + `SPIRAM_MODE_OCT` + `SPIFLASH`, and that
`flash_ops.o` must be deleted by hand after that config change (stale-object
trap — see the build-environment memory note).

### Boot wiring (after the map is fixed)

At startup: map the PSRAM pgpool at its chosen entries (as `esp32s3_spiram.c`
does, `cache_dbus_mmu_set(SOC_MMU_ACCESS_SPIRAM, …, paddr=0, …)`) →
`up_allocate_pgheap()` already reports `ARCH_PGPOOL_PBASE`/`SIZE` to
`mm_pginitialize()` → set PMS **once** (WORLD1 gets the user cache windows,
WORLD0 everything), per the isolation-via-remap decision. Then bring up to
`nsh` under BUILD_KERNEL via OpenOCD/GDB (expect iterative EPC1 root-causing,
as in the protected WROOM-2 bring-up).

First things to check on target, in order: does it reach `board_late_initialize`
and register `/dev/ram0`; does `/system/bin` mount; does binfmt load
`/system/bin/init` (watch `up_addrenv_create` for `-E2BIG`, meaning a region
needs more pages than `ARCH_*_NPAGES`); does `up_addrenv_select` produce a
sane window; does the first user instruction at `0x42800008` fetch.

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
