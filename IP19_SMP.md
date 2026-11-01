# IP19 Challenge/Onyx SMP topology and bring-up

Status: two CPUs on one IP19 board boot 6.5.22 to multiuser/login with both
`-m 512M` and `-m 2G`. Bootmaster election is deterministic, and the kernel
secondary-CPU handoff works under single-threaded TCG. The old failure,
“Processor #1 did not start!”, was a boot-time scheduling race rather than a
CPU1 fault; see the diagnostic notes and blocker section below.

Sources used:

- `irix/kern/sys/EVEREST/{IP19.h,everest.h,evconfig.h,gda.h,evintr.h,IP19addrs.h,evmp.h,mc3.h}`
- `irix/kern/ml/EVEREST/{EVERESTasm.s,init.c,evmp.c,evslave.s,evintr.c}`
- `stand/arcs/IP19prom/{config.c,bmarb.s,slave.s,pod_master.c,pod_cc.s,main.c}`
- `stand/arcs/IO4prom/sysinit.c`, `stand/arcs/lib/libsk/ml/evslave.c`
- SGI Hardware Developer Handbook tables 4-32, 4-80, 4-82, 4-83, 5-2, 5-3
- SGI *POWER CHALLENGE and CHALLENGE XL Rackmount Owner's Guide*
  (IP19 = 2 or 4 R4400; 9 IP19s = 36 CPUs)
- SGI *Challenge Deskside Server Owner's Guide* (up to three IP19s, 12 CPUs)
- current `hw/mips/sgi_ip19.c`, `hw/mips/sgi_mc3.c`, `sgi/challenge/ip19prom.bin`

## 1. CPU boards and slots

An IP19 is a 4-slice CPU board. Each slice is an independent R4400 with its
own CC/A-chip support logic. `EV_CPU_PER_BOARD`/`EV_MAX_CPUS_BOARD` are 4 for
that reason, and the IP19 PROM loops over four slices when it discovers CPUs.
The board may be populated with 1, 2, or 4 CPUs. The common 2-CPU board is
what the handbook shorthand “IP19 = 2xR4400” describes; it is not the
architectural maximum. A 4-CPU IP19 uses slices 0-3 on the same board.

Physical SPNUM is six bits: `(slot << 2) | slice`. Absent slices are marked
`CPUST_NORESP`/`EVDIAG_NOTFOUND`; they are not counted. Bootmaster arbitration
uses the SPNUM value directly, so the lowest populated slot and lowest
populated slice wins.

CPU capacity by chassis:

| Chassis | CPU-capable slots | CPU boards in maximum config | CPUs |
|---|---|---|---|
| Onyx deskside (“Eveready”) | slot 2 only | 1 | 4 |
| Challenge L deskside (“Eveready”) | slots 2, 3, 4 | 3 | 12 |
| Onyx rack (“Terminator”) | slots 1, 3, 5, 7, 9, 11, 13 (and 2, 4, 6, 8, 10, 12, 14 with IMB/PC2 restrictions) | 6 | 24 |
| Challenge XL rack (“Terminator”) | slots 1-14, odd slots with PC2 | 9 | 36 |

Deskside slot tables:

- Challenge L server: slot 1 = MC3; slots 2-4 = CPU or MC3 or IO4; slot 5 =
  main IO4; slots 7-11 = VME64 or graphics.
- Onyx deskside: slot 1 = MC3; slot 2 = CPU; slot 3 = IO4; slot 4 =
  VCAM/GCAM; slots 5-7 = VME64.

Rack slot table (Challenge XL): slots 1-10 and 11-14 are CPU/IMB/PC2, with
odd slots also accepting PC2; IO4 and graphics occupy later slots.

CPU configurations requested here:

| CPUs | Board count and placement |
|---|---|
| 1 | One IP19 board, slice 0. Deskside: slot 2. Rack: lowest populated CPU slot (normally 1). |
| 2 | One 2-CPU IP19 board, slices 0-1. Deskside: slot 2. Rack: slot 1, or any CPU slot. |
| 4 | One 4-CPU IP19 board, slices 0-3. Deskside: slot 2. Rack: slot 1. Equivalent spread: two 2-CPU boards, for example rack slots 1 and 3 or deskside slots 2 and 3. |

The current QEMU machine uses a synthetic rack-style layout: CPU board in
slot 1, MC3 in slot 2, IO4 in slot 3. That is accepted by the PROM, but it is
not the Onyx/Challenge L deskside layout. A deskside-accurate model should
use MC3 slot 1, CPU slot 2, IO4 slot 3 (main IO4 slot 5 for some Challenge L
configurations).

