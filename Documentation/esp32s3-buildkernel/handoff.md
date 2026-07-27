# BUILD_KERNEL on ESP32-S3: handoff

Self-contained handoff for continuing the Xtensa/ESP32-S3 address-environment
port. Everything needed to pick this up cold is here: what works, what does
not, how to build and flash it, how to debug it, and what to do next. Read
this first; the design note `mmu-isolation.md` is the background.

Branch: `esp32s3/build-kernel`, in both `nuttx/` and `apps/`.

**It is no longer based on Apache master, and it is no longer developed in a
workspace of its own.** The branch now lives in `/Users/marco/ia/nuttx-fork/`
— the fork/vfork semantics workspace — on top of that work, which gives
`fork()`, `vfork()` and `task_fork()` separate meanings and separate
syscalls. It supersedes the generic half of what this port used to carry, so
Xtensa is wired onto it rather than duplicating it (§7.3, §7.4). In
particular the port's own `CONFIG_ARCH_FORK_STACK_INHERIT` is gone: stack
inheritance is unconditional there now. The apache-master-based line is
preserved at the `pre-semantics-rebase` / `apps-pre-semantics` tags, and the
older standalone workspace is `/Users/marco/ia/nuttx-distro/`.

Build and run it with `run/xtensa.sh` at that workspace's top level; it
carries the traps that have cost sessions here, and `run/README.md` lists
them alongside the other architectures'. The toolchain is still the one in
`/Users/marco/ia/nuttx-distro/.tools`, reached through a `.tools` symlink.

The other half of the conversation is `fork-vfork-semantics-issue.md` (the
original proposal) and `fork-stack-semantics-followup.md` (our feedback),
both at the `nuttx-distro` workspace root.  Their §2 — a `fork()` child must
keep the parent's stack address — is what the semantics branch implemented,
and this port is the architecture that cannot fake it.

The dated experiment log this port was originally developed alongside lived in
`Documentation/gsoc/2026-dynamic-elf-progress-log.md` on the older
`gsoc/dynamic-elf-baseline` branch, which is preserved at the
`pre-upstream-rebase` tag. Everything from it that still matters — the
go/no-go results, the on-target bug list — is reproduced in §7 and §8 here.

---

## 1. Where this stands

**`CONFIG_BUILD_KERNEL` boots to an interactive NSH on the ESP32-S3, user
programs run from the shell, the kernel is protected from them, and `ostest`
runs to `Exiting with status 0` including its `fork()` and `vfork()` tests**
(§7.5). As of 2026-07-27, on the WROOM-2 board:

```
load_absmodule: Successfully loaded module /system/bin/init
exec_module: Initialize the user heap (heapsize=1048576)

NuttShell (NSH)
nsh> uname -a
NuttX 0.0.0 ... xtensa esp32s3-devkit
nsh> free
      total       used       free  ...  name
     386080       9608     376472  ...  Kmem     <- internal DRAM kernel heap
    4194304    1245184    2949120  ...  Page     <- PSRAM page pool
nsh> ls /system/bin
 dd  getprime  init  ostest  pffault  sh
nsh> /system/bin/getprime
thread #0 finished, found 1230 primes, last one was 9973
Done
/system/bin/getprime took 1200 msec
nsh> pffault r 0x3fc90954              <- read the kernel's data
pffault: user-space read of kernel addr 0x3fc90954 ...
pms_violation_isr: SIGSEGV (PMS) task pffault: PC=42c00164
nsh>                                   <- only pffault died
```

`/system/bin/init` is a real user ELF process with its own address
environment, its text and data in PSRAM behind the cache MMU, and a 1 MB
process heap. `getprime` is a second process that loads, runs and exits.

Programs can be run repeatedly from the shell; `ps` shows no zombies, and the
page pool returns to its 1245184-byte baseline after each run once the
deferred free has drained.

The kernel/user boundary is real and measured (§7.1): a user process reaches
neither the kernel's data nor its code, its vectors or a peripheral, and an
attempt kills only that process.  `ostest` runs to `Exiting with status 0`,
including the signal handler test.

**Processes are also isolated from each other** as of `c63c939817` (§7.2).
The page pool is no longer mapped into the kernel; a pool page is reachable
only through a two-entry scratch region, for the duration of one operation.
The measurement that drove it, and the one that closed it:

```
before:  nsh> pffault r 0x3c410000
         pffault: SURVIVED unexpectedly, read 42c09118   <- the shell's data page
after:   nsh> pffault r 0x3c410000
         pffault: SURVIVED unexpectedly, read 00000000   <- entry 64 invalid
```

Read that second line with §8's warning in mind: a zero read is not evidence
on its own. The evidence is the JTAG dump (pool entries 64–127 = `0x4000`)
and the fact that `0x3c420000` went from `ffffffff` to `00000000` — mapped
erased PSRAM to an invalid entry. See §7.2.

### What is broken

1. A violation by the kernel itself is still a whole-system panic, by
   design: only a WORLD1 violation is survivable, and it is the interrupted
   task's saved PS that tells the two apart.
2. Nothing else known.  The two failures this section used to list --
   `vfork()` hanging and `fork_test` returning `ENOMEM` -- are fixed; §7.5
   has both diagnoses.  What is left there is coverage, not repair.

### Scope reminder: what the silicon already ruled out

Copy-on-write and demand paging are **proven infeasible** on this chip —
there is no synchronous, restartable *write* fault (details and the
experiments are in the progress log). `fork()` does not need them: it is to
be implemented **eagerly** (allocate the child's pages, copy the parent's
regions, clone the address environment). The target envelope is a small
number of long-lived, statically sized isolated processes.

---

## 2. Hardware and build environment

- Board: **ESP32-S3-DevKitC-1** with an **ESP32-S3-WROOM-2 (N32R8V)** module —
  32 MB octal flash, 8 MB octal PSRAM. Not the XIAO the older docs assume.
- Toolchain, kconfig shims and esptool live under `<workspace>/.tools/`;
  `source ../.tools/env.sh` from `nuttx/`. `genromfs` was built from source
  into `.tools/venv/bin` (no Homebrew formula).
- Serial: the CP2102 bridge is flash + console (115200, esptool auto-reset
  works); `/dev/cu.usbmodem*` is the native USB-JTAG used by OpenOCD —
  different port, no conflict.  **The device name changes with the USB port
  it is plugged into** — it has been `/dev/cu.usbserial-2140` and
  `/dev/cu.usbserial-1140`; check `ls /dev/cu.*` rather than assuming.
  `.tools/console.py` takes `NUTTX_CONSOLE_PORT` to override its default.
- Debugger (both present, outside the repo):
  - `~/.espressif/tools/openocd-esp32/v0.11.0-esp32-20221026/openocd-esp32/bin/openocd`
  - `~/.espressif/tools/xtensa-esp-elf-gdb/12.1_20221002/xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb`

---

## 3. The ESP32-S3 address-environment model

There is **no general paging MMU and no page-table base register**. External
PSRAM is reached through **one global cache-MMU remap table**, shared by the
instruction and data buses, at 64 KB granularity. Consequences baked into the
port:

- `struct arch_addrenv_s` (`arch/xtensa/include/arch.h`) is **not**
  page-table based. It holds `textvbase/datavbase/heapvbase/heapsize` plus
  **physical page arrays** `textpages[]/datapages[]/heappages[]` (one 64 KB
  page each) and the counts `ntext/ndata/nheap`.
- "Mapping" a page only **records** it in the array. The global table is
  (re)programmed in **`up_addrenv_select()`**, which is the novel core of the
  port: fast-path return if the environment is already resident, else suspend
  the data cache, rewrite the IBUS (.text) and DBUS (.data/heap) window
  entries to this group's pages, invalidate, resume.
- `CONFIG_MM_PGSIZE` is **65536**, so one `mm_pgalloc()` page is exactly one
  cache-MMU page. `mm/pgalloc` was extended to permit 32 K/64 K (`2956eaf7f0`).

### The single shared table — the fact that bites

The entry index is `(vaddr & SOC_MMU_VADDR_MASK) >> 16` with
`SOC_MMU_VADDR_MASK == 0x1FFFFFF`, and `ext_mem_defs.h` carries a
`_Static_assert` that the IRAM0 and DRAM0 linear addresses are equal.
Therefore **IBUS `0x42000000 + X` and DBUS `0x3C000000 + X` are the same
table entry** — the same physical page seen two ways. This is how the kernel
sees its own flash as code at `0x42xxxxxx` and as rodata at `0x3Cxxxxxx`.

Three things follow, all of which have already caused bugs:

- Text does not need mapping "into both buses" — it already is.
- Text must be *written* through the data-bus alias (NuttX does this via
  `up_textheap_data_address()`, which subtracts `0x6000000`), then made
  visible to instruction fetch by a cache writeback + I-cache invalidate.
- One global table means a table entry reused by a different process — or by
  the kernel's own PSRAM window — can leave **stale instruction-cache lines**
  behind. They read back as zeroes.

### The memory map, measured over JTAG

Table base `0x600c5000`, entry `i` at `+i*4`. Entry format:
`physical_page | 0x8000` (SPIRAM), with `0x4000` marking it invalid.

| what | address | entry | observed |
|---|---|---|---|
| kernel PSRAM window | `0x3c0b0000`–`0x3c8b0000` | 11–138 | entry 11 = `0x8000` (PSRAM page 0) |
| page pool | (not mapped) | 64–127 | all `0x4000` since `c63c939817` — PSRAM offset `0x350000`, 4 MB |
| user `.text` (IBUS) | `0x42c00000` | 192–199 | `0x8035` |
| user `.data` (DBUS) | `0x3d000000` | 256–263 | `0x8036`, `0x8037` |
| user heap (DBUS) | `0x3d200000` | 288–303 | `0x8038`…`0x8047` |
| kernel scratch (DBUS) | `0x3d400000` | 320–321 | `0x4000` at rest; one page while the kernel works on it |

The pool's *virtual* base is no longer configured. `up_allocate_pgheap()`
derives it from what the cache MMU reports the PSRAM window maps, checks the
pool's physical range lies inside it, and then takes the mapping away. Only
`ESP32S3_PGPOOL_PBASE`/`_SIZE` remain, and they are physical.

`esp32s3_spiram.c` maps PSRAM at `mmu_valid_space()`, i.e. immediately after
the last entry the flash mappings occupy — so the window **moves as the
kernel image grows**, and it already has: it was at entry 10 when this table
was first written and is at entry 11 now.

Everything derived from where that window lands has to be checked against the
hardware rather than assumed, because getting it wrong is nearly silent.
`up_allocate_pgheap()` panics on any of:

- the PSRAM window base maps nothing;
- the pool's physical range does not lie inside the mapped PSRAM, asking the
  cache MMU (`esp32s3_mmu_paddr()`) rather than deriving it;
- any two windows overlap, or one overlaps the kernel's, compared **by
  entry**, since the two buses share one entry per 64 KB.

The user `.text` window used to sit at entry 128, inside the kernel window,
which is what the overlap check exists to catch. It is now at entry 192.

The old check that `CONFIG_ARCH_PGPOOL_PBASE` matched what the MMU mapped at
`CONFIG_ARCH_PGPOOL_VBASE` is gone because the constant it guarded is gone:
the pool's virtual base is derived, so it cannot drift. That drift had
already happened twice — see the fifth entry in §8's bug list — and this is
the fix for the class, not just the instance.

---

## 4. Locked design decisions

1. **User pages live in octal PSRAM**, via the IBUS (.text) and DBUS
   (.data/heap) cache windows.
2. **Isolation comes from the window remap alone, not from per-select PMS.**
   All processes share the same window virtual addresses and only one
   environment is resident at a time, so PMS is to be programmed **once at
   boot**; `up_addrenv_select()` does not touch it.
   **Corollary, and the whole of §7.2:** this only works if the *kernel* has
   no window onto the pool either. The pool is therefore unmapped, and the
   kernel reaches a page through the scratch region, holding `sched_lock()`
   so that no user task can be running while a mapping is live.
3. **`up_addrenv_mprot()` cannot be honoured.** A cache-MMU entry has only a
   valid bit, a memory type and a page number — no permission bits. It
   returns OK, and a process can therefore write its own `.text`. Isolation
   between groups is unaffected. It is *not* a no-op though: it is the
   loader's "text is now executable" moment and does the cache sync.
4. **Each thread has a kernel stack** (`CONFIG_ARCH_KERNEL_STACK`). **This
   supersedes the earlier "user stacks come from the process heap, kernel
   stack off" decision**, which was wrong: a system call runs its body in the
   calling thread's context, so without a kernel stack the kernel runs on the
   *user* stack — which `up_addrenv_select()` then remaps out from under it.
5. **`fork()` is eager.** `up_addrenv_clone()` is a plain descriptor memcpy
   (threads of a group *share* an environment); the process-duplicating
   `fork()` is a separate higher-level operation.

### The former deferred hardening item — done

`up_addrenv_select()` used to remap only the pages a group actually uses,
leaving the window entries **above** a group's page count pointing at the
previously resident group's pages. Those are now invalidated
(`esp32s3_mmu_unmap()`). The sharp edge is still real and worth knowing: an
invalid in-window entry reads 0 silently rather than faulting, so this was
proved over JTAG, not from the shell (§7.2).

---

## 5. What is committed

Newest first, on `gsoc/dynamic-elf-baseline`:

| commit | what |
|---|---|
| `4c260f497f` | **eager `fork()`** — a child gets its own memory (§7.3), and `vfork()` split off (§7.4) |
| `710c168cd6` | **`binfmt/elf`: `nx_priority == 0` means "default"** — an upstream bug, see below |
| `21fc83d861` | kernel_oct stack sizes, after upstream moved them into the ELF |
| `c63c939817` | **the page pool is no longer mapped** — processes are isolated from each other (§7.2) |
| `7677557cd5` | user cache-MMU window cleanup — see the note below |
| `b859e49c3d` | **PMS permissions** — the kernel/user boundary is enforced (§7.1) |
| `790a2120ca` | **signal delivery** to a user process (§8, seventh bug) |
| `3a47869397` | **WORLD1 vector table** and the world/entry setup (§7.1) |
| `e15f277b94` | the world split moved into `esp32s3_isolation.c` |
| `514b26a956` | the board defconfig `esp32s3-devkit:kernel_oct` (§6.1) |
| `eb3c2d9957` | **addrenv_switch() on voluntary context switch** — the spawn fix |
| `133798d75a` | per-thread **kernel stack** |
| `7cacecadde` | doc: first BUILD_KERNEL boot |
| `6df87015da` | **cache coherency** for loaded text + page-pool validation |
| `2895ec8c27` | `sig_trampoline` in crt0 for BUILD_KERNEL |
| `3c8f7da049` | `up_allocate_kheap()` for BUILD_KERNEL |
| `cc3a6fc032` | **IRAM placement** for kernel builds (`ARCHLIB` macro) |
| `00cecc30b4` | board `gnu-elf.ld` + boot ROMFS plumbing |
| `95ec66b509` | `up_addrenv_mprot()` |
| `c24550f1c8` | fully linked ELF programs on Xtensa |
| `c8ae883a95` | `esp32s3_pgalloc.c` — page pool and heap growth |
| `44a7e2d6a4` | kernel-mode trampolines gated `!BUILD_FLAT` |
| `0267135a00` | syscall privilege path for BUILD_KERNEL |
| `6c8383e5e1` `7628274dba` `678b0a8bd6` | Unit E — the `up_addrenv_*` set |
| `2956eaf7f0` | `mm/pgalloc` 32 K/64 K page support |
| `8b70856f04` `a5514a8e01` | Units D and C — `arch_addrenv_t`, capability Kconfig |

Everything is gated so that existing **flat and protected builds are
unchanged**; `esp32s3-devkit:elf_oct` (flat, silicon-validated) and
`esp32s3-devkit:knsh` (protected, boots to nsh) both still build clean.

**Not committed:** `boards/xtensa/esp32s3/esp32s3-devkit/src/romfs_boot.c` is
a generated artifact and is deliberately untracked.

`7677557cd5` was the window cleanup, landed as one commit as a checkpoint
before the page-pool rework. It carries three independent changes, which is
how to read it and how to split it if it goes upstream. Note that
`c63c939817` builds on it and removed the first row's `PBASE` check by
removing the constant it guarded, so the two no longer revert independently:

| files | what |
|---|---|
| `esp32s3_mmu.{c,h}`, `esp32s3_pgalloc.c`, `kernel_oct/defconfig` | `PBASE` corrected to `0x350000`, `esp32s3_mmu_paddr()`, and the boot check that catches the next drift |
| `esp32s3_mmu.{c,h}`, `esp32s3_addrenv.c` | `esp32s3_mmu_unmap()` and the unused-entry invalidation in `up_addrenv_select()` |
| `kernel_oct/defconfig`, `scripts/gnu-elf.ld`, `esp32s3_pgalloc.c` | text window entry 128 → 192, plus the overlap check |

Note the first of those is a **data-leak fix, not hygiene** — reverting the
commit to unblock something else reinstates one unwiped page per region.

---

## 6. How to build, flash and debug

### 6.1 Configure

From `nuttx/`, `source ../.tools/env.sh`, then:

```sh
./tools/configure.sh esp32s3-devkit:kernel_oct
make -j8
```

That defconfig is the protected `knsh` configuration retargeted at this
board and switched to a kernel build. What it sets, and why:

- **Build model.** `BUILD_KERNEL`, `ARCH_USE_MMU`, `ARCH_ADDRENV`,
  `MM_PGALLOC`, `SCHED_LPWORK`. `BUILD_2PASS` is *off*: the second pass
  builds the protected userspace blob, which a kernel build has no use for
  (§2).
- **The map** (§3): `ARCH_TEXT_VBASE=0x42c00000` in the IBUS window with 8
  pages; `ARCH_DATA_VBASE=0x3d000000` and `ARCH_HEAP_VBASE=0x3d200000` in
  the DBUS window with 8 and 16; the kernel scratch region
  `ARCH_KMAP_VBASE=0x3d400000` with `ARCH_KMAP_NPAGES=2`; page pool
  `ESP32S3_PGPOOL_PBASE=0x350000` — a PSRAM *offset*, not an address — and
  `ESP32S3_PGPOOL_SIZE=4194304`, which is Kconfig type `int` and so must be
  decimal. `MM_PGSIZE=65536` matches the cache MMU's page.
  `ARCH_PGPOOL_MAPPING` is **off**, and `esp32s3_addrenv.h` errors out if it
  is turned on; `ARCH_KVMA_MAPPING` is selected by `ESP32S3_PGPOOL_SCRATCH`,
  since it has no prompt of its own and a defconfig cannot set it.
- **Init and the other programs** are separate ELFs in a ROMFS image mounted
  at `/system/bin` and exec'd from there: `ELF`,
  `BINFMT_ELF_EXECUTABLE` without `BINFMT_ELF_RELOCATABLE`, `FS_ROMFS`,
  `LIBC_EXECFUNCS`, `LIBC_ENVPATH`, `INIT_MOUNT`,
  `INIT_FILEPATH="/system/bin/init"`, and NSH built as a program named
  `init`.
- **Stacks.** `ARCH_KERNEL_STACK` with `ARCH_KERNEL_STACKSIZE=8192` is
  required (§4.4) and the 1568-byte default is far too small — the whole
  ELF-loader system-call body runs on it. `INIT_STACKSIZE`,
  `POSIX_SPAWN_DEFAULT_STACKSIZE` and `ELF_STACKSIZE` are 8192 for the same
  reason; the 2048 defaults do not survive NSH being a process.
- **This board.** `ARCH_CHIP_ESP32S3WROOM2N32R8V`, `ESP32S3_FLASH_MODE_OCT`
  with **STR** sampling (`esp32s3_spiflash.c` rejects octal + DTR),
  `ESP32S3_SPIFLASH`, `ESP32S3_SPIRAM` in octal mode.
  `ESP32S3_APP_FORMAT_LEGACY` is off — it is only "default y if
  BUILD_PROTECTED", and a kernel build is one image, so the configuration
  selects `SIMPLE_BOOT` and `nuttx.bin` flashes whole at 0x0 with **no**
  ESP-IDF bootloader.

To change any of it by hand, `kconfig-tweak` still works, but run the tweaks
**one per line**: `zsh` does not word-split unquoted variables, so a `for`
loop over tweak strings silently applies nothing.

### 6.2 Build the user programs and the boot ROMFS

The first `make` links against an empty ROMFS placeholder. Then:

```sh
make export
cd ../apps
./tools/mkimport.sh -z -x ../nuttx/nuttx-export-*.tar.gz
make import
./tools/mkromfsimg.sh ../nuttx/arch/xtensa/src/board/board/romfs_boot.c
cd ../nuttx && make -j8
```

Check it took: `xtensa-esp32s3-elf-nm nuttx | grep romfs_img` must show a
**strong `D`** symbol, and `.flash.rodata` should grow by the image size.

