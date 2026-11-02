"""
test_min_rx_seconds.py — the fork's min_rx_seconds and post_write_script file options.

Fork-delta item 1. Neither had end-to-end coverage, and upstream's
split_tx_drop_shorter_than (#580) reworked the same close_file() path they live
in. Both rules can drop a file now, so this checks they combine, and that
post_write_script runs only for files that are kept.

Same two-burst fixture as test_split_on_transmission's drop test: a SHORT_TX_S
burst, a gap, then a 5 s transmission.
"""

import time
from pathlib import Path

from conftest import BinaryUnderTest, run_rtl_airband
from helpers import config_writer, iq_generator, output_validator, stats_validator

SAMPLE_RATE = 2_048_000
CENTERFREQ_HZ = 120_000_000
SHORT_TX_S = 0.5
GAP_S = 3.0
LONG_TX_S = 5.0
MIN_RX_S = 2.0
TOTAL_IQ_DURATION_S = SHORT_TX_S + GAP_S + LONG_TX_S + 2 * iq_generator.NOISE_PAD_S
# Splitting is wall-clock driven, so replay in real time (see test_split_on_transmission).
SPEEDUP_FACTOR = 1.0
TIMEOUT_S = TOTAL_IQ_DURATION_S * 3 + 30


def pytest_generate_tests(metafunc):
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins[:1],
            ids=[b.label for b in am_bins[:1]],
        )


def _split_output(directory: Path, template: str, overrides: dict) -> dict:
    return {"directory": str(directory), "template": template, "overrides": overrides}


def _wait_for_lines(path: Path, count: int, timeout_s: float = 10.0) -> list[str]:
    """post_write_script runs in a detached grandchild, so it can finish after rtl_airband exits."""
    deadline = time.monotonic() + timeout_s
    lines: list[str] = []
    while time.monotonic() < deadline:
        if path.exists():
            lines = path.read_text().splitlines()
            if len(lines) >= count:
                break
        time.sleep(0.2)
    return lines


def test_min_rx_seconds_drops_short_files_and_post_write_script_sees_only_kept_ones(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    cache_dir: Path,
) -> None:
    iq_file = iq_generator.get_or_generate_scan(
        duration_a_s=SHORT_TX_S,
        gap_s=GAP_S,
        duration_b_s=LONG_TX_S,
        cache_dir=cache_dir,
    )
    script_log = test_output_dir / "post_write.log"
    script = test_output_dir / "post_write.sh"
    script.write_text(f'#!/bin/sh\necho "$1" >> "{script_log}"\n')
    script.chmod(0o755)
    stats_path = test_output_dir / "stats.prom"

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
                    _split_output(test_output_dir, "keepall", {}),
                    _split_output(
                        test_output_dir,
                        "minrx",
                        {
                            "min_rx_seconds": MIN_RX_S,
                            "post_write_script": f'"{script}"',
                        },
                    ),
                    # Both rules on: the short burst must still be dropped exactly once,
                    # and only upstream's rule counts toward the drop stat.
                    _split_output(
                        test_output_dir,
                        "both",
                        {
                            "min_rx_seconds": MIN_RX_S,
                            "split_tx_drop_shorter_than": MIN_RX_S,
                        },
                    ),
                ],
            }
        ],
        output_dir=test_output_dir,
        speedup_factor=SPEEDUP_FACTOR,
        mode="multichannel",
        stats_filepath=stats_path,
    )
    run_rtl_airband(binary_under_test.path, config_path, timeout_s=TIMEOUT_S)

    assert output_validator.count_mp3_files(test_output_dir, "keepall") == 2
    for template in ("minrx", "both"):
        kept = output_validator.count_mp3_files(test_output_dir, template)
        kept_s = output_validator.total_mp3_duration_s(test_output_dir, template)
        assert (
            kept == 1
        ), f"{template}: only the {SHORT_TX_S}s burst should be dropped, got {kept} files"
        assert (
            kept_s > LONG_TX_S - 1.0
        ), f"{template}: kept file should be the {LONG_TX_S}s transmission, got {kept_s:.2f}s"

    lines = _wait_for_lines(script_log, 1)
    assert (
        len(lines) == 1
    ), f"post_write_script should run once, for the kept file: {lines}"
    kept_path = Path(lines[0])
    assert (
        kept_path.exists()
    ), f"post_write_script got a path that doesn't exist: {kept_path}"
    assert kept_path.name.startswith("minrx"), kept_path
    assert kept_path.suffix == ".mp3", kept_path

    freq_hz = CENTERFREQ_HZ + iq_generator.SCAN_DEMOD_OFFSET_HZ
    count = stats_validator.load(stats_path).channel(
        "channel_split_tx_dropped_counter", freq_hz
    )
    assert (
        count == 1
    ), f"Only the 'both' output's split_tx_drop_shorter_than drop is counted, got {count}"
