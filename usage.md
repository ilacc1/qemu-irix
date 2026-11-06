# CLI and environment reference

Everything needed to launch the emulator: the launchers, their environment
variables, and the runtime helper tools.

## Launchers

| Machine | Linux | Windows | Default disk |
| --- | --- | --- | --- |
| Indy, IRIX 6.5 | `run-irix.sh` | `run-irix.bat` | `sgi/irix65_4020.chd` |
| Indy, IRIX 5.3 | `run-irix53.sh` | `run-irix53.bat` | `sgi/irix53.chd` |
| Challenge/Onyx (IP19) | `run-challenge.sh` | `run-challenge.bat` | searched, see `IRIX_DISK` |

Shorthand used below: **Indy sh** = `run-irix.sh` and `run-irix53.sh`;
**Indy bat** = `run-irix.bat` and `run-irix53.bat`;
**IP19 sh/bat** = `run-challenge.sh`/`run-challenge.bat`.

All launchers forward their command-line arguments to QEMU, after the
launcher's own arguments. For example:

```sh
./run-irix.sh -snapshot
./run-challenge.sh -d int -D /tmp/qemu.log
run-irix.bat -snapshot
```

The launchers already set the machine, disk, CD, network, display and audio
options; use the environment variables below instead of passing duplicate
QEMU options.

`-snapshot` is the simplest way to guarantee that a launch does not write to
the disk image. IP19 launchers also accept `IRIX_SNAPSHOT=1`.

## Image and disk variables

| Variable | Launchers | Default | Meaning |
| --- | --- | --- | --- |
| `IRIX_DISK` | all | see launchers above | Boot disk image path |
| `IRIX_DISK_FORMAT` | IP19 (sh, bat) | `chd` | QEMU block format for the disk |
| `IRIX_CDROM` | Indy (sh, bat) | unset | CD image; raw format, SCSI ID 4 |
| `IRIX_CD` | IP19 (sh, bat) | unset | CD image; SCSI ID 5 |
| `IRIX_CD_FORMAT` | IP19 sh | `raw` | Block format for `IRIX_CD` |
| `IRIX_DISK_COW` | Indy sh | `off` | `off` = direct read-write, `overlay` = qcow2 copy-on-write |
| `IRIX_DISK_OVERLAY` | Indy sh | `<disk>.cow.qcow2` | Overlay path when `IRIX_DISK_COW=overlay` |
| `IRIX_SNAPSHOT` | IP19 (sh, bat) | `0` | `1` adds QEMU `-snapshot` |

Notes:

- Indy disk format is always CHD. Windows Indy launchers do not support
  `IRIX_DISK_COW`.
- `IRIX_DISK_COW=overlay` uses `run/mkoverlay.py`, which is a local helper
  and is not part of the published tree.
- On IP19, if `IRIX_DISK` is unset, the launcher searches
  `sgi/ip19-main.chd`, then `./ip19-main.chd`, then `../ip19-main.chd`
  (Windows: `sgi\ip19-main.chd` first).

## Machine and memory variables

| Variable | Launchers | Default | Meaning |
| --- | --- | --- | --- |
| `IRIX_SMP` | IP19 (sh, bat) | `2` | Number of emulated CPUs |
| `IRIX_RAM` | Indy sh, IP19 (sh, bat) | `512M` (Indy), `2G` (IP19) | QEMU `-m` value |
| `IRIX_MACHINE` | Indy sh | `indy` | QEMU machine name |
| `IRIX_BIOS` | Indy sh | `ip24prom.070-9101-011.bin` | PROM file under `sgi/indy/` |
| `IRIX_MC_SIMMS` | Indy sh | `0x0eee` | `sgi-mc.simms` per-bank SIMM nibbles |
| `IRIX_MC_REV` | Indy sh | `3` | `sgi-mc.mc-rev` revision nibble |

Windows Indy launchers use a fixed `-m 512M` and fixed MC settings. The PROM
sizes Indy memory from the emulated MC registers regardless of `IRIX_RAM`.

## Display, console and monitor variables

| Variable | Launchers | Default | Meaning |
| --- | --- | --- | --- |
| `IRIX_DISPLAY` | Indy sh, IP19 sh | `gtk,zoom-to-fit=on` | QEMU `-display` value |
| `IRIX_CONSOLE` | IP19 sh, all Windows launchers | `window` | Console mode, see below |
| `IRIX_HEADLESS` | IP19 sh | `0` | `1` = no display, serial and monitor on Unix sockets |
| `IRIX_TELNET_PORT` | Indy sh | `4444` | TCP port of the PIO console chardev |
| `IRIX_MONITOR` | Indy sh, IP19 sh, IP19 bat | see below | HMP monitor endpoint |
| `IRIX_SERIAL_SOCK` | IP19 sh with `IRIX_HEADLESS=1` | `run/ip19-serial.sock` | Serial console Unix socket |
| `IRIX_ESCC_LOG` | Indy sh | unset | Directory for `esccA.log` and `esccB.log` serial captures |
| `IRIX_ESCC_SOCK` | Indy sh | unset | Unix socket path for the second ESCC port |

