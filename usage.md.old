# Running qemu-irix

This document describes the source tree, the launchers, their command-line
options and the environment variables they honour.

## Tree layout

```
qemu-irix/
├── qemu/            QEMU source and build output (qemu/build/)
├── sgi/             PROM and NVRAM files (tracked)
│   ├── indy/
│   └── challenge/
├── iso/             CD, overlay and netinstall images (gitignored)
├── run/             helper tools (monitor, console, tracing)
├── archive/         reference material (gitignored)
├── datasheets/      reference PDFs (gitignored)
├── build.sh/.bat
├── install.sh/.bat
├── release.sh/.bat
├── run-irix.sh/.bat       Indy, IRIX 6.5
├── run-irix53.sh/.bat     Indy, IRIX 5.3
├── run-challenge.sh/.bat  Challenge/Onyx (IP19)
├── readme.md
├── building.md
├── usage.md
└── networking.md
```

Disk images live in `sgi/` and are gitignored. CD, overlay and netinstall
images live in `iso/` and are gitignored. PROM and NVRAM files under `sgi/`
are tracked.

## Build

Linux:

```sh
./build.sh
```

Windows:

```bat
build.bat
```

Both produce `qemu/build/qemu-system-mips64[.exe]`. Extra configure options
are passed through, for example `./build.sh --enable-debug`.

## Choosing a launcher

| Machine | Linux | Windows | Default disk |
| --- | --- | --- | --- |
| Indy, IRIX 6.5 | `run-irix.sh` | `run-irix.bat` | `sgi/irix65_4020.chd` |
| Indy, IRIX 5.3 | `run-irix53.sh` | `run-irix53.bat` | `sgi/irix53.chd` |
| Challenge/Onyx (IP19) | `run-challenge.sh` | `run-challenge.bat` | `sgi/ip19-main.chd` |

All launchers accept extra QEMU arguments after the launcher's own
arguments, for example `./run-irix.sh -snapshot`. Use `-snapshot` to run
without writing to the disk image.

## Common environment variables

These are read by the launchers where applicable. Unset variables keep the
documented default.

| Variable | Meaning | Default |
| --- | --- | --- |
| `IRIX_DISK` | Path to the boot disk image | per-launcher, see above |
| `IRIX_DISK_FORMAT` | QEMU block format for the disk | `chd` |
| `IRIX_DISK_COW` | `off` for direct read-write, `overlay` for qcow2 COW | `off` |
| `IRIX_DISK_OVERLAY` | Overlay path when `IRIX_DISK_COW=overlay` | `<disk>.cow.qcow2` |
| `IRIX_CDROM` | CD image for the Indy SCSI CD drive | none |
| `IRIX_CD` | CD image for the IP19 SCSI CD drive | none |
| `IRIX_CD_FORMAT` | QEMU block format for the IP19 CD | `raw` |
| `IRIX_RAM` | Guest RAM (`512M` Indy, `2G` IP19 by default) | per-launcher |
| `IRIX_SMP` | IP19 CPU count | `2` |
| `IRIX_MACHINE` | QEMU machine name for the Indy launchers | `indy` |
| `IRIX_BIOS` | PROM file name under `sgi/indy/` | `ip24prom.070-9101-011.bin` |
| `IRIX_DISPLAY` | QEMU display backend | `gtk,zoom-to-fit=on` |
| `IRIX_CONSOLE` | Console mode: `window`, `console`, `shell`, `telnet` (Windows) | `window` |
| `IRIX_MONITOR` | HMP monitor socket or TCP endpoint | `run/mon.sock` (Linux), `tcp:127.0.0.1:4445` (IP19 Windows) |
| `IRIX_SERIAL_SOCK` | Unix socket for the IP19 serial console | `run/ip19-serial.sock` |
| `IRIX_TELNET_PORT` | TCP port for the Indy PIO console | `4444` |
| `IRIX_TELNET_PORT` | TCP port for the IP19 PIO console in headless mode | `4444` |
| `IRIX_SNAPSHOT` | `1` runs IP19 with `-snapshot` (no disk writes) | `0` |
| `IRIX_MTTCG` | `1` enables TCG multi-threading for IP19 | `1` |
| `IRIX_HEADLESS` | `1` runs the IP19 launcher without a display | `0` |
| `IRIX_NET` | IP19 networking: `tap`, `user`, `off`, or a `-nic` spec | `tap` |
| `IRIX_NETMODE` | Indy networking: `tap` or `user` (SLIRP) | `tap` |
| `IRIX_NONET` | Set to drop the NIC entirely | unset |
| `IRIX_TAP_IF` | Host TAP interface name | `tap-iris` (Linux), `my-tap` (Windows) |
| `IRIX_NET_PORT` | Host forward port for IP19 user-mode networking | `2323` |
| `IRIX_AUDIO` | Audio backend: `pa`, `wav`, `none`, `off` (Linux), `sdl` (Windows) | `pa` / `sdl` |
| `IRIX_AUDIO_WAV` | WAV output path when `IRIX_AUDIO=wav` | `run/chime.wav` |
| `IRIX_ESCC_LOG` | Directory for ESCC serial logs | unset |
| `IRIX_ESCC_SOCK` | Unix socket for an ESCC channel | unset |
| `IRIX_MC_SIMMS` | Indy memory-controller SIMM nibble | `0x0eee` |
| `IRIX_MC_REV` | Indy memory-controller revision nibble | `3` |

