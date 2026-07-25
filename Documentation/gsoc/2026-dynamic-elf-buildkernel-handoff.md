# GSoC 2026 Dynamic ELF — BUILD_KERNEL on ESP32-S3: handoff

Self-contained handoff for continuing the Xtensa/ESP32-S3 address-environment
port. Everything needed to pick this up cold is here: what works, what does
not, how to build and flash it, how to debug it, and what to do next. Read
this first; the design note `2026-dynamic-elf-mmu-isolation.md` and the dated
evidence in `2026-dynamic-elf-progress-log.md` are the background.

Branch: `gsoc/dynamic-elf-baseline` (both `nuttx/` and `apps/`).

---

## 1. Where this stands

**`CONFIG_BUILD_KERNEL` boots to an interactive NSH on the ESP32-S3, user
programs run from the shell, and the kernel is protected from them.** As of
2026-07-25, on the WROOM-2 board:

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

### What is broken

1. **Processes are not isolated from each other.** The kernel/user boundary
   is enforced (§7.1), but every user page comes from one pool the kernel
   keeps mapped, and the external-memory permissions are indexed by physical
   address, so one process can still reach another's pages through the
   kernel's own PSRAM window. Measured, not inferred:

   ```
   nsh> pffault r 0x3c410000
   pffault: SURVIVED unexpectedly, read 3d01176c   <- NSH's .bss address
   ```

   §7.2 is that work, and it is the next unit. The window hygiene it was
   originally scoped as — invalidating unused entries, moving the map — is
   done and did not close this; see §7.2 for why the fix has to be virtual.
2. A violation by the kernel itself is still a whole-system panic, by
   design: only a WORLD1 violation is survivable, and it is the interrupted
   task's saved PS that tells the two apart.

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
- Serial: **`/dev/cu.usbserial-2140`** is flash + console (115200, CP2102
  bridge, esptool auto-reset works). `/dev/cu.usbmodem*` is the native
  USB-JTAG used by OpenOCD — different port, no conflict.
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
  cache-MMU page. `mm/pgalloc` was extended to permit 32 K/64 K (`e6a105169e`).

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
| page pool | `0x3c400000`–`0x3c800000` | 64–127 | PSRAM offset `0x350000`, 4 MB |
| user `.text` (IBUS) | `0x42c00000` | 192–199 | `0x8036` = PSRAM page 54 |
| user `.data` (DBUS) | `0x3d000000` | 256–263 | `0x8037`, `0x8038` |
| user heap (DBUS) | `0x3d200000` | 288–303 | `0x8039`…`0x8048` |

`esp32s3_spiram.c` maps PSRAM at `mmu_valid_space()`, i.e. immediately after
the last entry the flash mappings occupy — so the window **moves as the
kernel image grows**, and it already has: it was at entry 10 when this table
was first written and is at entry 11 now.

Everything derived from where that window lands has to be checked against the
hardware rather than assumed, because getting it wrong is nearly silent.
`up_allocate_pgheap()` now does three checks and panics on any of them:

- the pool lies inside the mapped window;
- `CONFIG_ARCH_PGPOOL_PBASE` equals what the cache MMU *actually* maps at
  `CONFIG_ARCH_PGPOOL_VBASE` (`esp32s3_mmu_paddr()`), not what arithmetic
  says it should be. This one had already drifted: `PBASE` was `0x360000`
  against a window that had moved, making `esp32s3_pgvaddr()` one page low —
  see the second entry in §8's bug list;
- no user window overlaps the kernel's, compared **by entry**, since the two
  buses share one entry per 64 KB.

The user `.text` window used to sit at entry 128, inside the kernel window,
which is what that last check exists to catch. It is now at entry 192.

---

## 4. Locked design decisions

1. **User pages live in octal PSRAM**, via the IBUS (.text) and DBUS
   (.data/heap) cache windows.
