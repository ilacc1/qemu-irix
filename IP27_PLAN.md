# IP27 / Onyx2 Implementation Plan

Status: active working plan. The IP19 work is stashed as
`wip-ip19-prom-bringup`; this plan starts from a clean QEMU tree.

## 1. Target and scope

- **Primary target:** SGI **Onyx2 (IP27)** with **R10000** CPUs, SMP, and
  IRIX **6.5.30**.
- **First boot target:** **Origin 200** profile of the same IP27 core:
  one or two R10000 CPUs, 512 MiB, no graphics. This isolates the
  Hub/XIO/IO6/PROM bring-up from InfiniteReality graphics.
- **Final target:** Onyx2 with multiple R10000 CPUs, IO6 storage/network,
  InfiniteReality graphics, X and useful OpenGL on IRIX 6.5.30.
- **Implementation order:** stubs first, then minimum registers/behavior
  from the PROM/kernel/driver sources, then real device behavior.

The deciding constraint is IRIX 6.5.30: it supports Onyx2/Origin2000
(R10000/IP27), but not the older R4000/R4400 Onyx/Challenge/IP19 systems.

## 2. Why Origin 200 first

Origin 200 and Onyx2 share the IP27 Hub/XIO/IO6 architecture. Origin 200
has no graphics and a small CPU/memory configuration, so it is the best
first IP27 milestone. Once the IP27 PROM, IO6, SCSI, and IRIX text boot
work on Origin 200, the same core can be extended to Onyx2 and its
InfiniteReality/XIO graphics path.

## 3. Current QEMU state

- QEMU currently has the **Indy/IP24** machine and its HPC3/GIO64 devices.
- There is **no IP27, Origin 200/Onyx2, Hub, XIO, IO6, or InfiniteReality**
  machine/device.
- `sgi_gio64.c` is an Indy GIO64/Newport implementation, not IP27 XIO.
- **No R10000 CPU model is present.** `target/mips/cpu-defs.c.inc` has
  R4000-class MIPS III models; IP27 PROM and IRIX 6.5.30 need an R10000
  (MIPS IV) CPU model. Adding this is a prerequisite.
- The IP19 machine/wiring/PROM/plan/launcher are not in the working tree.

## 4. Firmware and source inventory

### IP27 CPU PROM

`../irix-src/irix-657m/irix-657m-src/stand/arcs/IP27prom` contains:

- Full PROM source (`entry.s`, `cache.s`, `main.c`, `hubuart.c`,
  `ioc3uart.c`, `mdir.c`, `discover.c`, `nasid.c`, `partition.c`,
  `router.c`, `broute.c`, `segldr.c`, `unzip.c`, etc.).
- Prebuilt uuencoded images:
  - `SN0ip27prom.uunc`
  - `SN0ip27prom.img.uunc`
  - `SN0XXLip27prom.uunc`
  - `SN0XXLip27prom.img.uunc`
- Build targets: `make`, `make sable`, `make rtl`, `make gate`, `make io6`.
- The Makefile uses `-mips4`, ELF, 64-bit, `PROMADDR = -T
  c00000001fc00000`, and an `IP27_CONFIG` selection.

Use the **SN0XXL** image for the Onyx2/Origin2000 class first unless the
PROM source/discovery indicates otherwise. Decode the `.uunc` files to
inspect the real image format and entry point.

Decoded during planning (Python `binascii.a2b_uu`; host `uudecode` is not
installed):

- `SN0XXLip27prom.bin`: ELF64 MSB MIPS-IV executable; entry
  `0xc00000001fc00000`; loadable text/data `0xdfa40`, bss `0x9882`;
  the accompanying raw `SN0XXLip27prom.img` is 920,115 bytes and fits the
  1 MiB IP27 PROM window.
- `SN0XXLPROMio6prom.bin`: ELF64 MSB MIPS-IV; entry
  `0xc000000011c00140`; it must be extracted into the IO6 flash image
  expected by the IP27 PROM. The IP27 PROM build also embeds an
  `io6prom.img.c` image.

### IO6 PROM

`stand/arcs/IO6prom` contains the IO6 PROM source and prebuilt
`SN0PROMio6prom.uunc` / `SN0XXLPROMio6prom.uunc` images. The IP27 PROM
loads the IO6 PROM from the IO6 flash window. Build or decode the SN0XXL
IO6 PROM and model the flash window expected by the IP27 PROM.

