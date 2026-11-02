# IP19 / Challenge-Onyx Implementation Plan

Status: active working plan. This is the implementation roadmap; the short
current checklist remains in `IP19_TODO.md`.

## 1. Decisions

- **Full target:** SGI IP19 **Onyx** with **Venice** graphics, running IRIX
  6.5 and ultimately four CPUs. Challenge is the same IP19 base without
  Onyx graphics; the full Onyx/Venice path is the primary native target.
- **First boot milestone:** one CPU, 512 MiB RAM, serial/text console. Keep
  the machine model SMP-ready (`max_cpus=4`) but do not require four CPUs
  until the PROM/kernel text path is stable.
- **Final milestone:** four CPUs/SMP, 512 MiB, network/SCSI, native Venice
  graphics, X and useful OpenGL.
- **OS:** **IRIX 6.5.22m** is the target. Onyx/Challenge support was dropped
  after 6.5.22; **6.5.30m is not supported** on IP19 Onyx/Challenge. The
  6.5.30 media contains IP19/Onyx strings, but that is not platform support.
  Do not use 6.5.30m as the target for this machine.
- **First graphics milestone:** the simplest working path is a **GIO64 +
  Newport/VC2 compatibility card** using the existing `sgi-gio64` emulation,
  with a host-OpenGL renderer. This is a bring-up/display path, not native
  Onyx graphics.
- **Native graphics milestone:** **Venice/Onyx** using the `GE10.bin` GS PROM
  and a host-OpenGL rendering backend.
- **Host OpenGL:** there is no generic guest-to-host OpenGL passthrough for
  IRIX. QEMU virtio-gpu/virgl needs a guest virtio driver (IRIX has none), and
  VFIO/PCI passthrough needs a guest PCI GPU driver (IP19 has no suitable
  path). Host GL is viable only as the **renderer backend of an emulated SGI
  graphics device**, after decoding the SGI command stream.
- **Simplest native alternative:** Challenge **Express/GR2** is likely
  simpler than Venice, but we do not currently have an Express GFX PROM.
  Venice is the better full target because `GE10.bin` is present.
- **IRIX media:** use the named installation media under
  `/mnt/10-1-1-41-ssd/vms/isos/sgi/irixcds/`; ignore the unnamed/numbered
  `01.iso`–`08.iso`. The directory provides:
  `irix6.5Foundation1Jun98.iso`, `irix6.5Foundation2Jun98.iso`,
  `patches_6.5.22m_26_30.iso`, 6.5.30 overlays, applications and tools.
  These are the bring-up media; verify the exact 6.5.22m base/overlay before
  installation.

## 2. Current machine state

The current `hw/mips/sgi_ip19.c` is a boot skeleton, not a working IP19.

| Block | Current state | Gap |
|---|---|---|
| CPU | `R4000SC`, big-endian, `max_cpus=4`, no default-CPU count | Start 1; final 4; SMP bring-up |
| RAM | 128 MiB default; RAM region at 0 | 512 MiB default; real MC3 decode |
| CC | `0x18000000`: SPNUM, SYSCONFIG, IP/HPIL/CEL/ILE/IGRMASK, SENDINT, KZRESET, write-only UART | CC UART RX/system-controller handshake, timers, interrupts, errors |
| Config bus | `0x18008000`: CPU/MC3/IO4 defaults | 1/4 CPU config, graphics/GIO discovery, VDS/debug |
| MC3 | Identification and one bank-size register | 512 MiB banks, interleave, BIST, ECC/errors |
| PROM | 128 KiB at `0x1fc00000` | Reset-vector/endianness bug; full image handling |
| IO4 flash | 8 MiB at `0x448000000`, zeros | `io4prom.bin` and load/checksum path |
| IO4 | EPC DUART/NVRAM/RTC/control stubs | IO4 windows, adapters, SCSI, Ethernet, DANG |
| EPC | Very minimal DUART/NVRAM/RTC | Z8530 SCC, DS1386, EPC Ethernet, interrupts |
| GIO | Stub at `0x1f400000` (GIO1) | GIO64 bus at `0x1f000000`; GFX slot; DANG |
| Graphics ROM | `GE10.bin` available | Load at `0x01700000`; Venice device |
| Graphics | None | GIO64 graphics card, VRAM, RAMDAC, video, host GL |
| SCSI/network | None | IO4 SCSI and EPC Ethernet |
| SMP | Partial CC scaffolding | Four-CPU PROM arbitration and kernel SMP |

### Immediate boot status

The reset-vector/byte-order bug is fixed. IP19 boots the PROM through
CC-local/CC-config, EAROM, bus tags, one-bank MC3 memory BIST, secondary cache,
and MPCONF initialization, then runs the interactive IO4 PROM Monitor. The
S1/WD95 SCSI controller is implemented: the PROM sees SCSI disks and CD-ROMs,
reads the IRIX volume header, and loads `sash` from a real IRIX CHD. The
install-tools CD starts the IRIX installer. The current stop is KSEG2/K2 TLB
behavior during the installer (`excessive R4000 badvaddr(1) ... K2 addresses`);
6.5.30 `/unix` panics with a UTLB miss, as expected for an unsupported IP19
kernel. See `IP19_PROM_PATCHES.md` and `IP19_TODO.md`.