`apps/bin` is where the user programs land, and it does not survive a clean;
`make import` in `apps/` rebuilds it, which is also how to get symbols back
for `addr2line` on a user-space address.  The whole sequence has to be re-run
whenever a program is added or its configuration changes -- turning on
`EXAMPLES_PFFAULT`, for instance -- since the ROMFS is generated from
`apps/bin` and linked into the kernel.

### 6.3 Flash and console

```sh
esptool.py -c esp32s3 -p /dev/cu.usbserial-2140 -b 460800 \
           write_flash 0x0 nuttx.bin
```

A console harness is trivial to write and worth having, since the board has
to be reset to be driven: open `/dev/cu.usbserial-2140` at 115200 with
pyserial, `dtr = False` (GPIO0 high, normal boot) and pulse `rts` (EN), read
the boot log, then write commands terminated with `\r` and read between
them.  Allow generously for the slow ones -- `ostest` needs ~90 s, and far
longer with scheduler debug output on.

### 6.4 JTAG debugging (this is how the hard bugs were found)

```sh
$OPENOCD -s $OPENOCD_SHARE/openocd/scripts -f board/esp32s3-builtin.cfg \
         -c "gdb_memory_map disable" -c "gdb_flash_program disable" &
$GDB -batch -x probe.gdb nuttx
```

The two `-c` flags are not optional on this board: OpenOCD runs a flasher
stub to probe the flash when GDB attaches, that stub cannot cope with octal
flash, and the failure ends in `auto_probe failed` and a **rejected GDB
connection** — not an obviously flash-related error.

with `probe.gdb` doing `set remotetimeout 30`, `target remote :3333`,
`monitor reset halt`, `hb xtensa_user_panic`, `continue`, then
`info registers pc ps epc1 exccause` and `x/8xb` at the addresses of
interest.  Attaching resets the target, so to inspect a running system let it
boot first and use `monitor halt` rather than `monitor reset halt`.

To read the cache-MMU table, entry `i` is at `0x600c5000 + i*4`:

```
x/16xw 0x600c5100     # page pool, entries 64-79    -- must all be 0x4000
x/8xw 0x600c5300      # text window, entries 192-199
x/8xw 0x600c5400      # data window, entries 256-263
x/16xw 0x600c5480     # heap window, entries 288-303
x/2xw 0x600c5500      # kernel scratch, entries 320-321
```

`0x8000 | page` is a valid PSRAM entry, `0x4000` is invalid.

**Kill OpenOCD before running esptool** — it holds the target halted and the
flash will silently not happen.

The decisive trick for this port: **read the same address through both bus
aliases**. `x/8xb 0x4280919c` (IBUS) vs `x/8xb 0x3c80919c` (DBUS) is what
proved the stale-I-cache bug — zeros on one, correct bytes on the other.

### 6.5 Checking the isolation

`examples/pffault` is in the board configuration for this.  It takes an
access and an address, performs it from a WORLD1 task, and reports what
happened:

```sh
nsh> pffault r 0x3fc90954    # kernel data          -> SIGSEGV (PMS)
nsh> pffault w 0x3fc90954    # kernel data          -> SIGSEGV (PMS)
nsh> pffault r 0x40374000    # kernel vectors       -> SIGSEGV (PMS)
nsh> pffault r 0x4037dc00    # the WORLD1 table     -> SIGSEGV (execute-only)
nsh> pffault r 0x600d0000    # the World Controller -> SIGSEGV (PMS)
nsh> ps                      # only pffault is gone
```

Three more that are about process-to-process isolation rather than the
kernel/user boundary:

```sh
nsh> pffault r 0x3c410000    # the pool, via the old kernel window -> must be 0
nsh> pffault r 0x3d400000    # the kernel scratch region           -> must be 0
nsh> pffault r 0x3d2f0000    # own last heap page -> must read 0 (the wipe)
```

`SURVIVED unexpectedly` means the access went through.  Pick an address whose
contents you know -- `objdump -s -j .dram0.data nuttx` -- because a *denied*
read can also return zero, so reading zero proves nothing either way.  For
the same reason, run something like `getprime` first when checking the wipe:
against a freshly booted board a stale page may happen to be zero anyway.

`examples/forktest` is the `fork()` test and is in the board configuration:

```sh
nsh> /system/bin/forktest      # -> forktest: PASS
```

It checks the child's return value and pid, that a six-frame-deep spilled
register chain survives the copy (`sum=105`), and — the part that matters —
that a write by the child is *not* visible to the parent.

The first two are the weakest tests in this document and must not be trusted
alone: an invalidated entry reads 0 without faulting, so they cannot tell
"unmapped" from "denied" from "mapped to a zeroed page".  Confirm them over
JTAG (§6.4).  The third is the strong one, and it is what proves the scratch
region actually maps: if it did not, `esp32s3_pgwipe()` would memset into a
hole and the page would arrive carrying the previous tenant's data.

---

## 7. Remaining work, in priority order

### 7.1 The kernel/user boundary — done

`514b26a956`, `e15f277b94`, `3a47869397` and `b859e49c3d`.  A user process now
reaches nothing of the kernel's: not its data, not its code, not its vectors,
not a peripheral.  Measured on the WROOM-2 with `examples/pffault`, which is
in the board configuration for exactly this:

```
pffault r 0x3fc90954   kernel data           SIGSEGV, shell survives
pffault w 0x3fc90954   kernel data           SIGSEGV
pffault r 0x40374000   kernel vectors        SIGSEGV
pffault r 0x4037dc00   the WORLD1 table      SIGSEGV   (execute-only)
pffault r 0x600d0000   the World Controller  SIGSEGV
```

Each kills only the offending task; `ps` shows no zombies and the page pool
returns to its baseline.  Before, every one of them returned the real
contents.

Three things are worth carrying forward.

**WORLD1 needs a vector table of its own.**  It cannot use the kernel's,
because it would have to fetch it, and it cannot reach the kernel's through
World Controller entry addresses either: entering one switches the CPU to
WORLD0, and only the level 1 and level 3 paths restore the interruptee's
world on the way out (`exception_entry_hook` / `exception_exit_hook`).  A
window spill returns with `RFWO`/`RFWU`, which no hook covers, so a task that
overflowed a register window would come back **privileged**.  The WORLD1
table therefore handles window overflow and underflow itself — they touch
nothing but the task's own stack — and every other slot jumps to its kernel
counterpart, the jump running in WORLD1 and the fetch of its target changing
world.  Two entry slots, not twelve.

**The table has to live in Internal SRAM1.**  The permission control divides
SRAM0 into two 16 KB blocks and can say nothing finer, so a table there would
drag the kernel's own vectors and the start of its IRAM code into whatever
the table is granted.  SRAM1 is divided by split lines at 256-byte
granularity.  `esp32s3_isolation_permissions()` asserts the link put it
there, because the failure would otherwise be silent.

**The world switch was never compiled into a kernel build.**  `ESP32S3_WCL`
selected `XTENSA_HAVE_GENERAL_EXCEPTION_HOOKS` only `if BUILD_PROTECTED`, and
those hooks are the whole mechanism: without them `set_next_world()` never
runs and every task keeps executing in WORLD0 regardless of what
`xtensa_lowerprivilege()` writes into its context.  The permissions were
correct and enforced against a world nothing ever entered.  This is the shape
of failure to expect here — everything looks configured and nothing is.

