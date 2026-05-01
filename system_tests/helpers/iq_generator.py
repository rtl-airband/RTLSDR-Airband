"""
IQ fixture generator for RTLSDR-Airband system tests.

Generates U8 (unsigned 8-bit interleaved I/Q) files with known signals.
Files are cached in .generated_input/; if a file already exists it is reused.
"""

from pathlib import Path

import numpy as np

SAMPLE_RATE = 2_048_000  # Hz — common RTL-SDR rate
CENTERFREQ = 120_000_000  # Hz — aviation band (used in config, not physics)

# In scan mode rtl_airband always demodulates from a fixed FFT bin, computed as:
#   bin = fft_size - 21  (for DEFAULT_FFT_SIZE_LOG=9, fft_size=512 → bin 491)
# That bin corresponds to an offset of -(21 × bin_resolution) = -84 kHz from the
# IQ file center, regardless of which scan frequency is "active".  Signals in
# scan-mode test fixtures must sit at this offset so the scanner can detect them.
_FFT_SIZE = 512  # 1 << DEFAULT_FFT_SIZE_LOG
_BIN_RES_HZ = SAMPLE_RATE // _FFT_SIZE  # 4 000 Hz per bin
SCAN_DEMOD_OFFSET_HZ = -21 * _BIN_RES_HZ  # -84 000 Hz


def _write_iq(path: Path, I_u8: np.ndarray, Q_u8: np.ndarray) -> None:
    """Interleave I/Q arrays and write as raw bytes."""
    iq = np.column_stack([I_u8, Q_u8]).flatten()
    path.write_bytes(iq.tobytes())


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
    t = np.arange(num_samples) / SAMPLE_RATE
    audio = np.sin(2 * np.pi * audio_hz * t)
    envelope = 1.0 + 0.8 * audio  # 80% modulation index
    carrier_phase = 2 * np.pi * offset_hz * t
    I = envelope * np.cos(carrier_phase)
    Q = envelope * np.sin(carrier_phase)
    scale = 0.5 * 127.5
    I_u8 = np.clip(np.round(128 + I * scale), 0, 255).astype(np.uint8)
    Q_u8 = np.clip(np.round(128 + Q * scale), 0, 255).astype(np.uint8)
    _write_iq(path, I_u8, Q_u8)
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
    I = rng.normal(0, 0.02 * 127.5, num_samples)
    Q = rng.normal(0, 0.02 * 127.5, num_samples)
    I_u8 = np.clip(np.round(128 + I), 0, 255).astype(np.uint8)
    Q_u8 = np.clip(np.round(128 + Q), 0, 255).astype(np.uint8)
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
    t = np.arange(num_samples) / SAMPLE_RATE
    # Mix CTCSS sub-audible tone with voice tone
    audio = 0.3 * np.sin(2 * np.pi * ctcss_hz * t) + 0.7 * np.sin(2 * np.pi * 1000 * t)
    envelope = 1.0 + 0.8 * audio  # 80% modulation index
    carrier_phase = 2 * np.pi * offset_hz * t
    I = envelope * np.cos(carrier_phase)
    Q = envelope * np.sin(carrier_phase)
    scale = 0.5 * 127.5
    I_u8 = np.clip(np.round(128 + I * scale), 0, 255).astype(np.uint8)
    Q_u8 = np.clip(np.round(128 + Q * scale), 0, 255).astype(np.uint8)
    _write_iq(path, I_u8, Q_u8)
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
    t = np.arange(num_samples) / SAMPLE_RATE
    audio = np.sin(2 * np.pi * audio_hz * t)
    # FM phase modulation: integrate instantaneous frequency to get phase
    instantaneous_freq = offset_hz + deviation * audio
    phase = 2 * np.pi * np.cumsum(instantaneous_freq) / SAMPLE_RATE
    I = np.cos(phase)
    Q = np.sin(phase)
    scale = 0.5 * 127.5
    I_u8 = np.clip(np.round(128 + I * scale), 0, 255).astype(np.uint8)
    Q_u8 = np.clip(np.round(128 + Q * scale), 0, 255).astype(np.uint8)
    _write_iq(path, I_u8, Q_u8)
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
    t = np.arange(num_samples) / SAMPLE_RATE
    audio = np.sin(2 * np.pi * audio_hz * t)
    envelope = 1.0 + 0.8 * audio

    # Channel A
    carrier_phase_a = 2 * np.pi * offset_a_hz * t
    I_a = envelope * np.cos(carrier_phase_a)
    Q_a = envelope * np.sin(carrier_phase_a)

    # Channel B
    carrier_phase_b = 2 * np.pi * offset_b_hz * t
    I_b = envelope * np.cos(carrier_phase_b)
    Q_b = envelope * np.sin(carrier_phase_b)

    # Sum both channels and scale — divide by 2 to avoid overflow
    I = (I_a + I_b) / 2.0
    Q = (Q_a + Q_b) / 2.0
    scale = 0.5 * 127.5
    I_u8 = np.clip(np.round(128 + I * scale), 0, 255).astype(np.uint8)
    Q_u8 = np.clip(np.round(128 + Q * scale), 0, 255).astype(np.uint8)
    _write_iq(path, I_u8, Q_u8)
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

    scale = 0.5 * 127.5
    rng = np.random.default_rng(seed=42)

    def _am_segment(duration_s: float) -> tuple[np.ndarray, np.ndarray]:
        n = int(SAMPLE_RATE * duration_s)
        t = np.arange(n) / SAMPLE_RATE
        audio = np.sin(2 * np.pi * 1000 * t)
        envelope = 1.0 + 0.8 * audio
        carrier_phase = 2 * np.pi * SCAN_DEMOD_OFFSET_HZ * t
        I = envelope * np.cos(carrier_phase)
        Q = envelope * np.sin(carrier_phase)
        I_u8 = np.clip(np.round(128 + I * scale), 0, 255).astype(np.uint8)
        Q_u8 = np.clip(np.round(128 + Q * scale), 0, 255).astype(np.uint8)
        return I_u8, Q_u8

    def _noise_segment(duration_s: float) -> tuple[np.ndarray, np.ndarray]:
        n = int(SAMPLE_RATE * duration_s)
        I = rng.normal(0, 0.02 * 127.5, n)
        Q = rng.normal(0, 0.02 * 127.5, n)
        I_u8 = np.clip(np.round(128 + I), 0, 255).astype(np.uint8)
        Q_u8 = np.clip(np.round(128 + Q), 0, 255).astype(np.uint8)
        return I_u8, Q_u8

    I_a, Q_a = _am_segment(duration_a_s)
    I_gap, Q_gap = _noise_segment(gap_s)
    I_b, Q_b = _am_segment(duration_b_s)

    I_all = np.concatenate([I_a, I_gap, I_b])
    Q_all = np.concatenate([Q_a, Q_gap, Q_b])
    _write_iq(path, I_all, Q_all)
    return path
