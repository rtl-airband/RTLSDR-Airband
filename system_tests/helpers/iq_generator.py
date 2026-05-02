"""
IQ fixture generator for RTLSDR-Airband system tests.

Generates U8 (unsigned 8-bit interleaved I/Q) files with known signals.
Files are cached in .generated_input/; if a file already exists it is reused.
"""

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
# scan tests will fail with unexpected rawfile sizes. Delete .generated_input/ after any
# such change so fixtures are regenerated.
_FFT_SIZE = 512  # 1 << DEFAULT_FFT_SIZE_LOG
_BIN_RES_HZ = SAMPLE_RATE // _FFT_SIZE  # 4 000 Hz per bin
SCAN_DEMOD_OFFSET_HZ = -21 * _BIN_RES_HZ  # -84 000 Hz

_TWO_PI = np.float32(2 * np.pi)
_SCALE = np.float32(0.5 * 127.5)
_ORIGIN = np.float32(128)


def _write_iq(path: Path, I_u8: np.ndarray, Q_u8: np.ndarray) -> None:
    """Interleave I/Q arrays and write as raw bytes."""
    iq = np.column_stack([I_u8, Q_u8]).flatten()
    path.write_bytes(iq.tobytes())


def _quantize(signal: np.ndarray, scale: np.float32 = _SCALE) -> np.ndarray:
    return np.clip(np.round(_ORIGIN + signal * scale), 0, 255).astype(np.uint8)


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
    filename = f"am_sr{SAMPLE_RATE}_off{offset_hz}_audio{audio_hz}_dur{duration_s}.iq"
    path = cache_dir / filename
    if path.exists():
        return path

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
    _write_iq(path, _quantize(I), _quantize(Q))
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
        return path

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
        f"ctcss_sr{SAMPLE_RATE}_off{offset_hz}_ctcss{ctcss_hz}_dur{duration_s}.iq"
    )
    path = cache_dir / filename
    if path.exists():
        return path

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
    _write_iq(path, _quantize(I), _quantize(Q))
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
    filename = f"nfm_sr{SAMPLE_RATE}_off{offset_hz}_audio{audio_hz}_dur{duration_s}.iq"
    path = cache_dir / filename
    if path.exists():
        return path

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
    _write_iq(path, _quantize(I), _quantize(Q))
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
        f"_audio{audio_hz}_dur{duration_s}.iq"
    )
    path = cache_dir / filename
    if path.exists():
        return path

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
    _write_iq(path, _quantize(I), _quantize(Q))
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
        f"_durA{duration_a_s}_gap{gap_s}_durB{duration_b_s}.iq"
    )
    path = cache_dir / filename
    if path.exists():
        return path

    rng = np.random.default_rng(seed=42)

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

    def _noise_segment(duration_s: float) -> tuple[np.ndarray, np.ndarray]:
        n = int(SAMPLE_RATE * duration_s)
        amplitude = np.float32(0.02 * 127.5)
        I = rng.standard_normal(n, dtype=np.float32) * amplitude
        Q = rng.standard_normal(n, dtype=np.float32) * amplitude
        I_u8 = np.clip(np.round(_ORIGIN + I), 0, 255).astype(np.uint8)
        Q_u8 = np.clip(np.round(_ORIGIN + Q), 0, 255).astype(np.uint8)
        return I_u8, Q_u8

    I_a, Q_a = _am_segment(duration_a_s)
    I_gap, Q_gap = _noise_segment(gap_s)
    I_b, Q_b = _am_segment(duration_b_s)

    I_all = np.concatenate([I_a, I_gap, I_b])
    Q_all = np.concatenate([Q_a, Q_gap, Q_b])
    _write_iq(path, I_all, Q_all)
    return path