**What this does not do is isolate processes from each other.**  That needed
the pool to stop being mapped, which is §7.2 and is now done.  The
permission control contributed nothing to it: `APB_CTRL_SRAM_ACEn_*`, the
PSRAM counterpart of the `FLASH_ACE` registers `esp32s3_pms.c` drives,
remains unused, and §7.2 explains why it had to.

### 7.2 Isolate processes from each other — done

`c63c939817`.  A user process no longer reaches another's pages.  This was
scoped as window hygiene, and measuring that first showed it was never going
to be enough:

```
nsh> pffault r 0x3c410000
pffault: SURVIVED unexpectedly, read 42c09118   <- the resident shell's data
```

Cache-MMU entry 64 held `0x8035`, and so did the user `.text` window at entry
192: `0x3c400000` and `0x42c00000` were two views of one PSRAM page.
`0x3c410000` was entry 65, the shell's data page.  Nothing to do with stale
window entries — the kernel simply mapped the whole pool at `0x3c0b0000` as a
plain valid mapping.

**The permission control cannot express the difference, and this is settled.**
`APB_CTRL_SRAM_ACEn_*` — the PSRAM counterpart of the `FLASH_ACE` registers
`esp32s3_pms.c` drives — is indexed by **physical** address, and a process's
own pages *are* pool pages.  Any physical range that lets a process reach its
own memory lets it reach the pool.  Do not spend time looking for a setting.
(`esp32s3_pms_set_flash_cache_split_line()` takes an `ADDR`/`SIZE` pair in
flash-offset terms; the PSRAM registers are the same shape.)

So the lever is which virtual addresses have mappings at all.

**What was done.** The pool is no longer mapped.  `up_allocate_pgheap()` runs
its checks while the boot-time mapping esp32s3_spiram.c made is still there —
that is the only moment it can — and then withdraws the pool's share of it.
The kernel reaches a page through a scratch region instead:

```c
uintptr_t esp32s3_pgmap(uintptr_t paddr);   /* map, return the vaddr */
void      esp32s3_pgunmap(uintptr_t vaddr); /* write back, invalidate */
```

`esp32s3_pgvaddr()` was **deleted, not adapted**.  It was arithmetic whose
result a caller could hold indefinitely; arithmetic that still answers for an
address that is no longer mapped is exactly how this port fails silently.
Its callers:

| | |
|---|---|
| `esp32s3_pgwipe()` | map, `memset`, unmap, under `sched_lock()` |
| `up_addrenv_page_vaddr()` | returns 0 — it promises an address that outlives the call, which a scratch mapping cannot.  `CONFIG_MM_KMAP` is rejected at compile time for the same reason: `kmm_map()`'s single-page path is built on it |
| `up_addrenv_pa_to_va()` | answers from the slot table — which is what its own comment always claimed it did |
| `up_addrenv_va_to_pa()` | the inverse, over scratch addresses only |

Four things are worth carrying forward.

**`sched_lock()`, not a mutex.**  A scratch address is ordinary external
memory as far as the permission control is concerned, so a user task that ran
while a mapping was live could read the page through it — the same leak in
miniature.  A mutex excludes other *kernel* users; only holding off the
scheduler excludes user tasks.  It is taken per page rather than per
operation, so the cost is one 64 KB wipe of non-preemptible time (~800 µs at
40 MHz octal PSRAM), not a whole `up_addrenv_create()`.  Interrupts stay
enabled; only the entry rewrite is in a critical section.

**Cache maintenance has to be range-scoped.**  `esp32s3_dcache_suspend()`
invalidates the *whole* D-cache, which would discard the resident process's
dirty lines, and its write-back variant would put a whole-cache write-back
inside a critical section — both unacceptable at once per page of every
process created.  `esp32s3_mmu_scratch_map()`/`_unmap()` follow
`esp_spiram_map()` instead: suspend, rewrite the entry, `cache_invalidate_addr()`
for that page only, resume.

**The unmap must write back first.**  The wipe's zeros sit in D-cache lines
tagged by *virtual* address.  Repointing the entry with them still dirty
writes them back to whichever page the slot is used for next.
`cache_writeback_addr()` runs before the critical section, with interrupts on
— safe because `sched_lock()` means nothing else can reach the slot.

**Two entries, not one.**  Eager `fork()` (§7.3) has to hold a parent page and
a child page mapped at once to copy between them.  With one slot that becomes
a bounce buffer through kernel DRAM.

**What is deliberately left mapped.**  Only the pool's share of the PSRAM
window is withdrawn (entries 64–127); entries 11–63 and 128–138 stay.  They
map no page a process will ever own, and that is the aperture a kernel-side
PSRAM allocation would have to come from — `up_textheap_memalign()` falls back
to the kernel heap and derives an instruction-bus alias (`+0x6000000`) for
anything outside internal RAM, which is the path a `dlopen()`ed shared library
takes.  A PSRAM kernel heap carved from the *allocable* window would reopen
this from the other side, so `ESP32S3_SPIRAM_COMMON_HEAP` is now an `#error`.

**How it was measured.**  A zero read proves nothing (§8), so:

| | before | after |
|---|---|---|
| JTAG, pool entries 64–127 | `0x8035`…`0x8074` | all `0x4000` |
| JTAG, scratch entries 320–321 | — | `0x4000` at rest |
| JTAG, the process's own windows | `0x8035` / `0x8036`,`0x8037` / `0x8038`… | unchanged |
| `pffault r 0x3c410000` | `42c09118` | `00000000` |
| `pffault r 0x3c420000` | `ffffffff` | `00000000` |

That last row is the useful shell-level one: `ffffffff` is erased-but-mapped
PSRAM, `00000000` is an invalid entry, so the transition distinguishes the two
without relying on what zero means.  The positive control that the scratch
really maps is `pffault r 0x3d2f0000` reading 0 *after* `getprime` has churned
the pool — a mapping that silently failed would leave the previous tenant's
data there.  Regressions: `getprime` 1200 msec (unchanged), `ostest` exits 0,
pool back to its 1245184 baseline, no zombies, §7.1's five boundary accesses
still SIGSEGV.

**A possible follow-on, not done.**  Wiping and copying by DMA would remove
the CPU-visible mapping altogether and make `sched_lock()` unnecessary.  It
needs a GDMA path and its own cache-coherency story, and `fork()`'s page copy
would want it too — worth revisiting when §7.3 lands rather than before.

### 7.3 Eager `fork()` — done

`4b22fe69c5` (was `4c260f497f` before the rebase; the generic half now comes
from the semantics branch and only the Xtensa wiring is ours).  `fork()` means what POSIX says on this port: the child gets
its own copy of the parent's memory, at the same virtual addresses.
Measured on the WROOM-2 with `examples/forktest`, which forks from six
frames deep so several register windows are live and spilled:

```
nsh> /system/bin/forktest
forktest: parent pid=4, forking from 6 frames deep
forktest: parent fork() returned child pid=5, sum=105
forktest: child  pid=5, fork() returned 0, sum=105, inherited "parent"
forktest: child  wrote "child" to its own stack copy
forktest: parent reaped child, exit status 42 (expected 42)
forktest: parent stack still says "parent" (expected "parent")
forktest: PASS
```