## 2. Four CPUs per board versus “2xR4400”

Both statements are true at different levels:

- Hardware/software maximum: 4 CPUs per IP19 board, four slices, four sets of
  CC/A logic. `EV_CPU_PER_BOARD = 4`, `EV_MAX_CPUS_BOARD = 4`, the PROM
  assumes four slices per board, and the kernel probes all four.
- Common product configuration and board photo: “IP19 2x R4400”. A standard
  board carries two R4400s; 4-CPU boards and 1-CPU variants exist. The
  handbook processor table itself lists 1, 2, or 4 microprocessors per IP19.

So QEMU must not hard-code two CPUs per board. It should model four slices
per board and expose only the populated ones.

## 3. MC3 memory limits and maximum RAM

MC3 organization:

- 2 leaves x 4 banks per leaf = 8 banks.
- 4 SIMMs per bank = 32 SIMM slots per board.
- 16 MB or 64 MB ECC SIMMs in groups of four.
- Maximum per MC3 board: 32 x 64 MB = 2 GB.
- `EV_MAX_MC3S = 8`; the theoretical rack maximum is 8 MC3 boards = 16 GB.

Practical maximums:

| Chassis with 4 CPUs | CPU boards | Maximum MC3 boards | Maximum RAM |
|---|---|---|---|
| Onyx deskside | 1 (slot 2) | 1 (slot 1) | 2 GB |
| Challenge L deskside | 1 (4-CPU board) | documented system limit 1 memory board | 2 GB |
| Onyx rack | 1 | 6 CPU boards max system; 4 CPUs use 1, leaving memory slots | 16 GB theoretical (8 MC3) |
| Challenge XL rack | 1 | 8 | 16 GB theoretical (8 MC3) |

Adding more CPU boards reduces the number of free Ebus slots, so a
24/36-CPU rack cannot also carry 8 MC3 boards; with exactly 4 CPUs there is
plenty of slot room.

QEMU current limit: the machine creates a single `sgi-mc3` with the `-m` RAM
region. `mc3_split_ram()` can split up to 8 banks, each up to 1 GB, so the
model accepts at most 8 GB and `-m 16G` fails. The host has 60 GiB total
(about 27 GiB currently available). Recommended bring-up value is **2 GB**:
it matches the deskside MC3 limit and avoids bank-layout/stress noise.
Use 512 MB for normal regression and 2 GB for SMP/RAM tests; 4 GB is only a
model stress case, not a hardware-accurate MC3 population.

## 4. Bootstrap requirements

### Reset, SPNUM, CPU IDs

1. Every populated slice starts at the IP19 PROM reset vector.
2. Each CPU reads `EV_SPNUM` = `(slot << 2) | slice` and computes
   `TPID = SPNUM`.
3. Arbitration waits `TPID * (RTC_SPEED / 64)`; the lowest SPNUM wins. The
   winner sets `IGRMASK = 1` (master group 0) and broadcasts interrupt level
   6 to `0x7f`; losers set `IGRMASK = 2` (slave group 1).
4. Each slave sends an “aliveness” interrupt at level `2 * TPID` to group 0.
   The master reads `IP0/IP1`; presence bit for slot `s`, slice `c` is
   `1 << (((s % 4) * 8) + c * 2)` in the corresponding 64-bit IP word.
5. `configure_cpus()` assigns virtual CPU IDs (`cpu_vpid`) in slot/slice
   order; absent slices get `CPUST_NORESP`. The kernel later builds
   `cpuid_to_slot[]`, `cpuid_to_cpu[]`, and `logical_cpuid[slot][slice]`.

### Board/config discovery

`EV_SYSCONFIG` layout:

- bits 0-15: occupied slots
- bits 16-31: CPU-class boards
- bits 32-47: memory-class boards

The CPU presence bit is per CPU **board**, not per CPU. Slice presence comes
from the aliveness bits and `cpu_diagval`. The IP19 config bus is at
`0x18008000`, slot stride `0x800`, register stride 8 bytes, per-processor
stride `0x40` registers. CPU board registers: A_ENABLE (slice vector),
BOARD_TYPE=1 (IP19), LEVEL, timeouts, error; per-slice CMPREG0-3, PGBRDEN,
ECCHKDIS, CPERIOD, DATARATE, IW/RR_TRIG, CACHE_SZ, CCREV.

