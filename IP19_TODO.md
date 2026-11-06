# IP19/Challenge (Everest) implementation plan

> Full implementation plan: [`IP19_PLAN.md`](IP19_PLAN.md).
> IRIX target is 6.5.22m; 6.5.30m is not supported on IP19 Onyx/Challenge.
> Bring-up starts with one CPU; the final target is four CPUs/SMP.
>
> SMP topology, MC3/RAM limits, bootstrap sequence and the first 2-CPU
> bring-up are documented in [`IP19_SMP.md`](IP19_SMP.md).

## Network/telnet checkpoint (2026-11-01)

- Installed 2-CPU/2-GiB IP19 root on `ip19-yoda2.raw` boots to
  `yoda console login:`.
- `/etc/sys_id` is `yoda`; `/etc/hosts` contains `10.6.2.1 yoda`.
- IRIX network startup is enabled with `chkconfig network on`; no startup
  script was modified. `ifconfig-1.options` contains only `up` so the
  EPC interface reaches `UP,RUNNING` at boot.
- Host TAP is `tap-iris` on `br0`; no SLIRP is used by default.
- Verified: host `ping 10.6.2.1` succeeds, `telnet 10.6.2.1` logs in as
  `root` (no password), and `hostname` returns `yoda`.
- Launchers are explicit: `run-irix.sh/.bat`, `run-irix53.sh`, and
  `run-challenge.sh/.bat`.
- MTTCG is forced for Challenge with `-accel tcg,thread=multi`
  (`IRIX_MTTCG=1`; default in `run-challenge.sh/.bat`). Verified with two
  separate host threads (`CPU 0/TCG`, `CPU 1/TCG`), 2 CPUs detected, and
  telnet login. QEMU still prints its conservative MIPS64 MTTCG warning.

## SMP bring-up checkpoint (2026-11-01, 2 CPUs/2 GiB booting)

- An IP19 is a 4-slice CPU board. Two CPUs is the common board population
  and the meaning of the handbook's “IP19 = 2xR4400”; four CPUs use slices
  0-3 on the same board. Deskside Onyx has one CPU slot (up to 4 CPUs);
  Challenge L has three (up to 12); rack Onyx/Challenge XL use up to 6/9
  boards (24/36 CPUs).
- MC3 is 2 leaves x 4 banks x 4 SIMMs, 2 GB maximum per board, 8 boards
  maximum (`EV_MAX_MC3S`). Four CPUs in a deskside are limited to 2 GB; a
  rack can theoretically reach 16 GB with 8 MC3 boards. The current QEMU
  machine has one MC3 and accepts at most 8 GB; recommend 2 GB.
- Implemented: group-SENDINT latching in `sgi_ip19.c` so MPCONF init is not
  lost when a slave joins its group late; deterministic secondary reset hold
  until CPU 0 claims the master group; one-shot SMP RTC/`us_delay()` boot
  yield for single-threaded TCG. Local PROM patches make `check_slaves()`
  wait for `EVDIAG_NOT_SET` and skip the slave cache-init block on the
  no-diags path.
- Verified `-smp 1 -m 512M`, `-smp 2 -m 512M`, and `-smp 2 -m 2G` all reach
  `IRIS console login:`. `hinv` on `-smp 2 -m 2G` reports two IP19 processors
  (R4000 rev 3.0) and 2048 MB.
- The old “Processor #1 did not start!” failure was a false timeout: CPU1 was
  in the kernel and later set `slave_loop_ready[1]`/`pdaindr[1].CpuId = 1`,
  but CPU0's boot timeout expired first. When it failed, CPU1 was in a normal
  timer-interrupt/klog path spinning on `putbuflck`; CPU0 held that lock
  inside the nested-panic path. The RTC/`us_delay()` yield fixes the
  scheduling race.
- Next blocker: replace the fixed `cpu_index -> slot 1/slice index` mapping
  with an explicit topology and multi-board `SYSCONFIG`/config defaults for
  `-smp 4`; then add a multi-MC3 model.

## Installed root boots to stable multiuser/login (2026-11-01, latest)

Validated with the installed 6.5.22 root on `ip19-installed.chd`, one CPU,
512 MiB, `-snapshot`, headless EPC serial:

```
qemu-system-mips64 -M ip19 -smp 1 -m 512M \
  -bios sgi/challenge/ip19prom.bin -L sgi/challenge -snapshot \
  -drive file=.../ip19-installed.chd,format=chd,if=none,id=ip19disk \
  -device scsi-hd,drive=ip19disk,bus=main-system-bus/sgi-s1/scsi.0,scsi-id=1 \
  -display none -serial unix:...,server=on,wait=off
```

Select menu option 1.  The system reaches `IRIS console login:`.  `root`
has no password; accept the default `TERM = (vt100)`.  The root shell,
`date`, `ifconfig et0 up`, `who`, `ps`, and a logout/relogin cycle all work.

### 64-bit `/lib64/rld` and savecore fixed