`sum=105` from the child is the frame chain surviving the copy; the last
line is the isolation — the child's write did not reach the parent.

**Same virtual addresses is the whole trick, not a detail.**  A stack is
full of pointers into itself, and on the windowed ABI every frame's base
save area holds an absolute `a1`.  They stay correct only because the copy
is addressed identically.  A *relocated* copy — which sharing an address
environment forces — cannot work here at all: the child's first `retw`
underflow reloads a stack pointer into the parent's stack and runs on the
parent's live frames from then on.  That is very likely why Xtensa never
selected `ARCH_HAVE_FORK`.

The copy itself is the pair of scratch slots §7.2 exists for, both held
across one `sched_lock()` region so a page is never exposed between them
(`copy_region()` in `esp32s3_addrenv.c`).

What it cost on the generic side, kept as small as it can be:

| | |
|---|---|
| `ARCH_HAVE_ADDRENV_FORK` | the architecture can duplicate an address environment, so it can provide `fork()`.  Selects `ARCH_HAVE_FORK` |
| `addrenv_fork()` | allocate the child an environment and let the architecture fill it, beside `addrenv_join()` |
| `up_addrenv_fork()` | the architecture hook.  **Not** `up_addrenv_clone()`, which copies the *descriptor* so a group's threads share one environment |
| `nxtask_setup_fork(retaddr, share, usp)` | which semantics the caller wants, and the caller's stack pointer |

Every other architecture passes `(true, 0)` and reaches the unchanged path.
The new branches compile out without `ARCH_HAVE_ADDRENV_FORK` or
`ARCH_HAVE_VFORK`, which only `kernel_oct` selects — `elf_oct`,
`sotest_oct` and `knsh` report no fork and no vfork, exactly as before.

**Three bugs found on silicon, all one shape:** kernel code writing to the
child's stack, which only bites once that stack is at the parent's address.
Worth knowing, because anything else that touches a child's stack will hit
it too.

- `up_initial_state()` laid the register frame at the top of the user stack
  and memset it, destroying the parent's outermost frame.  It now uses the
  thread's kernel stack when there is one — where that frame belongs anyway,
  since the user should not be able to scribble on the register set it is
  about to be started with.
- `group_allocate()` and `env_dup()` allocate out of whichever heap is
  instantiated — the parent's — so the child carried pointers to blocks its
  own heap never had and corrupted its free list on exit.  The child's
  environment is now current for its whole setup (`addrenv_select()`, the
  idiom `binfmt_execmodule.c` already uses), which works because everything
  the setup reads from the parent is legible under it, at the same address,
  being a copy.
- `tls_dup_info()` and `nxtask_setup_stackargs()` rebuilt what the copy
  already held, carving a second TLS frame off a stack that has one and
  writing the argument vector over the parent's outermost frame.  Both are
  skipped when the child inherited the stack; only `tl_task` and `tl_tid`
  are corrected, or the child reports the parent's pid.

### 7.4 `vfork()`, and why it had to be separated here

With `fork()` copying, `fork()` and `vfork()` can no longer be the same
call — and in NuttX they are: both are wrappers around one `up_fork()`,
there is no `SYS_vfork`, and the shared implementation *shares* memory.  So
implementing real `fork()` broke `ostest`'s `vfork` test, which is correct
and tests exactly the sharing.

This port therefore has `up_vfork()`/`SYS_vfork`, selected only by
`kernel_oct`, and the parent is suspended **in the kernel** —
`nxtask_start_vfork()`, released by `nxtask_vfork_release()` from `_exit()`
and from `exec()` — not by a `waitpid()` in libc after `up_fork()` has
already returned.  That matters: a `vfork()` child borrows the parent's
stack outright, so the parent must not execute a single instruction while
it lives.  Suspending in libc is too late — the parent returns through
several frames first, onto stack the child is already using.

The general fix belongs upstream and someone else is carrying it.  The
argument, with the code citations and the history (NuttX's `fork()` is
literally its old `vfork()`, renamed in `c33d1c9c97`), is written up in
`<workspace>/fork-vfork-semantics-issue.md`.  **Do not extend the local
change to other architectures** — that was tried and reverted; it is a
large, untestable diff and it is not this port's job.

### 7.5 Both open failures are closed; `ostest` passes

This port now lives in `/Users/marco/ia/nuttx-fork/`, rebased onto the
semantics branch's *unconditional* stack inheritance — the port's own
`CONFIG_ARCH_FORK_STACK_INHERIT` and its `sched/task/task_fork.c` changes are
gone, superseded by "sched/arch: give the fork() child the parent's stack
address". `up_initial_state()` putting the register frame on the kernel stack
stays: it is Xtensa's half of that same fix, and armv7-a needed the identical
one.

On an ESP32-S3-WROOM-2, `esp32s3-devkit:kernel_oct`:

```
vfork_test: Child 6 ran and exited before the parent resumed
fork_test: Child running independently (child)
fork_test: Parent and child had independent memory
ostest_main: Exiting with status 0
```

The page pool returns to its 720896-byte baseline after the run.

**`vfork()` hung** because `xtensa_fork()` relocated the child's stack.  Both
primitives put the child on the parent's stack at the parent's addresses, but
the borrowing child's *top* is the parent's stack pointer less the reserve,
so `newsp = newtop - stackutil` came out below `usp` and the copy ran. It was
also short at exactly the wrong end: `SPILL_ALL_WINDOWS` puts a frame's own
`a0`-`a3` in the 16 bytes *below* its stack pointer, so `[usp, top)` does not
contain the base save area the child's first `retw` reads. The child resumed
into uninitialised memory. `newsp` is now simply `usp`, which is what
`arm_fork.c` has always done for a borrowing child, and the relocating branch
is gone — the assertion in its place says what this architecture can support.
It was our defect, not the semantics branch's.

**`fork_test`'s `ENOMEM` was page-pool exhaustion after all.** It had been
ruled out on the strength of `alloc_region()`'s `berr()` never appearing, and
that `berr()` was compiled out: `CONFIG_DEBUG_BINFMT` was set,
`CONFIG_DEBUG_BINFMT_ERROR` was not. With it on:

```
alloc_region: ERROR: page pool exhausted at page 4 of 16
```

An eager `fork()` costs a full copy of the parent's image, `ostest` runs as
*two* processes (`ostest_main` and the `user_main` it spawns), and at
`CONFIG_ARCH_HEAP_NPAGES=16` a process is 19 of the pool's 64 pages. With
`init` resident that is 57 before `fork_test` starts. The heap is now 8 pages
— 512 KB against a 9 KB high-water mark — which puts a process at 11 and
leaves the child room. The pool cannot usefully grow instead: it is carved
from the same 8 MB of PSRAM the kernel's own window maps, and the most it
could gain is 11 pages, one short of a 1 MB-heap child.

Read the second one as a warning about the first kind of evidence: **the
absence of an error message is not evidence, unless the message was compiled
in.**

