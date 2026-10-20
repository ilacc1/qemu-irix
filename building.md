# Building qemu-irix

This document describes, in complete detail, how to build this fork of QEMU
for the SGI Indy (IP24) target on **Linux** and on **Windows**, and how to run
what you build.

The build produces a single-system QEMU binary:

| Platform | Binary                          |
|----------|---------------------------------|
| Linux    | `build/qemu-system-mips64` |
| Windows  | `build\qemu-system-mips64.exe` |

only for `mips64-softmmu`, which contains the `indy` machine.

---

## 1. What you need

### 1.1 The source tree

You need this repository checked out. It is a fork of upstream QEMU with
the SGI/IP24 support and the CHD block driver added (branch
`sgi-ip22-local`). The layout is:

```
qemu-irix/
├── readme.md
├── building.md
├── run-irix.sh              # Linux launcher
├── run-irix.bat             # Windows launcher
├── indy/                     # Indy firmware and NVRAM seeds
│   ├── ip24prom.070-9101-011.bin
│   └── nvram/{eeprom,rtc}
├── build/                    # build output (created by configure)
└── ...                       # the QEMU source tree
```

### 1.2 Runtime data

The PROM image and NVRAM seeds ship in `indy/` with the source. The disk
image is **not** distributed with the source (size and licensing) — you
supply it yourself. A CD ISO is never required; one can optionally be
attached as the install/overlay CD:

| File | Purpose | Where it goes |
|------|---------|---------------|
| `ip24prom.070-9101-011.bin` | IP24 PROM BIOS (bios set `b10`, the `indy_4020` default), 524288 bytes | In the repo: `indy/` |
| `A CHD image of Irix` | The bootable IRIX 6.5 disk (CHD format) | You supply it; path set via `IRIX_DISK` (Linux) or `IRIX_DISK`/`DISK` in `run-irix.bat` |
| CDROM ISO (optional) | CD on SCSI ID 4, attached only when configured | Anywhere; path set via `IRIX_CDROM` (Linux) or `CDROM` in `run-irix.bat` |
| `nvram/{eeprom,rtc}` | PROM NVRAM seed (IP24 uses a DS1386 8 KiB RTC) | In the repo: `indy/nvram/`; seeded automatically into `build/nvram/indy_4020/` on first run |

The PROM image is the `ip24prom.070-9101-011.bin` member of the MAME
indy_40xx ROM set and is already extracted into `indy/` for you.

Do **not** substitute a different PROM, or a
disk image from another machine — the emulation is matched to this exact
configuration. Image created with indy_4015 will work fine.

Alternatively you can of course try creating a clean install on CHD or on qemu native formats (qcow, raw).
I have not tried it but see no obvious reason why it wouldn't work. You will need to fight with qemu 
to load/eject mulitple CDROM images during installation.

### 1.3 Toolchain versions 

For reference, this fork has been built and verified with:

| Tool | Linux (Ubuntu 26.04) | Windows (MSYS2) |
|------|----------------------|-------------------------|
| C compiler | gcc 15.2.0 | mingw-w64 gcc 16.2.0 |
| Python | 3.14 | 3.14 |
| Meson | 1.10.1 | 1.12.1 |
| Ninja | 1.13.2 | 1.13.2 |
| GLib | 2.88.0 | 2.90.1 |
| Pixman | 0.46.4 | 0.46.4 |
| GTK3 | 3.24.52 | 3.24.52 |

Any reasonably recent versions work; QEMU's `configure` checks for what it
needs and reports anything missing.

---

## 2. Building on Linux

### 2.1 Install the build dependencies

On Debian/Ubuntu:

```sh
sudo apt-get update
sudo apt-get install -y \
    git build-essential \
    python3 python3-venv python3-tomli \
    ninja-build meson pkg-config \
    libglib2.0-dev libpixman-1-dev \
    libgtk-3-dev
```

On Fedora:

```sh
sudo dnf install -y \
    git gcc gcc-c++ make \
    python3 python3-devel \
    ninja-build meson pkgconf-pkg-config \
    glib2-devel pixman-devel \
    gtk3-devel
```

On Arch:

```sh
sudo pacman -S --needed \
    git base-devel \
    python ninja meson pkgconf \
    glib2 pixman gtk3
```

What each is for:

- `build-essential` / `base-devel` / `gcc` — the C compiler and `make`.
- `python3` (+ `venv`) — QEMU's build system and code generators are Python.
- `meson` + `ninja` + `pkg-config` — QEMU configures with Meson and builds with
  Ninja; `pkg-config` locates the libraries.
- `libglib2.0-dev` and `libpixman-1-dev` — hard requirements (GLib is used
  throughout, Pixman for the display path).
- `libgtk-3-dev` — the GTK display front end, which is how you actually see
  the Indy's screen and type at its keyboard. Without it you can still build
  and run headless, but there is no window.

Verify the tools are found:

```sh
gcc --version
python3 --version
meson --version
ninja --version
pkg-config --modversion glib-2.0 pixman-1 gtk+-3.0
```

### 2.2 Configure

From the repository root:

```sh
./configure \
    --target-list=mips64-softmmu \
    --enable-gtk \
    --disable-werror \
    --disable-docs \
    --disable-tools
```

Notes on the options:

- `--target-list=mips64-softmmu` — build only the MIPS64 system emulator,
  which is where the `indy` machine lives. Building one target keeps the build
  small and fast.
- `--enable-gtk` — the graphical window used by `run-irix.sh`.
- `--disable-werror` — do not turn warnings into errors. Recommended, since a
  newer host compiler may warn about upstream code.
- `--disable-docs` — skip Sphinx documentation (not needed to run).
- `--disable-tools` — skip `qemu-img`, `qemu-ga`, etc.

`configure` writes its output to `build/`. At the end it prints a summary
table; confirm you see `GTK+ support : YES` and `mips64-softmmu` in the list
of targets. If a required library is missing, `configure` stops with an error
naming it — install the corresponding `-dev`/`-devel` package and re-run.

To start over, delete the build directory first:

```sh
rm -rf build
```

### 2.3 Build

```sh
ninja -C build qemu-system-mips64
```

This compiles the whole emulator; on a modern machine it takes a few minutes.
The result is `build/qemu-system-mips64`.

### 2.4 Install the runtime data

The PROM and NVRAM seeds already ship in `indy/`. Copy your disk image
into place and point the launcher at it:

```sh
# from the repository root
cp /path/to/irix65_4020.chd ~/irix65_4020.chd
IRIX_DISK=~/irix65_4020.chd ./run-irix.sh
```

NVRAM is seeded automatically on first run from
`indy/nvram/` into `build/nvram/indy_4020/`.

### 2.5 Run

```sh
./run-irix.sh
```

This launches QEMU with the Indy machine, the PROM, the CHD disk on SCSI ID 1,
the overlay CD on ID 4 (when `IRIX_CDROM` names a file that exists), a GTK
window, a Telnet-able PIO console on port 4444,
and a QEMU monitor socket at `run/mon.sock`. Useful environment overrides:

| Variable | Effect |
|----------|--------|
| `IRIX_DISK` | Path to your IRIX 6.5 disk image (required). |
| `IRIX_DISPLAY` | GTK mode; default `gtk,zoom-to-fit=on`. Use plain `gtk` for 1:1 pixels. |
| `IRIX_CDROM` | Path to the overlay ISO (attached only if the file exists). |
| `IRIX_TELNET_PORT` | PIO console port (default 4444). |
| `IRIX_MONITOR` | Monitor socket path (default `run/mon.sock`). |
| `IRIX_NONET=1` | Drop the NIC entirely. |
| `IRIX_NETMODE=user` | Use SLIRP user-mode networking instead of TAP. |
| `IRIX_AUDIO` | Audio backend for the HAL2 boot chime (default `pa`). |

