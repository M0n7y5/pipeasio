<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/logo.svg">
    <img alt="PipeASIO" src="docs/logo-light.svg" width="300">
  </picture>
</p>

<p align="center">
  <a href="https://m0n7y5.github.io/pipeasio/"><b>Website &amp; docs</b></a>
</p>

<p align="center">
  <a href="https://github.com/M0n7y5/pipeasio/releases"><img alt="Release" src="https://img.shields.io/github/v/release/M0n7y5/pipeasio?include_prereleases&amp;label=release&amp;color=ff6a1f"></a>
  <a href="https://aur.archlinux.org/packages/pipeasio"><img alt="AUR version" src="https://img.shields.io/aur/version/pipeasio?label=AUR&amp;color=ff6a1f"></a>
  <a href="https://fluxer.gg/THKL1aSc"><img alt="Fluxer guild" src="https://img.shields.io/badge/Fluxer-join%20the%20guild-4641D9"></a>
  <img alt="License" src="https://img.shields.io/badge/license-GPL--3.0-blue">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Linux%20x86__64-lightgrey">
  <img alt="PipeWire" src="https://img.shields.io/badge/PipeWire-1.4.2%2B-ff6a1f">
</p>

PipeASIO lets Windows music software running under Wine or Proton use fast,
low-latency audio on Linux.

ASIO is the standard low-latency audio driver on Windows. Music programs (DAWs)
such as FL Studio, Ableton Live, and Reaper rely on it for responsive playback
and recording. PipeASIO provides that driver inside Wine and connects it
straight to PipeWire, the audio system modern Linux distributions use. Your DAW
sees a normal ASIO device. PipeWire sees a normal audio app it can route
anywhere.

It works in plain Wine and inside Proton and Steam (Faugus, Proton-CachyOS),
and it installs safely next to WineASIO: your DAW lists them as two separate
drivers.

![PipeASIO settings panel](docs/panel-settings.png)