Every 64-bit dynamic executable (including `/usr/etc/savecore` and
`/usr/sbin/strace`) failed with `Could not map rld from file /lib64/rld`.
The failure was not in the rld file or in `SGI_ELFMAP`:

- IRIX places a 64-bit user stack just below `DEFUSRSTACK_64 = 0x10000000000`
  (1 TiB), i.e. in **xuseg**, and clears `Status.UX` while running in the
  kernel (`SR_KERN_SET` is only `SR_KX` for R4000).
- `copyin()`/`copyout()` then address that xuseg stack with `UX=0`.
  QEMU's `get_physical_address()` required `UX` for *all* xuseg accesses,
  returned `TLBRET_BADADDR`, and the kernel turned the copyout fault into
  `EFAULT`.  `read()` of the first 512 bytes of `/lib64/rld` therefore
  returned `-1`, and `__get_rld` printed the failure.
- Fix (`target/mips/system/physaddr.c`): when the IP19 machine model is
  active, allow kernel-mode xuseg access when `KX` is set even if `UX` is
  clear.  User-mode xuseg still requires `UX`, and other machines are
  unchanged (the condition is gated on `ip19_io4_window`).
- Trace evidence: libc `__elfmap` entered with the correct path, then the
  kernel `copyout` of user address `0x000000ffffffac60` returned
  `TLBRET_BADADDR` with `Status=0xff81` (`UX=0`, `KX=1`).  Forcing `UX=1`
  at that fault immediately made `/lib64/rld` map and let savecore run.

`S48savecore` now runs `savecore` successfully.  The remaining
`S48savecore[108] ... Memory fault(coredump)` is from the N32 `icrash`
processing the **stale crash dump already in the image's swap area** (from
the old pre-fix `init` SIGSEGV).  The dump in `/dev/swap` still contains
`PANIC: init died ... what = 0xb` and the old warnings.  `icrash` segfaults
on that stale dump; this is not the 64-bit rld path anymore.  It is
non-fatal to boot/login and will disappear once the dump is cleared on a
writable image; under `-snapshot` the stale swap contents recur every boot.

### NVRAM, TOD, serial, console warnings

Implemented in `hw/mips/sgi_ip19.c` from `sys/EVEREST/{epc,nvram,sysctlr}.h`
and the IRIX `if_ee`/`evclock` drivers:

- **DS1397 RTC**: dynamic host-backed time in register bank `0x200`; `VRT`
  set, oscillator on, BCD/binary and 12/24-hour control bits honoured,
  `SET`-mode writes parsed, `SEC/MIN/HOUR/DAY/MONTH/YEAR` advance.  Both
  `IO4 NVRAM/time-of-day chip reports invalid RAM or time` and `System
  time-of-day chip is not advancing` are gone.
- **NVRAM defaults**: revision 9 (matching the 1993 IP19/IO4 PROM), console
  `d`, `SystemPartition/OSLoadPartition/OSLoader/OSLoadFilename`, `root`,
  `dbaud/rbaud`, `AutoLoad=No`, `TimeZone`, `scsihostid`, `sgilogo`,
  `nonstop`, and a valid NVRAM serial (`S1234567`).  The old (revision-9)
  checksum is valid, so `NVRAM checksum is incorrect: reinitializing` is
  gone, as is the kernel `serial number ... not valid` warning.
- **CC UART sysctlr protocol**: responds to `SC_GET SC_SERIAL` and accepts
  `SC_SET SC_SERIAL`, so the PROM and kernel both read a valid serial.
- **Inventory**: the PROM still prints `WARNING: hardware inventory is
  invalid.  Reinitializing...` because the MC3 config bus does not report
  per-bank sizes to `EVCFGINFO` the way the PROM's `write_inventory()`
  expects (it records 16 MiB/empty banks instead of the 512 MiB layout).
  The PROM self-heals this each boot and it does not affect operation; see
  the MC3 notes below for the remaining model gap.
- **IO4 revision**: board level raised to rev 2 (`0x21`), removing
  `Downrev IA Chip (1) on the IO4 board in slot 3`.

### EPC Ethernet (`et0`)

- Added the EPC PBUS Ethernet ID PROM at EPC offset `0x2000`; `if_ee` reads
  slot 5 first, so the MAC is stored in reverse slot order.  The driver now
  sees `08:00:69:19:00:01`, so `WARNING: et0: bad enet addr` is gone.
- With the existing `TINDEX == TTOP` reporting, `ee_init()` completes.
  `et0` configures as `192.0.2.1`, `ifconfig et0 up` sets `UP,RUNNING`,
  and `netstat -in` shows the interface without hanging.
- The EPC transmit/receive DMA engines and EPC interrupt delivery are still
  not modelled and no QEMU net backend is attached, so packets do not reach
  a host network.  This is an attach/config milestone, not working IP yet.

### Earlier KPTE/user-PTE fix (2026-11-01, historical)

Root cause of the original `init`/N32-rld SIGSEGV was the IP19 KPTE
workaround, not the IO4/uncached redirect:

- `ip19_install_kpte_entry()` synthesized TLB entries for both the kernel
  K2SEG KPTE window (`KPTE_KBASE`, `0xc00000fc60000000`) and the **user**
  KPTE window (`KPTEBASE`, `0xc00000fc00000000`), backing both with the
  global kernel `kptbl` array.
- The user KPTEBASE window maps the **current process's** page tables,
  which are separate allocations.  Backing it with `kptbl` aliased user
  PTEs onto unrelated kernel PTEs.
- Traces showed the failure modes: the rld stack TLB entry was replaced by
  the shared zero page (`pfn 0x225c000`, `C=7`, `D=0`) after a normal
  `invaltlb`, so rld read a zeroed stack, called through a null pointer,
  and init looped in `_sigtramp`.  Earlier runs also mapped init text at
  `0x10000000` to a page containing `/unix` at file offset `0x4ac8`.
- Fix: synthesize only the kernel KPTE windows with `kptbl`; leave the
  user KPTEBASE window to the kernel's own TLB management.
  `target/mips/tcg/system/tlb_helper.c` (`ip19_kpte_bases_installed`).

The uncached-user adjustment was not responsible and is left in place for
real device mappings: a full boot trace recorded **zero** user TLB entries
with cache algorithm 2 and **zero** `ip19_device_window_phys()` redirects
for user addresses.  Such pages are cacheable `C=5`; only K1/XKPHYS device
accesses were redirected.  The 512 MiB EPC/`Inst>` path still works.

## init SIGSEGV diagnosis (2026-11-01, historical)

The clean-installed image still panics immediately after the EPC console is
configured:

```
PANIC: init died (why = 3, what = 0xb)
Dumping to /hw/scsi_ctlr/0/target/1/lun/0/disk/partition/1/block ...
```

`why = 3` is `CLD_DUMPED`; `what = 0xb` is signal 11 (`SIGSEGV`).  The image
and volume header are intact.  Single-user (`single`, or
`boot -f dksc(0,1,0)/unix --s` in sash) still runs `/sbin/init`; when the
SIGSEGV is held/ignored the kernel reports:

```
ALERT: Process [init] 1 generated trap, but has signal 11 held or ignored
    epc 0xfafb08c ra 0xfafb084 badvaddr 0x0
Process has been killed to prevent infinite loop
```

Evidence collected on a qcow2 overlay (base image never written):

- Root XFS starts at byte `0x8200000` (volume-header partition 0).  Modern
  `xfs_db` refuses the installed V1-directory filesystem; a locally built
  xfsprogs 3.1.11 (`/tmp/xfsvers/xfsprogs-dev-v3.1.11/`) can read it.
  `xfs_db -r -c 'blockget -n' -c 'ncheck' <partition>` lists the tree.
- Extracted files: `/tmp/unixIP19.elf`, and the installed
  `{sbin_init,etc_inittab,lib32_libc.so.1,lib32_rld}`.
  `/sbin/init` is `ELF N32 MSB`, interpreter `/lib32/libc.so.1`, RPATH
  `/lib32`.  `/lib32/rld` is the N32 runtime loader, linked at `0x0fb60000`.
- `qemu-system-mips64 -d int` shows the fatal user faults are inside
  **/lib32/rld**, not libc's `_sigtramp`:
  - `resolve_symbol` / `general_find_symbol` (`rld.c:2915,2939,3004`) store
    through a null output pointer (`sw s0,0(s1)`, `s1=0`).
  - `resolve_relocations` (`rld.c:3447`) is called with `a0=0` and faults at
    `lw at,104(a0)` (`badvaddr 0x68`).
  - `read_aux_vec_and_build_objlist` (`rld.c:6820,6853`) is on the same path.
  - A `coprocessor unusable` exception at `_get_fpc_csr` (`cfc1`) occurs
    just before the loader walks into the bad object list.
  The first fault occurs before init reaches `main`/inittab.  The later
  `_sigtramp` loop is init's own SIGSEGV handler (`siglvl`, `inittab`
  `LVLa`) returning through a nested signal frame with a corrupted GP/RA.
- Replacing both `/lib32/rld` and `/lib32/libc.so.1` with the matching 6.5.22m
  copies extracted from the install-tools miniroot (`mr` XFS on
  `IRIX-6.5.22-Overlay1.iso`) did **not** change the crash.  This makes a
  simple rld/libc version mismatch unlikely and points at the emulated N32
  dynamic-linking/exception path (FP emulation, auxv/object list, or signal
  context restore).

### Follow-up trace (2026-11-01, later)

A fresh `-d int` run was captured on a clean qcow2 overlay
(`/tmp/ip19int6.log`; older copies are not kept in the tree).  The first
event chain is:

- The kernel enters `/lib32/rld` at `newmain` (`0xfb604ec`) normally.  The
  user stack and auxv are correct:
  `argc=1`, `argv[0]="/etc/init"`, empty `envp`, then
  `AT_PHDR=0x10000034`, `AT_PHENT=0x20`, `AT_PHNUM=7`,
  `AT_ENTRY=0x1000284c`, `AT_BASE=0x0fa00000`, `AT_PAGESZ=0x4000`.
  There is no auxv/argv/envp construction error at entry.
