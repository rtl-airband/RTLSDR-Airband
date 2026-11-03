"""
test_rtlsdr_buffer_size.py — rtlsdr buffer_size validation through the real binary.

The auto transfer sizing itself is unit tested (src/test_input_common.cpp) and
was validated on RTL-SDR hardware. This covers the parser rejecting a
buffer_size librtlsdr can't use. Config parsing runs before any device is
opened, so these never touch USB and pass with or without a dongle attached.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from conftest import BinaryUnderTest, run_rtl_airband

EXPECTED = "buffer_size must be a positive multiple of 512"


def pytest_generate_tests(metafunc):
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins[:1],
            ids=[b.label for b in am_bins[:1]],
        )


def write_rtlsdr_config(config_path: Path, output_dir: Path, buffer_size: str) -> None:
    config_path.write_text(f"""
devices: (
  {{
    type = "rtlsdr";
    index = 0;
    gain = 25;
    centerfreq = 120.0;
    buffer_size = {buffer_size};
    channels: (
      {{
        freq = 120.025;
        outputs: (
          {{
            type = "file";
            directory = "{output_dir}";
            filename_template = "out";
          }}
        );
      }}
    );
  }}
);
""")


@pytest.mark.parametrize("buffer_size", ["1000", "0", "-512", "16777728"])
def test_invalid_buffer_size_is_rejected(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    buffer_size: str,
) -> None:
    config_path = test_output_dir / "rtl_airband.conf"
    write_rtlsdr_config(config_path, test_output_dir, buffer_size)
    result = run_rtl_airband(
        binary_under_test.path, config_path, timeout_s=30, check=False
    )
    assert result.returncode != 0, result.stderr
    assert EXPECTED in result.stderr, result.stderr