> [!NOTE]
> PipeASIO is at **1.3.0**. It is verified with FL Studio under Proton-CachyOS and with the [VB-Audio ASIO Test](https://forum.vb-audio.com/viewtopic.php?p=4259#p4259) utility (64-bit and 32-bit). Other ASIO hosts such as Reaper and Ableton Live should work but are not yet confirmed. x86_64, with experimental opt-in 32-bit (WoW64) support. Bug reports are very welcome on the [issue tracker](https://github.com/M0n7y5/pipeasio/issues).

## Support

If PipeASIO is useful to you, you can support its development on Ko-fi:

[![Support me on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/m0n7y5)

## Contributing

**Issues are open to everyone.** Bug reports, questions and feature requests are
welcome on the [issue tracker](https://github.com/M0n7y5/pipeasio/issues) - no
invitation needed, and a good bug report is usually worth more than a patch.

**Pull requests are only reviewed from manually selected contributors.** PipeASIO
has one maintainer, and the repository was attracting pull-request spam from
botted accounts; sifting through it was eating the time that goes into the
driver. 

If you would like to work on PipeASIO, come and say so in the Fluxer guild, with
a rough outline of what you want to change:

<a href="https://fluxer.gg/THKL1aSc">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/fluxer-logo-white.svg">
    <img alt="Fluxer" src="docs/fluxer-logo-color.svg" width="170">
  </picture>
</a>

**[Join the guild &rarr;](https://fluxer.gg/THKL1aSc)**

Once you are on the contributor list the usual rules apply: run `clang-format`
(the config is in-tree) before submitting, and keep changes x86_64 and C11.
Pull requests from outside the list are closed without review - open an issue or
reach out in the guild instead.

## Quick start

On Arch Linux and derivatives (CachyOS, EndeavourOS, Manjaro), install
[`pipeasio` from the AUR](https://aur.archlinux.org/packages/pipeasio):

```sh
paru -S pipeasio   # or: yay -S pipeasio

# Register in the current Wine prefix
pipeasio-register
```

Everywhere else, build from source:

```sh
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build

# Install (user-local, or --prefix /usr for system-wide)
cmake --install build --prefix "$HOME/.local"

# Register in the current Wine prefix
pipeasio-register
```

Under Proton or Steam, also set `WINEDLLPATH=$HOME/.local/lib/wine` in the launcher and register inside the game's prefix. See the Proton / Steam / Faugus section below.

## Building

CMake only. The driver is 64-bit. Opt-in 32-bit (WoW64) support for 32-bit
Windows hosts is covered in [32-bit applications](#32-bit-applications-experimental).

Requirements: `cmake` (3.20 or newer), `ninja` (recommended) or GNU make,
`gcc`, `pkg-config`, the Wine SDK (headers plus `winebuild` and `winegcc`), and
the PipeWire development headers. The Qt6 settings panel is optional: it builds
when a C++ compiler and Qt6 Widgets are present and is skipped with a warning
otherwise, which does not affect the driver. Pass `-DBUILD_SETTINGS_PANEL=OFF`
to skip it deliberately and silence the warning.

Package names differ per distribution. These are the sets CI builds against,
plus the Qt6 package for the optional panel:

```sh
# Arch / CachyOS / EndeavourOS / Manjaro
sudo pacman -S --needed cmake ninja gcc pkgconf wine libpipewire qt6-base

# Fedora
sudo dnf install cmake ninja-build gcc gcc-c++ pkgconf \
    wine-devel pipewire-devel qt6-qtbase-devel

# Debian / Ubuntu
sudo apt install cmake ninja-build gcc g++ pkg-config \
    wine64-tools libwine-dev libpipewire-0.3-dev qt6-base-dev
```

There is no `libpipewire-0.3-dev` or `winehq-*-dev` on Fedora, and no
`wine-devel` on Debian; installing the wrong name makes the whole transaction
fail, and the build then stops at `libpipewire-0.3 not found` or `Wine SDK
headers not found`. Those are the distributions' own packages. Wine from the
WineHQ repositories works differently (see
[Wine outside /usr](#wine-outside-usr)): Fedora's `wine-devel` is a headers
package in `/usr/include/wine`, while WineHQ's `wine-devel` is a whole Wine
branch in `/opt/wine-devel`.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Wine outside /usr

The WineHQ packages install into `/opt/wine-{devel,stable,staging}`, and some
installer scripts drop a private Wine under `$HOME`. Put that Wine's `bin/` on
`PATH`: CMake finds `winebuild` there and takes the headers from the same
prefix, so the SDK belongs to the Wine you build against.

```sh
PATH=/opt/wine-devel/bin:$PATH cmake -B build -DCMAKE_BUILD_TYPE=Release
```

This works only if the SDK is installed in that prefix too. WineHQ splits every
branch in two: the branch package ships `bin/` (including `winebuild`), while a
separate companion ships `include/wine/`. An `/opt/wine-devel` with no
`include/` directory is the usual case:

```sh
# Fedora, WineHQ repository - match the branch you installed
sudo dnf install wine-devel-devel     # or wine-stable-devel / wine-staging-devel

# Debian / Ubuntu, WineHQ repository
sudo apt install wine-devel-dev       # or wine-stable-dev / wine-staging-dev
```

Configure warns when `winebuild` comes from a prefix whose `include/` has no
SDK and another Wine's headers are used instead.

Only if that fails, override the probe with `-DWINE_INCLUDE_DIRS` - and pass
the include *directories*, not the install root. All three matter: the root
resolves `wine/debug.h`, `wine/` holds `unixlib.h` (needed by the 32-bit WoW64
halves), and `wine/windows/` is the Win32 SDK:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DWINE_INCLUDE_DIRS="/opt/wine-devel/include;/opt/wine-devel/include/wine;/opt/wine-devel/include/wine/windows"
```

Debian's `libwine-dev` nests one level deeper (`/usr/include/wine`,
`/usr/include/wine/wine`, `/usr/include/wine/wine/windows`). Configure checks
the list before compiling and names whichever header it cannot reach.

Debug build (assertions and Wine debug-channel macros):

```sh
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

## Installing

### From the AUR (Arch Linux)

```sh
paru -S pipeasio
```

The package installs the driver system-wide under `/usr`, plus the
`pipeasio-settings` panel with a desktop entry and icon. Note that a
system-wide install is invisible to Proton's container. See the Proton /
Steam / Faugus section below.

### From a GitHub release

[Releases](https://github.com/M0n7y5/pipeasio/releases) carry a prebuilt
`pipeasio-<version>-archlinux-x86_64.tar.gz` (the 64-bit driver plus the opt-in
32-bit WoW64 front end) for the Arch / CachyOS family. Extract it over a prefix:

```sh
# user-local (required for Proton / Faugus / Steam, see below)
tar -xzf pipeasio-*-archlinux-x86_64.tar.gz -C "$HOME/.local"
# or system-wide
sudo tar -xzf pipeasio-*-archlinux-x86_64.tar.gz -C /usr
```

then [register](#registering). The download is *scoped*: its `BUILD-INFO.txt`
lists the exact Wine, glibc, and PipeWire it was built against. A different Wine
version or an older glibc can fail to load (`regsvr32` `c0000135`). Build it
[from source](#from-source) in that case.

### From source

System-wide:

```sh
sudo cmake --install build --prefix /usr
```

That puts the driver under `/usr/lib/wine`, which is where distro Wine reads
it on Arch and similar layouts. Elsewhere Wine's library dir differs:
`/usr/lib64/wine` on Fedora, `/usr/lib/x86_64-linux-gnu/wine` on
Debian/Ubuntu, and `lib/wine` inside the tree for WineHQ builds under
`/opt/wine-<branch>`. For those, point the driver files at the right root (the
tools still land under the normal prefix):

```sh
# Fedora
cmake -B build -DPIPEASIO_WINE_INSTALL_ROOT=/usr/lib64/wine
sudo cmake --install build --prefix /usr

# Debian/Ubuntu
cmake -B build -DPIPEASIO_WINE_INSTALL_ROOT=/usr/lib/x86_64-linux-gnu/wine
sudo cmake --install build --prefix /usr

# WineHQ (e.g. wine-staging)
cmake -B build -DPIPEASIO_WINE_INSTALL_ROOT=/opt/wine-staging/lib/wine
sudo cmake --install build --prefix /usr
```

User-local (no `sudo`, under `$HOME`, required for Proton / Faugus / Steam, see
below):

```sh
cmake --install build --prefix "$HOME/.local"
```

Either one lays down:

```
<prefix>/lib/wine/x86_64-windows/pipeasio64.dll
<prefix>/lib/wine/x86_64-windows/pipeasio.dll    -> pipeasio64.dll
<prefix>/lib/wine/x86_64-unix/pipeasio64.dll.so
<prefix>/lib/wine/x86_64-unix/pipeasio.dll.so    -> pipeasio64.dll.so
```

The `pipeasio.dll{,.so}` symlinks satisfy the unified PE name that Wine 10+
expects. Without them, `regsvr32 pipeasio64.dll` fails with status `c0000135` on
newer Wine.

By default the driver installs under `<prefix>/lib/wine`, matching Wine's
upstream layout (`CMAKE_INSTALL_LIBDIR` does not move it);
`PIPEASIO_WINE_INSTALL_ROOT` overrides that root with an absolute path. Where
the driver *should* go depends on your Wine; see [Registering](#registering)
for why it matters at runtime.

## Registering

After installing, register the driver in each Wine prefix. A helper script is
provided:

```sh
pipeasio-register
```

This activates PipeASIO for the current Wine prefix (the default is `~/.wine`).
To target another prefix:

```sh
env WINEPREFIX="$HOME/asioapp" pipeasio-register
```

`pipeasio-register` searches for the install root in this order:
`$PIPEASIO_PREFIX/lib/wine`, then `$HOME/.local/lib/wine`, then
`/usr/local/lib/wine`, then `/usr/lib/wine`, then distro variants, then
`/opt/wine-{devel,stable,staging}/lib{,64}/wine`. Set `PIPEASIO_PREFIX` to
point it at a non-standard install, or `PIPEASIO_REGISTER_CANDIDATES` to
replace the built-in list entirely (colon-separated; `PIPEASIO_PREFIX` is
still searched first).

Registration only makes the driver *visible* to a prefix. At launch, Wine
resolves the driver's Unix half only in its own library dir and in
`WINEDLLPATH`, so the install root must be that library dir: `/usr/lib/wine`
on Arch/Fedora layouts, `/usr/lib/x86_64-linux-gnu/wine` on Debian/Ubuntu, or
the `lib/wine` inside a WineHQ `/opt/wine-<branch>` tree.
`pipeasio-register` compares the install root against the `wine` on your
`PATH` and warns when they mismatch. The fix is to install into the matching
root (`-DPIPEASIO_WINE_INSTALL_ROOT=...`, see [Installing](#installing)) or to
set `WINEDLLPATH=<driver-install-root>` in the host's launch environment.

## 32-bit applications (experimental)

PipeASIO is 64-bit by default. A front end for **32-bit** Windows ASIO hosts
(foobar2000 `foo_out_asio`, older REAPER builds, ...) can be built opt-in via
Wine's *new WoW64*: the 32-bit half is a thin PE thunk that forwards every ASIO
call to the same 64-bit PipeWire backend over `__wine_unix_call`. There is no
32-bit libpipewire and no 32-bit Linux userspace - only the Windows-facing half
is i386.

> **Experimental, off by default.** The 64-bit driver is byte-for-byte
> unaffected. The 32-bit path is validated end-to-end by the `asio_probe32`
> host and by VB-Audio's VBASIOTest32 (a real i386 ASIO host): COM
> `Init -> CreateBuffers -> Start`, real-time streaming, autoconnect to the
> selected hardware, and live config reload all work. Bit-exact loopback
> validation on 32-bit is still pending.

It needs a MinGW cross-compiler on top of the normal build requirements:

```sh
sudo pacman -S mingw-w64-gcc          # Arch / CachyOS
sudo apt install gcc-mingw-w64-i686   # Debian / Ubuntu
```

Configure with `-DBUILD_WOW64_32=ON` (default OFF). Set `WINE_LIB_ROOT` if your
Wine library prefix is not `/usr/lib/wine`:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_WOW64_32=ON
cmake --build build
```

Installing then lays the 32-bit halves alongside the 64-bit ones:

```
<prefix>/lib/wine/i386-windows/pipeasio32.dll    (PE thunk, builtin)
<prefix>/lib/wine/x86_64-unix/pipeasio32.so      (unixlib, 64-bit ELF)
```

`pipeasio-register` handles both architectures automatically: it registers the
CLSID under the 64-bit registry view and, when `pipeasio32.dll` is present, the
same CLSID under the 32-bit view (`regsvr32 /reg:32`). 32-bit and 64-bit hosts
can then use PipeASIO from the same prefix.

Under Proton, 32-bit apps additionally need `PROTON_USE_WOW64=1` (or Faugus's
WoW64 toggle) so Proton runs them through new WoW64 - see
[Proton / Steam / Faugus](#proton--steam--faugus).

## Proton / Steam / Faugus

Proton runs Wine inside a pressure-vessel container (steamrt4). The container does
not expose the host's `/usr/lib/wine/`, so a system-wide install is invisible to
Proton's Wine. Two things make PipeASIO work inside Proton (32-bit apps need a
third, see step 3):

1. Install PipeASIO under `$HOME` (pressure-vessel exposes the home directory by
   default):

   ```sh
   cmake --install build --prefix "$HOME/.local"
   ```

2. Set `WINEDLLPATH=$HOME/.local/lib/wine` in the launcher's per-game environment
   so Proton's Wine finds the ELF `.so` half. Proton has honored a host-provided
   `WINEDLLPATH` since
   [PR #9420](https://github.com/ValveSoftware/Proton/pull/9420), and
   Proton-CachyOS ships this fix.

   - **Steam:** put it in the game's launch options (right-click the game >
     Properties > General):

     ```
     WINEDLLPATH=/home/<you>/.local/lib/wine %command%
     ```

   - **Faugus:** put it in the per-game "Launch options" environment field:

     ```
     WINEDLLPATH=/home/<you>/.local/lib/wine
     ```

   Use the absolute path. Faugus does not expand `~` or `$HOME` in that field.

3. **32-bit games and hosts only:** enable Proton's *new WoW64* mode, which the
   32-bit front end requires (see
   [32-bit applications](#32-bit-applications-experimental)). By default Proton
   runs 32-bit apps through the classic split-WoW64 Wine libraries, which
   cannot load PipeASIO's 32-bit half.

   - **Steam:** add `PROTON_USE_WOW64=1` alongside `WINEDLLPATH` in the game's
     launch options:

     ```
     PROTON_USE_WOW64=1 WINEDLLPATH=/home/<you>/.local/lib/wine %command%
     ```

   - **Faugus:** enable the **WoW64** option in the game's settings, or add
     `PROTON_USE_WOW64=1` to the same per-game "Launch options" environment
     field as `WINEDLLPATH`.

   64-bit games and hosts do not need this.

Then register PipeASIO in the Proton wineprefix as usual:

```sh
env WINEPREFIX="$HOME/Faugus/<game>" pipeasio-register
```

The PE stub lands in `<prefix>/drive_c/windows/system32/` and the CLSID
registration persists in the prefix registry, both shared across Wine versions.

## Other distributions

Active support targets Arch Linux and its derivatives (CachyOS, EndeavourOS,
Manjaro), but PipeASIO itself is distribution-agnostic: it is plain
`libpipewire-0.3` plus Wine, with no distro-specific code paths, so it runs
wherever those are available. A few environment details differ between distros
and are the usual causes of trouble elsewhere:

- **Wine library layout.** Both DLL halves must land under the distro's
  `lib/wine/x86_64-{windows,unix}/` directory: `/usr/lib/wine` on Arch,
  `/usr/lib/x86_64-linux-gnu/wine` on Debian/Ubuntu, `/usr/lib64/wine` on
  Fedora. Match it with `-DPIPEASIO_WINE_INSTALL_ROOT=...` at configure time
  (see [Installing](#installing)); `WINE_LIB_ROOT` only locates the 32-bit
  import libraries at build time and does not affect install locations. A
  user-local `--prefix "$HOME/.local"` install plus `WINEDLLPATH` sidesteps
  the question entirely.
- **Wine version.** The 64-bit driver runs on current Wine. Wine 10+ needs the
  `pipeasio.dll` symlinks the install creates (see [Installing](#installing)).
  The experimental 32-bit front end additionally requires Wine's *new WoW64*.
  Older or split-WoW64 Wine cannot load it.
- **PipeWire version.** 1.4.2 or newer, the version Steam Runtime 4 and Debian
  13 ship; configure refuses anything older. The forced quantum/rate that pins
  low latency is not what sets this floor - those properties have existed since
  PipeWire 0.3.45. What does raise latency is the daemon clamping the forced
  quantum to `clock.min-quantum` / `clock.max-quantum`, at any version; the
  driver logs a warning naming both when that happens.
- **Real-time priority.** The driver runs at normal scheduling by default;
  `SCHED_FIFO` priority 15 is opt-in. See [Performance](#performance) for
  access requirements.

## Configuration

PipeASIO talks to PipeWire 1.4.2+ natively through `libpipewire-0.3`. The graph
quantum is locked to the ASIO host's negotiated buffer size with
`PW_KEY_NODE_FORCE_QUANTUM` (unless `follow_device_clock` is set, in which case the
target device drives the cycle). The sample rate follows the graph unless pinned
with `sample_rate`, in which case `PW_KEY_NODE_FORCE_RATE` is set.

Settings live in a flat INI file at `$XDG_CONFIG_HOME/pipeasio/config.ini`
(fallback `~/.config/pipeasio/config.ini`). The driver reads it at startup and
re-reads it while running, so saving in the settings panel applies within about a
second without reselecting the driver or restarting the host. Every option can
also be overridden by an environment variable. A missing file means built-in
defaults, and unknown keys are ignored. The file has a single `[pipeasio]`
section.

### inputs / outputs
Number of PipeWire DSP ports PipeASIO opens. Default 2 / 2.
Env: `PIPEASIO_NUMBER_INPUTS`, `PIPEASIO_NUMBER_OUTPUTS`.

### auto_connect
Default 1: connect the channels to a hardware device on start. Set to 0 to leave
the node unconnected and patch it yourself in a PipeWire patchbay.
Env: `PIPEASIO_CONNECT_TO_HARDWARE` (`on`/`off`).

### output_device / input_device
The PipeWire `node.name` of the sink (output) and source (input) to auto-connect
to. Empty (the default) follows the PipeWire default sink/source, re-resolved each
time the driver reconnects.
Env: `PIPEASIO_OUTPUT_DEVICE`, `PIPEASIO_INPUT_DEVICE`.

### sample_rate
`0` (default) follows the PipeWire graph rate. A non-zero value pins the rate with
`PW_KEY_NODE_FORCE_RATE`.
Env: `PIPEASIO_SAMPLE_RATE`.

### fixed_buffer_size
Default 1: the buffer size is controlled by PipeWire and the ASIO host cannot
change it. Set to 0 to let the host change PipeWire's quantum (via
`PW_KEY_NODE_FORCE_QUANTUM`) in `CreateBuffers()`.
Env: `PIPEASIO_FIXED_BUFFERSIZE` (`on`/`off`).

### follow_device_clock
Default 0 (off): the driver pins the PipeWire graph quantum to the host's buffer
size and runs as its own low-latency clock master, which is correct for wired
devices. Set to 1 to make the driver a follower instead: it drops `FORCE_QUANTUM`
so the target device drives the cycle. This is required for Bluetooth sinks, whose
clock is the radio link and cannot be slaved to the host. Otherwise PipeWire
silently drops the links and you get no sound. The buffer size is then dictated by
the device (the driver settles to the device's quantum after one automatic reset),
so latency is higher.
Env: `PIPEASIO_FOLLOW_DEVICE_CLOCK` (`on`/`off`).

### buffer_size
The preferred size returned by `GetBufferSize()`. Must be a power of two within
[16, 8192]. Out-of-range values fall back to 1024.
Env: `PIPEASIO_PREFERRED_BUFFERSIZE`.

A size the hardware does not support makes PipeWire reject the request or insert
resampling, and either way you may get xruns.

### node_name
Overrides the PipeWire client/node name (otherwise derived from the host program
name).
Env: `PIPEASIO_CLIENT_NAME`.

### realtime
Default 0 (off), experimental. When enabled, the thread carrying the host's
`bufferSwitch` callback uses `SCHED_FIFO` priority 15, covering both the native
PipeWire data loop and the WoW64 PE pump.

Only that one thread is elevated. A host that dispatches DSP work from the
callback to its own worker threads leaves those workers at `SCHED_OTHER`, and
the resulting priority asymmetry can make things much worse: measured on FL
Studio 2025 under Wine at 64 frames / 48 kHz, enabling this produced roughly 39x
more xruns on the PipeASIO node than leaving it off.

Useful as a diagnostic for the scheduling-related xruns in
[#4](https://github.com/M0n7y5/pipeasio/issues/4), not as a fix for them. It
takes effect the next time the host starts the driver.

Env: `PIPEASIO_RT_PRIORITY` (`off`/`on`). The environment overrides the file.

## Performance

A few knobs affect xrun-free, low-latency operation:

- Real-time scheduling. The callback thread runs at normal scheduling by
  default; `SCHED_FIFO` is opt-in via [`realtime`](#realtime), and on
  multi-threaded hosts it often costs more xruns than it saves. When you do opt
  in, the requested priority is 15 (the previous native/WoW64 defaults were
  77/80). It must stay below the PipeWire daemon's data loop: RTKit caps that
  loop at 20 on stock desktops, while PAM/realtime-group setups may run it
  higher, where either value is below the daemon. On other distributions you
  usually set real-time access up yourself; the driver does not use RTKit.
  Without the grant the thread stays at normal scheduling. Verify with
  `ulimit -r` (at least 15; use a higher limit only if you pin `loop.rt-prio`).
- Channel count. Every input and output is a PipeWire port the graph must
  schedule. Defaults are 2 in / 2 out. Raise `inputs` / `outputs` only to what you
  route. Fewer ports mean a smaller graph and less overhead.
- Buffer size. Smaller buffers cut latency but raise CPU and xrun risk.
- Debug logging. `PIPEASIO_DEBUG=1` makes the driver log on the audio path. Leave
  it off for normal use.

## Settings panel

The native settings panel (`pipeasio-settings`, C++/Qt6 Widgets) is built from the
`gui` subdirectory and installed to `bin`, together with a desktop entry and icon,
so it also appears in the application menu as **PipeASIO Settings**. It is built
only when Qt6 Widgets is found at configure time - otherwise configure warns,
the driver still builds, and no panel binary is produced. It runs on
your Linux host. The in-app ASIO control-panel button shows a message pointing
here, because the Qt panel cannot run inside the Wine/Proton container the host
loads the driver into.

## Troubleshooting

**Configure fails with `libpipewire-0.3 not found` or `Wine SDK headers not found`.** The development packages are missing or named differently on your distribution - see [Building](#building) for the per-distro sets. Fedora has `pipewire-devel` and `wine-devel`, not `libpipewire-0.3-dev` or `winehq-*-dev`.

**Configure fails with `Package 'libpipewire-0.3' has version 'X', required version is '>= 1.4.2'`.** Your distribution's PipeWire predates the minimum. Ubuntu 24.04 LTS (1.0.5) is the case that actually hits people; Ubuntu 25.10 (1.4.7), Ubuntu 26.04 LTS (1.6.2), Debian 13 (1.4.2), Fedora 43 (1.4.8), Fedora 44 (1.6.2) and Arch are all above the floor. There is no workaround in the driver: `src/audio.c` uses `spa_json_str_object_find()`, added in PipeWire 1.4.0, so an older PipeWire could never build - it only used to fail later with `implicit declaration of function`. Upgrade the distribution, or build PipeWire 1.4.2+ yourself and point `PKG_CONFIG_PATH` at it. The build then links that copy by absolute path rather than by `-l` name, so it cannot silently fall back to the system library - but the runtime loader still has to find it, via `LD_LIBRARY_PATH` or an `ldconfig` entry.

**Build fails with `wine/debug.h` or `unixlib.h`: `No such file or directory`.** Configure picked an SDK that does not hold the headers. For Wine outside `/usr`, install its SDK companion (`wine-devel-devel` on Fedora, `wine-devel-dev` on Debian/Ubuntu, matching your branch) and put that prefix's `bin/` on `PATH`. A manual `-DWINE_INCLUDE_DIRS` must list the include directories, not the install root, and needs all three: `/opt/wine-devel/include;/opt/wine-devel/include/wine;/opt/wine-devel/include/wine/windows`. The `wine/` one carries `unixlib.h`, used only by the 32-bit WoW64 build.

**`... holds no Wine SDK, so ... is used instead`.** `winebuild` came from a private prefix with no headers beside it, so another Wine's SDK was used. Install the matching `-devel`/`-dev` companion package, or the driver is compiled against a different Wine version than it runs on.

**`Qt6 Widgets not found - skipping the settings panel`.** The driver still builds, but `pipeasio-settings` does not, so there is no panel to open. Install Qt6 Widgets (`qt6-base` on Arch, `qt6-qtbase-devel` on Fedora, `qt6-base-dev` on Debian/Ubuntu), then re-run configure, rebuild and re-install. Run it from a host terminal or the **PipeASIO Settings** menu entry; your DAW's ASIO control-panel button never opens it, with or without Qt, and only shows a message pointing here. Pass `-DBUILD_SETTINGS_PANEL=OFF` if you want no panel.

**No sound, or the driver does not load under Proton.** Proton's container cannot see `/usr/lib/wine`. Install under `$HOME` and set `WINEDLLPATH` in the game's launch options (Steam: `WINEDLLPATH=/home/<you>/.local/lib/wine %command%`; Faugus: the same variable in the per-game environment field), then register in that prefix.

**A 32-bit game or host does not list PipeASIO under Proton.** Proton runs 32-bit apps through classic split WoW64 by default, which cannot load the 32-bit front end. In Steam, add `PROTON_USE_WOW64=1` alongside `WINEDLLPATH` in the game's launch options (`PROTON_USE_WOW64=1 WINEDLLPATH=/home/<you>/.local/lib/wine %command%`). In Faugus, enable the WoW64 option in the game's settings. The install must also include the 32-bit half (`-DBUILD_WOW64_32=ON`).

**Registering fails with status `c0000135`.** Wine could not find the unified PE name. The install creates `pipeasio.dll` symlinks next to `pipeasio64.dll` for Wine 10+, so re-run `cmake --install` to create them, then register again.

**Bluetooth headphones produce no sound.** Turn on `follow_device_clock` (or set `PIPEASIO_FOLLOW_DEVICE_CLOCK=on`). A Bluetooth sink's clock is the radio link and cannot be slaved to the host, so the driver follows it instead.

**Does it conflict with WineASIO?** No. PipeASIO has its own CLSID and registry identity, so it installs side by side with WineASIO and hosts list them as separate drivers.

**How do I select it in my DAW?** After registering, pick PipeASIO from the host's ASIO device list. In FL Studio that is Options > Audio settings > Device.

## Uninstalling

Unregister from each Wine prefix you registered, then remove the files:

```sh
# Unregister (set WINEDLLPATH the same way pipeasio-register does)
env WINEDLLPATH="$HOME/.local/lib/wine" wine regsvr32 /u pipeasio64.dll
rm -f "$WINEPREFIX/drive_c/windows/system32/pipeasio64.dll"

# Remove the installed files (CMake records them at install time)
xargs rm -f < build/install_manifest.txt
```

## Development

Recommended VS Code extensions are listed in `.vscode/extensions.json`. The build
emits `build/compile_commands.json` for clangd. The in-tree `.clang-format` and
`.editorconfig` keep diffs clean.

### Technical background

PipeASIO is a fork of [WineASIO](https://github.com/wineasio/wineasio) that
talks to PipeWire directly through `libpipewire-0.3`, with no `libjack.so.0`
runtime dependency. The fork exists because the Steam Runtime `steamrt4`
container that Proton uses ships `libpipewire-0.3` but not `libjack.so.0`,
which makes upstream WineASIO crash on `dlopen`.

The driver has its own COM identity: CLSID
`{2D3CA9E2-1193-4C5D-B5FD-38798F3DC074}`, ASIO registration under
`HKCU\Software\ASIO\PipeASIO`, and DLL filename `pipeasio64.dll`. That is why
it coexists with WineASIO: neither overrides the other.

### Testing

`ctest --test-dir build` runs the Linux-native unit and contract tests plus the
Wine integration tests below (they SKIP without wine, `pw-cli`, a running
PipeWire daemon, and an installed driver). The two Wine-based tools can also be
run directly against a live PipeWire session:

```sh
./build/tests/asio_probe/run.sh        # COM lifecycle + bufferSwitch cycle rate
./build/tests/asio_loopback/run.sh     # digital loopback analyzer
SWEEP=1 ./build/tests/asio_loopback/run.sh   # buffer-size x sample-rate matrix
```

The loopback analyzer plays a per-channel frame counter out of the driver's
output ports and verifies it on the input ports after a PipeWire null-sink
loopback. Because PipeASIO is float32 end-to-end, the round trip must be
**bit-exact**. The tool fails on any corrupted sample, dropped or duplicated
buffer, swapped channel, or a measured round-trip latency that disagrees with
`GetLatencies()` by more than one buffer. `SWEEP=1` repeats this across buffer
sizes 128-1024 and forced sample rates 44.1/48/96 kHz, re-negotiating buffers
in-process the same way a DAW does when you change the buffer size.

When built with `-DBUILD_WOW64_32=ON`, a 32-bit analogue of the probe exercises
the full COM + real-time round trip on a 32-bit (WoW64) host:

```sh
./build/tests/asio_probe/run32.sh      # 32-bit WoW64 driver (needs MinGW + a 32-bit prefix)
```

Cross-distro builds (needs [distrobox](https://distrobox.it/) plus podman or
docker) exercise the same Release configure against each distro's injected
package-build flags - the path that broke on Fedora's `-flto=auto` before
`#6` was fixed:

```sh
./tests/distro/run.sh                  # fedora + ubuntu + arch
DISTROS="fedora ubuntu" ./tests/distro/run.sh
FRESH=1 ./tests/distro/run.sh          # recreate containers first
./tests/distro/run.sh --clean          # remove harness containers
```

The script SKIPs (exit 77) when distrobox or a container backend is missing.
CI runs the Fedora and Ubuntu legs on every push/PR via the `build-distros`
matrix job.

If you package PipeASIO, consider installing the `pipeasio-register` helper script
as part of the package.

## Acknowledgements

PipeASIO builds on [WineASIO](https://github.com/wineasio/wineasio) and the work of its authors: Robert Reif, Ralf Beck, Johnny Petrantoni, Stephane Letz, William Steidtmann, Peter L Jones, Torben Hohn, Nedko Arnaudov, Christian Schoenebeck, Joakim Hernberg, and Filipe Coelho.

## License

PipeASIO is licensed under the GNU General Public License, version 3 or later
(`GPL-3.0-or-later`). See [`COPYING`](COPYING). It is a fork of WineASIO and
retains the original authors' copyright notices, and every source file carries
an `SPDX-License-Identifier` tag.

## Changelog

See [`CHANGELOG.md`](CHANGELOG.md).
