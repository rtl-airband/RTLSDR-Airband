"""
test_auto_ppm.py — auto_ppm config validation through the real binary.

The closed loop (measure, adjust, re-measure) needs a driver with a live
correction hook, which the file driver used by these tests doesn't have; the
decision logic is unit tested in src/test_auto_ppm.cpp and the loop was
validated on RTL-SDR hardware. What this covers is that the parser refuses
auto_ppm where it can't work, instead of silently ignoring it.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from conftest import BinaryUnderTest, run_rtl_airband
from helpers import config_writer, iq_generator

SAMPLE_RATE = iq_generator.SAMPLE_RATE
CENTERFREQ_HZ = 120_000_000


def pytest_generate_tests(metafunc):
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins[:1],
            ids=[b.label for b in am_bins[:1]],
        )


@pytest.mark.parametrize(
    "extra_lines, expected",
    [
        (["auto_ppm = true;"], "auto_ppm is not supported by the file driver"),
        (["auto_ppm = false;"], "auto_ppm is not supported by the file driver"),
    ],
)
def test_auto_ppm_is_rejected_on_a_driver_without_live_correction(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    extra_lines: list[str],
    expected: str,
) -> None:
    iq_file = test_output_dir / "silence.iq"
    iq_file.write_bytes(bytes([128]) * SAMPLE_RATE * 2)
    config_path = test_output_dir / "rtl_airband.conf"
    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[{"freq_hz": CENTERFREQ_HZ + 25_000}],
        output_dir=test_output_dir,
        extra_device_lines=extra_lines,
    )
    result = run_rtl_airband(
        binary_under_test.path, config_path, timeout_s=30, check=False
    )
    assert result.returncode != 0, result.stderr
    assert expected in result.stderr, result.stderr
