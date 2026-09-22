"""
test_split_on_transmission.py — per-output split file time overrides.

Two file outputs on one channel see the same audio: two transmissions separated
by a GAP_S silence. The only difference is split_max_idle_time, so the file count
each produces shows whether the per-output override took effect.

  inherit  → global split_max_idle_time (> GAP_S) → stays open across the gap → 1 file
  override → per-output split_max_idle_time (< GAP_S) → closes in the gap  → 2 files
"""

from pathlib import Path

from conftest import BinaryUnderTest, run_rtl_airband
from helpers import config_writer, iq_generator, output_validator

SAMPLE_RATE = 2_048_000
CENTERFREQ_HZ = 120_000_000
# Same fixture parameters as test_scan so the cached IQ file is shared.
DURATION_A_S = 5.0
GAP_S = 3.0
DURATION_B_S = 5.0
TOTAL_IQ_DURATION_S = (
    DURATION_A_S + GAP_S + DURATION_B_S + 2 * iq_generator.NOISE_PAD_S
)  # 15 s

# Both sides of GAP_S, leaving margin for squelch close lag at either end.
IDLE_INHERITED_S = 10.0
IDLE_OVERRIDE_S = 1.5

# Splitting is driven by wall-clock time (gettimeofday in close_if_necessary), so
# this test replays in real time regardless of --mode. A 10x speedup would compress
# every burst below the 1.0 s split_min_file_time floor and nothing would split.
SPEEDUP_FACTOR = 1.0
TIMEOUT_S = TOTAL_IQ_DURATION_S * 3 + 30  # 75 s


def pytest_generate_tests(metafunc):
    """
    Run against one binary only.

    File splitting is wall-clock driven and modulation-independent, so the NFM
    build exercises the same code; a second real-time replay buys no coverage.
    """
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins[:1],
            ids=[b.label for b in am_bins[:1]],
        )


def test_split_on_transmission_per_output_override(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    cache_dir: Path,
) -> None:
    """A per-output split_max_idle_time overrides the global; an unset one inherits it."""
    iq_file = iq_generator.get_or_generate_scan(
        duration_a_s=DURATION_A_S,
        gap_s=GAP_S,
        duration_b_s=DURATION_B_S,
        cache_dir=cache_dir,
    )

    config_path = test_output_dir / "rtl_airband.conf"

    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[
            {
                "freq_hz": CENTERFREQ_HZ + iq_generator.SCAN_DEMOD_OFFSET_HZ,
                "split_outputs": [
                    {
                        "directory": str(test_output_dir),
                        "template": "inherit",
                    },
                    {
                        "directory": str(test_output_dir),
                        "template": "override",
                        "overrides": {"split_max_idle_time": IDLE_OVERRIDE_S},
                    },
                ],
            }
        ],
        output_dir=test_output_dir,
        speedup_factor=SPEEDUP_FACTOR,
        mode="multichannel",
        split_times={"split_max_idle_time": IDLE_INHERITED_S},
    )

    run_rtl_airband(binary_under_test.path, config_path, timeout_s=TIMEOUT_S)

    inherited = output_validator.count_mp3_files(test_output_dir, "inherit")
    overridden = output_validator.count_mp3_files(test_output_dir, "override")

    assert inherited == 1, (
        f"Output inheriting the global split_max_idle_time={IDLE_INHERITED_S}s should "
        f"stay open across the {GAP_S}s gap and produce 1 file, got {inherited}"
    )
    assert overridden == 2, (
        f"Output overriding split_max_idle_time={IDLE_OVERRIDE_S}s should close during "
        f"the {GAP_S}s gap and produce 2 files, got {overridden}"
    )
