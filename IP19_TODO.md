# IP19/Challenge (Everest) implementation plan

> Full implementation plan: [`IP19_PLAN.md`](IP19_PLAN.md).
> IRIX target is 6.5.22m; 6.5.30m is not supported on IP19 Onyx/Challenge.
> Bring-up starts with one CPU; the final target is four CPUs/SMP.

## Current bring-up checkpoint

Verified in QEMU with the rebuilt PROM and `-smp 1 -m 512M`:

- Reset vector, cache flush, CC-local, CC-config, EAROM writeback, bus tags,
  and secondary-cache tests pass.
- MCC/MC3 memory configuration and the built-in memory test pass with one
  256 MiB bank. Two-bank 512 MiB interleave is not yet modelled.
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
  all three WD95 channels, and a `-drive if=none,id=ip19disk` disk appears
  as `scsi(0)disk(1)`.
- **Milestone 2 complete:** with the real IRIX CHD attached, `ls` reads the
  volume header and `boot` loads `sash` (`Standalone Shell SGI Version 6.5
  ARCS`). The 6.5.30 `/unix` kernel panics with a UTLB miss, consistent
  with IP19 support ending at 6.5.22m.
- S1 now exposes two SCSI CD-ROM targets (5 and 6) with 512-byte logical
  blocks. The install-tools CD boots the IRIX installer; Foundation1 is
  available as the second CD.
- A disposable writable CHD copy is the install target. The installer
  proceeds, but currently floods `WARNING: excessive R4000 badvaddr(1)
  occurrances for K2 addresses - may be affecting performance!`.
- **Current blocker:** KSEG2/K2 TLB behavior during the 6.5.22m installer,
  not storage or PROM SCSI. Next: determine whether this is a missing TLB
  mapping/SEGCTL emulation or an R4000SC 32/64-bit address mode issue, then
  finish the installer and boot 6.5.22m.

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

1. Provide boot media: write a valid IRIX volume header and `sash` to the
   disk image, or attach an IRIX install CD to SCSI target 1.
2. Add SCSI write/CDB coverage needed by the install/boot path (the current
   milestone emphasises INQUIRY/TEST UNIT READY/MODE SENSE/READ CAPACITY/
   READ/REQUEST SENSE).
3. Model MC3 two-bank 512 MiB interleave and restore 512 MiB reporting.
4. Then kernel storage/media, EPC Ethernet, and graphics.

Goal: enough emulation to boot IRIX on IP19 with multiple CPUs (M:N host/guest flexible). Don't need full hardware accuracy - just what PROM/kernel expects.

## Current state
- Skeleton `hw/mips/sgi_ip19.c` exists (unstashed). Wired in Kconfig/meson. IRQ path partially there.
- Using R4000SC by default. Big endian. CC/config/RTC/EAROM/IO4/GIO mapped.
- EPC DUART console stub in IO4 small window. SPNUM/SYSCONFIG/interrupt scaffolding present.
- `tlb_helper.c` touches requester_id (already present in tree).
- IP19/Challenge PROM built: `sgi/challenge/ip19prom.bin` (QEMU raw),
  `ip19prom.hex` (swizzled EPROM image), plus `ip19prom.coff`/map/nm.
  `run-ip19.sh` loads the raw image from `sgi/challenge/`.

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
1. **PROM present**: built at `sgi/challenge/ip19prom.bin`; `run-ip19.sh` uses it. Continue with CC/EAROM/EPC bring-up.
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
- The machine passes `blk_by_name("ip19disk")` to the S1 `drive` property;
  `sgi_s1.c` attaches it as an async narrow `scsi-hd` at target 1, bus 0.
  Legacy `-drive if=scsi` still works when no `ip19disk` is named.

Exact command line used (headless, serial on a Unix socket):

```
qemu-img create -f raw /tmp/ip19disk.raw 64M
./build/qemu-system-mips64 -M ip19 -smp 1 -m 512M \
  -bios sgi/challenge/ip19prom.bin -L sgi/challenge \
  -display none -monitor none \
  -serial unix:/tmp/ip19.sock,server=on,wait=off \
  -drive if=none,id=ip19disk,file=/tmp/ip19disk.raw,format=raw
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