### Kernel/driver sources

- `irix/kern/sys/SN/SN0/IP27.h`, `addrs.h`, `hub.h`, `ip27config.h`,
  `arch.h`, `klconfig.h`
- `irix/kern/sys/SN/klconfig.h`, `kldir.h`, `nvram.h`, `promhdr.h`,
  `xbow.h`, `xtalkaddrs.h`
- `irix/kern/io/io6.c`, `mace.c`, `xbow.c`
- `irix/kern/sys/PCI/ioc3.h` (IOC3 Ethernet/serial)
- `irix/kern/sys/ISPcode*.h` (QLogic ISP SCSI firmware)
- `irix/kern/sys/SN/SN0/klhwinit_gfx.h`

### Graphics caution

The tree contains generic `gfxstubs.c` and SN0 graphics configuration
headers, but **no InfiniteReality (IR) command/driver implementation**.
Onyx2 graphics cannot be implemented from the current source alone. We
will need the IR driver/firmware interface or a dump/reference, in
addition to an IR/XIO device model and a host-GL backend. Graphics is
therefore the last milestone, after a working Origin 200 text system.

## 5. IP27 architecture notes

IP27 is a ccNUMA node architecture, not an Indy-style single bus:

- **Hub/SN0**: CPU, memory/directory, and XIO bridge per node.
- **R10000**: MIPS IV CPU, primary/secondary caches, R4000-style TLB.
- **Memory/directory (MD)**: Hub-managed local memory; directory state.
- **XIO**: crossbar/IO links (eight ports in the architecture).
- **IO6**: I/O board with IOC3 UART/Ethernet, QLogic ISP SCSI, RTC,
  NVRAM/PROM flash, and bridge/XIO logic.
- **Origin 200**: small IP27 node; useful first profile.
- **Origin 2000**: larger IP27 node; SMP.
- **Onyx2**: IP27 node plus XIO InfiniteReality graphics.

Key addresses from `SN/SN0/addrs.h`:

- IP27 PROM mapped entry: `0x1fc00000`
- IP27 PROM max size: `0x100000`
- IP27 PROM flash/base: `0x01300000`/`0x01a00000` region
- IP27 PROM vectors: `0x1fc00000`, `0x1fc00008`, `0x1fc00010`,
  `0x1fc00018`, ...
- Hub RTC frequency: `1250` (800 ns cycle in the config header)

## 6. Implementation phases

### Phase 0 — R10000 CPU model and IP27 skeleton

- Add an **R10000** CPU model (MIPS IV, R4000 MMU, CP0/cache/FPU/status
  fields, `CPU_MIPS4` instruction flags). Validate against the IP27 PROM
  and later against IRIX 6.5.30.
- Add `hw/mips/sgi_ip27.c`, `include/hw/mips/sgi_ip27.h`, Kconfig and
  meson wiring; start with a machine named `ip27`/Origin 200 profile.
- Add `run-ip27.sh` with GTK, serial, monitor, `-smp`, RAM, and firmware
  options.
- Load the decoded IP27 PROM at `0x1fc00000`; start with one CPU and
  512 MiB.
- Add stubs for Hub, XIO, IO6, memory/directory, RTC, NVRAM, UART, LEDs.
- **Acceptance:** QEMU executes the IP27 PROM entry without an immediate
  exception; one CPU/512 MiB; serial/monitor usable.

### Phase 1 — IP27 PROM console and Origin 200 core

- Implement Hub identification, node/NASID discovery, KL configuration,
  memory/directory sizing, RTC, NVRAM, PROM flash/config, LEDs, and the
  Hub/IO6 console UART path.
- Follow `IP27prom`/`IO6prom` source for register order and required
  responses; prefer exact readback behavior over full diagnostics.
- Implement enough `iod`/Hub interrupt routing for the PROM.
- **Acceptance:** IP27 PROM reaches its console on Origin 200; memory and
  configuration discovery complete; no unexpected exception.

### Phase 2 — XIO, IO6, storage, network, IRIX 6.5.30 text boot

