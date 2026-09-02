# Changelog

All notable changes to PipeASIO are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims to
follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed

- The settings panel names the scheduling mode. `follow_device_clock` has always
  set two properties at once (the target device drives the cycle, and the node
  turns asynchronous), but the checkbox read "Follow device clock (Bluetooth)"
  and its tooltip mentioned only the quantum, so the extra buffer period and the
  synchronous to asynchronous switch stayed invisible until `pw-top` showed the
  node as `=` instead of `+`. The checkbox is now "Follow device clock", the
  tooltip names both effects, and a read-only `Scheduling` row reads
  `synchronous` or `asynchronous (+1 period, 2.7 ms)`, tracking the checkbox and
  the buffer period live.

### Fixed

- The docs asserted that the driver "is scheduled synchronously" without
  qualification, which is wrong whenever `follow_device_clock` is on. The
  `realtime` section, the Performance list, the FL Studio troubleshooting entry
  and the website FAQ now attach that claim to the synchronous case only.
- The docs called `follow_device_clock` a requirement for Bluetooth sinks and
  promised silence without it. Some Bluetooth sinks accept a forced quantum and
  run synchronously, so the README, the panel tooltip and the website now say to
  try it both ways.

## [1.5.0] - 2026-08-06

### Added

- `GetLatencies()` now reports the connected device chain's own delay on top of
  the driver's buffer period, instead of the buffer period alone. The filter
  connects with `PW_FILTER_FLAG_CUSTOM_LATENCY`, which is the only way
  libpipewire hands a filter its peers' `SPA_PARAM_Latency` (without it
  `pw_filter` swallows the param and runs an inert default algorithm), and the
  values arrive per port and per direction. Measured against a wired card at
  1024/48000, input goes from 1024 to 2080 samples and output from 1024 to 2048,
  the extra 32 being the converter offset the driver used to discard. Hosts that
  compensate for latency were misaligning recorded tracks by the device delay.
- `kAsioLatenciesChanged` is relayed when that figure moves, so a host re-reads
  `GetLatencies()` without needing a full reset.
- The ASIO host can select a sample rate. With `sample_rate = 0` (follow the
  graph), `CanSampleRate()` offers 44100, 48000, 88200, 96000, 176400 and
  192000, and `SetSampleRate()` pins the graph to the host's choice through
  `PW_KEY_NODE_FORCE_RATE`, asking the host to reset when one is already
  running. Previously only the rate the graph happened to be on was accepted, so
  a host that demands a particular rate could not start at all; Rocksmith
  through RS_ASIO requires 48 kHz. An explicit `sample_rate` in the config still
  wins over the host, mirroring how `fixed_buffer_size` holds the buffer size.
- The driver counts and logs cycles it was too slow for
  (`xrun: missed the cycle deadline`), reporting the first and then every 64th
  so a storm cannot flood the log. `SPA_IO_CLOCK_FLAG_XRUN_RECOVER` cannot serve
  here, because PipeWire only sets it around the driver node's own
  `process_node` and the driver is deliberately a follower; the count comes from
  gaps in `clock.position` instead, ignoring quantum changes, timeline rebases
  and the idle cycles of a deliberate `Stop`.
- `Init()` logs the product version and which half of a WoW64 pair is talking
  (`PipeASIO 1.5.0 (64-bit)`), so a debug log identifies the build the host
  loaded. `PIPEASIO_VERSION` was documented as reaching the driver log but only
  ever reached the settings-panel title, which left bug reports with no version
  in them. It is a `TRACE`, so it follows `PIPEASIO_DEBUG` like the rest of the
  informational output.
- Documented that FL Studio's **Mix in buffer switch** (Options > Audio
  settings > Input / output) has to be off. It moves the whole mixer and plugin
  pass into the ASIO `bufferSwitch` callback, which the driver delivers on the
  PipeWire data loop with one buffer period to return; the pass overruns that,
  and a synchronously scheduled driver stalls the graph instead of absorbing
  it. Reported against `realtime` mode, where the same thread additionally
  carries `SCHED_FIFO` priority.

### Changed

- `node.lock-quantum` accompanies `node.force-quantum`, and `node.lock-rate`
  accompanies `node.force-rate`, so another client cannot resize the graph under
  a running host. Since 1.4.3 the driver is scheduled synchronously, so a
  quantum it did not ask for is a glitch it cannot absorb. Follow-device mode
  takes the device's quantum on purpose and locks neither.