2. **Isolation comes from the window remap alone, not from per-select PMS.**
   All processes share the same window virtual addresses and only one
   environment is resident at a time, so PMS is to be programmed **once at
   boot**; `up_addrenv_select()` does not touch it.
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
| `568f377c15` | **PMS permissions** — the kernel/user boundary is enforced (§7.1) |
| `1018acde8c` | **signal delivery** to a user process (§8, seventh bug) |
| `c31801705a` | **WORLD1 vector table** and the world/entry setup (§7.1) |
| `7c57f06f3c` | the world split moved into `esp32s3_isolation.c` |
| `67e9b2fb84` | the board defconfig `esp32s3-devkit:kernel_oct` (§6.1) |
| `63e50dde0e` | **addrenv_switch() on voluntary context switch** — the spawn fix |
| `1b02d09dd3` | per-thread **kernel stack** |
| `3e98f4e7a7` | doc: first BUILD_KERNEL boot |
| `907d814aff` | **cache coherency** for loaded text + page-pool validation |
| `df99d323dd` | `sig_trampoline` in crt0 for BUILD_KERNEL |
| `fbe07821b8` | `up_allocate_kheap()` for BUILD_KERNEL |
| `d04701d22f` | **IRAM placement** for kernel builds (`ARCHLIB` macro) |
| `4b862af4cb` | board `gnu-elf.ld` + boot ROMFS plumbing |
| `ff6cf241bb` | `up_addrenv_mprot()` |
| `f3c29741ef` | fully linked ELF programs on Xtensa |
| `3029abe27c` | `esp32s3_pgalloc.c` — page pool and heap growth |
| `6bf42e73eb` | kernel-mode trampolines gated `!BUILD_FLAT` |
| `4f34a9853f` | syscall privilege path for BUILD_KERNEL |
| `f32134db35` `69e905cfd7` `ffb8544ac7` | Unit E — the `up_addrenv_*` set |
| `e6a105169e` | `mm/pgalloc` 32 K/64 K page support |
| `9c1c4cc487` `bb61a86d0e` | Units D and C — `arch_addrenv_t`, capability Kconfig |

Everything is gated so that existing **flat and protected builds are
unchanged**; `esp32s3-devkit:elf_oct` (flat, silicon-validated) and
`esp32s3-devkit:knsh` (protected, boots to nsh) both still build clean.

**Not committed:** `boards/xtensa/esp32s3/esp32s3-devkit/src/romfs_boot.c` is
a generated artifact and is deliberately untracked.

**Also not committed yet** — the §7.2 window work, sitting in the working
tree and validated on silicon (see the 2026-07-25 window-cleanup entry in the
progress log). It is three separable units:

| files | what |
|---|---|
| `esp32s3_mmu.{c,h}`, `esp32s3_pgalloc.c`, `kernel_oct/defconfig` | `PBASE` corrected to `0x350000`, `esp32s3_mmu_paddr()`, and the boot check that catches the next drift |
| `esp32s3_mmu.{c,h}`, `esp32s3_addrenv.c` | `esp32s3_mmu_unmap()` and the unused-entry invalidation in `up_addrenv_select()` |
| `kernel_oct/defconfig`, `scripts/gnu-elf.ld`, `esp32s3_pgalloc.c` | text window entry 128 → 192, plus the overlap check |

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
- **The map** (§3): `ARCH_TEXT_VBASE=0x42800000` in the IBUS window with 8
  pages; `ARCH_DATA_VBASE=0x3d000000` and `ARCH_HEAP_VBASE=0x3d200000` in
  the DBUS window with 8 and 16; page pool `ARCH_PGPOOL_VBASE=0x3c400000`,
  `ARCH_PGPOOL_PBASE=0x360000` — a PSRAM *offset*, not an address — and
  `ARCH_PGPOOL_SIZE=4194304`, which is Kconfig type `int` and so must be
  decimal. `MM_PGSIZE=65536` matches the cache MMU's page.
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
x/8xw 0x600c5300      # text window, entries 192-199
x/8xw 0x600c5400      # data window, entries 256-263
x/16xw 0x600c5480     # heap window, entries 288-303
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

Two more that are about process-to-process isolation rather than the
kernel/user boundary:

```sh
nsh> pffault r 0x3c410000    # the pool, via the kernel window -> STILL LEAKS
nsh> pffault r 0x3d2f0000    # own last heap page -> must read 0 (the wipe)
```