For a guaranteed-pristine boot, delete the NVRAM working copy so it
re-seeds from `indy/nvram/`:

```sh
rm -rf build/nvram
```

If the guest occasionally hangs on a black screen at cold start, open the
Machine menu in the QEMU window and hit "Reset" (see also Known issues in
readme.md).

---

## 3. Building on Windows

### 3.1 Install MSYS2 and the toolchain

Install MSYS2 from <https://www.msys2.org/> (default location
`C:\msys64`), then open the **MSYS2 MINGW64** shell (`mingw64.exe`):

```
C:\msys64\mingw64.exe
```

Then install everything QEMU needs:

```sh
pacman -Syu
# if it asks you to close the terminal, do so, reopen and continue:
pacman -S --needed \
    base-devel \
    mingw-w64-x86_64-gcc \
    mingw-w64-x86_64-python \
    mingw-w64-x86_64-python-packaging \
    mingw-w64-x86_64-ninja \
    mingw-w64-x86_64-meson \
    mingw-w64-x86_64-pkgconf \
    mingw-w64-x86_64-glib2 \
    mingw-w64-x86_64-pixman \
    mingw-w64-x86_64-gtk3
```

`run-irix.bat` finds your MSYS2 through the `MSYS2_ROOT` environment
variable and falls back to `msys64` next to the script. If your MSYS2
lives anywhere else, set it before running the launcher, e.g. in
`cmd.exe`:

```
set MSYS2_ROOT=C:\msys64
run-irix.bat
```

(or set it permanently under System Properties → Environment Variables).
The build itself only needs the MINGW64 shell above; the variable is
just how the launcher locates the GTK runtime DLLs.

Make sure you are in the **MINGW64** environment, not plain MSYS. Check with:

```sh
echo $MSYSTEM        # should print MINGW64
gcc --version
python3 --version
meson --version
ninja --version
pkg-config --modversion glib-2.0 pixman-1 gtk+-3.0
```

### 3.2 A note on the crypto backend

Newer MSYS2 ships **nettle 4.0**, whose `gcm_digest` API differs from what
this QEMU snapshot expects, and that breaks the default `libcrypto` build with:

```
error: too many arguments to function 'nettle_gcm_digest'
```

The fix is to disable QEMU's external crypto backends so it uses its internal
implementation. The configure line below passes `--disable-nettle
--disable-gcrypt --disable-gnutls` for exactly this reason. Nothing the Indy
needs (disk, graphics, network) uses those backends.

### 3.3 Configure

From the repository root inside the MINGW64 shell:

```sh
cd /c/apps/0irix/qemu-irix

./configure \
    --target-list=mips64-softmmu \
    --enable-gtk \
    --disable-sdl \
    --disable-vnc \
    --disable-spice \
    --disable-opengl \
    --disable-werror \
    --disable-docs \
    --disable-tools \
    --disable-guest-agent \
    --disable-capstone \
    --disable-fdt \
    --disable-libusb \
    --disable-usb-redir \
    --disable-brlapi \
    --disable-curl \
    --disable-tests \
    --disable-slirp \
    --disable-vte \
    --disable-nettle \
    --disable-gcrypt \
    --disable-gnutls
```

QEMU auto-selects the `windows.txt` machine configuration under
`configs/meson/`, so no cross-file argument is needed. Confirm the summary
shows `GTK+ support : YES` and lists `mips64-softmmu`.

To start over:

```sh
rm -rf build
```

### 3.4 Build

```sh
ninja -C build qemu-system-mips64.exe
```

The result is `build\qemu-system-mips64.exe`.

### 3.5 Install the runtime data

The PROM and NVRAM seeds already ship in `indy/`. Copy your disk image
into place (from the repository root, in the MINGW64 shell paths look
like `/c/apps/0irix/qemu-irix/...`):

```sh
cp /path/to/irix65_4020.chd /c/apps/0irix/irix65_4020.chd
```

### 3.6 Run

