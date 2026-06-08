# PipeASIO

PipeASIO is an ASIO driver for Wine that talks to PipeWire directly — no `libjack.so.0` runtime dependency.

It's a fork of [WineASIO](https://github.com/wineasio/wineasio), created so the driver loads cleanly inside the Steam Runtime `steamrt4` container that FL Studio runs in under Faugus / Proton-CachyOS — that container ships `libpipewire-0.3` but not `libjack.so.0`, which makes upstream WineASIO SEGV on `dlopen`.

PipeASIO has its own distinct COM identity — CLSID `{2D3CA9E2-1193-4C5D-B5FD-38798F3DC074}`, ASIO registration under `HKCU\Software\ASIO\PipeASIO`, DLL filename `pipeasio64.dll`. Installing PipeASIO alongside WineASIO is safe; neither overrides the other, and hosts (FL Studio etc.) see them as separate ASIO drivers.

ASIO is the most common Windows low-latency driver, so is commonly used in audio workstation programs like FL Studio, Ableton Live, and Reaper.

![PipeASIO settings panel](docs/panel-settings.png)

### BUILDING

This fork uses CMake. x86_64 only.

Build requirements: `cmake` (≥ 3.20), `ninja-build` (recommended) or GNU make,
`gcc`, Wine SDK (`wine-devel` / `winehq-stable-dev`), `pkg-config`, and
`libpipewire-0.3-dev`.  The optional Qt6 settings panel additionally needs a
C++ compiler and `qt6-base-dev` (Qt6 Widgets); it builds by default when those
are present and is skipped otherwise (`-DBUILD_SETTINGS_PANEL=OFF` to force off).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Debug build with assertions and Wine debug channel macros:

```sh
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

### INSTALLING

System-wide (matches the distro Wine layout most ASIO hosts use):

```sh
sudo cmake --install build --prefix /usr
```

User-local (no `sudo`, sandboxed under `$HOME` — required for Proton /
Faugus / Steam, see below):

```sh
cmake --install build --prefix "$HOME/.local"
```

Either lays down:

```
<prefix>/lib/wine/x86_64-windows/pipeasio64.dll
<prefix>/lib/wine/x86_64-windows/pipeasio.dll    -> pipeasio64.dll
<prefix>/lib/wine/x86_64-unix/pipeasio64.dll.so
<prefix>/lib/wine/x86_64-unix/pipeasio.dll.so    -> pipeasio64.dll.so
```

The `pipeasio.dll{,.so}` symlinks satisfy the unified PE name that Wine 10+
expects; without them `regsvr32 pipeasio64.dll` fails with status `c0000135`
on newer Wine.

**NOTE:** Wine library directories vary across distros — adjust `--prefix`
or override `CMAKE_INSTALL_LIBDIR` if your distro is non-standard.

### DEVELOPMENT (VS Code)

Recommended extensions are listed in `.vscode/extensions.json`; VS Code
prompts on first open. The build emits `build/compile_commands.json` for
clangd; the in-tree `.clang-format` and `.editorconfig` keep diffs clean.

#### EXTRAS

For user convenience a `pipeasio-register` script is included in this repo, if you are packaging PipeASIO consider installing it as part of PipeASIO.

Additionally a native settings panel (`pipeasio-settings`, C++/Qt6 Widgets) is
built from this repository's `gui` subdir and installed to `bin`.  Run it from a
terminal on your Linux host; the in-app ASIO "control panel" button shows a
message pointing here, because the Qt panel cannot run inside the Wine/Proton
container the host loads the driver into.  It needs Qt6 Widgets at build time;
pass `-DBUILD_SETTINGS_PANEL=OFF` to skip it.

### REGISTERING

After building and installing PipeASIO, we still need to register it on each Wine prefix.  
For your convenience a script is provided on this repository, so you can simply run:

```sh
pipeasio-register
```

to activate PipeASIO for the current Wine prefix.

#### CUSTOM WINEPREFIX

The `pipeasio-register` script will register the PipeASIO driver in the default Wine prefix `~/.wine`.  
You can specify another prefix like so:

```sh
env WINEPREFIX="$HOME/asioapp" pipeasio-register
```

`pipeasio-register` searches for the install root in this order:
`$PIPEASIO_PREFIX/lib/wine` → `$HOME/.local/lib/wine` → `/usr/lib/wine`
→ distro variants → `/opt/wine-{devel,stable,staging}`. Set
`PIPEASIO_PREFIX` to point it at a non-standard prefix.

#### PROTON / STEAM / FAUGUS

Proton runs Wine inside a pressure-vessel container (steamrt4). The
container does **not** expose the host's `/usr/lib/wine/`, so a
system-wide install is invisible to Proton's Wine. Two things make
PipeASIO work inside Proton:

1. Install PipeASIO under `$HOME` (pressure-vessel exposes the user's
   home directory by default):

   ```sh
   cmake --install build --prefix "$HOME/.local"
   ```

2. Set `WINEDLLPATH=$HOME/.local/lib/wine` in the launcher's per-game
   environment so Proton's Wine finds the ELF `.so` half. Proton has
   honored a host-provided `WINEDLLPATH` since
   [PR #9420](https://github.com/ValveSoftware/Proton/pull/9420);
   Proton-CachyOS ships this fix.

   In **Faugus-launcher**, put it in the per-game "Launch options"
   environment field:

   ```
   WINEDLLPATH=/home/<you>/.local/lib/wine
   ```

   (Use the absolute path — Faugus doesn't expand `~` or `$HOME` in
   that field.)

Then register PipeASIO in the Proton wineprefix as usual:

```sh
env WINEPREFIX="$HOME/Faugus/<game>" pipeasio-register
```

The PE stub lands in `<prefix>/drive_c/windows/system32/` and the
CLSID registration persists in the wineprefix's registry, both shared
across Wine versions.

### GENERAL INFORMATION

PipeASIO talks to PipeWire 1.6+ natively via `libpipewire-0.3`. The graph
quantum is locked to the ASIO host's negotiated buffer size via
`PW_KEY_NODE_FORCE_QUANTUM` (unless `follow_device_clock` is set, in which case
the target device drives the cycle — see below); the sample rate follows the
graph unless pinned (see `sample_rate`), in which case `PW_KEY_NODE_FORCE_RATE`
is set.

PipeASIO is configured by a flat INI file at
`$XDG_CONFIG_HOME/pipeasio/config.ini` (fallback `~/.config/pipeasio/config.ini`).
The driver reads it natively at startup and re-reads it while running, so saving in the settings panel applies changes within about a second without reselecting the driver or restarting the host.  Every
option can also be overridden by an environment variable.  A missing file means
built-in defaults, and unknown keys are ignored.  The file has a single
`[pipeasio]` section:

#### inputs / outputs
Number of PipeWire DSP ports PipeASIO opens.  Defaults 2 / 2.
Env: `PIPEASIO_NUMBER_INPUTS`, `PIPEASIO_NUMBER_OUTPUTS`.

#### auto_connect
Defaults to 1: PipeASIO connects its channels to a hardware device on start.
Set to 0 to leave the node unconnected and patch it yourself in a PipeWire
patchbay.  Env: `PIPEASIO_CONNECT_TO_HARDWARE` (`on`/`off`).

#### output_device / input_device
The PipeWire `node.name` of the sink (output) and source (input) to
auto-connect to.  Empty (the default) follows the PipeWire default sink/source,
re-resolved each time the driver (re)connects.
Env: `PIPEASIO_OUTPUT_DEVICE`, `PIPEASIO_INPUT_DEVICE`.

#### sample_rate
`0` (default) follows the PipeWire graph rate; a non-zero value pins the rate
via `PW_KEY_NODE_FORCE_RATE`.  Env: `PIPEASIO_SAMPLE_RATE`.

#### fixed_buffer_size
Defaults to 1: the buffer size is controlled by PipeWire and the ASIO host
cannot change it.  Set to 0 to let the host change PipeWire's quantum (via
`PW_KEY_NODE_FORCE_QUANTUM`) in `CreateBuffers()`.
Env: `PIPEASIO_FIXED_BUFFERSIZE` (`on`/`off`).

#### follow_device_clock
Defaults to 0 (off): the driver pins the PipeWire graph quantum
(`PW_KEY_NODE_FORCE_QUANTUM`) to the host's buffer size and runs as its own
low-latency clock master — correct for wired devices.  Set to 1 to make the
driver a *follower* instead: it drops `FORCE_QUANTUM` so the target device
drives the cycle.  This is required for Bluetooth sinks, whose clock is the
radio link and cannot be slaved to the host — otherwise PipeWire silently drops
the links and you get no sound.  The buffer size is then dictated by the device
(the driver settles to the device's quantum after one automatic reset), so
latency is higher.  Env: `PIPEASIO_FOLLOW_DEVICE_CLOCK` (`on`/`off`).

#### buffer_size
The preferred size returned by `GetBufferSize()` (see the ASIO docs).  Must be
a power of two within [16, 8192] (`PIPEASIO_MINIMUM_BUFFERSIZE` /
`PIPEASIO_MAXIMUM_BUFFERSIZE` in the source); out-of-range values fall back to
1024.  Env: `PIPEASIO_PREFERRED_BUFFERSIZE`.

Be careful: a size the hardware doesn't support makes PipeWire reject the
request or insert resampling — either way you may get xruns.

#### node_name
Overrides the PipeWire client/node name (otherwise derived from the host
program name).  Env: `PIPEASIO_CLIENT_NAME`.

### PERFORMANCE

A few knobs affect xrun-free, low-latency operation:

- **Channel count.** Every input/output is a PipeWire port the graph must
  schedule. Defaults are 2 in / 2 out; raise `inputs` / `outputs` (settings panel
  or `PIPEASIO_NUMBER_INPUTS` / `PIPEASIO_NUMBER_OUTPUTS`) only to what you route —
  fewer ports mean a smaller graph and less overhead.
- **Buffer size.** Smaller buffers cut latency but raise CPU and xrun risk (see
  `buffer_size` above).
- **Debug logging.** `PIPEASIO_DEBUG=1` makes the driver log on the audio path;
  leave it off for normal use.

### LICENSE

PipeASIO is a fork of [WineASIO](https://github.com/wineasio/wineasio).

The driver (the `pipeasio64.dll` library — everything under `src/` and
`include/`) is licensed under the **GNU Lesser General Public License v2.1 or
later**; see [`COPYING.LIB`](COPYING.LIB).  The original WineASIO copyright
notices are preserved.

The settings panel (`pipeasio-settings` — everything under `gui/`) is a separate
program licensed under the **GNU General Public License v2 or later**; see
[`COPYING.GUI`](COPYING.GUI).

Every source file carries an `SPDX-License-Identifier:` tag naming the license
that applies to it.

### CHANGE LOG

#### 1.0.0-rc1
* 08-JUN-2026: Flush subnormal floats to zero (FTZ/DAZ) on the audio thread, avoiding rare multi-hundred-cycle CPU stalls — and the dropouts they cause — when the host's DSP produces denormals
* 08-JUN-2026: Derive the ASIO host timestamp (`systemTime`) from the PipeWire graph clock instead of the system tick count — a monotonic, audio-domain clock, and one fewer call on the audio path
* 08-JUN-2026: Lower the default channel count to 2 in / 2 out (was 16 / 16) so a fresh install opens a smaller PipeWire graph; raise inputs/outputs in the settings panel as needed
* 08-JUN-2026: Fix a potential crash (use-after-free / heap corruption) when a PipeWire device connects or disconnects while the driver is starting or reconnecting — the device-discovery caches are now locked against the registry thread, and returned port-name lists are copied so they can't be freed underfoot
* 08-JUN-2026: Fix possibly garbled or out-of-bounds output when following a device clock (e.g. Bluetooth) or when PipeWire clamps the forced quantum — the driver no longer publishes more audio per cycle than it actually produced
* 08-JUN-2026: Fix a memory leak when the audio backend fails to start during buffer setup, and free the discovered-port lists on the driver-init error paths
* 08-JUN-2026: The settings panel no longer briefly freezes on every Monitor refresh — `pw-top` and `pw-dump` now run asynchronously instead of blocking the UI thread
* 08-JUN-2026: The panel keeps a saved output/input device or sample rate that isn't currently available (shown as "(unavailable)") instead of silently resetting it on Apply
* 08-JUN-2026: Harden channel-count limits (clamp `inputs`/`outputs` from both the INI and the `PIPEASIO_NUMBER_*` environment overrides) and tighten COM teardown plus several NULL/error paths
* 08-JUN-2026: The Monitor tab's DSP load is now a rolling history graph (green / amber / red bars by level, current % shown, frozen and dimmed when idle) instead of a single bar, so you can see load trends and spikes over time
* 08-JUN-2026: Fix the Monitor tab never showing live values — it failed to recognise the driver's own PipeWire node (the `pipeasio.node` marker is serialised by `pw-dump` as a JSON number, not a string), then read `pw-top`'s all-zero baseline sample instead of the measured iteration and choked on locale comma decimals. Quantum / sample rate / DSP load / xruns / state now populate while audio plays
* 08-JUN-2026: Add explanatory tooltips to every Settings and Monitor field
* 07-JUN-2026: Add a "Follow device clock" option (`follow_device_clock`, off by default): makes the driver follow the target device's clock instead of forcing the graph quantum, so output to a Bluetooth sink (whose clock can't be slaved) works — at the cost of the device-dictated buffer size and latency
* 07-JUN-2026: "Follow default" output/input now connects to the *actual* PipeWire default sink/source (read from the "default" metadata) instead of the first device discovered — fixes audio going to the wrong device when the system default is e.g. Bluetooth headphones
* 07-JUN-2026: The settings panel's "OK" button is now "Apply": it saves without closing the window, so you can adjust settings and hear each change take effect live
* 07-JUN-2026: Settings saved in the panel now apply to a running driver automatically — the driver watches `config.ini` and asks the host to re-initialize on change (no more reselect/restart), and the panel writes the file atomically so a half-written config is never read
* 07-JUN-2026: Fix slow / pitched-down playback at any buffer size other than the backend default — `CreateBuffers()` now always syncs the negotiated size to the PipeWire quantum
* 07-JUN-2026: The in-app ASIO "control panel" button now shows a message directing you to run `pipeasio-settings` on the host (the Qt panel can't run inside the Wine/Proton container) instead of silently failing
* 07-JUN-2026: Monitor tab auto-discovers the driver's PipeWire node (the host names it after its own executable), via a `pipeasio.node` marker
* 07-JUN-2026: New native C++/Qt6 settings panel (replaces the PyQt GUI), with Settings and live Monitor (PipeWire DSP load / xruns / quantum) tabs
* 07-JUN-2026: Move configuration from the Windows registry to a flat INI at `$XDG_CONFIG_HOME/pipeasio/config.ini`
* 07-JUN-2026: Add PipeWire output/input device selection (`output_device` / `input_device`), honored by autoconnect
* 07-JUN-2026: Add `sample_rate` setting (0 = follow the graph, else `FORCE_RATE`)
* 07-JUN-2026: Drop the dead "Autostart server" option

#### 1.3.0
* 24-JUL-2025: Make GUI settings panel compatible with PyQt6 or PyQt5
* 17-JUL-2025: Load libjack.so.0 dynamically at runtime, removing build dep
* 17-JUL-2025: Remove useless -mnocygwin flag
* 28-JUN-2025: Remove dependency on asio headers

#### 1.2.0
* 29-SEP-2023: Fix compatibility with Wine > 8
* 29-SEP-2023: Add pipeasio-register script for simplifying driver registration

#### 1.1.0
* 18-FEB-2022: Various bug fixes (falkTX)
* 24-NOV-2021: Fix compatibility with Wine > 6.5

#### 1.0.0
* 14-JUL-2020: Add packaging script
* 12-MAR-2020: Fix control panel startup
* 08-FEB-2020: Fix code to work with latest Wine
* 08-FEB-2020: Add custom GUI for PipeASIO settings, made in PyQt5 (taken from Cadence project code)

#### 0.9.2
* 28-OCT-2013: Add 64-bit support and some small fixes

#### 0.9.1
* 15-OCT-2013: Various bug fixes (JH)

#### 0.9.0
* 19-FEB-2011: Nearly complete refactoring of the PipeASIO codebase (asio.c) (JH)

#### 0.8.1
* 05-OCT-2010: Code from Win32 callback thread moved to JACK process callback, except for bufferSwitch() call.
* 05-OCT-2010: Switch from int to float for samples.

#### 0.8.0
* 08-AUG-2010: Forward port JackWASIO changes... needs testing hard. (PLJ)

#### 0.7.6
* 27-DEC-2009: Fixes for compilation on 64-bit platform. (PLJ)

#### 0.7.5
* 29-Oct-2009: Added fork with call to qjackctl from ASIOControlPanel(). (JH)
* 29-Oct-2009: Changed the SCHED_FIFO priority of the win32 callback thread. (JH)
* 28-Oct-2009: Fixed wrongly reported output latency. (JH)

#### 0.7.4
* 08-APR-2008: Updates to the README.TXT (PLJ)
* 02-APR-2008: Move update to "toggle" to hopefully better place (PLJ)
* 24-MCH-2008: Don't trace in win32_callback.  Set patch-level to 4. (PLJ)
* 09-JAN-2008: Nedko Arnaudov supplied a fix for Nuendo under WINE.

#### 0.7.3
* 27-DEC-2007: Make slaving to jack transport work, correct port allocation bug. (RB)

#### 0.7
* 01-DEC-2007: In a fit of insanity, I merged JackLab and Robert Reif code bases. (PLJ)

#### 0.6
* 21-NOV-2007: add dynamic client naming (PLJ)

#### 0.0.3
* 17-NOV-2007: Unique port name code (RR)

#### 0.5
* 03-SEP-2007: port mapping and config file (PLJ)

#### 0.3
* 30-APR-2007: corrected connection of in/outputs (RB)

#### 0.1
* ???????????: Initial RB release (RB)

#### 0.0.2
* 12-SEP-2006: Fix thread bug, tidy up code (RR)

#### 0.0.1
* 31-AUG-2006: Initial version (RR)

### LEGAL STUFF

Copyright (C) 2006 Robert Reif  
Portions copyright (C) 2007 Ralf Beck  
Portions copyright (C) 2007 Johnny Petrantoni  
Portions copyright (C) 2007 Stephane Letz  
Portions copyright (C) 2008 William Steidtmann  
Portions copyright (C) 2010 Peter L Jones  
Portions copyright (C) 2010 Torben Hohn  
Portions copyright (C) 2010 Nedko Arnaudov  
Portions copyright (C) 2011 Christian Schoenebeck  
Portions copyright (C) 2013 Joakim Hernberg  
Portions copyright (C) 2020-2023 Filipe Coelho  

The PipeASIO library code is licensed under LGPL v2.1, see COPYING.LIB for more details.  
The PipeASIO settings UI code is licensed under GPL v2+, see COPYING.GUI for more details.  
