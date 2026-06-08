<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/logo.svg">
    <img alt="PipeASIO" src="docs/logo-light.svg" width="300">
  </picture>
</p>

<p align="center">
  <a href="https://m0n7y5.github.io/pipeasio/"><b>Website &amp; docs</b></a>
</p>

PipeASIO is an ASIO driver for Wine that talks to PipeWire directly, with no
`libjack.so.0` runtime dependency.

It is a fork of [WineASIO](https://github.com/wineasio/wineasio), created so the
driver loads cleanly inside the Steam Runtime `steamrt4` container that FL Studio
runs in under Faugus / Proton-CachyOS. That container ships `libpipewire-0.3` but
not `libjack.so.0`, which makes upstream WineASIO crash on `dlopen`.

PipeASIO has its own COM identity: CLSID `{2D3CA9E2-1193-4C5D-B5FD-38798F3DC074}`,
ASIO registration under `HKCU\Software\ASIO\PipeASIO`, and DLL filename
`pipeasio64.dll`. Installing it alongside WineASIO is safe; neither overrides the
other, and hosts such as FL Studio see them as separate ASIO drivers.

ASIO is the most common low-latency audio driver on Windows, used by workstations
such as FL Studio, Ableton Live, and Reaper.

![PipeASIO settings panel](docs/panel-settings.png)

## Building

CMake only, x86_64 only.

Requirements: `cmake` (3.20 or newer), `ninja-build` (recommended) or GNU make,
`gcc`, the Wine SDK (`wine-devel` / `winehq-stable-dev`), `pkg-config`, and
`libpipewire-0.3-dev`. The optional Qt6 settings panel also needs a C++ compiler
and `qt6-base-dev`. The panel builds by default when those are present and is
skipped otherwise; pass `-DBUILD_SETTINGS_PANEL=OFF` to force it off.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Debug build (assertions and Wine debug-channel macros):

```sh
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

## Installing

System-wide (matches the distro Wine layout most ASIO hosts use):

```sh
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

Wine library directories vary across distros. Adjust `--prefix` or override
`CMAKE_INSTALL_LIBDIR` if yours is non-standard.

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
`$PIPEASIO_PREFIX/lib/wine`, then `$HOME/.local/lib/wine`, then `/usr/lib/wine`,
then distro variants, then `/opt/wine-{devel,stable,staging}`. Set
`PIPEASIO_PREFIX` to point it at a non-standard install.

## Proton / Steam / Faugus

Proton runs Wine inside a pressure-vessel container (steamrt4). The container does
not expose the host's `/usr/lib/wine/`, so a system-wide install is invisible to
Proton's Wine. Two things make PipeASIO work inside Proton:

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

   In Faugus-launcher, put it in the per-game "Launch options" environment field:

   ```
   WINEDLLPATH=/home/<you>/.local/lib/wine
   ```

   Use the absolute path; Faugus does not expand `~` or `$HOME` in that field.

Then register PipeASIO in the Proton wineprefix as usual:

```sh
env WINEPREFIX="$HOME/Faugus/<game>" pipeasio-register
```

The PE stub lands in `<prefix>/drive_c/windows/system32/` and the CLSID
registration persists in the prefix registry, both shared across Wine versions.

## Configuration

PipeASIO talks to PipeWire 1.6+ natively through `libpipewire-0.3`. The graph
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
`0` (default) follows the PipeWire graph rate; a non-zero value pins the rate with
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
clock is the radio link and cannot be slaved to the host; otherwise PipeWire
silently drops the links and you get no sound. The buffer size is then dictated by
the device (the driver settles to the device's quantum after one automatic reset),
so latency is higher.
Env: `PIPEASIO_FOLLOW_DEVICE_CLOCK` (`on`/`off`).

### buffer_size
The preferred size returned by `GetBufferSize()`. Must be a power of two within
[16, 8192]; out-of-range values fall back to 1024.
Env: `PIPEASIO_PREFERRED_BUFFERSIZE`.

A size the hardware does not support makes PipeWire reject the request or insert
resampling, and either way you may get xruns.

### node_name
Overrides the PipeWire client/node name (otherwise derived from the host program
name).
Env: `PIPEASIO_CLIENT_NAME`.

## Performance

A few knobs affect xrun-free, low-latency operation:

- Channel count. Every input and output is a PipeWire port the graph must
  schedule. Defaults are 2 in / 2 out; raise `inputs` / `outputs` only to what you
  route. Fewer ports mean a smaller graph and less overhead.
- Buffer size. Smaller buffers cut latency but raise CPU and xrun risk.
- Debug logging. `PIPEASIO_DEBUG=1` makes the driver log on the audio path; leave
  it off for normal use.

## Settings panel

The native settings panel (`pipeasio-settings`, C++/Qt6 Widgets) is built from the
`gui` subdirectory and installed to `bin`. Run it from a terminal on your Linux
host. The in-app ASIO control-panel button shows a message pointing here, because
the Qt panel cannot run inside the Wine/Proton container the host loads the driver
into.

## Development

Recommended VS Code extensions are listed in `.vscode/extensions.json`. The build
emits `build/compile_commands.json` for clangd; the in-tree `.clang-format` and
`.editorconfig` keep diffs clean.

If you package PipeASIO, consider installing the `pipeasio-register` helper script
as part of the package.

## License

The driver (`src/` and `include/`) is licensed under LGPL v2.1 or later; the
settings panel (`gui/`) under GPL v2 or later. See [`COPYING.LIB`](COPYING.LIB) and
[`COPYING.GUI`](COPYING.GUI). PipeASIO is a fork of WineASIO, and source files
retain their `SPDX-License-Identifier` tags.

## Changelog

See [`CHANGELOG.md`](CHANGELOG.md).