### 7.6 All three primitives, in every build

Two things this port had asserted about Xtensa were false, and both were
load-bearing — they are why `vfork()` was gated on `CONFIG_LIB_SYSCALL` and
why `task_fork()` was not offered at all.

**"A flat build has no way to snapshot the caller."**  It has.
`SYS_save_context` is one of four internal system calls defined *outside*
`CONFIG_LIB_SYSCALL` in `arch/xtensa/include/syscall.h` and dispatched
unconditionally by `xtensa_swint()`: it runs `SPILL_ALL_WINDOWS` and copies
the whole exception frame out. That is exactly what `arm/fork.S` exists to
do, and it is the path every voluntary context switch here already takes.

The one subtlety is *where* the syscall is issued. The snapshot records the
stack pointer of whatever frame issues it, and the child is built to resume
on the stack from that point up — so that frame has to outlive the fork.
Issue it inside `up_saveusercontext()` and the recorded frame is that
function's, which dies on return and is then reused by `nxtask_setup_fork()`
and everything after it; the child resumes on overwritten memory. It is
issued directly in `xtensa_fork_direct()` for that reason.

**"A windowed ABI cannot relocate a stack."**  It can, if the frame chain is
corrected with it. Each frame's base save area holds the caller's *absolute*
`a1`, one word into `[sp - 16)`, so `xtensa_fork_rebase()` walks the chain
and adds the relocation offset to each link — the same walk `x86_64_fork.c`
does over the saved frame-pointer chain. What stays uncorrected is data
rather than links (spilled `a4`-`a15` that happen to hold stack addresses),
which is the residue `arm_fork.c` calls a "feeble effort" and which
`task_fork()`'s contract already disclaims.

So: `fork()` inherits (kernel build only, it needs an address environment to
duplicate), `vfork()` borrows where the parent has a kernel stack to be
suspended on and relocates otherwise, `task_fork()` always relocates.
Measured:

```
esp32s3-devkit:kernel_oct       esp32s3-devkit:elf_oct   (BUILD_FLAT)
  task_fork_test: PASS            task_fork_test: PASS
  vfork_test:     PASS            vfork_test:     PASS
  fork_test:      PASS            fork_test:      absent by design
  ostest status 0                 ostest status 0
```

That also fixed a live bug in `esp32s3-devkit:knsh`: it is `BUILD_PROTECTED`
with `ARCH_KERNEL_STACK` unset, so its `vfork()` child borrowed a stack whose
parent was suspended on frames just below the borrow point.
`ARCH_VFORK_STACK_BORROW` now depends on `ARCH_KERNEL_STACK`, generically.

Still open:

- **Failure injection on `fork()`.** `up_addrenv_fork()` failing part-way
  calls `up_addrenv_destroy()` while the child's environment is current; that
  path has still never executed. Also `fork()` from a pthread, `fork()` then
  `exec()` in the child, nested fork, and repeated fork/exit against the
  `free` baseline.
- **`esp32s3-devkit:knsh` builds `up_vfork()`/`up_task_fork()` and has never
  run them.** A protected build has a privilege transition the flat build
  does not, and it is the one mode of the four that no hardware run covers.
  It needs its own flashing recipe (separate user blob), which is the only
  reason it has not been done.
- **Upstreaming**, in the six series set out below. Group 1 is unaffected by
  any of the above and could go out at any time. The Xtensa fork/vfork/
  task_fork work is a seventh series, and the `ARCH_VFORK_STACK_BORROW`
  dependency inside it belongs to the semantics branch rather than to this
  port.
- **Deliberately not next:** the DMA route for page copy and wipe (end of
  §7.2), and building anything on top of the port.

| # | what | commits |
|---|---|---|
| 1 | **standalone bug fixes**, no dependency on this port | `mm/pgalloc` 32/64 KB pages · octal-flash init in IRAM (repairs an existing *protected* build) · ARCHLIB IRAM placement · `up_allocate_kheap()` BUILD_KERNEL branch · `sig_trampoline` in crt0 · `addrenv_switch()` on voluntary context switch |
| 2 | **Unit A**, expose MMU/WCL/PMS primitives — pure refactor |
| 3 | **recoverable faults + per-task abort** — useful without BUILD_KERNEL, plus `apps` `examples/pffault` |
| 4 | **the address-environment port** — Units C/D/E and BUILD_KERNEL enablement |
| 5 | **isolation** — world split, WORLD1 vector table, PMS permissions |
| 6 | **process isolation and `fork()`** |

