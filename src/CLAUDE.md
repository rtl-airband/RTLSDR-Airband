# src/ notes

## Device Modes

Each device operates in one of two modes, set via `mode = "multichannel"` (default) or `mode = "scan"` in config.

**`R_MULTICHANNEL`** — The SDR is tuned to a fixed center frequency and multiple channels are demodulated simultaneously from the same wideband capture. Each channel has a single `freq` value that must fall within the SDR's bandwidth. This is the common case for monitoring several frequencies at once.

**`R_SCAN`** — The device has exactly one channel, but that channel holds a `freqs` list of frequencies to cycle through. A controller thread monitors the squelch: after ~2 seconds of no signal (10 × 200 ms polls), it retunes the SDR hardware to the next frequency via `input_set_centerfreq()`. When a signal is detected, it stays on the current frequency. Per-frequency settings (labels, `squelch_threshold`/`squelch_snr_threshold`, `modulations`, `notch`/`notch_q`, `ctcss`, `bandwidth`, `ampfactor`) are **parallel lists** to `freqs` (same index = same frequency), not a list of objects — `freqs` is parsed as numbers. Only one channel per device in scan mode. (`rtl_airband.cpp:101-140`, `config.cpp:312-729`)

## Threading Model

- **RX thread** (1 per device, always) — reads SDR samples into the circular buffer (`input-common.cpp`)
- **Controller thread** (1 per device, `R_SCAN` mode only) — scanning/squelch state machine for devices that scan across frequencies; not created for `R_MULTICHANNEL` devices (`rtl_airband.cpp:1005-1013`)
- **Demod thread** (1 total by default; 1 per device if `multiple_demod_threads=true` in config) — FFT, demodulation, filter, CTCSS, squelch, AGC for all assigned devices (`rtl_airband.cpp:1044`)
- **Output thread** (1 total by default; 1 per device + 1 for mixers if `multiple_output_threads=true`) — MP3 encoding and streaming
- **Mixer thread** (1 total, only if any mixers are configured) — processes all mixers; not per-mixer (`rtl_airband.cpp:1091-1092`)

## Configuration Format

Config files use libconfig++ syntax. Sample configs in `config/`. Top-level sections:

```
devices: ( { type = "rtlsdr"; centerfreq = 120.0; gain = 25;
             channels: ( { freq = 119.5; modulation = "AM";
                           outputs: ( { type = "icecast"; ... } ); } ); } );
mixers: { mix1: { outputs: ( { type = "icecast"; ... } ); } };
```

Mixers have **no `inputs` section** — a channel connects to a mixer implicitly via an output `type = "mixer"` + `name` (+ optional `ampfactor`, `balance`), see `mixer_connect_input` (`src/config.cpp:168-193`, `src/mixer.cpp:57`).

Output types: `icecast`, `file`, `rawfile`, `udp_stream`, `mixer`, `pulse`.

`file`/`rawfile` outputs with `split_on_transmission = true` can set `split_include_transmission_start` (per output > global > default `false`; the global is parsed in `main()` in `src/rtl_airband.cpp`, the per-output value in `parse_split_file_times` in `src/config.cpp`). It changes only file names, never when files close. A transmission cut by `split_max_file_time` is named `<template>_<tx start>_<part start>[_<freq>]<suffix>`; part 1 repeats the tx start, and unsplit transmissions keep `<template>_<start>`. Names are built by `build_output_filename` (`src/output.cpp`). Part 1 is written under the standalone `.tmp` name and renamed to the part name in `close_if_necessary` when it is split; parts 2+ open under the part name, in the tx start's dated folder. A file that reaches max time while squelch is closed ends the transmission, as it always closed there. Examples: `config/split_include_transmission_start*.conf`.

`split_tx_drop_shorter_than` (seconds, global or per output, default `0` = off, must be `< split_max_file_time`) deletes the `.tmp` file instead of renaming it when a transmission that fit in one file spans (first to last write) less than the limit; every part of a split transmission is kept. The check is in `close_if_necessary` (`should_drop_split_tx`), so files closed at shutdown are never dropped. Drops are counted per channel frequency in the stats file as `channel_split_tx_dropped_counter`.
