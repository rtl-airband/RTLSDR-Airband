"""
test_scan.py — Scan mode: audio written across two scan frequencies.

In scan mode rtl_airband tunes hardware 20 FFT bins above each target frequency so
the signal always lands at the same fixed bin (bin 491 for fft_size=512).  With a
file input the hardware center cannot move, so the scanner always demodulates from
that one bin — see iq_generator.SCAN_DEMOD_OFFSET_HZ for the exact offset.

The IQ fixture is a three-segment file wrapped in NOISE_PAD_S of leading and
trailing noise. BOTH signal segments are placed at the actual demodulation bin
so they are detectable regardless of which scan freq is active:
  Lead:      noise for NOISE_PAD_S (squelch warm-up)
  Segment 1: AM at SCAN_DEMOD_OFFSET_HZ for 5s  (scanner locked on freq A)
  Segment 2: noise for 3s                         (scanner switches A → B)
  Segment 3: AM at SCAN_DEMOD_OFFSET_HZ for 5s  (scanner locked on freq B)
  Tail:      noise for NOISE_PAD_S (clean shutdown)

Config: scan mode, two configured frequencies (120.025 and 119.975 MHz).
Expected: MP3 total audio ≈ 10s (5s A + 5s B).

Parametrized over all provided binaries (non-NFM and NFM if available).
"""

from pathlib import Path

from conftest import CACHE_DIR, BinaryUnderTest, run_rtl_airband
from helpers import config_writer, iq_generator, output_validator, stats_validator

SAMPLE_RATE = 2_048_000
CENTERFREQ_HZ = 120_000_000
FREQ_A_HZ = 120_025_000  # 120.025 MHz — first scan frequency
FREQ_B_HZ = 119_975_000  # 119.975 MHz — second scan frequency
DURATION_A_S = 5.0
GAP_S = 3.0  # 3s gap gives scanner plenty of time to switch (needs ~2s)
DURATION_B_S = 5.0
# The scan fixture has NOISE_PAD_S of noise prepended and appended around the
# A/gap/B sequence — same squelch warm-up + clean-shutdown role as other tests.
TOTAL_IQ_DURATION_S = (
    DURATION_A_S + GAP_S + DURATION_B_S + 2 * iq_generator.NOISE_PAD_S
)  # 15 s
EXPECTED_AUDIO_S = DURATION_A_S + DURATION_B_S  # ≈10s of actual signal
TIMEOUT_S = TOTAL_IQ_DURATION_S * 3 + 30  # 75 s


def pytest_generate_tests(metafunc):
    """Parametrize test_scan over all available binaries."""
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins,
            ids=[b.label for b in am_bins],
        )


def test_scan(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    mp3_tolerance: float,
    max_overrun_count: int,
    speedup_factor: float,
) -> None:
    """
    Scan mode with two frequencies → MP3 contains ≈10s of combined audio.
    """
    iq_file = iq_generator.get_or_generate_scan(
        duration_a_s=DURATION_A_S,
        gap_s=GAP_S,
        duration_b_s=DURATION_B_S,
        cache_dir=CACHE_DIR,
    )

    config_path = test_output_dir / "rtl_airband.conf"
    filename_template = "scan_out"

    # Scan mode: single channel entry with freqs list
    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[
            {
                "freq_hz": FREQ_A_HZ,  # not used in scan mode
                "scan_freqs_hz": [FREQ_A_HZ, FREQ_B_HZ],
                "ctcss": None,
                "output_filename_template": filename_template,
            }
        ],
        output_dir=test_output_dir,
        speedup_factor=speedup_factor,
        mode="scan",
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
    # At least one of the two scan frequencies must have had activity
    activity_a = stats.channel("channel_activity_counter", FREQ_A_HZ) or 0
    activity_b = stats.channel("channel_activity_counter", FREQ_B_HZ) or 0
    assert (
        activity_a + activity_b > 0
    ), "Expected non-zero total activity across scan frequencies A and B"
    assert (
        stats.device("buffer_overflow_count") == 0
    ), "Unexpected device buffer overflow"
    stats_validator.assert_no_excessive_overruns(stats, max_overrun_count)