Use `git log --oneline` to recover the hashes; they change with every rebase,
and this branch has been rebased twice.
`binfmt/elf`'s `nx_priority` fix is already out as
[apache/nuttx#19537](https://github.com/apache/nuttx/pull/19537) and is also
carried on the semantics branch.

---

## 8. Gotchas that cost real time

- **esp-hal stale objects.** The `esp-hal-3rdparty` objects do not depend on
  `sdkconfig.h`. Any flash/PSRAM/boot-format config change needs `make clean`
  or the build silently keeps the old settings. Symptom: an image byte-identical
  to the previous one.
- **An error path that is compiled out cannot report.** `CONFIG_DEBUG_BINFMT`
  being set does not turn on `berr()` — `CONFIG_DEBUG_BINFMT_ERROR` is a
  separate symbol and was off, so `alloc_region()`'s pool-exhaustion message
  was never emitted and pool exhaustion was wrongly ruled out for a day.
  `kernel_oct` now enables the error output for binfmt and sched. Before
  concluding "that path never ran", check that its message exists in the
  binary.
- **A stale `romfs_stub.o` outranks the real ROMFS.** Both define
  `romfs_img`, and the placeholder is the earlier member of `libboard.a`, so
  a kernel built once before the image was generated links the placeholder
  and boots to "Boot ROMFS image is empty". Fixed by making the placeholder
  depend on the image; verify by the linked symbol's *size*, never its
  presence.
- **`zsh` does not word-split unquoted variables.** A `for` loop over
  `kconfig-tweak` argument strings applies nothing and reports success.
- **`CONFIG_ARCH_KERNEL_STACK` defaults to `CONFIG_LIBC_EXECFUNCS`**, so it
  turns itself on when execfuncs is enabled. That is now what we want, but it
  is why it appeared unbidden earlier.
- **`ARCH_PGPOOL_SIZE` is Kconfig type `int`** — decimal, not hex.
- **This board wants STR flash sampling, not DTR.** `esp32s3_spiflash.c` has
  a hard `#error "Not yet implemented"` for octal + DTR. The committed
  `elf_oct` config is the reference for what this board needs.
- **Kill OpenOCD before flashing.**
- **An unmapped cache window swallows writes and reads back zero without
  faulting.** Every mapping mistake in this port has been silent. Assume
  nothing; read it back over JTAG. This cuts both ways when testing
  isolation: a `pffault` read returning `00000000` is equally consistent with
  "denied", "invalid entry" and "mapped to a wiped page", so it proves
  nothing on its own. Only a *non-zero* read or a SIGSEGV is evidence.
- **OpenOCD cannot probe this board's octal flash**, and refuses the GDB
  connection when it fails (`Failed to run flasher stub`, then `auto_probe
  failed`). Start it with `-c "gdb_memory_map disable" -c "gdb_flash_program
  disable"`; attaching resets the target, so let it boot before halting.
- **A cache-MMU entry rewrite orphans the dirty lines of the address it
  covered.** D-cache lines are tagged by *virtual* address, so anything
  written through a mapping and not yet written back will be flushed to
  whatever page that entry points at *later*. Any code that repoints an entry
  must write back first. This is why `esp32s3_mmu_scratch_unmap()` calls
  `cache_writeback_addr()` before it touches the table.
- **A program's stack size and priority now come from the ELF, not the
  kernel config.** `apps/Application.mk` stamps `nx_stacksize` and
  `nx_priority` into each binary as absolute symbols from that app's own
  `STACKSIZE`/`PRIORITY`, and `binfmt/elf.c` prefers them over
  `CONFIG_ELF_STACKSIZE`/`SCHED_PRIORITY_DEFAULT`. Two consequences, both of
  which bit on the move to upstream master and neither of which the build
  shows:
  - `CONFIG_ELF_STACKSIZE` is only a fallback. Raise
    `CONFIG_DEFAULT_TASK_STACKSIZE` (most apps derive from it) and the
    per-app symbols for the rest (`21fc83d861`).
  - `PRIORITY = SCHED_PRIORITY_DEFAULT` is encoded as **0**, and `elf.c`
    took it literally, creating the task at the idle task's priority so it
    was queued behind idle and never ran. The program loads, reports no
    error, and executes nothing. Fixed in `710c168cd6`; upstream bug, worth
    a PR. `nm bin/<prog> | grep nx_` is how to check what a program actually
    asks for.
- **The pool's virtual address moves, so do not hard-code it in a test.**
  `esp32s3_spiram.c` maps PSRAM after the last flash mapping, so the kernel
  window shifts as the image grows -- it has been at `0x3c0a0000`,
  `0x0x3c0b0000` and `0x3c0d0000`, putting the pool at `0x3c400000` and then
  `0x3c420000`. `up_allocate_pgheap()` derives the pool's virtual base from
  the cache MMU and is unaffected, but a `pffault r <hard-coded>` check is
  not: after a drift it reads *non-pool* PSRAM, which is deliberately still
  mapped, and looks exactly like the isolation having broken. Take the
  address from the `up_allocate_pgheap:` boot line, which prints where the
  pool actually landed.
- **checkpatch tracks braces per section banner.** A `static const struct
  foo g_x[] = {...}` under a `Private Functions` banner produces a stream of
  "Bad left brace alignment" errors that have nothing to do with the
  formatting; give it `Private Types` / `Private Data` banners and the same
  code passes. Designated initialisers, and no trailing comma after the last
  element.

### The on-target bugs already found and fixed — do not re-derive them

1. **IRAM placement** (`cc3a6fc032`) — the section scripts place IRAM code by
   archive name and every rule said `*libarch.a`; a kernel build archives into
   **`libkarch.a`**, so all 265 rules missed and the octal-flash bring-up
   functions stayed in mapped flash, called while that mapping was being
   reconfigured. Boot loop with no output. Fixed with an `ARCHLIB` macro.
2. **`up_allocate_kheap()`** (`3c8f7da049`) — had only PROTECTED and FLAT
   branches, so BUILD_KERNEL left `kbase`/`ktop` uninitialised and the heap
   landed at NULL. GCC warned about it.
3. **`crt0.c` `sig_trampoline`** (`2895ec8c27`) — referenced but never defined
   on Xtensa; the kernel path had never been compiled. Cannot come from
   `xtensa_signal_handler.S` (that is in libarch, and user programs link only
   `-lmm -lc -lproxies`). GCC has **no `naked` attribute on Xtensa**, so it is
   file-scope assembly.
4. **Cache coherency** (`6df87015da`) — the "runs, then executes a hole" bug.
   Text is written through the DBUS alias but fetched through IBUS, and one
   global table means stale I-cache lines from a previous mapping read back as
   zeroes. Fixed in `up_addrenv_coherent()` and `up_addrenv_mprot()`.
5. **`ARCH_PGPOOL_PBASE`, twice.** First (`6df87015da`) PSRAM mapped at
   `0x3c0a0000`, so the pool's physical base was `0x360000`, not `0x400000`;
   every page wipe was landing 640 KB away.  Then the kernel image grew, the
   window moved up one entry to `0x3c0b0000`, and `0x360000` was stale by
   exactly one page — so `esp32s3_pgwipe()` wiped the page *below* the one
   allocated.  `up_addrenv_create()` allocates ascending, so each page was
   wiped by its successor's allocation and only the **last page of each
   region** went out unwiped, carrying the previous tenant's data into a new
   process.  It presented as `pffault r 0x3d2f0000` — a process's own last
   heap page — reading `aabc6aaa` on a fresh boot.  `c63c939817` retired the
   class rather than the instance: there is no configured pool *virtual*
   base left to drift against, since `up_allocate_pgheap()` derives it from
   what `esp32s3_mmu_paddr()` reports the window maps.  Only the physical
   `ESP32S3_PGPOOL_PBASE` remains, and it is checked to lie inside mapped
   PSRAM.
6. **`addrenv_switch()` on voluntary context switches** (`eb3c2d9957`) —
   Xtensa called it only from `xtensa_irq_dispatch()`, but `up_switch_context()`
   is a `SYS_switch_context` system call here, so every voluntary switch left
   the address environment alone. A resumed shell ran against its successor's
   freed pages. Two dead ends were eliminated first, and are worth not
   repeating: it is **not** the signal-delivery path, and it is **not** the
   windowed-ABI base save area (moving `A1` does not need that copy here,
   and copying *back* would actively corrupt the user's save area).
7. **Signal delivery to a user process** (`790a2120ca`) — three faults in the
   same path, none of which a protected build can show, because it has no
   kernel stack and so already runs the kernel on the user stack. The handler
   ran on the *kernel* stack, because `SYS_signal_handler` switched back to
   user only when `xcp.ustkptr` was set and that holds a value only during a
   system call. A system call made *by* the handler restarted the kernel stack
   at its top, overwriting both the suspended dispatch and the context saved
   to resume the thread — which is why the trampoline appeared to run twice
   and the process ended up at PC 0. And the `siginfo` handed to the handler
   was a kernel pointer, which is latent today and fatal the moment WORLD1
   loses access to kernel memory. Note the earlier reading of this failure was
   wrong: the register dump showed a stale `EXCCAUSE`, and the assertion was
   ostest's own, its message lost in an unflushed stdio buffer.

---

## 9. References

- Design note: `mmu-isolation.md`
- Dated evidence, including why COW/demand paging are dead: the progress log
  on the `pre-upstream-rebase` tag (see the header note)
- Templates being mirrored: `arch/risc-v/src/common/riscv_addrenv*.c`,
  `riscv_addrenv_kstack.c`, `arch/arm/src/armv7-a/arm_syscall.c`
  (kernel-stack switch in C), `boards/risc-v/qemu-rv/rv-virt` and
  `boards/risc-v/k230/canmv230` (ROMFS kernel-build boards)
