"""
IQ fixture generator for RTLSDR-Airband system tests.

Generates U8 (unsigned 8-bit interleaved I/Q) files with known signals.
Each signal-bearing fixture is wrapped in NOISE_PAD_S of low-amplitude Gaussian
noise at the start and end so the squelch / AGC / IIR filter state has time to
converge before the carrier appears and so the demod and output pipeline can
drain cleanly before the input thread hits EOF.

Files are cached in .generated_input/; the cache filename embeds NOISE_PAD_S so
changing the pad invalidates old fixtures. If a file already exists it is reused.

The cache can be bounded to a byte budget via set_cache_budget() (wired to the
--generated-input-max-bytes CLI option). When set, the least-recently-used
fixtures are evicted before a new one is written so the on-disk total stays
within budget — useful when the cache lives on a small tmpfs. Default is
unlimited (no eviction).
"""

import os
from collections import OrderedDict
from pathlib import Path

import numpy as np

SAMPLE_RATE = 2_048_000  # Hz — common RTL-SDR rate
CENTERFREQ = 120_000_000  # Hz — aviation band (used in config, not physics)

# In scan mode, rtl_airband tunes the hardware center to (target_freq + 20 * bin_resolution).
# Combined with the -1.0 correction in the bin formula, the target frequency always lands at
# bin (fft_size - 21).  For the default fft_size=512 that is bin 491, which sits at offset
# -(21 × bin_resolution) = -84 kHz from center.
#
# If DEFAULT_FFT_SIZE_LOG or the +20 tuning offset in ever change, update _FFT_SIZE and the
# formula below to match — stale values will silently place the signal at the wrong bin and
# scan tests will fail with unexpected MP3 durations. Delete .generated_input/ after any
# such change so fixtures are regenerated.
_FFT_SIZE = 512  # 1 << DEFAULT_FFT_SIZE_LOG
_BIN_RES_HZ = SAMPLE_RATE // _FFT_SIZE  # 4 000 Hz per bin
SCAN_DEMOD_OFFSET_HZ = -21 * _BIN_RES_HZ  # -84 000 Hz

# Leading + trailing low-amplitude noise pad on every signal fixture. The lead
# gives the squelch noise-floor tracker, AGC `agcavgfast`, and IIR filter state
# time to converge before the carrier arrives, so squelch open is governed by
# the deterministic open_delay (~200 audio samples) instead of warm-up race.
# The tail lets the squelch close on noise and the demod/output/MP3 pipeline
# drain cleanly before the input thread hits EOF.
NOISE_PAD_S = 1.0
_NOISE_AMPLITUDE_U8 = np.float32(0.02 * 127.5)
_NOISE_SEED = 42

_TWO_PI = np.float32(2 * np.pi)
_SCALE = np.float32(0.5 * 127.5)
_ORIGIN = np.float32(128)


# ---------------------------------------------------------------------------
# LRU cache budget
#
# When _cache_max_bytes is None the cache is unlimited and nothing is ever
# evicted (original behavior). When set, _evict_for() removes least-recently-
# used fixtures before a new one is written so the on-disk total stays within
# budget. Recency is tracked in _lru (insertion order = LRU..MRU); a cache hit
# or a fresh write marks that file most-recently-used via _touch().
#
# State is process-global and reset per session by set_cache_budget(). A budget
# assumes a single generator process; conftest rejects a budget under xdist.
# ---------------------------------------------------------------------------

_cache_max_bytes: int | None = None
_lru: "OrderedDict[str, None]" = OrderedDict()
_seeded_dirs: set[Path] = set()


def set_cache_budget(max_bytes: int | None) -> None:
    """Set the cache byte budget (None = unlimited) and reset recency tracking."""
    global _cache_max_bytes  # pylint: disable=global-statement
    _cache_max_bytes = max_bytes
    _lru.clear()
    _seeded_dirs.clear()


def _touch(path: Path) -> None:
    """Mark *path* most-recently-used.

    Seeds pre-existing on-disk fixtures first (when a budget is active) so a
    cache hit before the session's first write can't jump ahead of older files
    in the LRU order.
    """
    if _cache_max_bytes is not None:
        _seed(path.parent)
    _lru.pop(path.name, None)
    _lru[path.name] = None


def _register_hit(path: Path) -> Path:
    """Record a cache hit and return the path (keeps reused fixtures warm)."""
    _touch(path)
    return path


