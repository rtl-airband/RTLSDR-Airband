"""
test_device_status.py — the control socket's read-only "status" command.

Checks the hardware health figures it reports against fixtures with known
properties: an AM carrier deliberately placed off the channel's configured
frequency (the per-channel frequency offset estimate must recover it), and a
signal driven past the 8-bit ADC's rails (clipping and peak level).

Same interactive, speedup_factor=1.0 setup as test_control_socket.py, for the
same reasons - the status has to be read while the fixture is still playing.
"""

from __future__ import annotations

import shutil
import tempfile
import time
from pathlib import Path

import numpy as np
import pytest
from conftest import BinaryUnderTest
from helpers import config_writer, iq_generator
from helpers.control_socket_client import send_command, wait_for_socket
from helpers.interactive_runner import run_rtl_airband_interactive

SAMPLE_RATE = iq_generator.SAMPLE_RATE
CENTERFREQ_HZ = 120_000_000
CHANNEL_OFFSET_HZ = 25_000
CARRIER_ERROR_HZ = 700
TONE_DURATION_S = 6.0
TOTAL_IQ_DURATION_S = TONE_DURATION_S + 2 * iq_generator.NOISE_PAD_S
TIMEOUT_S = TOTAL_IQ_DURATION_S * 3 + 30


@pytest.fixture
def short_socket_dir():
    """A short-path temp directory for just the control socket file (AF_UNIX length cap)."""
    d = tempfile.mkdtemp(prefix="rtla_")
    try:
        yield Path(d)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def pytest_generate_tests(metafunc):
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins[:1],
            ids=[b.label for b in am_bins[:1]],
        )


def _status_mid_tone(
    binary: Path, iq_file: Path, output_dir: Path, socket_path: Path
) -> dict:
    config_path = output_dir / "rtl_airband.conf"
    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[
            {
                "freq_hz": CENTERFREQ_HZ + CHANNEL_OFFSET_HZ,
                "output_filename_template": "status",
            }
        ],
        output_dir=output_dir,
        speedup_factor=1.0,
        mode="multichannel",
        mp3_tmp_dir=output_dir,
        control_socket_path=socket_path,
    )
    with run_rtl_airband_interactive(binary, config_path, shutdown_timeout_s=TIMEOUT_S):
        wait_for_socket(socket_path)
        # The tone starts NOISE_PAD_S in; give the estimate a few seconds of open squelch.
        time.sleep(iq_generator.NOISE_PAD_S + 4.0)
        status = send_command(socket_path, {"cmd": "status"})
        time.sleep(TOTAL_IQ_DURATION_S)
    return status


def test_status_reports_tuning_and_recovers_carrier_offset(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    cache_dir: Path,
    short_socket_dir: Path,
) -> None:
    iq_file = iq_generator.get_or_generate_am(
        offset_hz=CHANNEL_OFFSET_HZ + CARRIER_ERROR_HZ,
        audio_hz=1_000,
        duration_s=TONE_DURATION_S,
        cache_dir=cache_dir,
    )
    status = _status_mid_tone(
        binary_under_test.path,
        iq_file,
        test_output_dir,
        short_socket_dir / "control.sock",
    )

    assert status["ok"] is True, status
    assert len(status["devices"]) == 1
    dev = status["devices"][0]
    assert dev["driver"] == "file"
    assert dev["state"] == "running"
    assert dev["centerfreq"] == CENTERFREQ_HZ
    assert dev["sample_rate"] == SAMPLE_RATE
    # The fixture's AM envelope peaks at 1.8x its 0.5-of-full-scale carrier: about -0.9 dBFS,
    # and never touches the rails.
    assert -3.0 < dev["peak_dbfs"] < 0.0, dev
    assert dev["clipped_samples"] == 0, dev

    freq = dev["channels"][0]["freqs"][0]
    assert freq["freq"] == CENTERFREQ_HZ + CHANNEL_OFFSET_HZ
    assert freq["freq_offset_updates"] > 0, freq
    assert freq["freq_offset_hz"] == pytest.approx(CARRIER_ERROR_HZ, abs=25), freq
    # The auto_ppm copy of the estimate tracks the same carrier (no tuning change in this run).
    assert freq["ppm_offset_updates"] > 0, freq
    assert freq["ppm_offset_hz"] == pytest.approx(CARRIER_ERROR_HZ, abs=25), freq
    assert dev["auto_ppm"] is None, dev


def test_status_counts_clipped_samples(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    short_socket_dir: Path,
) -> None:
    # A carrier 2.5x the ADC's full scale, so most samples sit on a rail.
    n = int(SAMPLE_RATE * TOTAL_IQ_DURATION_S)
    t = np.arange(n, dtype=np.float64) / SAMPLE_RATE
    phase = 2 * np.pi * CHANNEL_OFFSET_HZ * t
    amp = 2.5 * 127.5
    i_u8 = np.clip(np.round(128 + amp * np.cos(phase)), 0, 255).astype(np.uint8)
    q_u8 = np.clip(np.round(128 + amp * np.sin(phase)), 0, 255).astype(np.uint8)
    iq_file = test_output_dir / "clipped.iq"
    iq_file.write_bytes(np.column_stack([i_u8, q_u8]).flatten().tobytes())

    status = _status_mid_tone(
        binary_under_test.path,
        iq_file,
        test_output_dir,
        short_socket_dir / "control.sock",
    )

    dev = status["devices"][0]
    assert dev["clipped_samples"] > SAMPLE_RATE, dev
    assert dev["peak_dbfs"] == pytest.approx(0.0, abs=0.01), dev
