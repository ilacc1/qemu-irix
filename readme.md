# qemu-irix

A fork of [QEMU](https://github.com/qemu/QEMU) that boots **IRIX 6.5** on
emulated **Silicon Graphics Indy (IP24)** hardware. 

All the new code is agent generated. Author makes no claim as to accuracy 
or suitability for any use. 

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

## Just get it running

1. Get the release zip / tar.gz for your OS and run either install.bat or install.sh
2. Get the indy_40xx compatible CHD from any source (mame presumably) and update run-irix.sh/bat DISK variable to point to your CHD image
3. See [networking.md](networking.md) if you want to have access to the network. Of course, these are ancient systems and highly insecure so beware.

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

Several fixes are in 0.1.1 - powerflip works perfectly now.

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

- `SGI_WD_SYNC=1` — forces every disk byte through the WD33C93
  single-step path. This is the **default** while the batched run-DMA fast
  path is under investigation; set `SGI_WD_SYNC=0` to opt into the fast
  path for testing.
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

This work stands entirely on the shoulders of two projects and would not exist
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