- rld resolves the libc object (`obj=0xfbe1418`, relocations 0..~2968 with
  addends in the `0xfa…` image) and the init object (`obj=0xfbe2438`,
  addends `0x100…`).  The object list itself is non-null throughout this
  path; the earlier `a0=0`/`rld.c:3447` trace is not reproduced in every
  boot.
- The first `coprocessor unusable` is the normal lazy-FP trap at
  `_get_fpc_csr` (`cfc1`).  The kernel returns with `Status.CU1` set and
  execution continues; forcing CP1 always-on for IP19 did not change the
  crash, so the FP trap is not the root cause.
- The actual `SIGSEGV` is generated after rld transfers control to
  init/libc.  The process then enters libc `_sigtramp` at `0xfafb020`,
  loads its errno pointer from GOT slot `0xfb4fd74`, gets zero, and repeats
  `sw t1,0(t0)` at `0xfafb08c` with `badvaddr=0`.  That is init's own
  signal handler looping, not the primary fault.
- `SGI_S1_TRACE=1` shows only successful disk commands (`status=0 resid=0`)
  and DMA writes to normal page-cache pages (for example `0x78b…`,
  `0x80a…`); no S1 data error or short read precedes the crash.
- A critical remaining anomaly: the code actually executed at the shared
  libc mapping (`0xe000000` region) does not match the `/lib32/libc.so.1`
  extracted from the installed image.  For example `-d in_asm` decodes
  `0xe046d78` as an unaligned-`strcpy` variant, while the on-disk file has
  `_vfprintf` at that link address (`strcpy` is at `0xfa334a4`).  Extracted
  disk files were re-read from the raw image through a read-only loop and
  match the extracted files byte-for-byte, so the disk image is intact.  This
  points at the kernel/rld page mapping (page-cache vs. user PTE
  assignment), not at the on-disk rld/libc version.

Attempts that did **not** help and were reverted: forcing CP1 usable in
`helper_mtc0_status`, and preferring the installed kernel `kptbl` address
before the miniroot address in the IP19 KPTE workaround.

Hard blocker for the next session: establish whether the user PTE for
`0x10000000`/the shared libc is being assigned a page that is still owned by
the kernel/page cache (user text page read at `0x10000000` resolved to a
physical page containing `/unix` file data at offset `0x4ac8`).  Capture the
TLB entry actually installed by the kernel (ASID, VPN, PFN) while the
process is in user mode, and compare its PFN against the S1 DMA target for
that page.  If the PFN is wrong, the fault is in the IP19 KPTE/PTEBase or
page-allocation path; if it is right, dump the page before and after the
copy and check the kernel `copyout`/page-in path.

Preserved state:
- Base `ip19-installed.raw` and its volume header were not modified.  All
  boot/experiment writes used qcow2 overlays; filesystem experiments used
  reflink clones (`ip19-fsck.raw`, `ip19-rldtest.raw`).
- Reproduction: create `qemu-img create -f qcow2 -F raw -b
  ip19-installed.raw over.qcow2`, then
  `IRIX_HEADLESS=1 IRIX_DISK=.../over.qcow2 IRIX_DISK_FORMAT=qcow2
  ./run-challenge.sh`; select menu 1 (or 5 and `boot -f dksc(0,1,0)/unix`).
  Historical console logs and traces are not kept in the tree.

Next concrete steps:
1. Capture `rld` diagnostics: `setenv _RLD_ARGS -trace` is passed through
   (it changes the failure to ALERTs from pid 8 and 1), but trace output is
   not reaching the console.  Use a static wrapper as `initfile` to set
   `_RLD_ARGS=-log /dev/console -debug general,map,symbol,olist` and exec
   `/sbin/init`, or log to an already-open console fd.
2. Instrument/dump the auxv and object list at rld entry (`0xfb604ec`) and
   `read_aux_vec_and_build_objlist`; verify `AT_PHDR/AT_ENTRY/AT_BASE/
   AT_PAGESZ` and the object array.  This is the nulled pointer source.
3. Re-test the installed rld/libc on the IP32 IRIX host with the same
   `/sbin/init` to separate bad userland from emulation.
4. If the N32 loader path remains a hard blocker, build a static N32
   init/getty/login set on the IRIX host as a workaround; the static
   `/sbin/sh` already executes, but the kernel gives it no interactive args
   when used directly as `initfile`.

## Current bring-up checkpoint

Verified in QEMU with the rebuilt PROM and `-smp 1 -m 512M`:

- Reset vector, cache flush, CC-local, CC-config, EAROM writeback, bus tags,
  and secondary-cache tests pass.
- MC3 memory configuration and the built-in memory test pass.  MC3 now owns
  the low physical memory decode (machine RAM is no longer mapped directly
  at 0), using 512 MiB as four 128 MiB banks in two 2-way interleaves.
  `hinv` reports 512 MiB and the S1 disk/CD remain visible.
- CPU is detected as `01/00` bootmaster. IO4 and EPC are found and the EPC
  UART produces the console output.