- Implement the XIO bridge/window and IO6 board model.
- Load the IO6 PROM into the IO6 flash window expected by the IP27 PROM.
- Implement IOC3 serial/Ethernet, QLogic ISP SCSI, RTC/NVRAM, and the
  bridge/interrupt paths needed by the kernel.
- Attach a SCSI disk/CD and install/boot IRIX 6.5.30 in text mode.
- Use the named 6.5.30 media; Onyx2/IP27 is supported by 6.5.30.
- **Acceptance:** IRIX 6.5.30 installs/boots to multiuser text on the IP27
  Origin 200 profile; SCSI and serial work.

### Phase 3 — SMP/IP27 scalability

- Implement multiple R10000 CPUs, Hub CPU startup, per-CPU NASID/slice
  mapping, cross-CPU interrupts, XIO coherency, and memory directory
  behavior.
- Validate two CPUs first, then four where the profile permits.
- **Acceptance:** 6.5.30 SMP boot; all CPUs online; no cross-CPU hangs.

### Phase 4 — Onyx2 InfiniteReality graphics

- Add XIO graphics transport and the Onyx2/InfiniteReality device.
- Load the IR GFX PROM/firmware; implement register/command stubs first.
- Use the kernel/driver interface and GE/RM/TM behavior; add a host-GL
  renderer only after the guest-visible command stream is understood.
- Start with framebuffer/console, then X, then simple OpenGL.
- **Acceptance:** Onyx2 reports InfiniteReality; PROM/kernel graphics
  initializes; X starts; simple OpenGL applications render.

### Phase 5 — Full Onyx2

- Multiple R10000 CPUs, 6.5.30, IO6 SCSI/network, InfiniteReality
  graphics, X/OpenGL, reset/shutdown, and stress testing.
- Update `run-ip27.sh`, `building.md`, and firmware documentation.

## 7. Initial file layout

```
hw/mips/sgi_ip27.c          # machine, memory map, PROM loading
hw/mips/sgi_ip27_hub.c      # Hub/SN0, NASID, memory/directory, RTC, UART
hw/mips/sgi_ip27_xio.c      # XIO bridge/windows
hw/mips/sgi_ip27_io6.c      # IO6, IOC3, ISP, MACE, flash
hw/mips/sgi_ip27_ir.c       # InfiniteReality (later)
include/hw/mips/sgi_ip27.h
run-ip27.sh
sgi/onyx2/                  # decoded IP27/IO6 PROM and future GFX ROMs
```

Do not reuse `sgi_gio64.c` as an IP27 machine bus; it is Indy GIO64.

## 8. Testing and acceptance

- **Phase 0:** IP27 PROM entry executes; one CPU/512 MiB; serial file.
- **Phase 1:** PROM console; Hub/NASID/KL/memory checks.
- **Phase 2:** IO6 PROM/SCSI/network; IRIX 6.5.30 text install.
- **Phase 3:** 2+ R10000 CPUs online; SMP stress.
- **Phase 4:** InfiniteReality/PROM/X/OpenGL bring-up.
- **Phase 5:** stable Onyx2 with mixed workloads.

## 9. Risks

- **R10000 CPU model is missing.** This is the first hard prerequisite.
- **IP27 Hub/ccNUMA/XIO/IO6** is substantially more complex than Indy/IP19.
- **IO6 PROM/SCSI/Ethernet** behavior must match the IP27 PROM and 6.5.30
  drivers; QLogic ISP and MACE need careful register models.
- **InfiniteReality source is absent** from the current tree. The graphics
  phase needs ROMs, driver interface, or a dump; no MAME SGI Onyx2 model
  is available locally.
- **PROM images are uuencoded**; decode and verify the SN0/SN0XXL variants.
- **6.5.30 media/install** must be verified for IP27; use the named media.

## 10. Immediate next work

1. Decode `SN0XXLip27prom.uunc` and `SN0XXLPROMio6prom.uunc`; inspect the
   ELF/load image, entry point, and sizes.
2. Add the R10000 CPU model and a minimal `ip27` machine skeleton.
3. Load the IP27 PROM at `0x1fc00000`; get the first instruction to
   execute and log the first exception.
4. Add Hub/IO6/XIO stubs, then follow the IP27 PROM source to bring up the
   console on the Origin 200 profile.
5. Only after the text system works, start Onyx2/InfiniteReality.
