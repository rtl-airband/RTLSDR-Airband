# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Working Norms

- **Keep this file updated** as changes are made to the codebase.
- **Never guess or make assumptions** — ask clarifying questions when requirements are unclear.
- **All code changes should include unit tests.** When possible, write tests first and implement code to make them pass (TDD).
- **After writing code, review comments** and remove any that don't explain non-obvious behavior — don't comment what the code already says.

## Development Environment

The repo is set up for development in VS Code using a devcontainer (`.devcontainer/`). When working inside the devcontainer, all compile, test, and run commands must be executed inside the container.

## Wiki Documentation

User-facing documentation lives in a separate repo: https://github.com/rtl-airband/RTLSDR-Airband/wiki

Flag when code changes require wiki updates and provide suggested content — do not edit the wiki directly.

## Code Review Guidelines

When reviewing code:
- Reference specific files and line numbers (`src/foo.cpp:42`)
- Start with architecture-level concerns before line-level feedback
- Consider SDR/DSP domain context (signal processing constraints, real-time threading, buffer management)
- Verify the testing approach covers the behavior being changed
- Structure feedback clearly: separate blocking issues from suggestions
- Be pragmatic — prefer working correct code over theoretical perfection
- Check for consistency with surrounding code style and conventions

## Project Overview

RTLSDR-Airband is a C++ SDR (Software-Defined Radio) application that receives analog radio voice channels from SDR devices (RTL-SDR, SoapySDR, MiriSDR) and produces MP3 audio streams for Icecast, file recording, UDP, and PulseAudio.

## Build Commands

Dependencies: libconfig++, libmp3lame, libshout, libfftw3f, librtlsdr, libsoapysdr, libpulse. Install via `.github/install_dependencies`.

```bash
# Standard debug build with unit tests
cmake -B builds/Debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_UNITTESTS=TRUE
cmake --build builds/Debug -j4

# Release build with NFM and SoapySDR
cmake -B builds/Release -DCMAKE_BUILD_TYPE=Release -DNFM=TRUE -DSOAPYSDR=ON
cmake --build builds/Release -j4

# Run unit tests
./builds/Debug/src/unittests
./builds/Release/src/unittests

# Run the binary
./builds/Debug/src/rtl_airband -c /path/to/config.conf
./builds/Release/src/rtl_airband -c /path/to/config.conf
```

Key CMake flags (all in `src/CMakeLists.txt`):

| Flag | Default | Purpose |
|------|---------|---------|
| `NFM` | OFF | Enable Narrow FM demodulation |
| `PLATFORM` | `native` | Optimization target: `native`, `generic`, `rpiv2` |
| `RTLSDR` | ON | RTL-SDR driver |
| `MIRISDR` | ON | Mirics SDR driver |
| `SOAPYSDR` | ON | SoapySDR (vendor-neutral) driver |
| `PULSEAUDIO` | ON | PulseAudio output |
| `BUILD_UNITTESTS` | OFF | Build Google Test unit tests |
| `BCM_VC` | OFF | Broadcom VideoCore GPU FFT (RPi v2 only) |

## Docker

The container image is built from a multi-stage `Dockerfile` based on `alpine:latest` (musl libc). Alpine is used because it ships a native `linux/arm/v6` image, so a single base covers every Raspberry Pi model (Pi 1/Zero armv6 through Pi 5 arm64) — Debian dropped the armel/`arm/v5` variant that the arm/v6 build previously relied on via fallback.

- Build/runtime dependencies come from `apk`. Note the Alpine-specific package names: `libconfig++` (Alpine splits the C++ binding into its own package), `libstdc++` (not in the base image), `fftw-single-libs`, `libpulse`, and `pkgconf` (provides `pkg-config`).
- SoapySDR, the rtl-sdr-blog fork (built from source for RTL-SDR Blog V4 support), and libmirisdr-4 are compiled from source with CMake and installed to `/usr` — musl only searches `/lib` and `/usr/lib`, not `/usr/local/lib`. `ENV CMAKE_POLICY_VERSION_MINIMUM=3.5` lets their pre-3.5 `cmake_minimum_required` configure under Alpine's CMake 4.
- Both stages run `unittests`, so a broken build fails `docker build`. `scripts/find_version` is POSIX `sh` (Alpine has no bash).

