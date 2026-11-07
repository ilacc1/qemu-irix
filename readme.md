# qemu-irix

A fork of [QEMU](https://github.com/qemu/QEMU) that boots **IRIX 6.5** on
emulated **Silicon Graphics Indy (IP24)** hardware. 

All the new code is agent generated. Author makes no claim as to accuracy 
or suitability for any use. 

## What does it do?

Currently emulates:

- SGI Indy (1xR4000CPU, 384Mb RAM). Supports Irix 5.3 and up to 6.5.30m. 
- SGI Challenge Desk-side IP19 (2x R4400 with 2Gb RAM). Supports Irix 5.3 to 6.5.22m.

## What's new in 0.1.2

- Windows audio goes through SDL2/WASAPI instead of DirectSound; the HAL2
  boot chime plays at the correct speed.
- Windows networking detects a bridged TAP adapter correctly.
- Machine → Removable drive inserts and ejects CD images at runtime. The CD
  drive is always present with an empty tray.
- The disk defaults to direct read-write access; set
  `IRIX_DISK_COW=overlay` for the old copy-on-write mode.
- `build.sh` / `build.bat` reproduce the documented build options, and the
  Windows build documents and enables SDL2.
- Firmware seeds now live under `sgi/indy/`.
- Fixes for HAL2 absence on IRIX 5.3, PBUS DMA control writes, and stalled
  WD33C93 transfers.

## Quick start (Indy)

From a source checkout:

1. `./build.sh` (Linux) or `build.bat` (Windows) to produce `qemu/build/qemu-system-mips64[.exe]`.
2. Put your Indy IRIX 6.5 CHD at `sgi/irix65_4020.chd` (or set `IRIX_DISK`).
3. Run `./run-irix.sh` (Linux) or `run-irix.bat` (Windows).
4. See [usage.md](usage.md) for every launcher option and environment
   variable, and [networking.md](networking.md) for TAP setup.

Release archives: run `install.sh`/`install.bat` first, then the same
launchers. Challenge/Onyx (IP19) uses `run-challenge.sh`/`.bat`.

## Disk images

I understand Indy R4000 images are relatively easy to find, but Challenge
images are not. 

So in release 0.1.4 I've included a minimal IP19 image. Decompress it
and place it in sgi/ subfolder to align with the expected location in
run-challenge.sh

You can, of course, install Irix youself using 6.5.22 images
on Challenge without issues. I wouldn't recommend earlier versions of 6.5. 
In case you want to start with original io4prom, remove the flash file -
sgi/challenge/io4prom.flash - this is essentially flashed as part of 
6.5.22 installation.


See also - **[usage.md](usage.md)** for detailed cli / env options.

## What's new

### What's new in 0.1.4

 - Fixed cdrom/scsi bug affecting install on challenge
 - Fixed prom upgrade / reflashing allowing 6.5.22 installer to update io4 prom
   on Challenge

### What's new in 0.1.3

- Support for Challenge Deskside based on IP19. This is the same board as 
  Onyx. Note however there is no graphics support here and no current
  plans to implement it. Main reason is to get as much performance as possible.  
  As an illustration, consider that the same 
  emulation code emulates Indy (reports ~200MHz on a modern host with
  graphics) and Challenge (reports ~500Mhz on a modern host without 
  graphics). 
- New qemu "terminal" mode (view - serial0) is used for IP19 GUI - it's essentially a 
  serial terminal than enables you to access the machine, configure
  network and do basic maintenance as you would on a VT100 connected
  to serial on a real headless machine.
- Gui menu option to manage cdrom - select image, eject etc. 
- Idle loop detection for R4000 and R4600. IRIX doesn't issue WAIT for 
  these CPUS so idle loop actually burns a real cpu. To achieve design 
  goal of keeping host cpus idle when guest idles, we detect memory
  locations of the idle loop (kernel specific) at runtime.
- R4600 - experimental for now.
- Many other fixes.

### What's new in 0.1.2

- Windows audio goes through SDL2/WASAPI instead of DirectSound; the HAL2
  boot chime plays at the correct speed.
- Windows networking detects a bridged TAP adapter correctly.
- Machine → Removable drive inserts and ejects CD images at runtime. The CD
  drive is always present with an empty tray.
- The disk defaults to direct read-write access; set
  `IRIX_DISK_COW=overlay` for the old copy-on-write mode.
- `build.sh` / `build.bat` reproduce the documented build options, and the
  Windows build documents and enables SDL2.
- Firmware seeds now live under `sgi/indy/`.
- Fixes for HAL2 absence on IRIX 5.3, PBUS DMA control writes, and stalled
  WD33C93 transfers.


## Why another IRIX emulator

### 1. The host sleeps when the guest sleeps

MAME and [iris](https://github.com/techomancer/iris) are both wonderful efforts
that have made this project possible. However, both keep the host CPU pinned
near 100% even when the emulated machine has nothing to do. A machine sitting
at a PROM prompt, a kernel waiting on a timer, or a guest blocked on I/O all
cost the same as a fully loaded one. That is fine for a benchmark, unpleasant
on a laptop, and wasteful on a server.

Because this is QEMU underneath, the guest runs on QEMU's event loop and TCG
with proper idle handling: when the guest executes the CPU's wait/idle
instruction, the emulation thread blocks instead of spinning. An idle IRIX
guest leaves an idle host.

### 2. Speed, with a coherent clock

CPU emulation here is substantially faster than mame or iris. On the same 
hardware it is typically around twice as fast as MAME running the same IRIX 
image, and faster host produces a genuinely faster guest. Timers still fire 
when they should, IRIX's timekeeping stays consistent, and you do not get 
the slow-motion / time-dilation behaviour that makes a sluggish emulator 
feel broken.

RTC is seeded from the host on boot, but allows the guest to set time and 
emulator respects guest time settings until next boot.

### 3. Native CHD disk support

The SGI community's disk images are **CHD** (MAME's Compressed Hunks of Data)
files. This fork adds a native CHD block driver to QEMU
(`block/chd.c`), so QEMU reads and writes CHD images directly. There is no
`chdman extracthd` step, no multi-gigabyte raw file to keep in sync, and QEMU
and MAME share the exact same disk image. 

So you can just use your existing CHD and get all the performance benefits.


## Known issues

### Graphics / OpenGL bugs

There are a few. Occasional shearing here and there, background refresh and 
colour maps in some demos are wrong. 

### ec3 sometimes reports error on boot

Safe to ignore. Unrelated to your main adapter which should be ec0.


## Notes and tips

### Mouse / keyboard capture

You can release your mouse/keyboard by hitting CTRL-ALT-G which Qemu hepfully
has in the title bar.

If your mouse jumps when you move it quickly, you can drop acceleration in Irix
(Toolchest-Find-ControlPanels-Mouse).

### NVRAM

Nvram is persisted. If you want to change MAC address on ec0 hit 
"Stop for maintenance" on boot, then hit "Enter command monitor" and then do

setenv -f eaddr 08:00:69:AB:CD:EF

replacing AB:CD:EF with the desired 3 octets.

### Optimisations (experimental)

If you experience problems or have time to play around to eke out maximum
performance, there are a few environment switches to experiment with. All of
them are value-based: `1` disables the offload, `0` (or leaving the variable
unset) leaves it enabled.

- `SGI_WD_SYNC=1` — disables the WD33C93 batched run-DMA fast path and
  forces every disk byte through the single-step transfer path (slower,
  but closer to real hardware behaviour). Default: fast path **enabled**.
- `SGI_CHD_SYNC=1` — decompresses CHD hunks inline instead of handing them
  to the block layer's thread pool. Default: thread pool **enabled**.
- `SGI_GFX_SYNC=1` — disables the GIO64 graphics worker thread and executes
  REX3 command lists synchronously on the main loop (slower, but avoids any
  worker/main-loop races). Default: worker thread **enabled**.

- `SGI_CHD_CACHE_MB=N` — size of the decompressed-hunk cache in MiB
  (default 256, capped at 4096). `0` disables the cache.

These are code defaults - os-specific startup scripts may have os-specific
defaults. Best to stick with current values, but if you want to play, change
values in the startup script.

## Credits

This work stands entirely on the shoulders of three projects and would not exist
without them. Sincere and heartfelt thanks to both:

- **[MAME](https://github.com/mamedev/mame)** — the SGI device models in this
  fork (memory controller / VDMA, GIO64 / REX3 graphics, HPC3, IOC2, HAL2,
  and more) are faithful ports of MAME's `ip24` and Indy drivers. MAME is the
  reference for register semantics, sequences and edge cases throughout. Thank
  you to the MAME developers and the SGI driver authors for years of careful,
  open, well-documented emulation work.
- **[iris](https://github.com/techomancer/iris)** — a clean, readable IRIX
  emulator that served as the parity reference at every step. When behaviour
  was ambiguous, iris was the tie-breaker. Thank you for showing how legible
  and approachable an IRIX emulator can be.
- QEMU!

Without the willingness of all three projects to publish their source, this 
would not have been possible. Thank you.


## Further reading

- **[building.md](building.md)** — how to install the toolchain and build
  this fork on Linux and Windows, in full detail.
- **[usage.md](usage.md)** — launchers, command-line options, environment
  variables and examples.
