# Changelog

All notable changes to PipeASIO are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims to
follow [Semantic Versioning](https://semver.org/).

## [1.0.0] - 2026-06-10

### Added

- CI on GitHub Actions: every push and pull request builds the driver and
  panel and runs the Linux-native test suite on Arch Linux.
- The integration probe now verifies that the sample position advances during
  the run and that the timecode `Future` selectors are denied.
- `tests/asio_loopback`: a digital loopback analyzer (RTL-Utility/RMAA
  equivalent for a converter-less driver). It plays a per-channel frame
  counter through the driver and a PipeWire null-sink loopback and fails on
  any non-bit-exact sample, dropped or duplicated buffer, swapped channel, or
  measured round-trip latency disagreeing with `GetLatencies()`; `SWEEP=1`
  covers buffer sizes 128–1024 at 44.1/48/96 kHz with in-process buffer
  re-negotiation.

### Changed

- Relicensed the entire project under GPL-3.0-or-later, replacing the previous
  split of LGPL-2.1-or-later (driver) and GPL-2.0-or-later (settings panel). The
  separate `COPYING.LIB` / `COPYING.GUI` files are now a single `COPYING` (the
  GPLv3 text). The original WineASIO authors' copyright notices are retained; the
  relicensing uses the "or later" upgrade path (and LGPL 2.1 section 3) that
  those licenses already grant.

### Removed

- Fake ASIO timecode support. The driver no longer answers `kAsioCanTimeCode`
  / `kAsioEnableTimeCodeRead` affirmatively or fills `ASIOTime.timeCode` with
  fabricated values — PipeWire has no transport timeline to source timecode
  from. Hosts fall back to sample-position sync, which is accurate.

### Fixed

- `GetSamplePosition` returned only the low 32 bits of the sample counter,
  wrapping to zero after about 25 hours at 48 kHz; it now reports the full
  64-bit position.
- `regsvr32 /u` now actually removes the driver's registry keys. Unregistration
  deleted keys through a handle opened without `DELETE` access, so every delete
  failed and the CLSID and `Software\ASIO\PipeASIO` entries were left behind;
  the recursive delete is now `RegDeleteTree`, and unregistering an already
  unregistered driver succeeds.
- Driver registration and unregistration now report real failures: raw win32
  error codes were previously returned where COM HRESULTs are expected, so
  errors like access-denied counted as success.
- The Wine test hosts (`asio_probe`, `asio_loopback`) parse their command line
  themselves: current Wine's CRT startup delivers `argc=0` to `main()`, so the
  probe's seconds argument was silently ignored.

## [1.0.0-rc1] - 2026-06-08

First PipeASIO release. Forked from WineASIO and reworked to talk to PipeWire
directly through `libpipewire-0.3`, with no `libjack.so.0` runtime dependency, so
the driver loads inside the Steam Runtime container that Proton uses.

### Added

- Native C++/Qt6 settings panel (`pipeasio-settings`) with a Settings tab and a
  live Monitor tab, replacing the old PyQt GUI.
- Monitor tab showing live PipeWire quantum, sample rate, DSP load, xruns, and
  state, auto-discovering the driver's own PipeWire node.
- DSP load drawn as a rolling history graph (color coded by level, current value
  shown, dimmed when idle) instead of a single bar.
- "Follow device clock" option (`follow_device_clock`) so output to a Bluetooth
  sink works, where the sink's clock cannot be slaved to the host.
- PipeWire sink and source selection (`output_device` / `input_device`), honored
  by autoconnect; an empty value follows the PipeWire default.
- `sample_rate` setting: `0` follows the graph rate, a non-zero value pins it.
- Tooltips on every Settings and Monitor field.
- Subnormal float flushing (FTZ/DAZ) on the audio thread to avoid rare CPU stalls
  and the dropouts they cause.
- ASIO host timestamp derived from the PipeWire graph clock rather than the
  system tick count.

### Changed

- Configuration moved from the Windows registry to a flat INI file at
  `$XDG_CONFIG_HOME/pipeasio/config.ini`. The driver re-reads it while running, so
  saving in the panel applies within about a second with no host restart, and the
  file is written atomically.
- Default channel count is now 2 in / 2 out (was 16 / 16) for a smaller default
  graph; raise it in the panel as needed.
- "Follow default" connects to the actual PipeWire default sink and source read
  from the default metadata, rather than the first device discovered.
- The panel's confirm button is now "Apply": it saves without closing, so each
  change can be heard live.
- The panel keeps a saved device or sample rate that is currently unavailable,
  marked "(unavailable)", instead of resetting it on Apply.
- The in-app ASIO control-panel button now points you to run `pipeasio-settings`
  on the host, since the Qt panel cannot run inside the Wine/Proton container.
- Removed the obsolete "Autostart server" option.

### Fixed

- Crash (use-after-free and heap corruption) when a PipeWire device connects or
  disconnects while the driver is starting or reconnecting. Device-discovery
  caches are now locked against the registry thread, and port-name lists are
  copied before use.
- Garbled or out-of-bounds output when following a device clock or when PipeWire
  clamps the forced quantum; the driver no longer publishes more audio per cycle
  than it produced.
- Slow or pitched-down playback at buffer sizes other than the backend default;
  `CreateBuffers()` now always syncs the negotiated size to the PipeWire quantum.
- Memory leak when the audio backend failed to start during buffer setup, and
  leaked discovered-port lists on the driver-init error paths.
- The settings panel no longer freezes on Monitor refresh; `pw-top` and `pw-dump`
  now run asynchronously off the UI thread.
- The Monitor tab now populates while audio plays. It previously failed to
  recognize the driver's node, read an all-zero baseline sample, and mishandled
  locale comma decimals.
- Hardened channel-count limits from both the INI and the environment overrides,
  and tightened COM teardown and several NULL and error paths.

[1.0.0]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.0.0
[1.0.0-rc1]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.0.0-rc1

---

PipeASIO is a fork of [WineASIO](https://github.com/wineasio/wineasio). For the
history before this project, see the WineASIO changelog.
