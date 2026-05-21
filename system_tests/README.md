# System Tests

End-to-end tests that run the actual `rtl_airband` binary against synthetically generated IQ files and validate the audio output (MP3 duration and Prometheus stats).

## How It Works

Each test:
1. Generates (or loads from cache) a synthetic IQ fixture in RTL-SDR U8 format
2. Writes a `libconfig++` config file pointing the binary at that IQ file
3. Runs the binary and waits for it to finish
4. Validates the output MP3s and the Prometheus stats file

IQ fixtures are cached in `.generated_input/` and reused across runs. Test outputs land in `test_output/<test-name>/` and are kept for debugging.

> **Cache invalidation**: the cache key is the fixture parameters (frequency offset, duration, etc.), not the generator code. If the signal generation logic in `helpers/iq_generator.py` is changed (e.g., to fix a bug), delete `.generated_input/` to force regeneration:
> ```bash
> rm -rf system_tests/.generated_input/
> ```

## Prerequisites

- [uv](https://docs.astral.sh/uv/) — Python package manager
- A built `rtl_airband` binary (see the top-level `CLAUDE.md` for build instructions)

## Running the Tests

```bash
# From the repo root — builds Release binaries first, then runs system tests
scripts/run_system_tests

# Or manually from inside system_tests/
cd system_tests
uv sync
uv run pytest tests/ \
    --binary ../builds/Release/src/rtl_airband \
    --nfm-binary ../builds/Release_nfm/src/rtl_airband \
    -v
```

The `--nfm-binary` argument is optional. If omitted, NFM tests are skipped.

### Test Modes

Pass `--mode` to trade off wall-clock time vs. strictness. MP3 duration tolerance is a flat ±10% in both modes; what differs is how many output-thread overruns the assertion will tolerate.

| Mode | Speedup | Overrun budget | Parallelism |
|------|---------|----------------|-------------|
| `thorough` (default) | 1x (real time) | up to 1 (first-batch warm-up) | Must run serially (no `-n`) |
| `fast` | 10x | up to 5 batches | Safe to run with `-n auto` |

```bash
# Fast parallel run
uv run pytest tests/ --binary ../builds/Release/src/rtl_airband -n auto --mode fast

# Strict serial run
uv run pytest tests/ --binary ../builds/Release/src/rtl_airband --mode thorough
```

### Output directory

By default tests write per-test artifacts under `system_tests/test_output/<test-name>/`. On hosts where SD-card writeback stalls inject timing jitter (notably the Pi 4B CI runner), point this at a tmpfs:

```bash
uv run pytest tests/ --binary ../builds/Release/src/rtl_airband --test-output-dir=/test_data
```

The `--test-output-dir` path must exist and be writable by the test process. The cleanup logic in `conftest.py` only wipes the *contents* of the directory, never the directory itself, so it is safe to point at a pre-mounted tmpfs.

The CI runner is provisioned with a `/test_data` tmpfs mount; the workflow at `.github/workflows/platform_build.yml` uses it via `--test-output-dir=/test_data`. The `ubuntu-22.04-arm` runner has fast enough storage that it stays on the default path.

## Test Coverage

| Test file | What it checks |
|-----------|----------------|
| `test_am_squelch_closed.py` | Noise-only input → no output (squelch blocks it) |
| `test_am_squelch_open.py` | AM signal opens the squelch → MP3 with correct duration |
| `test_squelch_disabled.py` | `squelch_snr_threshold = 0` → gate is always open, MP3 produced |
| `test_ctcss.py` | Correct CTCSS tone opens gate; wrong tone keeps it closed |
| `test_multichannel.py` | Two simultaneous AM channels each produce independent audio |
| `test_nfm.py` | NFM demodulation produces correct-duration audio (NFM binary only) |
| `test_scan.py` | Scan mode stitches audio from two frequencies with a noise gap in between |
| `test_user_provided.py` | Dynamically generated tests from JSON files in `user_provided/` |

## Adding User-Defined Tests

You can run tests against your own IQ files — no Python required. Drop one or more JSON files into `user_provided/` and they are picked up automatically on the next test run.

Place your IQ files in `user_provided/` (already `.gitignore`d). IQ file paths in the JSON are resolved relative to the JSON file's directory, so a bare filename like `"my_recording.iq"` looks for the file alongside the JSON.

### Example JSON file

```json
{
  "test_cases": [
    {
      "name": "my_am_channel",
      "description": "120.025 MHz AM channel, 10 s recording",
      "binary": "non-nfm",
      "iq_file": "my_recording.iq",
      "sample_rate": 2048000,
      "centerfreq_hz": 120000000,
      "mode": "multichannel",
      "channels": [
        {
          "freq_hz": 120025000,
          "squelch": 0.0,
          "ctcss": null,
          "label": "ch1",
          "expected_audio_s": 10.0
        }
      ]
    },
    {
      "name": "my_nfm_channel",
      "description": "NFM channel — expect audio",
      "binary": "nfm",
      "iq_file": "nfm_recording.iq",
      "sample_rate": 2048000,
      "centerfreq_hz": 118000000,
      "mode": "multichannel",
      "channels": [
        {
          "freq_hz": 118000000,
          "squelch": 0.0,
          "ctcss": null,
          "label": "nfm_ch",
          "expected_audio_s": 5.0
        }
      ]
    },
    {
      "name": "ctcss_gate_closed",
      "description": "CTCSS mismatch → no output expected",
      "binary": "non-nfm",
      "iq_file": "ctcss_recording.iq",
      "sample_rate": 2048000,
      "centerfreq_hz": 120000000,
      "mode": "multichannel",
      "channels": [
        {
          "freq_hz": 120025000,
          "squelch": 0.0,
          "ctcss": 100.0,
          "label": "gated",
          "expected_audio_s": 0.0
        }
      ]
    }
  ]
}
```

### Running user-defined tests

```bash
cd system_tests
uv run pytest tests/test_user_provided.py \
    --binary ../builds/Release/src/rtl_airband \
    --nfm-binary ../builds/Release_nfm/src/rtl_airband \
    -v
```

All `*.json` files in `user_provided/` are loaded automatically. Test names must be unique across all JSON files in the directory.

### JSON field reference

**Top-level fields:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `test_cases` | array | yes | List of test case objects |

**Per test case:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | yes | Unique test identifier (used as pytest test ID) |
| `description` | string | no | Human-readable description |
| `binary` | `"non-nfm"` \| `"nfm"` \| `"both"` | yes | Which binary to use; `"both"` runs the test twice, once per binary |
| `iq_file` | string | yes | Path to IQ file (relative to the JSON file's directory) |
| `sample_rate` | integer | yes | IQ sample rate in Hz (e.g. `2048000`) |
| `centerfreq_hz` | integer | yes | SDR center frequency in Hz |
| `mode` | `"multichannel"` \| `"scan"` | yes | Device operating mode |
| `channels` | array | yes | List of channel objects |

**Per channel:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `freq_hz` | integer | yes | Channel frequency in Hz |
| `squelch` | float | yes | Squelch threshold (`0.0` to disable) |
| `ctcss` | float \| null | yes | CTCSS tone in Hz, or `null` to disable |
| `label` | string | yes | Short identifier used in output filenames |
| `expected_audio_s` | float | yes | Expected audio duration in seconds; `0.0` asserts silence |
| `scan_freqs_hz` | array of integers | scan mode only | List of frequencies to scan (required when `mode` is `"scan"`) |

## IQ File Format

The binary expects raw U8 IQ: interleaved 8-bit unsigned integers, I first then Q, offset-binary centered on 128. This is the native output format of RTL-SDR hardware and is also what `rtl_sdr` captures by default.

```
byte 0: I[0]   byte 1: Q[0]   byte 2: I[1]   byte 3: Q[1]  ...
```

### Capturing a recording with rtl_sdr

Use `rtl_sdr` to record directly to a `.iq` file:

```bash
sudo rtl_sdr -f 151102500 -g 7.7 -s 2400000 -d 0 -p -1 recording.iq
```

| Flag | Value | Meaning |
|------|-------|---------|
| `-f` | `151102500` | Center frequency in Hz (151.1025 MHz here) |
| `-g` | `7.7` | Tuner gain in dB (`0` for auto-gain) |
| `-s` | `2400000` | Sample rate in Hz (must be > 16000; 2.4 MHz shown) |
| `-d` | `0` | Device index (if you have multiple RTL-SDR dongles) |
| `-p` | `-1` | PPM frequency correction for your dongle |

Press Ctrl-C to stop. Then reference the file in your test JSON:

```json
{
  "iq_file": "recording.iq",
  "sample_rate": 2400000,
  "centerfreq_hz": 151102500
}
```

Set `centerfreq_hz` and `sample_rate` in the JSON to match the `-f` and `-s` values you used when capturing.

## Python Tooling

```bash
cd system_tests

# Format
uv run black .
uv run isort .

# Lint
uv run pylint conftest.py helpers/ tests/
```

Configuration for all three tools lives in `pyproject.toml`.

## Pre-commit Hooks

The pre-commit hooks for Python files (`black`, `isort`, `pylint`) are configured with `language: system` and invoke `uv run ...`. This means **`uv` must be in your `PATH`** when pre-commit runs. If you installed `uv` via the official installer it lands in `~/.local/bin/uv` — make sure that is on your PATH, or run `pre-commit` from inside the devcontainer (where `uv` is always available at `/usr/local/bin/uv`).
