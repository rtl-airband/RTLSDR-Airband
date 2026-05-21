"""
test_multichannel.py — Two simultaneous AM channels on one device.

IQ contains two AM signals at different frequency offsets. Verifies that both
output MP3s contain approximately the expected amount of audio.

Parametrized over all provided binaries (non-NFM and NFM if available).

TODO: Add mixer tests here once mixer support is implemented in the system tests.
      Mixer tests would verify that audio from multiple channels is correctly
      mixed and routed to combined outputs. The IQ fixture (two simultaneous
      AM channels) is already suitable for mixer testing.
"""

from pathlib import Path

from conftest import CACHE_DIR, BinaryUnderTest, run_rtl_airband
from helpers import config_writer, iq_generator, output_validator, stats_validator

SAMPLE_RATE = 2_048_000
CENTERFREQ_HZ = 120_000_000
CHANNEL_A_OFFSET_HZ = +25_000  # 120.025 MHz
CHANNEL_B_OFFSET_HZ = -25_000  # 119.975 MHz
AUDIO_TONE_HZ = 1_000
DURATION_S = 10.0
# The IQ fixture has NOISE_PAD_S of noise prepended and appended around the
# signal so the squelch can warm up and close cleanly around it.
TOTAL_IQ_DURATION_S = DURATION_S + 2 * iq_generator.NOISE_PAD_S  # 12 s
TIMEOUT_S = TOTAL_IQ_DURATION_S * 3 + 30  # 66 s


def pytest_generate_tests(metafunc):
    """Parametrize test_multichannel over all available binaries."""
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins,
            ids=[b.label for b in am_bins],
        )


def test_multichannel(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    mp3_tolerance: float,
    max_overrun_count: int,
    speedup_factor: float,
) -> None:
    """Two simultaneous AM channels → both MP3s must contain ≈10s of audio."""
    iq_file = iq_generator.get_or_generate_multichannel(
        offset_a_hz=CHANNEL_A_OFFSET_HZ,
        offset_b_hz=CHANNEL_B_OFFSET_HZ,
        audio_hz=AUDIO_TONE_HZ,
        duration_s=DURATION_S,
        cache_dir=CACHE_DIR,
    )

    config_path = test_output_dir / "rtl_airband.conf"

    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[
            {
                "freq_hz": CENTERFREQ_HZ + CHANNEL_A_OFFSET_HZ,
                "output_filename_template": "ch_a",
            },
            {
                "freq_hz": CENTERFREQ_HZ + CHANNEL_B_OFFSET_HZ,
                "output_filename_template": "ch_b",
            },
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
        filename_template="ch_a",
        expected_duration_s=DURATION_S,
        tolerance=mp3_tolerance,
    )
    output_validator.validate_mp3(
        mp3_dir=test_output_dir,
        filename_template="ch_b",
        expected_duration_s=DURATION_S,
        tolerance=mp3_tolerance,
    )

    stats = stats_validator.load(test_output_dir / "stats.txt")
    freq_a_hz = CENTERFREQ_HZ + CHANNEL_A_OFFSET_HZ
    freq_b_hz = CENTERFREQ_HZ + CHANNEL_B_OFFSET_HZ
    assert (
        stats.channel("channel_activity_counter", freq_a_hz) > 0
    ), "Expected non-zero activity counter for channel A"
    assert (
        stats.channel("channel_activity_counter", freq_b_hz) > 0
    ), "Expected non-zero activity counter for channel B"
    assert (
        stats.device("buffer_overflow_count") == 0
    ), "Unexpected device buffer overflow"
    stats_validator.assert_no_excessive_overruns(stats, max_overrun_count)
