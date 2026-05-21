"""
test_ctcss.py — CTCSS gate: correct tone passes, wrong tone is blocked.

Two test functions, both parametrized over all provided binaries.

test_ctcss_correct_tone: IQ with 100.0 Hz CTCSS + 1000 Hz voice, config asks for
  100.0 Hz → output should contain audio (accounting for CTCSS_STARTUP_DELAY_S).

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
# The IQ fixture has NOISE_PAD_S of noise prepended and appended around the
# signal. Enabling squelch alongside CTCSS gives both gates time to settle on
# noise instead of racing CTCSS startup against an always-open squelch.
CONFIG_CTCSS_HZ = 100.0  # what the config requests
CORRECT_CTCSS_HZ = 100.0  # matches the config → should pass
WRONG_CTCSS_HZ = 125.0  # not a standard CTCSS tone, does not match → should block

# Time from carrier-on to the CTCSS gate first opening: squelch open_delay
# (~25 ms at 8 kHz audio rate) + one fast CTCSS window (0.05 s) ≈ 75 ms.
# 0.1 s gives a small margin for trailing-pad gate close timing.
# squelch.cpp:118-134 — is_open() consults the fast detector until the slow one
# has 0.4 s of samples; the fast detector checks every 0.05 s window with no
# confirmation count requirement (ctcss.cpp:124-163).
CTCSS_STARTUP_DELAY_S = 0.1
EXPECTED_AUDIO_S = DURATION_S - CTCSS_STARTUP_DELAY_S  # 14.9 s
TOTAL_IQ_DURATION_S = DURATION_S + 2 * iq_generator.NOISE_PAD_S  # 17 s
TIMEOUT_S = TOTAL_IQ_DURATION_S * 3 + 30  # 81 s


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
    mp3_tolerance: float,
    max_overrun_count: int,
    speedup_factor: float,
) -> None:
    """
    IQ with correct CTCSS tone (100.0 Hz) → CTCSS gate opens, audio written.

    Expected duration is DURATION_S - CTCSS_STARTUP_DELAY_S — see the constant
    for the CTCSS-fast-detector + squelch-open-delay derivation.
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

    output_validator.validate_mp3(
        mp3_dir=test_output_dir,
        filename_template=filename_template,
        expected_duration_s=EXPECTED_AUDIO_S,
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
    stats_validator.assert_no_excessive_overruns(stats, max_overrun_count)


def test_ctcss_wrong_tone(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    max_overrun_count: int,
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
    stats_validator.assert_no_excessive_overruns(stats, max_overrun_count)