`IRIX_ESCC_LOG` and `IRIX_ESCC_SOCK` are alternatives: the log directory is
used when set, otherwise the socket. IP19 `telnet` console mode uses a fixed
port 4444; `IRIX_TELNET_PORT` applies to the Indy launchers only.

`IRIX_CONSOLE` values:

| Value | Linux IP19 | Windows Indy/IP19 |
| --- | --- | --- |
| `window` (default) | GTK window, serial on a GTK VC | New QEMU window, serial on a GTK VC |
| `console` | Same as `window` | GTK window, serial on stdio |
| `shell` | No display, serial on stdio | No display, serial on stdio |
| `telnet` | No display, serial on `telnet 127.0.0.1:4444` | No display, serial on `telnet 127.0.0.1:4444` |

Any other value behaves as `window`.

`IRIX_MONITOR` endpoints:

| Launcher | Default | Notes |
| --- | --- | --- |
| Indy sh | `run/mon.sock` | Unix socket, always enabled |
| Indy Windows | not used | Monitor disabled (`-monitor none`) |
| IP19 sh | `run/ip19-mon.sock` | Used only with `IRIX_HEADLESS=1`; otherwise monitor is disabled |
| IP19 bat | `tcp:127.0.0.1:4445,server=on,wait=off` | TCP HMP endpoint, always enabled |

## Network variables

| Variable | Launchers | Default | Meaning |
| --- | --- | --- | --- |
| `IRIX_NET` | IP19 sh | `tap` | `tap`, `user`, `off`/`none`/empty, or a complete `-nic` specification |
| `IRIX_NETMODE` | Indy sh | `tap` | `tap` or `user` (SLIRP) |
| `IRIX_NONET` | Indy sh, IP19 bat | unset | Non-empty value drops the NIC |
| `IRIX_TAP_IF` | Indy sh, IP19 sh/bat | `tap-iris` (Linux), `my-tap` (IP19 bat) | Host TAP interface name |
| `IRIX_NET_PORT` | IP19 sh with `IRIX_NET=user` | `2323` | Host port forwarded to guest port 23 |

Notes:

- A missing TAP interface is a warning, not an error; the guest starts
  without networking.
- Windows Indy launchers use the fixed adapter name `my-tap` and do not
  honour `IRIX_NONET` or `IRIX_TAP_IF`.
- IP19 `-nic` specification is passed to QEMU verbatim; see the QEMU
  documentation for its syntax.

## Audio and Windows variables

| Variable | Launchers | Default | Meaning |
| --- | --- | --- | --- |
| `IRIX_AUDIO` | Indy sh | `pa` | Linux: QEMU audio backend. `wav` writes to `IRIX_AUDIO_WAV`; `off` omits the audio device; other values (`pa`, `none`, ...) pass through |
| `IRIX_AUDIO_WAV` | Indy sh | `run/chime.wav` | Output path when `IRIX_AUDIO=wav` |
| `IRIX_AUDIO` | Indy bats | `sdl` | Windows: QEMU audio backend passed to `-audiodev`; use `none` for silence |
| `SDL_AUDIODRIVER` | Indy bats | `wasapi` when `IRIX_AUDIO=sdl` | Override to select another SDL audio driver |
| `MSYS2_ROOT` | all `.bat` launchers | looked up near the script | MSYS2 root; otherwise `msys64` beside the script/parent, then `C:\msys64` |
| `IRIX_MTTCG` | IP19 (sh, bat) | `1` | `1` adds `-accel tcg,thread=multi` |

## Performance and hardware tuning (`SGI_*`)

These switch emulator fast paths. Boolean values are `0`/`1`; `0` or unset
keeps the fast path enabled (synchronous mode off).

| Variable | Default | Meaning |
| --- | --- | --- |
| `SGI_GFX_SYNC` | `0` | `1`: execute GIO64 surface draws inline on the vCPU thread instead of the graphics worker |
| `SGI_WD_SYNC` | `0` | `1`: force WD33C93 DMA through the per-byte path instead of the batched fast path |
| `SGI_CHD_SYNC` | `0` | `1`: inflate CHD hunks inline instead of in the block thread pool |
| `SGI_CHD_CACHE_MB` | `256` | Decompressed-hunk cache size in MiB, clamped to 4096; `0` disables the cache |
| `SGI_HAL2_ABSENT` | off | Any value other than exactly `0` reports HAL2 as absent. The IRIX 5.3 launchers set this to `1` |

The Indy launchers export `SGI_GFX_SYNC`, `SGI_WD_SYNC` and `SGI_CHD_SYNC` as
`0` when they are not already set. IP19 launchers leave them unset.

## Idle and PROM handling (`SGI_*`)

