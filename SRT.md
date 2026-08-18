# SRT output

RTLSDR-Airband can send audio over the [SRT protocol](https://www.srtalliance.org/).
Building with SRT support requires the development files for **libsrt**.
When configuring with CMake leave the `-DSRT` option enabled (default)
and ensure `pkg-config` can locate the library. If libsrt is missing the
feature is disabled automatically.

The SRT output supports three audio formats controlled by the `format`
setting in the configuration:

- `pcm` (default) – raw 16‑bit signed PCM (standard format)
- `mp3` – encoded using libmp3lame
- `wav` – 16‑bit PCM with WAV header so players like VLC auto-detect the format

For low latency playback with ffplay:

```bash
# For mp3 or wav formats (auto-detected):
ffplay -fflags nobuffer -flags low_delay srt://<host>:<port>

# For pcm format (match the configured sample_rate, default 8kHz mono):
ffplay -fflags nobuffer -flags low_delay -f s16le -ar 8000 -ac 1 srt://<host>:<port>
```

## Configuration

```
outputs: (
  {
    type = "srt";
    listen_address = "0.0.0.0";
    listen_port = 8890;
    format = "mp3";       # pcm|mp3|wav
    mode = "live";        # live|raw (default: live)
    sample_rate = 24000;  # optional, default: native (8000 or 16000)
    continuous = true;    # optional, default false
  }
);
```

`continuous` controls whether the stream pauses when the squelch is
closed. Set it to `true` if the receiving application does not handle
frequent reconnects well.

## SRT Mode

The `mode` setting controls SRT protocol behavior:

- `live` (default) – Standard SRT live mode with TSBPD (Timestamp-Based
  Packet Delivery), packet drop, and NAK reports enabled. Compatible with
  all SRT clients including gosrt, OBS, and other strict implementations.
  Adds approximately 120ms latency.

- `raw` – Minimal latency mode with TSBPD disabled. Only works with lenient
  clients like ffplay/ffmpeg. Use this if you need the absolute lowest
  latency and only use ffplay for playback.

## Sample Rate

The `sample_rate` setting resamples audio output to the specified rate
using linear interpolation. This applies to `pcm` and `wav` formats only
(`mp3` uses its own encoding rate).

The default is the native rate (`8000` Hz, or `16000` Hz with NFM).
Set to `24000` for use with the OpenAI Realtime API which requires
24 kHz mono 16-bit PCM.