def _seed(cache_dir: Path) -> None:
    """Populate the LRU from fixtures already on disk, oldest first.

    Runs once per directory so eviction works even without a --clean run that
    starts from an empty cache. Ordered by mtime so pre-existing files evict
    oldest-first.
    """
    if cache_dir in _seeded_dirs:
        return
    _seeded_dirs.add(cache_dir)
    existing = [f for f in cache_dir.glob("*.iq") if f.is_file()]
    for f in sorted(existing, key=lambda p: p.stat().st_mtime):
        if f.name not in _lru:
            _lru[f.name] = None


def _current_bytes(cache_dir: Path) -> int:
    return sum(f.stat().st_size for f in cache_dir.glob("*.iq") if f.is_file())


def _evict_for(cache_dir: Path, incoming_bytes: int, protect_name: str) -> None:
    """Evict LRU fixtures until *incoming_bytes* fits within the budget.

    A file larger than the whole budget is still written (after evicting
    everything else) — the current fixture is always needed.
    """
    if _cache_max_bytes is None:
        return
    _seed(cache_dir)
    total = _current_bytes(cache_dir)
    for name in list(_lru.keys()):
        if total + incoming_bytes <= _cache_max_bytes:
            break
        if name == protect_name:
            # Defensive: callers only write not-yet-existing files, so the
            # incoming name is never seeded into _lru — this branch never fires.
            continue
        victim = cache_dir / name
        if victim.exists():
            total -= victim.stat().st_size
            victim.unlink()
        _lru.pop(name, None)


def _write_iq(path: Path, I_u8: np.ndarray, Q_u8: np.ndarray) -> None:
    """Interleave I/Q arrays and write as raw bytes, honoring the cache budget.

    The write is atomic (temp file + os.replace) so a concurrent reader — e.g.
    another xdist worker whose rtl_airband is opening this same shared fixture —
    never sees a partially written file. The temp name avoids the *.iq glob so
    it isn't counted toward the budget or picked up as a fixture.
    """
    iq = np.column_stack([I_u8, Q_u8]).flatten()
    data = iq.tobytes()
    _evict_for(path.parent, len(data), path.name)
    tmp = path.with_name(f"{path.name}.tmp.{os.getpid()}")
    tmp.write_bytes(data)
    os.replace(tmp, path)
    _touch(path)


def _quantize(signal: np.ndarray, scale: np.float32 = _SCALE) -> np.ndarray:
    return np.clip(np.round(_ORIGIN + signal * scale), 0, 255).astype(np.uint8)


def _noise_arrays(
    duration_s: float, rng: np.random.Generator
) -> tuple[np.ndarray, np.ndarray]:
    """Generate (I_u8, Q_u8) of low-amplitude Gaussian noise."""
    n = int(SAMPLE_RATE * duration_s)
    I = rng.standard_normal(n, dtype=np.float32) * _NOISE_AMPLITUDE_U8
    Q = rng.standard_normal(n, dtype=np.float32) * _NOISE_AMPLITUDE_U8
    I_u8 = np.clip(np.round(_ORIGIN + I), 0, 255).astype(np.uint8)
    Q_u8 = np.clip(np.round(_ORIGIN + Q), 0, 255).astype(np.uint8)
    return I_u8, Q_u8


def _pad_with_noise(
    I_sig: np.ndarray, Q_sig: np.ndarray
) -> tuple[np.ndarray, np.ndarray]:
    """Prepend and append NOISE_PAD_S of noise around the signal arrays."""
    rng = np.random.default_rng(seed=_NOISE_SEED)
    I_lead, Q_lead = _noise_arrays(NOISE_PAD_S, rng)
    I_tail, Q_tail = _noise_arrays(NOISE_PAD_S, rng)
    return (
        np.concatenate([I_lead, I_sig, I_tail]),
        np.concatenate([Q_lead, Q_sig, Q_tail]),
    )