- The system-controller UART now has a real RX FIFO and answers the debug
  switch query; `VDS_NO_DIAGS` is honored.
- The IO4 PROM image was recovered from `IO4prom/io4prom.uunc` as
  `sgi/challenge/io4prom.bin`. Large-window flash aliases at
  `0x08000000`/`0x08100000` are mapped.
- The PROM now reports `Enabled 256 Megabytes` and `Enabled 1 processor`,
  which confirms the inventory/CPU path.
- **IO4 monitor:** the IO4 PROM handoff now works. The `.uunc` image was
  converted to a real flash image, and the R4000 segment was extracted,
  LZW-decompressed, and loaded at `0x1800000`. The IO4 PROM Monitor boots:
  `IO4 PROM Monitor SGI Version 4.21 Rev A IP19, Sep 3 1996 (BE)`.
- The correct old `JFK4` IO4 PROM is in use. The monitor boots as version
  1.09, completes saio, and reaches the interactive System Maintenance
  Menu. `hinv` reports IP19, 504 MHz R4000, 256 MB, caches.
- Serial input is wired through the EPC DUART, and the Command Monitor is
  fully usable (`hinv`, `help`, `version`, `boot`).
- **Milestone 1 complete:** `hw/mips/sgi_s1.c` now models the IO4 S1/WD95
  controller and DMA path. IO4 reports `SCSI` in `IODEV1`, the PROM probes
  all three WD95 channels, and an `-device scsi-hd` disk attached to
  `main-system-bus/sgi-s1/scsi.0` appears as `scsi(0)disk(1)`.
- **Milestone 2 complete:** with the real IRIX CHD attached, `ls` reads the
  volume header and `boot` loads `sash` (`Standalone Shell SGI Version 6.5
  ARCS`). The 6.5.30 `/unix` kernel panics with a UTLB miss, consistent
  with IP19 support ending at 6.5.22m.
- S1 exposes one SCSI CD-ROM target (5) with 512-byte logical blocks.
  The install-tools CD boots the IRIX installer; other CDs are swapped
  through the single drive.
- A disposable writable CHD copy is the install target. The installer
  proceeds, but currently floods `WARNING: excessive R4000 badvaddr(1)
  occurrances for K2 addresses - may be affecting performance!`.
- Extracted the real 6.5.22 IP19 miniroot kernel from `IRIX.tar`:
  `IRIX/6.5.22/Overlays_1_of_3/dist/miniroot/unix.IP19`.
- **K2/KPTE blocker fixed:** an IP19-gated workaround synthesizes the
  missing KPTE/kptbl wired entry on demand, using the kernel's `kptbl`
  pointer. It is enabled only for `-M ip19`; `R4000SC`/Indy is unchanged.
- **User-copyout blocker fixed:** IRIX 6.5.22 writes the KPTE base only to
  CP0 `Context` (`0x3f000000000`) while its refill handler reads
  `XContext`.  R4000/R4400 implement the `PTEBase` fields of those two
  registers as shared (the R4000 manual says both are updated on a miss;
  the MIPS R4400 note says "the PTEbase fields are shared").  QEMU modelled
  them independently, so the first user `copyout()` to `0x10000000` fetched
  its PTE from `c000000000020000` instead of `c00000fc00020000` and took a
  TLBS fault.  `ip19-pte-base-shared` now mirrors the shared bits for IP19
  only.
- With that fix the 6.5.22 kernel boots past `copyout`, prints the IRIX
  banner, probes all three WD95 controllers, configures EPC as `et0`,
  selects the root disk, and reaches `Creating miniroot devices`.
- **IO4 window blocker fixed:** `io4.h` defines IP19 `SWINDOW_BASE` as
  K1 `0x10000000`, with 19-bit region and 16-bit physical-adapter fields
  and 64 KiB per adapter slot.  `IP19prom` `initialize_io4()` programs
  `IO4_CONF_LW/SW` with window 1; the EPC is region 1/padap 1 and S1 is
  region 1/padap 4.  The real decoded small-window range is therefore
  `0x10080000..0x100fffff` (8 x 64 KiB), not the old 128 MiB
  `0x10000000..0x17ffffff` mapping.  The oversized region shadowed real
  RAM at `0x10214000`/`0x103b8000` and caused the `EBUS Error Register:
  0x9` panic from `page_copy`/`bcopy_stores`.
- With the window reduced to region 1, both 512 MiB and 256 MiB reach
  `Creating miniroot devices` without the EBUS panic.
- **Guest clock fixed:** the machine was driving CPU Cause IP7 from the
  system-controller interrupt state, which cleared the R4000 Count/Compare
  timer line whenever a device interrupt was recomputed.  The guest clock
  stopped after ~30 ticks.  IP7 is now left to the CPU timer.
- **CC timer comparator implemented:** IRIX timeouts (`sleep`, callout
  queues, network retries) use the CC 50 MHz RTC/comparator local
  interrupt (`EV_CMPINT`, Cause IP3 / `SRB_TIMOCLK`), not the CP0
  scheduling clock.  The machine now arms a per-CPU comparator timer from
  `EV_CMPREG0..3`, raises `CMPINT`, and clears it through `EV_CIPL124`.
  Without this, `sleep 10` never expired and the boot stalled after the
  network warning.