`SURVIVED unexpectedly` means the access went through.  Pick an address whose
contents you know -- `objdump -s -j .dram0.data nuttx` -- because a *denied*
read can also return zero, so reading zero proves nothing either way.  For
the same reason, run something like `getprime` first when checking the wipe:
against a freshly booted board a stale page may happen to be zero anyway.

---

## 7. Remaining work, in priority order

### 7.1 The kernel/user boundary — done

`67e9b2fb84`, `7c57f06f3c`, `c31801705a` and `568f377c15`.  A user process now
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

**What this does not do is isolate processes from each other.**  They all
draw pages from one pool that the kernel keeps mapped at 0x3c0b0000, and the
external-memory permissions (`APB_CTRL_SRAM_ACEn_*`, the PSRAM counterpart of
the `FLASH_ACE` registers `esp32s3_pms.c` drives, and still unused here) are
indexed by physical address.  §7.2 has the measurement and why the fix cannot
come from the permission control.

### 7.2 Isolate processes from each other — the next unit

Still the top of the list.  The window hygiene this unit was originally
scoped as is **done**, and measuring it first showed it was never going to be
enough on its own:

```
nsh> pffault r 0x3c410000
pffault: SURVIVED unexpectedly, read 3d01176c   <- NSH's .bss address
```

That is a user process reading a word out of NSH's text page through the
kernel's PSRAM window at 0x3c0b0000, which maps the whole pool as a plain
valid mapping.  It has nothing to do with stale window entries.

What is done:

- unused window entries invalidated on a select (`esp32s3_mmu_unmap()`),
  proved over JTAG since an invalidated entry reads 0 rather than faulting;
- the user `.text` window moved off the kernel's, entry 128 → 192, with a
  boot-time overlap check;
- `CONFIG_ARCH_PGPOOL_PBASE` corrected and made self-checking — it had gone
  stale, and one page per region was leaving `up_addrenv_create()` unwiped
  (§8).

**What is left is the kernel's own mapping, and the fix has to be virtual.**
The external-memory permissions (`APB_CTRL_SRAM_ACEn_*`, the PSRAM
counterpart of the `FLASH_ACE` registers `esp32s3_pms.c` drives) are indexed
by **physical** address.  A process's own pages *are* pool pages, so any
physical range that lets a process reach its own memory also lets it reach
the pool through the kernel's window.  There is no permission setting that
expresses the difference.  Do not spend time looking for one.

So the only lever is which virtual addresses have mappings at all: the kernel
must stop keeping the whole pool mapped while an unprivileged task runs.
NuttX already has the shape — `CONFIG_ARCH_KMAP_VBASE`/`ARCH_KMAP_NPAGES`,
the scratch region used when `ARCH_PGPOOL_MAPPING` is off ("There is no need
to use the scratch memory region if the page pool memory is statically
mapped", `include/nuttx/addrenv.h`).

The kernel's uses of the pool window are few and all short, all in kernel
context: `esp32s3_pgwipe()`, `up_addrenv_page_vaddr()`, `up_addrenv_pa_to_va()`
and `up_addrenv_va_to_pa()` in `esp32s3_addrenv_utils.c`, plus the fork copy
when it arrives.  A one- or two-entry scratch window mapped on demand and
invalidated after would serve all of them.  `Kmem` is internal DRAM in this
configuration (see the `free` output in §1), so the kernel needs PSRAM for
nothing else — the whole 8 MB window exists only to reach pool pages.

Two things to watch:

- `esp32s3_pgvaddr()` is currently a pure arithmetic function whose result
  callers may hold across operations.  With a scratch window it becomes a
  mapping with a lifetime; the callers above need auditing for that.
- `esp32s3_addrenv.h` has a hard `#error` requiring
  `CONFIG_ARCH_PGPOOL_MAPPING`.

### 7.3 Eager `fork()`

Once processes are isolated: `up_addrenv_create()` for the child sized to the
parent's regions, copy each parent page into the child's through the kernel
`page_vaddr` mappings, set the child's return frame (0 in the child, the pid
in the parent). No COW, no fault handling. Costs a full copy per fork and has
no lazy growth — acceptable for the static-sandbox target.