IRIX reads `EV_BOARDENB`/A_ENABLE and `EVCFGINFO` `eb_cpuarr[]` entries. A
2-CPU board has slices 0-1 enabled; a 4-CPU board has 0-3.

### MPCONF and launch

MPCONF is at physical `0x3000`, 64 bytes per entry, indexed by physical ID
during PROM boot and by virtual ID after the PROM assigns vpids. Fields
include magic `0xBADDEED2`, physical ID, virtual ID, launch and rendezvous
addresses, stack/real_sp, secondary-cache size, and PRID.

- `MPCONF_INIT_LEVEL = 7`: master sends a group-1 SENDINT; each slave
  initializes its MPCONF entry and updates its `cpu_diagval`.
- `MPCONF_LAUNCH_LEVEL = 8`: launched CPU-directed at `MPCONF[cpu].phys_id`.
- IO4 PROM `start_slave_loop()` also writes the MPCONF launch entry and sends
  level 8 to each slave.
- The kernel writes the kernel `slave_entry` address into each MPCONF entry
  and sends level 8 again. `slave_entry` calls `ip19_cache_init()` and
  `slave_loop()`, which sets `slave_loop_ready[vpid]`. The kernel then gives
  each CPU its PDA stack and bootstrap function.

### Interrupt state and cache sync

The CC has per-CPU `IP0/IP1` pending bits (levels 0-127), `HPIL`, `CEL`,
`ILE`, `IGRMASK`, `CIPL0`/`CIPL124`, and `ERTOIP`. Device interrupts use
`Cause IP2`; the CC comparator uses `IP3`; errors use `IP5`; the CPU timer
uses `IP7` and must not be driven by the CC. CPU-directed destination is
`(slot << 2) | slice`; group-directed is `0x40 | group`; broadcast is `0x7f`.

`ip19_cache_init()` (PROM and kernel) flushes/invalidates the secondary
cache, writes primary I/D tags, and writes secondary tags. A new secondary
CPU must run it before entering the kernel slave loop. QEMU has no cache
model, so the local PROM patches skip the destructive cache sequences; the
kernel version is still executed by the secondary CPU.

### NVRAM/inventory, restart, and handoff

The PROM reads NVRAM inventory (per-board/per-unit enable/type), EAROM
frequencies/checksums/cache size, and the virtual DIP switches. Invalid
inventory is reinitialized. CPU EAROM checksums are compared per CPU.
`KZRESET` resets the system; PROM vectors `RESTART`, `RESLAVE`, and POD mode
are at `0x1fc00008`, `0x1fc00010`, `0x1fc00018`; `EV_PROM_FLASHLEDS` is the
“go away forever” trap. `GDA.g_masterspnum` records the bootmaster SPNUM.
The NVRAM restart path must preserve the same CPU enable state and SPNUM
mapping.

## 5. QEMU implementation sequence

### Step 1 — 2 CPUs on one board (implemented)

- Create `machine->smp.cpus` CPUs, each reporting slot 1 and its own slice.
- Use slice 0 as the bootmaster and slice 1 as the second CPU when CPU 0 wins
  the existing PROM arbitration.
- Latch group-directed SENDINT edges until each CPU joins the group. The
  PROM sends MPCONF_INIT before a newly started slave has written IGRMASK;
  an edge latched per group and delivered on group join removes that race.
- Local PROM workarounds: skip the IP19 PROM slave cache-init block on the
  no-diags path, and make `check_slaves()` wait on `EVDIAG_NOT_SET` instead of
  immediately declaring “CPU can't access memory”.
- Hold secondary slices until CPU 0 claims the master group, making the PROM
  election deterministic.
- Add the one-shot SMP RTC/`us_delay()` yield for single-threaded TCG.
- Result: `-smp 2` reports “Enabled 2 processors”, the IO4 PROM runs, the
  System Maintenance Menu appears, and IRIX reaches multiuser/login with both
  512 MiB and 2 GiB. CPU1 sets `slave_loop_ready[1]` and
  `pdaindr[1].CpuId = 1`; no secondary panic occurs.

### Step 2 — 2 boards / 4 CPUs

- Replace the implicit “CPU index == slice, slot 1” mapping with an explicit
  table: `cpu_index -> (slot, slice)`. Use slot 1/slices 0-3 for a 4-CPU board
  or slots 1 and 3 with slices 0-1 each for two 2-CPU boards.
- Make `EV_SPNUM` return the mapped slot/slice.
- Make `EV_SYSCONFIG` set an occupied bit and a CPU-class bit for every CPU
  board slot, not only slot 1.