- **512 MiB IO4/RAM overlap fixed (K1/uncached alias):** on machines
  larger than 256 MiB the IO4 small-window physical range
  `0x10080000..0x100fffff` is also ordinary RAM.  Real hardware selects
  IO4 for K1/uncached references and RAM for cached K0/K2 references, but
  QEMU has no cache model.  `target/mips/system/physaddr.c` now redirects
  uncached IP19 references in that range to a private alias
  (`0x400080000`, same IO4 region field 1); the IO4/EPC/S1 model lives at
  the alias and cached references stay RAM.  The old per-padap RAM
  aliases are gone.  This stops cached RAM writes (for example the string
  `done`) from clobbering the EPC IID vector registers and the resulting
  `Write Address Error`/`KERNEL FAULT` panic.
- Both 256 MiB and 512 MiB reach `Inst>` and a root `sh`; both can mount
  the Foundation1 CD EFS partition 7 read-only.
- **S1 data-out fixed:** `s1_transfer_data()` now fills the SCSI target's
  data-out buffer directly from guest DMA.  MODE SELECT(6) (block size)
  succeeds, and installer disk WRITE(10)s complete with status 0.  The
  previous buffer path copied in the wrong direction and left target
  buffers unchanged.
- MODE SENSE page 0 from the CD volume path is normalized to all-pages
  for `TYPE_ROM` as well as `TYPE_DISK`, removing the non-fatal
  `asc=0x24` alert.
- The Foundation1 EFS partition must be mounted read-only, as the miniroot
  `/etc/mrinstrc` does (`efs ro,noquota`).  A read-write `mount -t efs`
  tries to update the EFS bitmap and correctly fails on the write-protected
  CD.  `mount -t efs -o ro /dev/dsk/dks0d6s7 /CDROM` works and exposes
  `/CDROM/dist` with `eoe`, `eoe.sw`, `io4prom`, etc.
- Running the real installer (`go`) on the 512 MiB machine reads product
  descriptions and writes the disk successfully.  The current test then
  stopped issuing SCSI commands after ~243 writes while the guest stayed
  CPU-busy in user mode; full installation did not complete in the test
  window.
- Generic harness completed: `run/console.py` works with any machine's
  serial socket; `run-challenge.sh` exposes `run/ip19-serial.sock` and
  `run/ip19-mon.sock` with `IRIX_HEADLESS=1`; `run/restart-guest.sh`
  accepts `--match`/`IRIX_QEMU_MATCH` and `IRIX_RUN_SCRIPT`.

PROM build work already applied to the local image (see
`IP19_PROM_PATCHES.md` for exact addresses; source under
`/home/ivan/development/irix-src/.../stand/arcs/IP19prom`):

- `libasm.s` lwin helpers changed from 32-bit `add` to `daddu` so the
  XKPHYS large-window address is not truncated.
- `POD_SCACHEADDR` relocated from `0x80000000` to `0x80800000` so cache
  tests do not clobber GDA/EVCFGINFO/MPCONF at physical 0.
- `test_master` cache calls are skipped for the no-diags path.
- The loader accepts `PROM_NEWMAGIC` and bypasses the checksum gate for
  bring-up; a real segmented loader is still needed.
- The binary `.bin`/`.coff` were patched to match; `.hex` still needs a
  proper rebuild/regeneration.

Immediate next actions:

1. Diagnose the installer stall after the initial disk writes: sample the
   user-mode PC/stack, check whether it is decompressing/verifying or a
   lost S1 interrupt/continuation, and confirm the written filesystem data.
2. Install into an IP19 root target.  The current disk root is an IP22
   installation; `inst -r /root -f /CDROM/dist` may treat the miniroot as
   the target.  Use the miniroot's `-r /root` path and a fresh/reformatted
   root partition (or a proper IP19 volume/root) for a complete install.
3. MC3: 512 MiB stable and one-way layouts boot; extend bank arrangements
   beyond the current sizes (see the MC3 notes below).
4. Then kernel storage/media, EPC Ethernet, and graphics.

## MC3 memory decode notes

- MC3 memory is added at physical 0 with priority -1.  The PROM, CC, IO4,
  GIO, EAROM and other device windows inside the low 512 MiB therefore win
  regardless of region insertion order; adding MC3 memory after the devices
  previously shadowed the PROM and caused the XTLB-miss panic.
- When the configured decode window fits in the attached RAM region,
  translation is physical-identity.  Otherwise banks are packed into the RAM
  region using the bank offsets computed by the layout pass.
- `BANKENB == 0` is treated as "not constrained" (all installed banks
  respond), because the PROM clears it while probing bank sizes and continues
  executing from RAM before calling `activate_memory`.
