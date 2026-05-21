"""
test_nfm.py — NFM demodulation end-to-end test.

Uses the NFM binary only (--nfm-binary). Skipped if --nfm-binary is not provided.
IQ: narrow FM signal at +25 kHz offset, ±3 kHz deviation, 1 kHz audio tone, 10s.
Expected: MP3 contains ≈10s of audio.
"""

from pathlib import Path

import pytest
from conftest import CACHE_DIR, run_rtl_airband
from helpers import config_writer, iq_generator, output_validator, stats_validator

SAMPLE_RATE = 2_048_000
CENTERFREQ_HZ = 120_000_000
CHANNEL_OFFSET_HZ = 25_000
AUDIO_TONE_HZ = 1_000
DURATION_S = 10.0
# The IQ fixture has NOISE_PAD_S of noise prepended and appended around the
# signal so the squelch can warm up and close cleanly around it.
TOTAL_IQ_DURATION_S = DURATION_S + 2 * iq_generator.NOISE_PAD_S  # 12 s
TIMEOUT_S = TOTAL_IQ_DURATION_S * 3 + 30  # 66 s


def test_nfm(
    nfm_binary,
    test_output_dir: Path,
    mp3_tolerance: float,
    max_overrun_count: int,
    speedup_factor: float,
) -> None:
    """
    NFM demodulation: narrow FM signal → MP3 must contain ≈10s of audio.
    Skipped if --nfm-binary is not provided.
    """
    if nfm_binary is None:
        pytest.skip("No NFM binary provided via --nfm-binary")

    iq_file = iq_generator.get_or_generate_nfm(
        offset_hz=CHANNEL_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=DURATION_S,
        cache_dir=CACHE_DIR,
    )

    config_path = test_output_dir / "rtl_airband.conf"
    filename_template = "nfm_out"

    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[
            {
                "freq_hz": CENTERFREQ_HZ + CHANNEL_OFFSET_HZ,
                "output_filename_template": filename_template,
            }
        ],
        output_dir=test_output_dir,
        speedup_factor=speedup_factor,
        mode="multichannel",
        mp3_tmp_dir=test_output_dir,
        stats_filepath=test_output_dir / "stats.txt",
    )

    run_rtl_airband(nfm_binary, config_path, timeout_s=TIMEOUT_S)

    output_validator.validate_mp3(
        mp3_dir=test_output_dir,
        filename_template=filename_template,
        expected_duration_s=DURATION_S,
        tolerance=mp3_tolerance,
    )

    stats = stats_validator.load(test_output_dir / "stats.txt")
    freq_hz = CENTERFREQ_HZ + CHANNEL_OFFSET_HZ
    assert (
        stats.channel("channel_activity_counter", freq_hz) > 0
    ), "Expected non-zero activity counter for NFM channel"
    assert (
        stats.device("buffer_overflow_count") == 0
    ), "Unexpected device buffer overflow"
    stats_validator.assert_no_excessive_overruns(stats, max_overrun_count)