```bash
# Build the image for the host architecture
docker build -t rtlsdr-airband .

# Build a specific architecture (requires QEMU/binfmt for cross-arch)
docker buildx build --platform linux/arm/v6 --load -t rtlsdr-airband:armv6 .
```

## Code Formatting and Pre-commit

Uses clang-format v14 with Chromium style (indent=4, column limit=200, config in `.clang-format`).

```bash
# Install pre-commit hooks (once, after cloning)
pre-commit install

# Run all pre-commit hooks manually
pre-commit run --all-files

# Format C++ source files manually (also used by CI)
./scripts/reformat_code
```

Pre-commit hooks (`.pre-commit-config.yaml`) run on every commit and check:
- YAML/JSON validity, trailing whitespace, EOF newlines, shebang permissions, large files, merge conflict markers, private keys
- clang-format on all `src/*.cpp` and `src/*.h` files
- shellcheck on all bash scripts (excluding `init.d/`)
- black, isort, and pylint on all `system_tests/**/*.py` files
- Build (AM and NFM) and C++ unit tests when `src/*.cpp`, `src/*.h`, or `CMakeLists.txt` are modified (`scripts/run_unit_tests`)
- Python system tests when `src/*.cpp`, `src/*.h`, `CMakeLists.txt`, or `system_tests/` are modified (`scripts/run_system_tests`); only runs if the build/unit-test step passes

## CI and Pull Request Checks

Four workflows run on pull requests (`.github/workflows/`); the container build also runs on merges to `main`, tags, and a daily schedule:

**`code_formatting.yml`** — runs `./scripts/reformat_code` and fails if any files differ.

**`ci_build.yml`** — builds and tests four configurations on Ubuntu (x86 and ARM) and macOS:
```bash
cmake -B builds/Debug          -DCMAKE_BUILD_TYPE=Debug   -DBUILD_UNITTESTS=TRUE
cmake -B builds/Debug_nfm      -DCMAKE_BUILD_TYPE=Debug   -DNFM=TRUE -DBUILD_UNITTESTS=TRUE
cmake -B builds/Release        -DCMAKE_BUILD_TYPE=Release -DBUILD_UNITTESTS=TRUE
cmake -B builds/Release_nfm    -DCMAKE_BUILD_TYPE=Release -DNFM=TRUE -DBUILD_UNITTESTS=TRUE
```
Then runs `unittests` for all four, installs the Release+NFM build, and smoke-tests `rtl_airband -v`.

**`platform_build.yml`** — builds and tests an AM Release configuration (`PLATFORM=native`) on a Pi 4B runner and an `ubuntu-22.04-arm` runner, then runs unit tests and system tests. (Pi 3B runner is currently disabled.)

**`build_docker_containers.yml`** — builds and pushes the multi-arch container image (`linux/amd64`, `386`, `arm64`, `arm/v6`, `arm/v7`) to GitHub Container Registry, one job per platform via QEMU. Each pushed image is smoke-tested (`rtl_airband -v`) before the per-arch digests are merged into a single manifest.

**Before submitting a PR**, the pre-commit hooks cover most checks automatically. For build system or config changes not touching `src/`, verify all four cmake configurations build cleanly by hand.

## System Tests