- Map CPU-directed SENDINT `(slot << 2) | slice` back to `cpu_index`; keep
  group broadcast and group membership per CPU.
- Give each CPU board its own config-bus defaults and A_ENABLE mask
  (`0x03` for two slices, `0x0f` for four).
- Keep bootmaster selection at the lowest populated slot/slice and verify
  that the PROM reports both boards.
- Only after the PROM path is stable should the kernel SMP path be debugged.

### Step 3 — RAM expansion

- Instantiate up to eight MC3 devices, one per memory slot, instead of one
  device backed by the whole host RAM region.
- Set the memory-class and occupied bits in `SYSCONFIG` for each MC3 slot.
- Decode the MC3 banks across boards into one physical address space and
  preserve the PROM's interleave/base registers per board.
- Enforce the real 2 GB per-board SIMM limit in the model; use 2 GB for the
  deskside default and up to 8 boards/16 GB only on the rack path.
- Test 2 GB first, then 4 GB and 8 GB, before claiming 16 GB.

## 6. Resolved blocker: false “Processor #1 did not start!”

The old failure was not a CPU1 exception and not a CC/serial lockup. CPU1
reaches the kernel, sets `slave_loop_ready[1]`, enters `cboot()`, and sets
`pdaindr[1].CpuId = 1`. When the failure occurred, however, CPU0 had already
expired the kernel's `BOOTTIMEOUT` window in `allowboot()` and checked a
stale `pdaindr[1].CpuId`. It then printed “did not start”, set `CpuId = -1`,
and entered the `CE_PANIC` path. CPU1 later overwrote `CpuId` with 1, so a
post-failure memory dump can show `pdaindr[1].CpuId == 1` even though CPU0
made the wrong decision.

Captured failure state with symbol-matched installed kernel
(`ip19extract/unix`, base `0xa800000000000000`):

- CPU1 PC was in `splx()` (`a800...22810`) called from
  `mutex_spinlock_spl()` (`a800...1b0634`), with `s0 = putbuflck`.
- CPU1's return stack was
  `intr -> counter_intr -> clock -> tick_actions ->
  klog_unlocked_wakeup -> putbuf_lock -> mutex_spinlock_spl -> splx`.
  It was a normal timer-interrupt/klog wakeup path trying to take the
  console lock, not an exception or panic.
- CPU0 was in `icmn_err()` (`a800...0f46e0`) holding `putbuflck`; its
  `panic_level` had reached the nested-panic spin. This is why CPU1 appeared
  blocked on `putbuflck`.
- After the failed run, `putbuflck` was held, `panic_level` was non-zero,
  and `pdaindr[1].CpuId` was later 1 even though CPU0 had disabled CPU1.

Root cause: MIPS64 is still single-threaded TCG (`mttcg_supported` is false
for 64-bit MIPS), so CPU0 and CPU1 share one host thread. The kernel's boot
timeout is guest real time, but in round-robin emulation the AP only runs
between vCPU exits. CPU1's `cboot()` was therefore too slow to reach
`pdaindr[1].CpuId` before CPU0's timeout expired, even though it was making
normal progress.

Fix in `hw/mips/sgi_ip19.c`: on an IP19 CC RTC read from a secondary CPU in
the kernel, arm a one-shot boot window. While that window is active, a CPU0
RTC read inside the kernel's `us_delay()` forces `cpu_exit()`, so
single-threaded round-robin switches to the AP. The window is armed once per
reset and lasts 30 seconds; normal post-boot RTC traffic does not keep
re-arming it. This is enabled by default for `-smp > 1`; set
`IP19_RTC_YIELD=0` to disable it for bisection.

Remaining limitations:

1. **Fixed topology:** `IP19_SLOT_CPU == 1`, `SPNUM` uses the CPU index as
   slice, `deliver_vector` only accepts slot 1, and `SYSCONFIG`/config
   defaults describe one CPU board and one MC3.
2. **PROM/kernel cache init:** QEMU has no cache model. The IP19 PROM slave
   cache-init sequence can hang or corrupt low memory; the local PROM patch
   skips it. The IO4/kernel `ip19_cache_init` calls still need a safe model
   or workaround.
3. **MC3 is one board:** RAM size is the host `-m` region split across banks;
   there is no multi-board MC3 slot/decoder model yet.

Next concrete blocker: add the explicit `cpu_index -> (slot, slice)` map and
multi-board `SYSCONFIG`/config defaults for `-smp 4` (one 4-slice board or two
2-slice boards). Keep the current single-board 1/2-CPU paths working.