The locally built PROM required two source/binary corrections that must be
kept in sync: lwin address arithmetic must use `daddu` (not 32-bit `add`), and
`POD_SCACHEADDR` must be moved out of physical page 0 because QEMU has no cache
model. See `IP19_TODO.md` for the exact state and next actions.

## 3. Hardware and address map

Relevant IP19/GIO map from the IRIX headers:

| Device | Address |
|---|---|
| CPU PROM | `0x1fc00000`, 128 KiB |
| GFX PROM window | `0x01700000`, 1 MiB |
| CC | `0x18000000` |
| Config bus | `0x18008000` |
| RTC | `0x18020000` |
| EAROM | `0x19000000` |
| LEDs | `0x19004000` |
| C sync | `0x1a000100` |
| IO4 | `0x10000000` |
| IO4 flash | `0x448000000` |
| GIO GFX slot | `0x1f000000` |
| GIO1 | `0x1f400000` |
| GIO2 | `0x1f600000` |
| GIO config | `0x1fa20000`/`0x1fa20004` |

The current IP19 `gio` stub is at `0x1f400000`, which is GIO1—not the GFX
slot. The existing `sgi-gio64` device is an Indy GIO64 + Newport/VC2
implementation; the bus portion can be reused, but its card is not a
Challenge/Onyx graphics device.

## 4. Firmware inventory

| Item | State / action |
|---|---|
| CPU PROM | `sgi/challenge/ip19prom.bin` (unswizzled for QEMU) and `ip19prom.hex` (swizzled for EPROM) |
| IO4 PROM | Build `io4prom.bin` from `stand/arcs/IO4prom`; load at the IO4 flash window |
| GFX PROM | `stand/arcs/IO4prom/GE10.bin` is a 277,920-byte ELF64 MIPS image with entry `0xa800000001700000`; treat it as the Venice/Onyx GFX PROM at `0x01700000` |
| NVRAM/EAROM | Need valid frequencies/checksums and IO4 NVRAM contents |
| SCSI CD/disk | Needed for the 6.5.22m install and boot |
| IRIX 6.5.22m media | Not present in the named media set; acquire a supported 6.5.22m base/overlay |

`GE10.bin` is not an IO4 PROM. Its symbols include `get_ven_type`,
`enoughRMs`, `fcg_*`, and `ge_tport`, and its census references
`gfx/VENICE`; it is the graphics firmware, not the IO4 PROM.

## 5. Implementation phases

### Phase 0 — Boot harness and reset fix

- Verify CPU big-endian configuration (`CP0_Config.BE`, `Status.BEV`) and the
  exact bytes at `0x1fc00000`.
- Reconcile the QEMU `.bin` (unswizzled) with the physical `.hex` (swizzled);
  do not silently truncate the last ROM bytes.
- Start with `-smp 1 -m 512M`; keep `max_cpus=4`.
- Add a `run-challenge.sh` harness with GTK display, serial file, monitor socket,
  optional GDB, and debug switches.
- **Acceptance:** first instruction is `j 0xbfc003c0`; no address-error loop;
  one CPU and 512 MiB configured.

### Phase 1 — PROM text console

- Complete CC: SPNUM, SYSCONFIG, KZRESET, SENDINT, HPIL, CEL/ILE/IGRMASK,
  CIPL, timer comparator, errors, and interrupt routing.
- Add CC UART RX and the minimum system-controller `O`/`K` handshake.
- Implement the config bus for one and four CPUs, plus MC3/IO4 discovery.
- Implement MC3 512 MiB bank mapping, memory sizing, BIST, and basic errors.
- Fill in EAROM frequencies/checksums and RTC comparator behavior.
- **Acceptance:** PROM console output; memory diagnostics pass; single-CPU
  boot stable.

### Phase 2 — IO4, EPC, IO4 PROM

- Implement IO4 small/large windows, map RAM, adapter decode, flash, and
  interrupt routing.
- Build `io4prom.bin` and pass `load_io4prom()` magic/length/entry/checksum
  checks.
- Replace the DUART stub with a Z8530/SCC model: RR/WR registers, channel
  A/B, TX/RX, RR0 status, interrupts; wire DUART0 to host serial.
- Implement EPC NVRAM/RTC and external interrupt behavior.
- Add DANG/SCSI adapter discovery.
- **Acceptance:** PROM enters the IO4 PROM; serial console/ARCS menu works.

### Phase 3 — Storage, network, IRIX 6.5.22m install

- Identify the real IO4 SCSI chip (WD33C93 vs WD95/other) and implement or
  reuse the matching model.
- Add SCSI CD/disk and get a text-mode IRIX 6.5.22m install.
- Add EPC Ethernet/EDLC if network installation is needed.
- Install with one CPU and 512 MiB; add four CPUs after text boot is stable.
- **Acceptance:** IRIX 6.5.22m boots to multiuser text on IP19; disk and serial
  work; `hinv` reports one CPU/512 MiB.

### Phase 4 — GIO64 and first working graphics