From a **`cmd.exe`** prompt (not the MSYS2 shell), double-click or run:

```
run-irix.bat
```

`run-irix.bat` expects the MinGW64 runtime DLLs; it looks for MSYS2 in
`%MSYS2_ROOT%`, else falls back to `msys64` next to the script. It uses:

- `build\qemu-system-mips64.exe`
- `indy\ip24prom.070-9101-011.bin`
- your IRIX 6.5 disk image (set `IRIX_DISK`, or `irix65_4020.chd` next to
  the script)
- the overlay/install CD only if `CDROM` names a file that exists
- GTK display, PIO console on `telnet 127.0.0.1 4444`, monitor on
  `telnet 127.0.0.1 5555`

Edit the paths at the top of `run-irix.bat` if your layout differs.

### 3.7 Headless smoke test

To confirm a build works without a display, run the PROM briefly with the
monitor and serial disabled:

```sh
./build/qemu-system-mips64.exe -M indy,audiodev=sgiaudio -audiodev none,id=sgiaudio \
    -m 256M -bios ./indy/ip24prom.070-9101-011.bin \
    -display none -serial null -serial null -monitor none
```

Check the binary reports the `indy` machine:

```sh
./build/qemu-system-mips64.exe -M help | grep indy
```

---

## 4. Troubleshooting

- **`configure` cannot find GTK/GLib/Pixman.** Install the matching
  `-dev`/`-devel` package (Linux) or `mingw-w64-x86_64-*` package (Windows) and
  re-run `./configure`. Delete `build/` first if you are changing options.
- **`nettle_gcm_digest` error on Windows.** You are missing
  `--disable-nettle --disable-gcrypt --disable-gnutls`; see section 3.2.
- **`qemu-system-mips64` not found by `run-irix.sh`.** Build it first
  (section 2.3); the launcher checks for it and prints this reminder.
- **No window / no display.** Without GTK, run with `-display none` (headless)
  and drive the guest through the PIO Telnet console and the monitor.
- **PROM does nothing / machine names unknown.** Confirm `indy/`
  holds `ip24prom.070-9101-011.bin` and you are using the `indy`
  machine, not `indigo2` or another SGI board.
- **Disk will not mount / XFS recovery loops.** The CHD must be the
  `indy_40xx` IRIX 6.5 disk; a disk from another SGI model does not match this
  board.

---

## 5. Quick reference

Linux:

```sh
sudo apt-get install -y git build-essential python3 python3-venv \
    ninja-build meson pkg-config libglib2.0-dev libpixman-1-dev libgtk-3-dev
./configure --target-list=mips64-softmmu --enable-gtk \
    --disable-werror --disable-docs --disable-tools
ninja -C build qemu-system-mips64
IRIX_DISK=/path/to/irix65_4020.chd ./run-irix.sh
```

Windows (MSYS2 MINGW64 shell):

```sh
pacman -S --needed base-devel mingw-w64-x86_64-gcc mingw-w64-x86_64-python \
    mingw-w64-x86_64-python-packaging mingw-w64-x86_64-ninja \
    mingw-w64-x86_64-meson mingw-w64-x86_64-pkgconf mingw-w64-x86_64-glib2 \
    mingw-w64-x86_64-pixman mingw-w64-x86_64-gtk3
cd /c/apps/0irix/qemu-irix
./configure --target-list=mips64-softmmu --enable-gtk --disable-sdl \
    --disable-vnc --disable-spice --disable-opengl --disable-werror \
    --disable-docs --disable-tools --disable-guest-agent --disable-capstone \
    --disable-fdt --disable-libusb --disable-usb-redir --disable-brlapi \
    --disable-curl --disable-tests --disable-slirp --disable-vte \
    --disable-nettle --disable-gcrypt --disable-gnutls
ninja -C build qemu-system-mips64.exe
```

then (from `cmd.exe`, with `IRIX_DISK` set to your IRIX 6.5 CHD)
`run-irix.bat`.
