"""
test_am_squelch_open.py — AM signal present → squelch opens, audio written.

Parametrized over all provided binaries (non-NFM and NFM if available).
Each parametrized case is identified as test_am_squelch_open[non-nfm] or
test_am_squelch_open[nfm].
"""

from pathlib import Path

from conftest import CACHE_DIR, BinaryUnderTest, run_rtl_airband
from helpers import config_writer, iq_generator, output_validator, stats_validator

SAMPLE_RATE = 2_048_000
CENTERFREQ_HZ = 120_000_000
CHANNEL_OFFSET_HZ = 25_000
AUDIO_TONE_HZ = 1_000
DURATION_S = 10.0
# The IQ fixture has NOISE_PAD_S of noise prepended and appended around the
# signal, so the squelch can warm up before the carrier arrives and close
# cleanly after it ends instead of racing input EOF.
TOTAL_IQ_DURATION_S = DURATION_S + 2 * iq_generator.NOISE_PAD_S  # 12 s
TIMEOUT_S = TOTAL_IQ_DURATION_S * 3 + 30  # 66 s


def pytest_generate_tests(metafunc):
    """Parametrize test_am_squelch_open over all available binaries."""
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins,
            ids=[b.label for b in am_bins],
        )


def test_am_squelch_open(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    mp3_tolerance: float,
    max_overrun_count: int,
    speedup_factor: float,
) -> None:
    """AM signal opens the squelch → MP3 must contain ≈10s of audio."""
    iq_file = iq_generator.get_or_generate_am(
        offset_hz=CHANNEL_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=DURATION_S,
        cache_dir=CACHE_DIR,
    )

    config_path = test_output_dir / "rtl_airband.conf"
    filename_template = "am_squelch_open"

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

    run_rtl_airband(binary_under_test.path, config_path, timeout_s=TIMEOUT_S)

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
    ), "Expected non-zero activity counter for AM channel opening the squelch"
    assert (
        stats.device("buffer_overflow_count") == 0
    ), "Unexpected device buffer overflow"
    stats_validator.assert_no_excessive_overruns(stats, max_overrun_count)