- Replace the `0x1f400000` stub with the full GIO64 map: GFX `0x1f000000`,
  GIO1 `0x1f400000`, GIO2 `0x1f600000`; add DANG at IO4 adapter `0x2B` and
  GIO interrupt routing.
- Refactor/reuse `sgi-gio64` as a GIO64 bus plus card.
- Load `GE10.bin` as `gfxprom.bin` at `0x01700000`.
- Attach a Newport/VC2 card as the **first compatibility graphics** and add
  a host-GL renderer for its REX3 path.
- **Acceptance:** a QEMU display window and guest-visible GIO64 graphics card;
  this is a compatibility bring-up, not native Onyx graphics.

### Phase 5 — Native Onyx/Venice graphics

- Implement Venice/Grinch: GFX PROM execution, registers, command FIFO,
  GE/RM/TM, VRAM, RAMDAC, video timing, and DMA.
- Use `GE10.bin` debug symbols/census and the IRIX Venice driver behavior as
  references.
- Decode the guest graphics command stream and use host OpenGL as the
  rasterization backend.
- **Acceptance:** native graphics initialization; PROM/kernel console; X;
  simple OpenGL applications render.

### Phase 6 — SMP and full-system stabilization

- Scale from one CPU to four CPUs/SMP; validate CC arbitration and per-CPU
  configuration.
- Stress boot/disk/network/graphics; reset/shutdown; save/restore.
- Final default: four CPUs, 512 MiB, 6.5.22m, working graphics.
- **Acceptance:** stable IRIX 6.5.22m Onyx with four CPUs, 512 MiB, SCSI/
  network, X and simple OpenGL, without crashes in the tested workload.

## 6. Graphics architecture

### Native vs host GL

The two options are not mutually exclusive:

1. Emulate the simplest native graphics command interface.
2. Translate the resulting drawing commands to host OpenGL as a backend.

There is no transparent host OpenGL passthrough. `virgl`/virtio-gpu and VFIO
are not usable without IRIX guest drivers. The host-GL backend only helps once
the emulated SGI graphics command stream is decoded.

### First compatibility path: Newport/VC2

Newport/VC2 is the simplest card because QEMU already has a detailed software
model. Use it to validate GIO64, DANG, IRQs, scanout, and the GL backend. It is
not a Challenge/Onyx native card and may not be recognized by the IP19 PROM or
Onyx kernel.

### Full native path: Venice

`GE10.bin` is the Venice/Onyx GFX PROM. Implement the Venice command path and
use host GL for rasterization. This is the primary full-graphics route.

### Optional path: Express/GR2

Express/GR2 is likely simpler than Venice, but we need an Express GFX PROM and
reference material. It is a fallback for Challenge if such a ROM becomes
available.

## 7. Testing

- Reset: `-d int,in_asm,guest_errors,unimp`; verify no exception at reset.
- PROM: serial output; memory sizing; CC/system-controller handshake.
- IO4: IO4 PROM magic/checksum; EPC console; ARCS.
- IRIX: 6.5.22m text install; `hinv`; disk; network; then `-smp 4`.
- Graphics: QEMU display; GIO64 card detection; PROM/kernel graphics; X;
  simple OpenGL; crash/stress loop.
- Stability: boot loops, disk I/O, SMP, graphics, network, reset/shutdown.

## 8. Risks

- **Venice documentation:** no MAME SGI Onyx model; GE10 debug firmware helps,
  but the GE/RM/TM command interface still needs reverse engineering.
- **Express availability:** simpler, but the GFX PROM/reference is missing.
- **IRIX 6.5.22m media:** the Jun98 Foundation pair plus
  `patches_6.5.22m_26_30.iso` are present; exact base/overlay validation is
  still needed.
- **IO4 SCSI:** chip identity must be confirmed before reusing Indy SCSI code.
- **SMP:** four-CPU PROM arbitration and interrupt routing are high-risk.
- **Host GL:** accelerates rendering but does not remove graphics-device
  emulation; host GL/Venice performance may be modest.
- **MC3:** memory decode/interleave/ECC must match PROM/kernel expectations.

## 9. Open items

1. Acquire IRIX 6.5.22m base/overlay media (6.5.30m is unsupported on IP19
   Onyx/Challenge).
2. Confirm `GE10.bin` as the Venice/Onyx GFX PROM and use it as `gfxprom.bin`.
3. Confirm whether the first Newport/VC2 compatibility path is acceptable
   before investing in Venice.
4. Identify the IO4 SCSI chip and matching QEMU model.
5. Decide R4000SC vs R4400SC for the final Onyx CPU model; start R4000SC for
   PROM bring-up, test R4400SC for IRIX.
6. Define the “working applications” acceptance set for X/OpenGL stability.

## 10. Immediate next actions

1. Fix the reset-vector/endianness bug and boot one CPU with 512 MiB.
2. Get the PROM console and memory diagnostics working.
3. Build/load `io4prom.bin` and get the IO4/EPC serial console.
4. Acquire 6.5.22m media and install a text-mode system.
5. Then add GIO64 + Newport/VC2 compatibility graphics and host GL.
6. After the working machine exists, implement native Venice graphics and SMP.
