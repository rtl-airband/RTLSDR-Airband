"""
test_am_squelch_closed.py — Noise only → squelch stays closed, output silent/absent.

Parametrized over all provided binaries (non-NFM and NFM if available).
Each parametrized case is identified as test_am_squelch_closed[non-nfm] or
test_am_squelch_closed[nfm].
"""

from pathlib import Path

from conftest import CACHE_DIR, BinaryUnderTest, run_rtl_airband
from helpers import config_writer, iq_generator, output_validator, stats_validator

SAMPLE_RATE = 2_048_000
CENTERFREQ_HZ = 120_000_000
CHANNEL_OFFSET_HZ = 25_000
DURATION_S = 10.0
SQUELCH = 5.0  # enabled — low-amplitude noise should stay below threshold
TIMEOUT_S = DURATION_S * 3 + 30  # 60s


def pytest_generate_tests(metafunc):
    """Parametrize test_am_squelch_closed over all available binaries."""
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins,
            ids=[b.label for b in am_bins],
        )


def test_am_squelch_closed(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    max_overrun_count: int,
    speedup_factor: float,
) -> None:
    """Noise-only IQ with squelch enabled → no MP3 file (or empty file) created."""
    iq_file = iq_generator.get_or_generate_noise(
        duration_s=DURATION_S,
        cache_dir=CACHE_DIR,
    )

    config_path = test_output_dir / "rtl_airband.conf"
    filename_template = "am_squelch_closed"

    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[
            {
                "freq_hz": CENTERFREQ_HZ + CHANNEL_OFFSET_HZ,
                "squelch": SQUELCH,
                "ctcss": None,
                "output_filename_template": filename_template,
            }
        ],
        output_dir=test_output_dir,
        speedup_factor=speedup_factor,
        mode="multichannel",
        mp3_tmp_dir=test_output_dir,
        stats_filepath=test_output_dir / "stats.txt",
    )

    run_rtl_airband(binary_under_test.path, config_path, timeout_s=TIMEOUT_S)

    output_validator.assert_mp3_silent(
        mp3_dir=test_output_dir,
        filename_template=filename_template,
    )

    stats = stats_validator.load(test_output_dir / "stats.txt")
    freq_hz = CENTERFREQ_HZ + CHANNEL_OFFSET_HZ
    assert (
        stats.channel("channel_squelch_counter", freq_hz) == 0
    ), "Squelch opened unexpectedly on noise-only signal"
    assert (
        stats.device("buffer_overflow_count") == 0
    ), "Unexpected device buffer overflow"
    stats_validator.assert_no_excessive_overruns(stats, max_overrun_count)
