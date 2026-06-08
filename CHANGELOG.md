# Changelog

All notable changes to PipeASIO are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims to
follow [Semantic Versioning](https://semver.org/).

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

[1.0.0-rc1]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.0.0-rc1

---

PipeASIO is a fork of [WineASIO](https://github.com/wineasio/wineasio). For the
history before this project, see the WineASIO changelog.