| Variable | Default | Meaning |
| --- | --- | --- |
| `SGI_IDLE_PC` | `0` (auto) | Force the idle-loop halt address. Setting it disables automatic learning |
| `SGI_IDLE_THRESH` | `100` | Idle-loop iterations before the vCPU is halted; `0` disables the idle halt |
| `SGI_IDLE_SPIN` | enabled | `0` (or `n`/`N`) disables the idle halt |
| `SGI_IDLE_SLEEP` | off | Any value other than `0`/`n`/`N`: sleep on the host instead of halting the vCPU |
| `SGI_PROM_PC` | `0` (inactive) | PROM menu loop address; the PROM yield is inactive unless this is set |
| `SGI_PROM_THRESH` | `1000` | PROM loop iterations between host yields; `0` disables |
| `SGI_PROM_SPIN` | enabled | `0` (or `n`/`N`) disables the PROM yield |

Idle detection runs on the IP19 and Indy machines. `SGI_IDLE_PC` and
`SGI_PROM_PC` accept decimal or `0x` hexadecimal.

## Diagnostics and tracing (`SGI_*`)

These are development aids; they write to QEMU's stderr and can be very
verbose. Setting the variable enables the listed trace unless stated
otherwise.

| Variable | Meaning |
| --- | --- |
| `SGI_TRACE` | GIO64 board MMIO trace. Requires a build with `-DSGI_DEBUG` |
| `SGI_GIO_DBG` | Scanout/REX3 parameter dumps. Requires a build with `-DSGI_DEBUG` |
| `SGI_TRACE_PIO` | HPC3 PIO window accesses |
| `SGI_TRACE_BBRAM` | DS1386 NVRAM window accesses |
| `SGI_TRACE_HAL2` | HAL2 register accesses |
| `SGI_S1_TRACE` | S1 SCSI trace; disabled when the value is exactly `0` |
| `SGI_S1_VERIFY` | Verify S1 DMA transfers with extra reads |
| `SGI_GIO_CMDT` | Wallpaper host-block command trace |
| `SGI_GIO_PRIMLOG` | REX3 draw primitives, including dropped draws |
| `SGI_GIO_FILLSTATS` | Fill-path statistics |
| `SGI_GIO_FULLREFRESH` | Force a full raster refresh every frame |
| `SGI_GIO_GLPROBE` | RGB-command addressing probe |
| `SGI_GIO_MODELOG` | Scanout/XMAP mode-table probe |
| `SGI_GIO_ROWMAP` | Per-pixel write attribution |
| `SGI_GIO_SEQLOG` | Per-command header log |
| `SGI_GIO_STORE` | Wrong-colour store probe |
| `SGI_GIO_UNMAPPED` | GIO unmapped-window accesses |
| `SGI_GIO_VRAMHASH` | FNV hash of VRAM and auxiliary buffers |
| `SGI_GIO_VRAMPEEK` | Dump one VRAM and auxiliary rectangle |

The `SGI_GIO_*` probes are temporary development instrumentation and may be
removed in future revisions.

## Runtime helpers

Linux helper tools live in `run/` (local-only, not in the published tree).

| Tool | Purpose | Key options |
| --- | --- | --- |
| `run/mon.py` | Send HMP commands; default socket `run/mon.sock` | `[socket] command...`; `tcp:host:port` accepted |
| `run/console.py` | Attach to a serial/console socket | `--sock SOCK`, `--log FILE`, `--timeout SEC`, `--expect TEXT`, `--send TEXT`, `--send-now TEXT` |
| `run/telnet.py` | Log in to the guest over telnet and run commands | `--host HOST`, `--port PORT`, `--user USER`, `--pass PASS`, `--wait SEC`, `--timeout SEC`, `--command-file FILE`, `command...` |
| `run/restart-guest.sh` | Stop and optionally relaunch a guest | `--match PATTERN`, `--run-script SCRIPT`, `--no-launch` |

Examples:

```sh
run/mon.py 'info registers'
run/console.py --sock unix:run/ip19-serial.sock --timeout 20
run/telnet.py --host 10.6.2.1 --user root "uname -a"
```

## Examples

```sh
# Indy, IRIX 6.5, no network, no disk writes
IRIX_NONET=1 ./run-irix.sh -snapshot

# Indy, IRIX 6.5, CD image and copy-on-write disk
IRIX_CDROM=/path/to/overlay.iso IRIX_DISK_COW=overlay ./run-irix.sh

# Indy, IRIX 5.3
./run-irix53.sh

# IP19, snapshot, no network
IRIX_SNAPSHOT=1 IRIX_NET=off ./run-challenge.sh

# IP19, TAP network with two CPUs
IRIX_DISK=sgi/ip19-main.chd IRIX_NET=tap IRIX_SMP=2 ./run-challenge.sh
```

```bat
rem Windows Indy, no TAP adapter required beyond the normal warning
set IRIX_DISK=sgi\irix65_4020.chd
run-irix.bat

rem Windows IP19, snapshot
set IRIX_SNAPSHOT=1
run-challenge.bat
```
