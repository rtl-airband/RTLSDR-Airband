"""
test_ctcss.py — CTCSS gate: correct tone passes, wrong tone is blocked.

Two test functions, both parametrized over all provided binaries.

test_ctcss_correct_tone: IQ with 100.0 Hz CTCSS + 1000 Hz voice, config asks for
  100.0 Hz → output should contain audio (accounting for ~2s detection startup delay).

test_ctcss_wrong_tone: IQ with 125.0 Hz CTCSS + 1000 Hz voice, config asks for
  100.0 Hz → output should be absent or empty (gate stays closed).

125.0 Hz was chosen as the wrong tone because it maps to a clearly distinct Goertzel
bin (k=6) in both the fast (0.05s) and slow (0.4s) CTCSS detectors at 8 kHz and
16 kHz audio rates, avoiding the ambiguity that 110.0 Hz (exactly k=5.5) would cause.
"""

from pathlib import Path

from conftest import CACHE_DIR, BinaryUnderTest, run_rtl_airband
from helpers import config_writer, iq_generator, output_validator, stats_validator

SAMPLE_RATE = 2_048_000
CENTERFREQ_HZ = 120_000_000
CHANNEL_OFFSET_HZ = 25_000
DURATION_S = 15.0
SQUELCH = 0.0  # disabled — CTCSS gate is the only gate
CONFIG_CTCSS_HZ = 100.0  # what the config requests
CORRECT_CTCSS_HZ = 100.0  # matches the config → should pass
WRONG_CTCSS_HZ = 125.0  # not a standard CTCSS tone, does not match → should block
TIMEOUT_S = DURATION_S * 3 + 30  # 75s


def pytest_generate_tests(metafunc):
    """Parametrize CTCSS tests over all available binaries."""
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins,
            ids=[b.label for b in am_bins],
        )


def test_ctcss_correct_tone(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    rawfile_tolerance: float,
    mp3_tolerance: float,
    speedup_factor: float,
) -> None:
    """
    IQ with correct CTCSS tone (100.0 Hz) → CTCSS gate opens, audio written.

    Expected duration uses 13s (not 15s) to account for the ~2s CTCSS detection
    startup delay. Tolerance is mode-dependent (15% thorough, 25% fast).
    """
    iq_file = iq_generator.get_or_generate_ctcss(
        offset_hz=CHANNEL_OFFSET_HZ,
        ctcss_hz=CORRECT_CTCSS_HZ,
        duration_s=DURATION_S,
        cache_dir=CACHE_DIR,
    )

    config_path = test_output_dir / "rtl_airband.conf"
    filename_template = "ctcss_correct"

    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[
            {
                "freq_hz": CENTERFREQ_HZ + CHANNEL_OFFSET_HZ,
                "squelch": SQUELCH,
                "ctcss": CONFIG_CTCSS_HZ,
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

    # Use 13s expected (not 15s) to account for ~2s CTCSS detection startup delay
    output_validator.validate_rawfile(
        output_dir=test_output_dir,
        filename_template=filename_template,
        expected_duration_s=13.0,
        wave_rate=binary_under_test.wave_rate,
        tolerance=rawfile_tolerance,
    )

    output_validator.validate_mp3(
        mp3_dir=test_output_dir,
        filename_template=filename_template,
        expected_duration_s=13.0,
        tolerance=mp3_tolerance,
    )

    stats = stats_validator.load(test_output_dir / "stats.txt")
    freq_hz = CENTERFREQ_HZ + CHANNEL_OFFSET_HZ
    assert (
        stats.channel("channel_ctcss_counter", freq_hz) > 0
    ), "Expected CTCSS detections with correct tone (100.0 Hz)"
    assert (
        stats.device("buffer_overflow_count") == 0
    ), "Unexpected device buffer overflow"


def test_ctcss_wrong_tone(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    speedup_factor: float,
) -> None:
    """
    IQ with wrong CTCSS tone (125.0 Hz, config expects 100.0 Hz) →
    CTCSS gate stays closed, output absent or empty.
    """
    iq_file = iq_generator.get_or_generate_ctcss(
        offset_hz=CHANNEL_OFFSET_HZ,
        ctcss_hz=WRONG_CTCSS_HZ,
        duration_s=DURATION_S,
        cache_dir=CACHE_DIR,
    )

    config_path = test_output_dir / "rtl_airband.conf"
    filename_template = "ctcss_wrong"

    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[
            {
                "freq_hz": CENTERFREQ_HZ + CHANNEL_OFFSET_HZ,
                "squelch": SQUELCH,
                "ctcss": CONFIG_CTCSS_HZ,
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

    output_validator.assert_output_silent(
        output_dir=test_output_dir,
        filename_template=filename_template,
    )

    output_validator.assert_mp3_silent(
        mp3_dir=test_output_dir,
        filename_template=filename_template,
    )

    stats = stats_validator.load(test_output_dir / "stats.txt")
    freq_hz = CENTERFREQ_HZ + CHANNEL_OFFSET_HZ
    assert (
        stats.channel("channel_ctcss_counter", freq_hz) == 0
    ), "CTCSS should not be detected with wrong tone (125.0 Hz, expected 100.0 Hz)"
    assert (
        stats.channel("channel_no_ctcss_counter", freq_hz) > 0
    ), "Expected non-zero no-CTCSS windows while waiting for tone that never matches"
    assert (
        stats.device("buffer_overflow_count") == 0
    ), "Unexpected device buffer overflow"
