# Repository Guidelines

## Project Overview

PipeASIO is an **ASIO driver for Wine that talks to PipeWire directly** — no `libjack.so.0` runtime dependency. It is a fork of [WineASIO](https://github.com/wineasio/wineasio), created so the driver loads cleanly inside the Steam Runtime `steamrt4` container (FL Studio under Faugus / Proton-CachyOS), which ships `libpipewire-0.3` but not `libjack.so.0`.

It has its own distinct COM identity so it can coexist with WineASIO:
- CLSID `{2D3CA9E2-1193-4C5D-B5FD-38798F3DC074}`
- Registry: `HKCU\Software\Wine\PipeASIO`, `HKCU\Software\ASIO\PipeASIO`
- DLL: `pipeasio64.dll`

Scope: **x86_64 only, C11**. Licensed under GPL v3 or later (`COPYING`), retaining the original WineASIO authors' copyright notices.

> Note: the driver is now PipeWire-native, but some callback names still carry **JACK-era terminology** from upstream (e.g. `jack_process_callback` in `asio.c`). Treat `jack_*` symbols as backend bridges into PipeWire, not a JACK dependency. (The old PyQt GUI's "JACK Options" group and "Autostart server" toggle are gone with the C++/Qt6 rewrite.)

## Architecture & Data Flow

Three layers, with a backend-agnostic seam at `include/audio.h`:

1. **Windows ASIO interface — `src/asio.c`** (~1740 lines, the core). Implements the `IPipeASIO` COM vtable (~25 ASIO methods: `Init`, `Start`, `Stop`, `CreateBuffers`, `GetChannels`, `GetBufferSize`, `GetSampleRate`, …). The `IPipeASIOImpl` struct holds all driver state (host callbacks, channel arrays, buffer tracking, backend client handle). Lifecycle: **Loaded → Initialized → Prepared → Running**.
2. **Audio backend — `include/audio.h` + `src/audio.c`** (~1517 lines). `audio.h` is the backend-agnostic API (opaque `audio_client_t` / `audio_port_t`, lifecycle + port + callback registration). `audio.c` is the libpipewire-0.3 implementation: `pw_thread_loop` RT thread, `pw_filter` DSP node, memfd-backed double buffers, registry walker for physical I/O ports. Provides a custom `spa_thread_utils` so PipeWire's RT thread gets a Wine TEB via `CreateThread`.
3. **COM plumbing — `src/main.c` + `src/regsvr.c`**. `main.c` has the DLL entry points (`DllMain`, `DllGetClassObject`, `DllCanUnloadNow`) and the `IClassFactory` (ref-counted via `InterlockedIncrement/Decrement`, delegates to `PipeASIOCreateInstance`). `regsvr.c` implements `DllRegisterServer` / `DllUnregisterServer` (CLSID + InProcServer32 + ProgID registry entries).

**RT data flow (per audio cycle):** `jack_process_callback` (in `asio.c`, called from the PipeWire filter process context) copies captured input from PipeWire into `callback_audio_buffer` at the current `host_buffer_index` half → invokes the host's `swapBuffers(index, 1)`, toggling `host_buffer_index` (0↔1) → copies the host's output back to PipeWire ports. Auxiliary callbacks (`buffer_size`, `sample_rate`, `latency`) relay PipeWire config changes to the host. Quantum and sample rate are locked to the host's negotiated values via `PW_KEY_NODE_FORCE_QUANTUM` / `PW_KEY_NODE_FORCE_RATE`.

**Buffer layout:** all offset math lives in `include/pipeasio_offsets.h` as **`static inline` compute-only helpers** (`pipeasio_memfd_size_bytes`, `pipeasio_mapoffset_bytes`, `pipeasio_host_*_offset_samples`), so `audio.c` and the unit tests share identical arithmetic. Offsets are precomputed at init — no runtime division on the RT path.

## Key Directories

| Path | Purpose |
|------|---------|
| `src/` | C driver implementation (`asio.c`, `audio.c`, `main.c`, `regsvr.c`) |
| `include/` | `audio.h` (backend API), `pipeasio_offsets.h` (buffer math, shared with tests) |
| `cmake/` | `WineDLL.cmake` — the `add_wine_dll()` build helper |
| `tests/unit/` | Linux-native C unit tests (host gcc, CTest) — buffer math + INI config parsing |
| `tests/asio_probe/` | Wine PE diagnostic host that drives the full COM stack |
| `tests/pw_probe/` | Linux-native libpipewire contract probes (filter thread / forced quantum / default node) |
| `tests/gui/` | Linux-native Qt6 panel unit tests (config round-trip, `pw-top`/`pw-dump` parsing) |
| `gui/` | Native C++/Qt6 Widgets settings panel (`pipeasio-settings`), built by the top-level CMake |

## Development Commands

```sh
# Build (Release)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Build (Debug — assertions + Wine debug-channel macros)
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug

# Build with AddressSanitizer + UBSan
cmake -B build-asan -DPIPEASIO_ASAN=ON
cmake --build build-asan

# Unit tests (Linux-native, no Wine needed)
ctest --test-dir build

# Driver probe under Wine (needs a running PipeWire + Wine)
./build/tests/asio_probe/run.sh        # FRESH=1 wipes the throwaway prefix
./build/tests/asio_probe/gdb.sh        # same run, with coredump/gdb analysis

# Install (system-wide vs user-local)
sudo cmake --install build --prefix /usr
cmake --install build --prefix "$HOME/.local"

# Register the driver into a Wine prefix
pipeasio-register
env WINEPREFIX="$HOME/asioapp" pipeasio-register

# Settings panel (pipeasio-settings) builds with the top-level CMake when Qt6
# Widgets is present; pass -DBUILD_SETTINGS_PANEL=OFF to skip it
```

Formatting: run `clang-format` (config is in-tree `.clang-format`). The build emits `build/compile_commands.json` for clangd.

## Code Conventions & Common Patterns

**C (driver):**
- **Naming:** CamelCase for Windows types/idioms (`HRESULT`, `LONG`, `BOOL`, `HWND`, `WINAPI`, `STDMETHODCALLTYPE`); `snake_case` for PipeWire/POSIX (`pw_thread_loop`, `pw_filter`, `pthread_t`). COM methods use `DEFINE_THISCALL_WRAPPER` for the x86-64 calling convention and the vtable pattern.
- **Error handling:** ASIO methods return `LONG` (0 = success; negatives: `-994` out-of-memory, `-998` invalid args, `-999` device error, `-1000` wrong state). COM methods return `HRESULT` (`S_OK`, `E_NOINTERFACE`, `E_INVALIDARG`, `E_OUTOFMEMORY`). PipeWire calls are checked for `< 0`.
- **Cleanup:** `goto error` labels with cascading releases; explicit paired open/close; `HeapAlloc`/`HeapFree` for host buffers.
- **Logging:** Wine `TRACE`/`WARN`/`ERR` channels, overridden to **raw `write()`** to bypass stderr buffering so logs survive a crash. Debug channel name is `pipeasio` (e.g. `WINEDEBUG=-all,+pipeasio`).
- **Style:** GNU `.clang-format` — 4-space indent, column limit 100, Allman braces, right-aligned pointers, aligned consecutive assignments. No short one-liners.

**C++ (settings panel, `gui/`):** Qt6 Widgets, C++17, same GNU `.clang-format` as the driver (no PyQt). Member widgets are `m_camelCase`; the pure, UI-free helpers — INI parse/serialize (`Config`) and `pw-top`/`pw-dump` parsing (`PipeWireMonitor`, `DeviceEnumerator`) — are kept free of Qt widget types so `tests/gui/` can unit-test them headlessly. Settings persist to the flat INI at `$XDG_CONFIG_HOME/pipeasio/config.ini`, not the Wine registry.

## Important Files

- `src/asio.c` — ASIO COM interface; the primary entry surface and most logic.
- `src/audio.c` / `include/audio.h` — PipeWire backend and its backend-agnostic API.
- `include/pipeasio_offsets.h` — buffer-offset math; **shared verbatim with `tests/unit/test_offsets.c`**, so changes here must keep tests passing.
- `src/main.c` / `src/regsvr.c` — COM entry points and registry (un)registration.
- `pipeasio.dll.spec` — Wine DLL export spec (`DllRegisterServer`, `DllGetClassObject`, `DllCanUnloadNow`, `DllUnregisterServer`); consumed by `winebuild`/`winegcc`.
- `CMakeLists.txt` + `cmake/WineDLL.cmake` — build entry and the Wine-DLL packaging logic.
- `pipeasio-register` — post-install registration script (locates binaries, stages the PE stub, `wine regsvr32`).
- `gui/` — native C++/Qt6 panel: `SettingsDialog` (window), `Config` (INI I/O), `DeviceEnumerator` (`pw-dump` device list + own-node discovery), `PipeWireMonitor` + `LoadHistogram` (`pw-top` telemetry), `main.cpp`; built via `gui/CMakeLists.txt`.
- `README.md` — authoritative for build/install/register flows and the runtime config keys.

## Runtime / Tooling Preferences

- **Build toolchain:** CMake ≥ 3.20, host `gcc` for PIC objects, then `winebuild` + `winegcc` (from the Wine SDK, `wine-devel` / `winehq-stable-dev`). `pkg-config` resolves `libpipewire-0.3` (REQUIRED at configure time). Ninja recommended over make.
- **Dual artifact per DLL:** `winebuild -m64 --dll --fake-module` produces the PE fake `pipeasio64.dll`; `winegcc -shared` produces the ELF `pipeasio64.dll.so`. Install lays both halves under `<prefix>/lib/wine/x86_64-{windows,unix}/` plus **`pipeasio.dll{,.so}` symlinks** required by Wine 10+ (`regsvr32` returns `c0000135` without them).
- **Runtime:** PipeWire 1.6+. Distro Wine library paths vary — override `--prefix` or `CMAKE_INSTALL_LIBDIR` for non-standard layouts. For Proton/Faugus, install under `$HOME/.local` and set `WINEDLLPATH=$HOME/.local/lib/wine`.
- **GUI:** native C++/Qt6 Widgets (`qt6-base-dev`); built by the top-level CMake into `pipeasio-settings` and installed to `bin`. No PyQt, no UI code generation.
- **Driver config** — a flat INI at `$XDG_CONFIG_HOME/pipeasio/config.ini` (`[pipeasio]` section), every key overridable by an env var and re-read live while running: `inputs`/`outputs` (16), `auto_connect` (1), `output_device`/`input_device` (empty = follow the PipeWire default), `sample_rate` (0 = follow graph), `fixed_buffer_size` (1), `follow_device_clock` (0), `buffer_size` (1024, power of two in [16, 8192]), `node_name`. `include/pipeasio_config.h` + `src/config.c` are the source of truth; README documents each key.

## Testing & QA

- **Unit tests (`tests/unit/`):** custom header-only harness in `test_helpers.h` — `TEST_GROUP("name") { ... }`, `EXPECT_EQ(got, expected)`, `EXPECT_TRUE(cond)`, and `int main(void) { return test_report(); }`. Assertions never abort; all failures print `file:line` and the run reports a count. Linux-native (no Wine). `test_offsets.c` verifies the `pipeasio_offsets.h` buffer math (memfd sizing, partition no-overlap, offset round-trips, host input/output layout, tiny-buffer edge cases).
  - **Add a test:** create `tests/unit/test_<name>.c`, then register it in `tests/unit/CMakeLists.txt` with `pipeasio_add_unit_test(test_<name> test_<name>.c)` (sets C11, `-Wall -Wextra -Wpedantic`, and `add_test`).
- **Integration probe (`tests/asio_probe/`):** `asio_probe.exe.so` is a real Wine PE host built with `winegcc -mconsole`. It `CoCreateInstance`s the driver, runs the full `Init → CreateBuffers → Start` path for N seconds (default 5), counts `bufferSwitch` invocations, and **passes when cycles ≈ (sample_rate / preferred_buffer_size) × seconds within 50%** (exit 0 pass, 2 fail). `run.sh` drives it in a throwaway wineprefix (auto-detects ASan and preloads `libasan`/`libubsan`); `gdb.sh` adds systemd-coredump + gdb triage.
- **PipeWire-contract probe (`tests/pw_probe/`):** `pw_filter_probe` is a Linux-native (gcc + `libpipewire-0.3`, no Wine) test that replicates `src/audio.c`'s filter/memfd/thread-utils setup and asserts the two invariants the driver depends on: the `pw_filter` `process()` callback runs on a thread our `spa_thread_utils` created (a CreateThread'd Wine thread — otherwise the host's COM `bufferSwitch` corrupts memory), and `PW_KEY_NODE_FORCE_QUANTUM` pins `clock.duration` to the buffer size. Default config mirrors the driver (data loop + restart "dance" + `PW_FILTER_FLAG_NONE`) and must pass; `--loop main` / `--no-dance` reproduce the broken configuration. Needs a running PipeWire daemon; exits 77 (CTest SKIP) when none is reachable. Registered as a CTest, so `ctest --test-dir build` runs it.
- **Qt panel tests (`tests/gui/`):** Linux-native (Qt6 Core, no Wine) checks of the panel's pure functions — the INI round-trip against the driver's C reader, plus `pw-top`/`pw-dump` parsing (`parsePwTop`, `parsePwDump`, `findOwnNode`). Registered as a CTest.
- **No coverage tooling is configured.** Verify behavioral changes by running the affected unit test via `ctest --test-dir build` and, for RT/driver changes, the `asio_probe` under a live PipeWire session.