- Bank size/base/if/ip writes update the live configuration registers, but
  the decode layout is committed only when `BANKENB` is written.  This
  matches the PROM's `activate_memory()` step and prevents transient holes
  while it reprograms an interleave.  Stable 2-way interleave remains the
  machine default; both stable and one-way boot at 512 MiB.
- Full-identity committed layouts install an alias child under the MC3
  memory region, so normal CPU and S1 DMA accesses resolve directly to the
  backing RAM instead of the slow per-access IO translate path.  Partial or
  non-identity layouts fall back to the IO decoder.
- With MC3 memory enabled, `boot -f dksc(0,5,8)/unixIP19` now reaches the
  same K2 `badvaddr`/page-table blocker as direct machine RAM.
- `tests/unit/test-sgi-mc3.c` covers the full 512 MiB physical read/write
  mapping, one-way identity, `BANKENB == 0`, bank-enable masking and
  `LEAFCTLENB`.

Goal: enough emulation to boot IRIX on IP19 with multiple CPUs (M:N host/guest flexible). Don't need full hardware accuracy - just what PROM/kernel expects.

## Current state
- Skeleton `hw/mips/sgi_ip19.c` exists (unstashed). Wired in Kconfig/meson. IRQ path partially there.
- Using R4000SC by default. Big endian. CC/config/RTC/EAROM/IO4/GIO mapped.
- EPC DUART console stub in IO4 small window. SPNUM/SYSCONFIG/interrupt scaffolding present.
- `tlb_helper.c` touches requester_id (already present in tree).
- IP19/Challenge PROM built: `sgi/challenge/ip19prom.bin` (QEMU raw),
  `ip19prom.hex` (swizzled EPROM image), plus `ip19prom.coff`/map/nm.
  `run-challenge.sh` loads the raw image from `sgi/challenge/`.

## What IRIX expects (from headers/kernel)

### Critical for PROM/early boot
| Component | Notes | Status | Action |
|---|---|---|---|
| CPU Controller (CC) on IP19 board | Local regs at 0x18000000. SPNUM, KZRESET, SYSCONFIG, SENDINT, IP/HPIL/CEL/ILE/CIPL/IGRMASK/ERTOIP. Interrupt routing. | Partial | Fix SPNUM per-cpu, SYSCONFIG bit layout, KZRESET behavior, ensure IRQs actually reach CPU (Cause/IP) correctly. Validate SENDINT broadcast/unicast. |
| Config bus (evconfig) | 16 slots * 2048 bytes at 0x18008000+. Boards report type/enable/level/revision. CPU board, MC3, IO4 must be discoverable. | Partial | Make defaults match what IP19 PROM expects. May need per-slot/proc config. Add sanity for missing boards? |
| Memory Controller MC3 | Bank config at config registers (MC3_BANKENB/TYPE/banks). Memory sizing/enable. | Missing | Need MC3 model (even minimal). PROM/kernel will probe banks. At least report enabled banks covering RAM. |
| PROM | IP19 PROM at 0x1fc00000, loaded from `sgi/challenge/ip19prom.bin`. Different from Indy IP24 PROM. | Built | Raw image and swizzled `.hex` are in `sgi/challenge/`; QEMU smoke test loads it but no console yet. |
| NVRAM/EAROM | Stores frequencies/checksums (EBUS/CPU/RTC), endian bit, cache settings. PROM reads EAROM for config. | Stub (reads 0) | Need minimal EAROM that passes checksum or PROM is lenient? Look up EAROM format from headers/FRU code. |
| RTC | EV_RTC counter. | Minimal counter | Maybe okay if stable. |

### Needed for kernel boot/console
| Component | Notes | Status | Action |
|---|---|---|---|
| IO4 | I/O controller base 0x10000000. Decodes adapters (EPC, SCSI). Routes interrupts. | Stub | Expand decode per IO4 layout. Need to model enough for EPC discovery. |
| EPC (Enet/Peripheral Controller) | Paired with IO4. Has DUART (Zilog 8530 SCC style or similar), possibly ethernet, interrupts. Console on EPC DUART. | Very minimal | Implement minimal EPC with SCC/DUART registers for serial console (we have path to chr). Need correct register offsets/behavior for IRIX EPC driver. |
| Interrupts (evintr) | Grouping by levels, CEL/ILE/IGRMASK, ERTOIP, mapping to Cause IP bits. SMP routing. | Partial | May need to match kernel expectations better (level mapping). Test against PROM first. |
| Clocks/timers | RTC frequency, compare registers. | Unknown | Ensure consistent. |

### Desirable for usability (later)
| Component | Notes | Status | Action |
|---|---|---|---|
| SCSI (WD33C93/IO4 SCSI) | Boot disk. Indy uses WD33C93; IO4 has different SCSI? Or use existing WD33C93 if attachable. | Missing | Can defer - try network boot or kernel boot args? Or add minimal. |
| Ethernet (EPC enet) | For network install/boot. | Missing | Defer. |
| Graphics (DINO/VIP/GIO) | Maybe not needed for text boot. | Missing | Defer. |
| VME, other boards | No. | Missing | Defer. |

