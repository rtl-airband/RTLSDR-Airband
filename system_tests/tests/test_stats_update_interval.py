"""
test_stats_update_interval.py — stats_update_interval controls how often the stats file is rewritten.

The stats file is written once at startup and once at shutdown regardless of the
interval, so a run shorter than the default 15 s interval rewrites it at most twice.
A 1 s interval over the same run rewrites it many times while the process is running.
"""

import subprocess
import time
from pathlib import Path

import pytest
from conftest import BinaryUnderTest, rtl_airband_cmd, run_rtl_airband
from helpers import config_writer, iq_generator

SAMPLE_RATE = 2_048_000
CENTERFREQ_HZ = 120_000_000
CHANNEL_OFFSET_HZ = 25_000
# Same fixture as test_am_squelch_closed so the cached IQ file is shared.
DURATION_S = 10.0
INTERVAL_S = 1
# Startup and shutdown writes account for at most 2 regardless of the interval.
MIN_WRITES_WHILE_RUNNING = 4
POLL_S = 0.1
# Rewrites are driven by wall-clock time, so replay in real time regardless of --mode.
SPEEDUP_FACTOR = 1.0
TIMEOUT_S = DURATION_S * 3 + 30


def pytest_generate_tests(metafunc):
    """Run against one binary only; the stats writer is modulation-independent."""
    if "binary_under_test" in metafunc.fixturenames:
        am_bins: list[BinaryUnderTest] = metafunc.config._rtlsdr_am_binaries
        metafunc.parametrize(
            "binary_under_test",
            am_bins[:1],
            ids=[b.label for b in am_bins[:1]],
        )


def _write_config(
    test_output_dir: Path,
    cache_dir: Path,
    stats_update_interval: float,
    with_stats_file: bool = True,
    speedup_factor: float = SPEEDUP_FACTOR,
) -> Path:
    iq_file = iq_generator.get_or_generate_noise(
        duration_s=DURATION_S,
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
                "freq_hz": CENTERFREQ_HZ + CHANNEL_OFFSET_HZ,
                "output_filename_template": "stats_update_interval",
            }
        ],
        output_dir=test_output_dir,
        speedup_factor=speedup_factor,
        mode="multichannel",
        mp3_tmp_dir=test_output_dir,
        stats_filepath=test_output_dir / "stats.txt" if with_stats_file else None,
        stats_update_interval=stats_update_interval,
    )
    return config_path


def test_stats_update_interval_rewrites_file(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    cache_dir: Path,
) -> None:
    """A short interval rewrites the stats file repeatedly while the process runs."""
    config_path = _write_config(test_output_dir, cache_dir, INTERVAL_S)
    stats_path = test_output_dir / "stats.txt"
    log_path = test_output_dir / "rtl_airband.log"

    write_times: set[int] = set()
    # Log to a file rather than a pipe: an unread pipe can fill and block the process.
    with open(log_path, "w", encoding="utf-8") as log_file:
        with subprocess.Popen(
            rtl_airband_cmd(binary_under_test.path, config_path),
            stdout=log_file,
            stderr=subprocess.STDOUT,
            # sudo ignores signals from its own process group, which would include pytest
            start_new_session=True,
        ) as proc:
            deadline = time.monotonic() + TIMEOUT_S
            while proc.poll() is None:
                if time.monotonic() > deadline:
                    # SIGTERM first: sudo relays it to rtl_airband, but cannot relay SIGKILL
                    proc.terminate()
                    try:
                        proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                    pytest.fail(f"rtl_airband did not exit within {TIMEOUT_S}s")
                try:
                    write_times.add(stats_path.stat().st_mtime_ns)
                except FileNotFoundError:
                    pass
                time.sleep(POLL_S)

    log = log_path.read_text(encoding="utf-8", errors="replace")
    assert proc.returncode == 0, f"rtl_airband exited with {proc.returncode}:\n{log}"
    assert len(write_times) >= MIN_WRITES_WHILE_RUNNING, (
        f"stats_update_interval={INTERVAL_S}s over a {DURATION_S}s run should rewrite the "
        f"stats file at least {MIN_WRITES_WHILE_RUNNING} times, saw {len(write_times)}"
    )


@pytest.mark.parametrize("interval", [0, -1])
def test_stats_update_interval_rejects_non_positive(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    cache_dir: Path,
    interval: int,
) -> None:
    """A zero or negative interval is a configuration error."""
    config_path = _write_config(test_output_dir, cache_dir, interval)

    result = subprocess.run(
        rtl_airband_cmd(binary_under_test.path, config_path),
        capture_output=True,
        text=True,
        timeout=TIMEOUT_S,
        check=False,
    )

    assert result.returncode != 0, "rtl_airband accepted a non-positive interval"
    assert "stats_update_interval must be greater than 0" in result.stderr


def test_stats_update_interval_warns_without_stats_file(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    cache_dir: Path,
) -> None:
    """Setting the interval without stats_filepath warns but still runs."""
    config_path = _write_config(
        test_output_dir,
        cache_dir,
        INTERVAL_S,
        with_stats_file=False,
        speedup_factor=10.0,
    )

    result = run_rtl_airband(binary_under_test.path, config_path, timeout_s=TIMEOUT_S)

    assert "stats_update_interval is ignored without stats_filepath" in result.stderr