---

## 8. Gotchas that cost real time

- **esp-hal stale objects.** The `esp-hal-3rdparty` objects do not depend on
  `sdkconfig.h`. Any flash/PSRAM/boot-format config change needs `make clean`
  or the build silently keeps the old settings. Symptom: an image byte-identical
  to the previous one.
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

### The on-target bugs already found and fixed — do not re-derive them

1. **IRAM placement** (`d04701d22f`) — the section scripts place IRAM code by
   archive name and every rule said `*libarch.a`; a kernel build archives into
   **`libkarch.a`**, so all 265 rules missed and the octal-flash bring-up
   functions stayed in mapped flash, called while that mapping was being
   reconfigured. Boot loop with no output. Fixed with an `ARCHLIB` macro.
2. **`up_allocate_kheap()`** (`fbe07821b8`) — had only PROTECTED and FLAT
   branches, so BUILD_KERNEL left `kbase`/`ktop` uninitialised and the heap
   landed at NULL. GCC warned about it.
3. **`crt0.c` `sig_trampoline`** (`df99d323dd`) — referenced but never defined
   on Xtensa; the kernel path had never been compiled. Cannot come from
   `xtensa_signal_handler.S` (that is in libarch, and user programs link only
   `-lmm -lc -lproxies`). GCC has **no `naked` attribute on Xtensa**, so it is
   file-scope assembly.
4. **Cache coherency** (`907d814aff`) — the "runs, then executes a hole" bug.
   Text is written through the DBUS alias but fetched through IBUS, and one
   global table means stale I-cache lines from a previous mapping read back as
   zeroes. Fixed in `up_addrenv_coherent()` and `up_addrenv_mprot()`.
5. **`ARCH_PGPOOL_PBASE`, twice.** First (`907d814aff`) PSRAM mapped at
   `0x3c0a0000`, so the pool's physical base was `0x360000`, not `0x400000`;
   every page wipe was landing 640 KB away.  Then the kernel image grew, the
   window moved up one entry to `0x3c0b0000`, and `0x360000` was stale by
   exactly one page — so `esp32s3_pgwipe()` wiped the page *below* the one
   allocated.  `up_addrenv_create()` allocates ascending, so each page was
   wiped by its successor's allocation and only the **last page of each
   region** went out unwiped, carrying the previous tenant's data into a new
   process.  It presented as `pffault r 0x3d2f0000` — a process's own last
   heap page — reading `aabc6aaa` on a fresh boot.  `PBASE` is now
   `0x350000` and `up_allocate_pgheap()` checks it against
   `esp32s3_mmu_paddr()` rather than trusting it.  **Expect this to drift
   again**; the check is what makes it loud.
6. **`addrenv_switch()` on voluntary context switches** (`63e50dde0e`) —
   Xtensa called it only from `xtensa_irq_dispatch()`, but `up_switch_context()`
   is a `SYS_switch_context` system call here, so every voluntary switch left
   the address environment alone. A resumed shell ran against its successor's
   freed pages. Two dead ends were eliminated first, and are worth not
   repeating: it is **not** the signal-delivery path, and it is **not** the
   windowed-ABI base save area (moving `A1` does not need that copy here,
   and copying *back* would actively corrupt the user's save area).
7. **Signal delivery to a user process** (`1018acde8c`) — three faults in the
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

- Design note: `2026-dynamic-elf-mmu-isolation.md`
- Dated evidence, including why COW/demand paging are dead:
  `2026-dynamic-elf-progress-log.md`
- Delivery plan / checklist: `2026-dynamic-elf-delivery-plan.md`,
  `2026-dynamic-elf-checklist.md`
- Templates being mirrored: `arch/risc-v/src/common/riscv_addrenv*.c`,
  `riscv_addrenv_kstack.c`, `arch/arm/src/armv7-a/arm_syscall.c`
  (kernel-stack switch in C), `boards/risc-v/qemu-rv/rv-virt` and
  `boards/risc-v/k230/canmv230` (ROMFS kernel-build boards)