## Immediate next steps (do one by one)
1. **PROM present**: built at `sgi/challenge/ip19prom.bin`; `run-challenge.sh` uses it. Continue with CC/EAROM/EPC bring-up.
2. **Fix CC essentials**: SPNUM to include real slot/proc from attrs/cpu index. SYSCONFIG to look plausible. Ensure KZRESET doesn't immediately kill emulation. Make IP/HPIL/CEL updates actually observable.
3. **MC3 minimal**: Add minimal MC3 model on slot 1 with bank covering machine->ram. Put enough in config registers.
4. **EAROM minimal**: Provide plausible EAROM values (EBUS/CPU/RTC frequencies, checksums) so PROM doesn't bail.
5. **EPC DUART**: Flesh out SCC registers to match what IRIX expects (RR0 TX empty, command/data channels). Test console output.
6. **Interrupt sanity**: Verify level mapping matches evintr expectations. Fix if PROM/kernel wedges on interrupts.
7. **SMP bringup**: Test with -smp 1,2,4. Ensure all CPUs initialized, config sees them.

## Files to touch
- `hw/mips/sgi_ip19.c` - main machine
- `hw/mips/Kconfig`, `hw/mips/meson.build` - done
- Maybe add `hw/mips/sgi_ip19_mc3.c`, `hw/mips/sgi_ip19_io4.c`, `hw/mips/sgi_ip19_epc.c` if getting complex
- `target/mips/tcg/system/tlb_helper.c` - already modified (requester_id)

## Milestone 1: WD95/S1 SCSI disk path (verified)

Implemented in `hw/mips/sgi_s1.c` (`TYPE_SGI_S1`, SysBusDevice), wired at
IO4 padap 4/window 1 = `0x100C0000`, size `0x8000`:

- WD95 setup/normal register banks at `+ch*0x800`, 32-bit accesses at
  `reg*8+4`; `S_CTL` bit 0 selects setup, `CVER` reads `0x22`, and
  `TIMOUT` is read/write scratch.
- S1 DMA registers at `+0x2000+ch*0x800`, `STATUS_CMD` at `+0x6000`,
  channel interrupts at `+0x7000`.
- DPR[0..31] and command dispatch from DPR[20] on `SQADR=0`; three
  independent SCSI buses, one per S1 channel.
- QEMU SCSI pass-through supports INQUIRY, TEST UNIT READY, MODE SENSE 6,
  READ CAPACITY 10, READ 10 and REQUEST SENSE. Successful commands set
  DPR[11]=status, `SQADR=0x4e`, `ISR=0x03`, residual TC and the S1
  `STATUS_CMD` channel interrupt bit. A missing target sets
  `STOPU_SCSIT`, `ISR=STOPU`, leaves `SQADR=0`, and returns without
  hanging.
- DMA uses the descriptor list programmed in `S1_DMA_TRANS_LOW/HIGH` and
  the offset from `S1_DMA_READ/WRITE`, resolved with
  `address_space_read/write` (matching `dma_map` in
  `stand/arcs/lib/libsk/ml/s1chip.c`).
- S1 exposes its three SCSI buses as `main-system-bus/sgi-s1/scsi.0` through
  `main-system-bus/sgi-s1/scsi.2`; the machine no longer accepts hardcoded drive or
  CD properties. Disks and CDs are attached with ordinary `-device
  scsi-hd`/`scsi-cd` options. Legacy `-drive if=scsi` also still works.

Exact command line used (headless, serial on a Unix socket):

```
qemu-img create -f raw /tmp/ip19disk.raw 64M
./build/qemu-system-mips64 -M ip19 -smp 1 -m 512M \
  -bios sgi/challenge/ip19prom.bin -L sgi/challenge \
  -display none -monitor none \
  -serial unix:/tmp/ip19.sock,server=on,wait=off \
  -drive if=none,id=ip19disk,file=/tmp/ip19disk.raw,format=raw \
  -device scsi-hd,drive=ip19disk,bus=main-system-bus/sgi-s1/scsi.0,scsi-id=1
```

At the menu, selecting option 5 and running `hinv -v` gives:

```
                   System: IP19
                Processor: 504 Mhz R4000, with FPU
     Primary I-cache size: 8 Kbytes
     Primary D-cache size: 8 Kbytes
     Secondary cache size: 4096 Kbytes
              Memory size: 256 Mbytes
                SCSI Disk: scsi(0)disk(1)
```

`ls` no longer reports `dksc(0,1,8): no such device`:

```
>> ls
dks0d1s8: volume header not valid
dks0d1s0: volume header not valid
```

`boot` now finds the device but the blank image has no file system:

```
>> boot
dks0d1s8: volume header not valid
Unable to load dksc(0,1,8)/sash: no recognizable file system on device.
```

With no `-drive`, the same menu/console still works and `ls` cleanly
reports `dksc(0,1,8)/: no such device`; there is no hang or S1 timeout.

Remaining blocker: valid boot media. The SCSI controller/disk detection,
INQUIRY, capacity and READ paths are working; the next step is a disk with
an IRIX volume header plus `sash`, or an install CD/ISO on the same bus.