- The smallest advertised and accepted buffer size is 32 frames, up from 16.
  Hosts mishandle smaller ASIO buffers (Max/MSP crashes on them, and FlexASIO
  enforces the same floor) and RS_ASIO rounds every request up to a multiple of
  32 anyway. A configured `buffer_size` of 16 now falls back to the 1024
  default.

## [1.4.3] - 2026-08-06

### Fixed

- Round-trip latency was one buffer period higher than it needed to be
  ([#21](https://github.com/M0n7y5/pipeasio/issues/21)). `pw_filter_connect()`
  without `PW_FILTER_FLAG_RT_PROCESS` sets both `node.loop.class=main` and
  `node.async=true`. The driver needs the first, which keeps the graph
  scheduling `process()` on the Wine-bridged loop so the host's COM
  `bufferSwitch` runs on a thread that has a TEB, but it was also paying for
  the second, which makes every link to the driver carry an extra graph
  quantum. The driver now clears `node.async` right after connecting, while no
  link exists yet, because PipeWire latches a link's async mode when the link
  is created. Measured round trip in `asio_loopback` drops from 2.00 to 1.00
  buffer periods at every size and rate, 42.7 ms to 21.3 ms at the shipped
  1024 / 48000 default, and the analyzer now asserts exactly one buffer period
  so a regression fails the suite. `follow_device_clock` keeps asynchronous
  scheduling on purpose, so a device-driven quantum cannot stall the graph.
  Connecting with `PW_FILTER_FLAG_RT_PROCESS` instead, as proposed in the
  issue, also drops `node.loop.class=main`, which moves `process()` onto a
  PipeWire pool data-loop thread with no Wine TEB and segfaults in ntdll
  during teardown.

## [1.4.2] - 2026-08-05

### Fixed

- Building from source on Debian works out of the box
  ([#20](https://github.com/M0n7y5/pipeasio/issues/20) follow-up). The
  `asio_probe` / `asio_loopback` test hosts failed with `windows.h: No such
  file or directory` because winegcc compiled them without the probed Wine
  SDK include dirs, and stock Debian could not even configure because its
  Wine tools are named `winebuild-stable` / `winegcc-stable` (the unsuffixed
  copies live off-PATH in `/usr/lib/wine`). Include propagation and tool
  discovery now handle the Debian and WineHQ `/opt` layouts.
- The `asio_probe_rt` test failed instead of skipping on machines where a
  PipeWire daemon is reachable but Wine or the installed driver is absent
  (e.g. a distrobox sharing the host PipeWire socket).

### Added

- Debian stable joined the CI build matrix and the `tests/distro` harness:
  the driver and both Wine test hosts now build against Debian's real
  hardening flags, Wine tool naming, and the PipeWire 1.4.2 floor on every
  push.

## [1.4.1] - 2026-08-05

### Fixed

- "Follow PipeWire" sample rate no longer sticks to 48 kHz
  ([#20](https://github.com/M0n7y5/pipeasio/issues/20)). The driver now reads
  the graph's clock rate from the PipeWire `settings` metadata
  (`clock.force-rate`, else `clock.rate`) before the stream runs, and while
  running treats the position clock as authoritative: the measured rate always
  reaches `GetSampleRate()`, and a `sampleRateChanged` that could not be
  delivered while the driver was still preparing is sent on the first running
  cycle. Hosts such as Adobe Audition no longer run at 48 kHz timing while the
  graph plays at another rate (sped-up or slowed-down playback).
- The settings panel's latency readout resolves the graph's actual clock rate
  when "Follow PipeWire" is selected instead of assuming 48 kHz.
- The ASIO probe now fails when the reported sample rate diverges from the
  measured sample cadence by more than 5%, so a rate misreport can no longer
  hide inside the cycle-count tolerance.

## [1.4.0] - 2026-08-04

### Added

- `PIPEASIO_WINE_INSTALL_ROOT` CMake option installs the driver files into an
  explicit Wine library dir (Debian/Ubuntu's
  `/usr/lib/x86_64-linux-gnu/wine`, a WineHQ `/opt/wine-<branch>/lib/wine`)
  while the tools stay under the normal install prefix. Previously the driver
  always landed in `<prefix>/lib/wine`, which Debian/Ubuntu Wine never reads.

### Fixed

- The `PIPEASIO_WINE_INSTALL_ROOT` absolute-path guard runs before
  `project()`, so the `wine_install_root_relative` test passes in containers
  without `make`: the inner configure previously aborted during toolchain
  probing before the guard could print its message.
- `pipeasio-register` now finds installs in `/usr/local/lib/wine` (the default
  CMake prefix) and in the `lib/wine` layout of current WineHQ packages under
  `/opt`. It warns when the install root is outside the Wine installation's
  own library dir, because such installs register successfully but no host can
  load the driver at launch, and when stale installs in earlier search
  locations shadow the one being registered. The library dir of the `wine`
  binary in use is searched too, covering Wine roots outside the built-in
  list (Bottles runners, self-built installs), and when it is one of several
  installs that copy wins, matching the load order Wine uses (its own library
  dir before WINEDLLPATH). Aliased candidates (lib/lib64
  symlinks, `PIPEASIO_PREFIX` naming a listed root) no longer trigger false
  stale-install warnings, and the library dir detection requires the 64-bit
  layout and a real Wine payload, so older WineHQ lib/lib64 splits resolve to
  the right dir and bare arch dirs left by a PipeASIO-only install are not
  mistaken for it. The built-in search list can be replaced with
  `PIPEASIO_REGISTER_CANDIDATES` (colon-separated). The discovery and warning logic is covered by a scripted
  test with a stubbed `wine`.
- `install_manifest.txt` now records the `pipeasio.dll` and `pipeasio.dll.so`
  symlinks, so the documented uninstall removes them.
- Final COM release now synchronizes with the lifecycle worker before object
  teardown, closing a Stop/Release use-after-free window. The TLS reentrancy
  walk stays outside Wine's mixed-ABI COM methods, avoiding a codegen-sensitive
  failed-init release crash. `GetErrorMessage()` now reports a bounded,
  actionable backend error instead of a generic failure.
- Buffer preparation now snapshots caller-owned channel descriptors before
  validation, centralizes the accepted frame bounds, and rejects invalid
  PipeWire quantum and WoW64 name values before they cross allocation or ABI
  boundaries. PipeWire registry object counts and property lengths are capped
  so an untrusted graph peer cannot grow the host cache indefinitely.
- The settings panel now shares the driver's canonical defaults and rejects
  multiline or unterminated device and node names before writing the INI file,
  preventing injected configuration entries.

## [1.3.0] - 2026-08-01

### Added

- **Real-time audio thread** setting. The `realtime` key is experimental and
  defaults to disabled; enabling it raises the native callback thread or WoW64
  pump to `SCHED_FIFO` 15. Because only that one thread is elevated,
  multi-threaded hosts under Wine (measured with FL Studio) can see far more
  xruns with it on. `PIPEASIO_RT_PRIORITY=off|on` overrides the setting. Added
  for [#4](https://github.com/M0n7y5/pipeasio/issues/4) diagnostics, not as a
  fix.
- CI now builds and tests against the pinned Steam Runtime 4 SDK and asserts
  that its PipeWire version is exactly 1.4.2.
- The release workflow checks that every `pw_` and `spa_` symbol imported by
  the shipped 64-bit and WoW64 Unix libraries exists in Steam Runtime 4's
  PipeWire 1.4.2 library.
- Native and WoW64 integration tests now check the scheduling policy of the
  callback thread and of an ordinary worker it hands off to, across three legs:
  shipped defaults, `realtime = 1`, and `realtime = 1` with
  `PIPEASIO_RT_PRIORITY=off`. Each thread stamps its own Linux `comm` from
  inside a live `bufferSwitch` and the runner reads `/proc/<pid>/task`.
- Real-time integration legs measure the user-visible per-node `pw-top` ERR
  counter from one long-lived batch stream (tracked by PipeWire node ID). A
  positive-control callback stall, armed only after the node is profiled,
  proves ERR can move; the clean policy legs stay stall-free and assert the
  window delta stays within `PROBE_XRUN_TOLERANCE`.
- A deterministic interleave test parks a method leaver between its gate check
  and its decrement, and releases it only once `CreateBuffers`' gate drain is
  asleep (a drain-wait handshake, not a timing assumption); the sliced drain
  must still complete promptly instead of stalling for the full timeout.
- Probe runners scrub ambient `PIPEASIO_*` and probe-control environment so a
  polluted caller environment cannot change probe behavior; orchestrating
  scripts pass their deliberate values through an explicit keep list.

### Fixed

- WoW64 configuration now starts from defaults before the Unix call. A missing
  Unix library or ABI mismatch can no longer leave the PE driver reading an
  uninitialised settings struct.
- Concurrent `Stop()` calls and overlapping restart now use generation-based
  completion instead of a resettable shared event, preventing stale or lost
  wakeups across `Stop`/`Start` cycles.
- The config watcher context is refcounted and stays attached to the driver
  until its thread has exited, and the final teardown joins the thread.
  A timed-out join can no longer free state the watcher still reads, and no
  watcher thread can outlive the driver object.
- A failed `DisposeBuffers` (for example a gate drain timeout while a modal
  `ControlPanel` is open) now restores `Prepared` and returns an error instead
  of reporting success while leaving the backend, callbacks, and buffers
  untouched.
- WoW64 auto-connect no longer requires the device's port count to equal the
  configured channel count; the marshalled endpoint list is a usable prefix,
  matching the native path.
- `pipeasio-register` now passes `/s` to `wine regsvr32`, so registration no
  longer blocks on a modal success dialog during unattended runs.
- The settings panel's Monitor tab holds the last good sample for one frame
  when the node briefly vanishes from a `pw-top` iteration or reports a
  transient idle cycle, instead of flashing zeros or "waiting for audio...".
  A persistent idle/suspended node now renders its actual I/S state, and the
  device rows clear once the node is gone.
- Settings panel tooltips now word-wrap to readable lines instead of
  rendering as one long single line.
- The INI line-length guard in the settings panel now matches the driver's C
  loader exactly; a line of exactly 1023 bytes is no longer accepted by the
  panel but ignored by the driver.
- Gate drains now re-check the admission count in bounded slices instead of
  relying solely on the idle event, eliminating a false 10-second timeout when
  a leaver's signal was lost.
- The probe's concurrent-`Stop` test pre-creates its stop threads parked on a
  start event; slow thread creation under Wine+ASan can no longer consume the
  blocked callback's budget and produce false failures.
- The native and WoW64 links now use the exact PipeWire library selected by
  `pkg-config`. This prevents headers from a custom prefix being combined with
  the system PipeWire library.
- Wine SDK discovery now derives and validates include directories from the
  `winebuild` prefix. Invalid `WINE_INCLUDE_DIRS` values fail at configure
  time, and WoW64 builds also require `unixlib.h`.
- Configure warns when `winebuild` and the selected Wine SDK come from
  different prefixes.

### Changed

- The WoW64 Unix-call ABI is now version 3. Endpoint and callback delivery is
  transactional, and the new `realtime` field shifts later fields without
  changing the struct size, so layout tests pin the interior offsets.
  Mismatched PE and Unix halves are rejected.
- README build instructions now use per-distribution dependencies and cover
  Wine SDK layouts outside `/usr`.
- The documented and enforced PipeWire floor is now 1.4.2. Earlier versions
  fail during configuration instead of compiling until the first unavailable
  API. The previous 1.6 requirement was unnecessary.
- Debug logs now report both the PipeWire header and runtime library versions.

## [1.2.3] - 2026-07-21

### Added

- Distro build matrix for toolchain drift that host-only CI misses.
  Locally, `tests/distro/run.sh` builds Fedora, Ubuntu, and Arch inside
  distrobox with each distro's package-build CFLAGS/LDFLAGS (Fedora RPM
  `%{optflags}` including `-flto=auto`, Ubuntu `dpkg-buildflags` with
  `hardening=+all`, Arch `makepkg.conf`); the Fedora leg asserts `-flto`
  is present so the issue #6 regression case stays covered. CI runs the
  same Fedora and Ubuntu legs via `build-distros` (Arch is already covered
  by the existing jobs).

### Fixed

- Building with distro-injected LTO CFLAGS (e.g. Fedora RPM's `-flto=auto`)
  failed at the winegcc link with `pipeasio.dll.spec:1: function
  'DllRegisterServer' not defined`
  ([#6](https://github.com/M0n7y5/pipeasio/issues/6)): winebuild's `ld -r`
  partial link leaves the `.spec` exports undefined in LTO objects even when
  `nm` still shows them on the individual `.o` files. Object libraries that
  feed winebuild/winegcc (the driver DLL and the WoW64 unixlib) now compile
  with `-fno-lto`.
- Debian/Ubuntu Wine SDK headers under the nested
  `/usr/include/wine/wine/windows` layout (from `libwine-dev`) are now found
  by `cmake/WineDLL.cmake`, so the driver configures and builds against
  distro Wine packages without a manual `-DWINE_INCLUDE_DIRS=...`.

## [1.2.2] - 2026-07-15

### Fixed

- RT thread priority dropped from 77/80 to 15 for the native callback thread
  and WoW64 pump. This did not resolve
  [#4](https://github.com/M0n7y5/pipeasio/issues/4): the reporter's PipeWire
  threads ran above both priorities, and the xruns persisted.

## [1.2.1] - 2026-07-02

### Changed

- The Wine integration probes (`asio_probe`, the PipeWire delivery/filter
  probes) now run under CTest alongside the Linux-native unit tests, so
  `ctest --test-dir build` drives the whole suite - including the run that
  gates every tagged release.

### Fixed

- The real-time audio thread ran at normal scheduling priority (`SCHED_OTHER`)
  on stock installs, causing xruns under any CPU load. PipeWire's data loop
  requests the "configured default" RT priority, which the thread-utils bridge
  treated as a no-op - and since that bridge bypasses module-rt/RTKit, nothing
  else promoted the thread either. The default request now maps to a real
  `SCHED_FIFO` priority (77, below the PipeWire daemon's data loop at 88), and
  on `EPERM` retries clamped to `RLIMIT_RTPRIO`. The thread driving the whole
  ASIO `bufferSwitch` chain now actually runs FIFO.
- Every `audio_open` leaked one zombie thread and its stack: the context's
  initial data loop was stopped through the Wine thread-utils bridge installed
  *after* the loop had started, so the original pthread was never joined. The
  loop is now stopped and joined through the utils that created it before the
  bridge is installed.
- Use-after-free when an ASIO host services `kAsioResetRequest` synchronously
  on the config-watcher thread and releases the driver from inside the
  notification: the watcher's state now lives in a refcounted heap context
  owned jointly by the driver and the thread, the only call-out is wrapped in
  `AddRef`/`Release`, and a same-thread stop orphans the context instead of
  waiting on itself. A synchronous `DisposeBuffers`/`CreateBuffers` reset no
  longer leaves two watchers racing the staged config or leaks the old
  thread's handles.
- The real-time output copy (and the silence paths) wrote a full host period
  into the PipeWire buffer with no capacity check, overrunning the mapping
  every cycle when the graph clamps the quantum below the host buffer size
  (`clock.quantum-limit`). Output and silence writes are now clamped to the
  dequeued buffer's capacity, mirroring the existing input-side clamp, in both
  the 64-bit and WoW64 paths.
- The 64-bit sample position wrapped every ~25 hours at 48 kHz again: on the
  wine64 ELF build `ULONG` is 32-bit but `ULONG_MAX` is 2^64-1, so the hi-word
  carry in the buffer-switch path never fired. The counters are now stored as
  atomic 64-bit values and split into the ASIO hi/lo wire format only at the
  edges, which also fixes a torn read of the position that raced the real-time
  writer.

## [1.2.0] - 2026-06-29

### Added

- The Monitor tab now shows the **Output device** and **Input device** the
  driver's ports are currently connected to - the live sink and source resolved
  from the PipeWire graph - so it is obvious which hardware the driver is
  feeding, especially when autoconnect or "follow default" picks the device.
  Each row also reports the device's negotiated format (rate, channels, sample
  format), its state, and the Bluetooth codec when applicable (e.g. aptX), and
  sits below the live State row.
- New **About** tab in the settings panel with the version, a short description,
  and links to the website and documentation, the GitHub repository, the issue
  tracker, and a Ko-fi support link.

### Changed

- Live config reload now diffs the INI before resetting. Saving the settings
  panel re-negotiates the audio graph only when a reset-worthy field actually
  changed, so a no-op save (or one that merely rewrites the file) no longer
  causes a dropout. Buffer size, sample rate, device selection, follow-device
  clock, and autoconnect now take effect on the reset itself - the new PipeWire
  quantum applies even when the host only re-creates buffers instead of fully
  re-initializing the driver, which previously left buffer-size changes silently
  unapplied. Channel-count and node-name changes are detected and logged as
  needing a driver reselect, since those ports are allocated at init.

### Fixed

- Deadlock when an ASIO host services `kAsioResetRequest` synchronously on the
  config-watcher thread: the watcher teardown no longer waits on itself - it
  signals stop and lets the COM-thread `DisposeBuffers`/`Release` path reap the
  thread and event handles.
- Data races on the driver run-state and the host-callback pointer, which the
  config-watcher and the real-time process callback read while the COM thread
  wrote them; both are now atomic.
- Real-time input copy could read past a PipeWire capture buffer smaller than
  the host period (graph quantum below the configured buffer size). The input
  gather is now clamped to the mapped buffer and the tail zero-filled, mirroring
  the existing output-side clamp; both the 64-bit and WoW64 paths are fixed.
- The experimental 32-bit WoW64 PE front end no longer fails to link. The input
  clamp above calls `audio_port_buffer_avail_frames`, which is unix-side only; the
  PE half (which links the WoW64 proxy instead of `audio.c`) now carries a matching
  stub, since that gather actually runs unix-side. `-DBUILD_WOW64_32=ON` builds
  again, with 32-bit load + streaming re-verified through `asio_probe32` and
  VB-Audio's VBASIOTest32.
- 32-bit (WoW64) autoconnect linked nothing to hardware: `wow64_port_register`
  never handed the PE side a token for the freshly registered port, so the
  proxy returned a NULL handle and `audio_port_name()` came back empty - the
  link factory then could not resolve our own ports (`node=4294967295`,
  `pw_port_id=0`) even though the device ports resolved. The handler now assigns
  the out-token (like `wow64_port_by_name` already did); our input/output ports
  resolve and link to the selected source/sink. Streaming was unaffected (it
  runs unix-side), so the regression only showed as silent autoconnect.
- 32-bit (WoW64) live config reload was always disabled. The watcher resolved
  the INI path with the PE-side `pipeasio_config_path()`, whose `getenv()`
  cannot see `$XDG_CONFIG_HOME`/`$HOME` in the Windows environment under Wine,
  so it logged "cannot resolve config path" and returned before reaching the
  unixlib fingerprint poll it already carried. It now skips the PE-side lookup
  in the WoW64 build and detects edits through
  `pipeasio_wow64_config_fingerprint()`, so saving the panel re-applies live in
  32-bit hosts too.

## [1.1.0] - 2026-06-25

### Added

- Experimental opt-in 32-bit (WoW64) front end for 32-bit Windows ASIO hosts,
  built with `-DBUILD_WOW64_32=ON` (default OFF) and a MinGW cross-compiler. A
  thin i386 PE thunk (`pipeasio32.dll`) forwards every ASIO call over
  `__wine_unix_call` to the same 64-bit PipeWire backend (`pipeasio32.so`), so no
  32-bit libpipewire or 32-bit Linux userspace is needed. `pipeasio-register`
  registers the CLSID under the 32-bit view when the DLL is present. The 64-bit
  driver is byte-for-byte unaffected. Validated end-to-end by a new `asio_probe32`
  host, with the WoW64 unix-call ABI layout locked by a compile-time test.

- The WoW64 DSP pump thread runs at `SCHED_FIFO` priority 80 (matching the native
  driver's RT ceiling) with FTZ/DAZ denormal flushing and an 8 MB stack, and its
  per-cycle reply deadline uses `CLOCK_MONOTONIC`, so the 32-bit path sustains
  64-128 frame buffers without xruns.

- Prebuilt Arch/CachyOS x86_64 binaries (the 64-bit driver plus the opt-in
  32-bit WoW64 front end) attached to each tagged GitHub release, labeled with
  the exact Wine, glibc, and PipeWire versions they were built against. CI now
  also builds the 32-bit WoW64 path.

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
  covers buffer sizes 128-1024 at 44.1/48/96 kHz with in-process buffer
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
  fabricated values - PipeWire has no transport timeline to source timecode
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

[Unreleased]: https://github.com/M0n7y5/pipeasio/compare/v1.5.0...HEAD
[1.5.0]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.5.0
[1.4.3]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.4.3
[1.4.2]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.4.2
[1.4.1]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.4.1
[1.4.0]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.4.0
[1.3.0]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.3.0
[1.2.3]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.2.3
[1.2.2]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.2.2
[1.2.1]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.2.1
[1.2.0]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.2.0
[1.1.0]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.1.0
[1.0.0]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.0.0
[1.0.0-rc1]: https://github.com/M0n7y5/pipeasio/releases/tag/v1.0.0-rc1

---

PipeASIO is a fork of [WineASIO](https://github.com/wineasio/wineasio). For the
history before this project, see the WineASIO changelog.