def get_or_generate_am(
    offset_hz: int,
    audio_hz: int,
    duration_s: float,
    cache_dir: Path,
) -> Path:
    """
    Generate an AM signal at *offset_hz* from center with *audio_hz* audio tone.
    Returns path to the cached .iq file.
    """
    filename = (
        f"am_sr{SAMPLE_RATE}_off{offset_hz}_audio{audio_hz}"
        f"_dur{duration_s}_pad{NOISE_PAD_S}.iq"
    )
    path = cache_dir / filename
    if path.exists():
        return _register_hit(path)

    num_samples = int(SAMPLE_RATE * duration_s)
    t = np.linspace(0, duration_s, num_samples, dtype=np.float32, endpoint=False)
    audio = np.sin(_TWO_PI * np.float32(audio_hz) * t)
    envelope = np.float32(1.0) + np.float32(0.8) * audio
    del audio
    carrier_phase = _TWO_PI * np.float32(offset_hz) * t
    del t
    I = envelope * np.cos(carrier_phase)
    Q = envelope * np.sin(carrier_phase)
    del carrier_phase, envelope
    I_all, Q_all = _pad_with_noise(_quantize(I), _quantize(Q))
    _write_iq(path, I_all, Q_all)
    return path


def get_or_generate_noise(
    duration_s: float,
    cache_dir: Path,
) -> Path:
    """
    Generate a low-amplitude Gaussian noise signal (squelch-closed fixture).
    Returns path to the cached .iq file.
    """
    filename = f"noise_sr{SAMPLE_RATE}_dur{duration_s}.iq"
    path = cache_dir / filename
    if path.exists():
        return _register_hit(path)

    num_samples = int(SAMPLE_RATE * duration_s)
    rng = np.random.default_rng(seed=42)
    amplitude = np.float32(0.02 * 127.5)
    I = rng.standard_normal(num_samples, dtype=np.float32) * amplitude
    Q = rng.standard_normal(num_samples, dtype=np.float32) * amplitude
    I_u8 = np.clip(np.round(_ORIGIN + I), 0, 255).astype(np.uint8)
    Q_u8 = np.clip(np.round(_ORIGIN + Q), 0, 255).astype(np.uint8)
    _write_iq(path, I_u8, Q_u8)
    return path


def get_or_generate_ctcss(
    offset_hz: int,
    ctcss_hz: float,
    duration_s: float,
    cache_dir: Path,
) -> Path:
    """
    Generate an AM signal with a CTCSS sub-audible tone mixed into the audio.
    Returns path to the cached .iq file.
    """
    filename = (
        f"ctcss_sr{SAMPLE_RATE}_off{offset_hz}_ctcss{ctcss_hz}"
        f"_dur{duration_s}_pad{NOISE_PAD_S}.iq"
    )
    path = cache_dir / filename
    if path.exists():
        return _register_hit(path)

    num_samples = int(SAMPLE_RATE * duration_s)
    t = np.linspace(0, duration_s, num_samples, dtype=np.float32, endpoint=False)
    audio = np.float32(0.3) * np.sin(_TWO_PI * np.float32(ctcss_hz) * t) + np.float32(
        0.7
    ) * np.sin(_TWO_PI * np.float32(1000) * t)
    envelope = np.float32(1.0) + np.float32(0.8) * audio
    del audio
    carrier_phase = _TWO_PI * np.float32(offset_hz) * t
    del t
    I = envelope * np.cos(carrier_phase)
    Q = envelope * np.sin(carrier_phase)
    del carrier_phase, envelope
    I_all, Q_all = _pad_with_noise(_quantize(I), _quantize(Q))
    _write_iq(path, I_all, Q_all)
    return path


def get_or_generate_nfm(
    offset_hz: int,
    audio_hz: int,
    duration_s: float,
    cache_dir: Path,
) -> Path:
    """
    Generate an NFM signal at *offset_hz* with *audio_hz* audio tone.
    Returns path to the cached .iq file.
    """
    filename = (
        f"nfm_sr{SAMPLE_RATE}_off{offset_hz}_audio{audio_hz}"
        f"_dur{duration_s}_pad{NOISE_PAD_S}.iq"
    )
    path = cache_dir / filename
    if path.exists():
        return _register_hit(path)

    deviation = 3000  # Hz, narrow FM ±3 kHz
    num_samples = int(SAMPLE_RATE * duration_s)
    t = np.linspace(0, duration_s, num_samples, dtype=np.float32, endpoint=False)
    audio = np.sin(_TWO_PI * np.float32(audio_hz) * t)
    del t
    instantaneous_freq = np.float32(offset_hz) + np.float32(deviation) * audio
    del audio
    # FM phase modulation: integrate instantaneous frequency to get phase.
    # cumsum must use float64 to avoid phase drift over millions of samples.
    phase = _TWO_PI * np.cumsum(instantaneous_freq, dtype=np.float64) / SAMPLE_RATE
    del instantaneous_freq
    I = np.cos(phase).astype(np.float32)
    Q = np.sin(phase).astype(np.float32)
    del phase
    I_all, Q_all = _pad_with_noise(_quantize(I), _quantize(Q))
    _write_iq(path, I_all, Q_all)
    return path