IP19-only launcher variables are `IRIX_SMP`, `IRIX_CD`, `IRIX_CD_FORMAT`,
`IRIX_NET`, `IRIX_NET_PORT`, `IRIX_SERIAL_SOCK`, `IRIX_SNAPSHOT`,
`IRIX_MTTCG` and `IRIX_HEADLESS`.

The Indy launchers additionally support `IRIX_DISK_COW` and
`IRIX_DISK_OVERLAY`. The Windows launchers use the same `IRIX_` names but
have platform-specific defaults for audio, TAP adapter and the console
mode.

## Emulator tuning variables

These are software switches inside QEMU. Defaults are chosen for normal use;
change them only when investigating behaviour.

| Variable | Meaning | Default |
| --- | --- | --- |
| `SGI_GFX_SYNC` | `1` executes GIO64 command lists synchronously | `0` |
| `SGI_WD_SYNC` | `1` disables the WD33C93 batched DMA fast path | `0` |
| `SGI_CHD_SYNC` | `1` inflates CHD hunks inline instead of in the thread pool | `0` |
| `SGI_CHD_CACHE_MB` | Decompressed-hunk cache size in MiB (`0` disables) | `256` |
| `SGI_HAL2_ABSENT` | `1` reports HAL2 as absent (IRIX 5.3 default) | `1` for IRIX 5.3 |
| `SGI_IDLE_PC` | Force the idle-loop halt address instead of learning it | `0` (auto) |
| `SGI_IDLE_THRESH` | Idle-loop iterations before halting | `100` |
| `SGI_IDLE_SPIN` | `0` disables the idle halt | enabled |
| `SGI_IDLE_SLEEP` | `1` uses host sleeps instead of the halt | off |
| `SGI_PROM_PC` | Force the PROM menu spin address | `0` (disabled) |
| `SGI_PROM_THRESH` | PROM spin yields after this many iterations | `1000` |
| `SGI_PROM_SPIN` | `0` disables the PROM spin yield | enabled |

## Monitor and console access

Linux Indy launchers create `run/mon.sock`. Use the bundled helpers:

```sh
run/mon.py 'info registers'
run/mon.py 'info status'
run/console.py --sock unix:run/ip19-serial.sock --timeout 20
```

The IP19 Linux launcher defaults to TAP networking and a monitor socket.
With `IRIX_HEADLESS=1` it also exposes the serial console on a Unix socket.
The IP19 Windows launcher exposes the monitor on
`tcp:127.0.0.1:4445` and the PIO console on `telnet 127.0.0.1 4444` when
`IRIX_CONSOLE=telnet`.

## Examples

Indy IRIX 6.5, direct disk, no network:

```sh
IRIX_NONET=1 ./run-irix.sh
```

Indy IRIX 6.5 with a CD image and copy-on-write disk:

```sh
IRIX_CDROM=iso/IRIX-6.5.22-Overlay1.iso \
IRIX_DISK_COW=overlay ./run-irix.sh
```

Indy IRIX 5.3:

```sh
./run-irix53.sh
```

IP19 Challenge, snapshot (no disk writes), network off:

```sh
IRIX_SNAPSHOT=1 IRIX_NET=off ./run-challenge.sh
```

IP19 Challenge with TAP networking and extra QEMU tracing:

```sh
IRIX_DISK=sgi/ip19-main.chd ./run-challenge.sh -d int
```

Windows, from `cmd.exe`:

```bat
set IRIX_NONET=1
run-irix.bat
```

```bat
set IRIX_DISK=sgi\ip19-main.chd
set IRIX_SNAPSHOT=1
run-challenge.bat
```

## Where to put images

- `IRIX_DISK` and `IRIX_CDROM`/`IRIX_CD` accept any path. The defaults expect
  the images in `sgi/` (disks) and `iso/` (CDs).
- Do not commit disk or ISO images. Both directories are gitignored for
  those file types.
- PROMs and NVRAM seeds under `sgi/indy` and `sgi/challenge` are tracked and
  must stay in place; the launchers and the release scripts read them there.