End-to-end tests live in `system_tests/`. They run the actual binary against generated IQ files and validate the audio output (MP3 duration, rawfile size). Managed with [uv](https://docs.astral.sh/uv/).

```bash
# Run system tests (requires Release binaries — run scripts/run_unit_tests first)
scripts/run_system_tests

# Run manually from the system_tests directory
cd system_tests
uv sync
uv run pytest tests/ \
    --binary ../builds/Release/src/rtl_airband \
    --nfm-binary ../builds/Release_nfm/src/rtl_airband \
    -v
```

Python tooling (formatter, import sorter, linter) is configured in `system_tests/pyproject.toml` under `[tool.black]`, `[tool.isort]`, and `[tool.pylint]`. Run them manually:

```bash
cd system_tests
uv run black .
uv run isort .
uv run pylint conftest.py helpers/ tests/
```

## Architecture

### Reception Pipeline

```
SDR device (input-*.cpp)
  → RX thread → circular sample buffer
  → demod thread: FFT (FFTW3) → demod (AM/NFM) → filter → CTCSS → squelch → AGC
  → channel output handlers
  → output thread: MP3 encode (lame) → Icecast / file / UDP / PulseAudio
```

### Key Source Files

| File | Purpose |
|------|---------|
| `src/rtl_airband.cpp` | Main entry point, demod loop, thread management |
| `src/rtl_airband.h` | All major struct/enum definitions (`device_t`, `channel_t`, `mixer_t`, `output_t`) |
| `src/config.cpp` | libconfig++ parsing for devices, channels, mixers, outputs |
| `src/output.cpp` | MP3 encoding, Icecast connections, file/UDP output |
| `src/mixer.cpp` | Multi-channel mixer with ampfactor/balance |
| `src/input-*.cpp` | SDR device drivers (rtlsdr, soapysdr, mirisdr, file) |
| `src/input-common.cpp/h` | Input device abstraction (`input_t` function-pointer interface) |
| `src/filters.cpp/h` | IIR lowpass and notch filters |
| `src/squelch.cpp/h` | Noise-power-based voice activity detection |
| `src/ctcss.cpp/h` | CTCSS tone detection |

### Device Modes

Each device operates in one of two modes, set via `mode = "multichannel"` (default) or `mode = "scan"` in config.

**`R_MULTICHANNEL`** — The SDR is tuned to a fixed center frequency and multiple channels are demodulated simultaneously from the same wideband capture. Each channel has a single `freq` value that must fall within the SDR's bandwidth. This is the common case for monitoring several frequencies at once.

**`R_SCAN`** — The device has exactly one channel, but that channel holds a `freqs` list of frequencies to cycle through. A controller thread monitors the squelch: after ~2 seconds of no signal (10 × 200 ms polls), it retunes the SDR hardware to the next frequency via `input_set_centerfreq()`. When a signal is detected, it stays on the current frequency. Per-frequency settings (labels, `squelch_threshold`/`squelch_snr_threshold`, `modulations`, `notch`/`notch_q`, `ctcss`, `bandwidth`, `ampfactor`) are **parallel lists** to `freqs` (same index = same frequency), not a list of objects — `freqs` is parsed as numbers. Only one channel per device in scan mode. (`rtl_airband.cpp:101-140`, `config.cpp:312-729`)

### Threading Model

- **RX thread** (1 per device, always) — reads SDR samples into the circular buffer (`input-common.cpp`)
- **Controller thread** (1 per device, `R_SCAN` mode only) — scanning/squelch state machine for devices that scan across frequencies; not created for `R_MULTICHANNEL` devices (`rtl_airband.cpp:1005-1013`)
- **Demod thread** (1 total by default; 1 per device if `multiple_demod_threads=true` in config) — FFT, demodulation, filter, CTCSS, squelch, AGC for all assigned devices (`rtl_airband.cpp:1044`)
- **Output thread** (1 total by default; 1 per device + 1 for mixers if `multiple_output_threads=true`) — MP3 encoding and streaming
- **Mixer thread** (1 total, only if any mixers are configured) — processes all mixers; not per-mixer (`rtl_airband.cpp:1091-1092`)

### Configuration Format

Config files use libconfig++ syntax. Sample configs in `config/`. Top-level sections:

```
devices: ( { type = "rtlsdr"; centerfreq = 120.0; gain = 25;
             channels: ( { freq = 119.5; modulation = "AM";
                           outputs: ( { type = "icecast"; ... } ); } ); } );
mixers: { mix1: { outputs: ( { type = "icecast"; ... } ); } };
```

Mixers have **no `inputs` section** — a channel connects to a mixer implicitly via an output `type = "mixer"` + `name` (+ optional `ampfactor`, `balance`), see `mixer_connect_input` (`src/config.cpp:168-193`, `src/mixer.cpp:57`).

Output types: `icecast`, `file`, `rawfile`, `udp_stream`, `mixer`, `pulse`.

### Unit Tests

Tests use Google Test (fetched via CMake FetchContent). Test files in `src/test_*.cpp` cover filters, squelch, CTCSS, helper functions, and signal generation. `src/test_base_class.h` provides test utilities.