## 7. Implemented changes and tests

Machine changes in `hw/mips/sgi_ip19.c`:

- Added per-group pending/delivered state for group-directed SENDINT. A
  group interrupt is delivered to current members and latched for CPUs that
  join the group later. Writing `IGRMASK = 0` clears the latched state.
- Added a reset hold for secondary slices until CPU 0 claims the master
  group, making the PROM bootmaster election deterministic.
- Added the one-shot SMP RTC/`us_delay()` boot yield described above. It is
  enabled by default for `-smp > 1` and reset per machine reset.
- No changes to Indy, IP22, or the R4000SC CPU model.

Local PROM patches in `sgi/challenge/ip19prom.bin`:

- `0x16a8`: branch to the slave “passed” path instead of cache init when
  `BS_NO_DIAGS` is set.
- `0x16c4`: same for `VDS_NO_DIAGS`.
- `0xe640`: in `check_slaves`, convert `EVDIAG_NOT_SET` to a waiting value
  (243, testing dcache) instead of 120 (can't see memory), so the master
  waits for a late slave instead of immediately disabling it.

Test commands and results (patched PROM, installed `ip19-installed.chd`,
`-snapshot`, headless serial):

```
qemu-system-mips64 -M ip19 -smp 1 -m 512M -bios sgi/challenge/ip19prom.bin \
  -L sgi/challenge -snapshot -drive file=.../ip19-installed.chd,format=chd,if=none,id=ip19disk
```

Result: PROM, IO4 menu, IRIX banner, SCSI, `IRIS console login:` — no
regression.

```
qemu-system-mips64 -M ip19 -smp 2 -m 512M -bios sgi/challenge/ip19prom.bin \
  -L sgi/challenge -snapshot -drive file=.../ip19-installed.chd,format=chd,if=none,id=ip19disk
```

Result: “CPU 01/00 is bootmaster”, “Enabled 2 processors”, IO4 “Starting
processor #1”, System Maintenance Menu, IRIX banner, SCSI probes, multiuser
startup, and `IRIS console login:` — no “did not start” message and no panic.

The same `-smp 2` test with `-m 2G` also reaches login. `hinv` after login
reports two 48/50 MHz IP19 processors, `MIPS R4000 ... Revision: 3.0` for both,
`2048 Mbytes`, 2-way interleaved, and the same 4 MiB secondary cache.

Runtime state after the successful 2G boot (symbol-based guest reads):

- `slave_loop_ready[0] = 0`, `slave_loop_ready[1] = 1`.
- `pdaindr[0].CpuId = 0`, `pdaindr[1].CpuId = 1`.
- `putbuflck` clear, `panic_level = 0`.
- CPU1 in the kernel idle loop with interrupts enabled and no pending
  `Cause` bits; CPU0 also in kernel space.

Per-CPU verification:

- **PRID:** both CPUs report R4000 revision 3.0 via `hinv`; the machine sets
  `env.CP0_PRid = IP19_CPU_PRID` for every CPU at init and again in reset.
- **CC SPNUM:** CPU0 reads `(slot 1 << 2) | slice 0 = 0x04`, CPU1 reads
  `(slot 1 << 2) | slice 1 = 0x05`. The successful PROM arbitration,
  “Enabled 2 processors”, and CPU1's use of the slave path exercise both.
- **Context/XContext:** each CPU has `ip19_pte_base_shared`; the
  `mtc0 Context`/`mtc0 XContext` helpers mirror PTEBase per CPU, and the
  refill path derives XContext from Context per CPU.
- **KPTE workaround:** each CPU has `ip19_kpte_workaround`; the on-demand
  synthesis uses that CPU's TLB and the shared kptbl pointer. CPU1 runs
  kernel code and takes timer interrupts after login; no KPTE fault occurs.
- **Timer/interrupt state:** CPU1 takes its own Count/Compare timer
  interrupts (observed in `counter_intr -> clock -> tick_actions`), has
  interrupts enabled, and has no stuck CC pending state after boot.

## 8. Recommended next steps

1. Add explicit `cpu_index -> (slot, slice)` mapping and multi-board
   `SYSCONFIG`, then test `-smp 4` on either one 4-CPU board or two 2-CPU
   boards.
2. Add a multi-MC3 model only after `-smp 4` is stable; keep 2 GB as the
   hardware-accurate maximum for the single-board/deskside target.
3. Consider replacing the fixed `us_delay()` PC window with a symbol-free
   check (for example, yield while a secondary is in kernel memory) if a
   different IRIX kernel is used.