def get_or_generate_multichannel(
    offset_a_hz: int,
    offset_b_hz: int,
    audio_hz: int,
    duration_s: float,
    cache_dir: Path,
) -> Path:
    """
    Generate a combined AM signal with two simultaneous channels.
    Channel A at offset_a_hz and channel B at offset_b_hz, both with audio_hz tone.
    Returns path to the cached .iq file.
    """
    filename = (
        f"multichannel_sr{SAMPLE_RATE}_offA{offset_a_hz}_offB{offset_b_hz}"
        f"_audio{audio_hz}_dur{duration_s}_pad{NOISE_PAD_S}.iq"
    )
    path = cache_dir / filename
    if path.exists():
        return _register_hit(path)

    num_samples = int(SAMPLE_RATE * duration_s)
    t = np.linspace(0, duration_s, num_samples, dtype=np.float32, endpoint=False)
    audio = np.sin(_TWO_PI * np.float32(audio_hz) * t)
    envelope = np.float32(1.0) + np.float32(0.8) * audio
    del audio
    carrier_phase_a = _TWO_PI * np.float32(offset_a_hz) * t
    carrier_phase_b = _TWO_PI * np.float32(offset_b_hz) * t
    del t
    # Combine both channels directly to avoid holding four separate channel arrays.
    I = envelope * (np.cos(carrier_phase_a) + np.cos(carrier_phase_b)) * np.float32(0.5)
    Q = envelope * (np.sin(carrier_phase_a) + np.sin(carrier_phase_b)) * np.float32(0.5)
    del carrier_phase_a, carrier_phase_b, envelope
    I_all, Q_all = _pad_with_noise(_quantize(I), _quantize(Q))
    _write_iq(path, I_all, Q_all)
    return path


def get_or_generate_scan(
    duration_a_s: float,
    gap_s: float,
    duration_b_s: float,
    cache_dir: Path,
) -> Path:
    """
    Generate a three-segment scan fixture at the FFT bin rtl_airband uses in scan mode.

    Both signal segments are placed at SCAN_DEMOD_OFFSET_HZ from center — the single
    bin the scanner always demodulates regardless of which scan frequency is "active".

      Segment 1: AM signal for duration_a_s  (scanner locked on freq A)
      Segment 2: noise for gap_s             (scanner switches A → B)
      Segment 3: AM signal for duration_b_s  (scanner locked on freq B)

    Returns path to the cached .iq file.
    """
    filename = (
        f"scan_sr{SAMPLE_RATE}_demod{SCAN_DEMOD_OFFSET_HZ}"
        f"_durA{duration_a_s}_gap{gap_s}_durB{duration_b_s}_pad{NOISE_PAD_S}.iq"
    )
    path = cache_dir / filename
    if path.exists():
        return _register_hit(path)

    rng = np.random.default_rng(seed=_NOISE_SEED)

    def _am_segment(duration_s: float) -> tuple[np.ndarray, np.ndarray]:
        n = int(SAMPLE_RATE * duration_s)
        t = np.linspace(0, duration_s, n, dtype=np.float32, endpoint=False)
        audio = np.sin(_TWO_PI * np.float32(1000) * t)
        envelope = np.float32(1.0) + np.float32(0.8) * audio
        del audio
        carrier_phase = _TWO_PI * np.float32(SCAN_DEMOD_OFFSET_HZ) * t
        del t
        I = envelope * np.cos(carrier_phase)
        Q = envelope * np.sin(carrier_phase)
        del carrier_phase, envelope
        return _quantize(I), _quantize(Q)

    I_lead, Q_lead = _noise_arrays(NOISE_PAD_S, rng)
    I_a, Q_a = _am_segment(duration_a_s)
    I_gap, Q_gap = _noise_arrays(gap_s, rng)
    I_b, Q_b = _am_segment(duration_b_s)
    I_tail, Q_tail = _noise_arrays(NOISE_PAD_S, rng)

    I_all = np.concatenate([I_lead, I_a, I_gap, I_b, I_tail])
    Q_all = np.concatenate([Q_lead, Q_a, Q_gap, Q_b, Q_tail])
    _write_iq(path, I_all, Q_all)
    return path
