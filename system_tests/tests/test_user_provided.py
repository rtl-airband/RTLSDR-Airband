"""
test_user_provided.py — Dynamically generated tests from user-provided IQ files.

All *.json files found in system_tests/user_provided/ are loaded automatically.
Each test case in those files becomes one test instance named test_user_provided[<name>].
If user_provided/ contains no JSON files, zero tests are collected (no error).

See conftest.py for the UserTestCase / UserChannelCase schemas and load_extra_test_cases().
See README.md for the full JSON format and field reference.
"""

from __future__ import annotations

import dataclasses
import os
from pathlib import Path

import pytest
from conftest import (
    UserTestCase,
    load_extra_test_cases,
    run_rtl_airband,
)
from helpers import config_writer, output_validator, stats_validator

_USER_PROVIDED_DIR = Path(__file__).parent.parent / "user_provided"


def pytest_generate_tests(metafunc):
    """
    Dynamically parametrize test_user_provided over all cases found in
    system_tests/user_provided/*.json. Silently collects zero tests if the
    directory is empty or contains no JSON files.
    """
    if metafunc.function.__name__ != "test_user_provided":
        return

    json_files = sorted(_USER_PROVIDED_DIR.glob("*.json"))
    if not json_files:
        metafunc.parametrize("test_case", [None], ids=["no-user-provided-tests"])
        return

    all_cases: list[UserTestCase] = []
    for json_path in json_files:
        try:
            all_cases.extend(load_extra_test_cases(json_path))
        except ValueError as exc:
            pytest.exit(
                f"Failed to load test cases from {json_path}: {exc}",
                returncode=4,
            )

    if not all_cases:
        metafunc.parametrize("test_case", [None], ids=["no-user-provided-tests"])
        return

    expanded = []
    ids = []
    seen: set[str] = set()
    for tc in all_cases:
        if tc.binary == "both":
            for label in ("non-nfm", "nfm"):
                test_id = f"{tc.name}-{label}"
                if test_id in seen:
                    pytest.exit(
                        f"Duplicate test ID {test_id!r} across user_provided/*.json files",
                        returncode=4,
                    )
                seen.add(test_id)
                expanded.append(dataclasses.replace(tc, binary=label))
                ids.append(test_id)
        else:
            test_id = tc.name
            if test_id in seen:
                pytest.exit(
                    f"Duplicate test ID {test_id!r} across user_provided/*.json files",
                    returncode=4,
                )
            seen.add(test_id)
            expanded.append(tc)
            ids.append(test_id)

    metafunc.parametrize("test_case", expanded, ids=ids)


@pytest.mark.timeout(0)  # subprocess timeout in run_rtl_airband handles this
def test_user_provided(
    test_case: UserTestCase | None,
    request: pytest.FixtureRequest,
    test_output_dir: Path,
    mp3_tolerance: float,
    max_overrun_count: int,
    speedup_factor: float,
) -> None:
    """
    Run a user-provided IQ file through rtl_airband and validate output.

    Each test case is loaded from the JSON file given via --extra-tests.
    Speedup and tolerance come from the --mode flag.
    """
    # Sentinel: --extra-tests not provided or JSON had no cases
    if test_case is None:
        pytest.skip(
            "No user-provided test cases (--extra-tests not given or file is empty)"
        )

    # Resolve the binary
    if test_case.binary == "nfm":
        binary_path_str = request.config.getoption("--nfm-binary")
        if binary_path_str is None:
            pytest.skip("NFM binary not provided via --nfm-binary")
        binary_path = Path(binary_path_str).resolve()
    else:  # "non-nfm"
        binary_path = Path(request.config.getoption("--binary")).resolve()

    # Check IQ file exists
    iq_file = test_case.iq_file
    if not iq_file.exists():
        pytest.skip(f"IQ file not found: {iq_file}")

    # Compute timeout based on IQ file size
    iq_size_bytes = os.path.getsize(iq_file)
    iq_duration_s = iq_size_bytes / (test_case.sample_rate * 2)
    timeout_s = iq_duration_s / speedup_factor * 3 + 30

    # Build channel list for config_writer
    config_channels = []
    for ch in test_case.channels:
        ch_dict: dict = {
            "freq_hz": ch.freq_hz,
            "modulation": ch.modulation,
            "squelch": ch.squelch,
            "ctcss": ch.ctcss,
            "bandwidth": ch.bandwidth,
            "notch": ch.notch,
            "mixer_output": ch.mixer_output,
            "output_filename_template": ch.label,
        }
        if test_case.mode == "scan" and ch.scan_freqs_hz is not None:
            ch_dict["scan_freqs_hz"] = ch.scan_freqs_hz
        config_channels.append(ch_dict)

    config_path = test_output_dir / "rtl_airband.conf"
    stats_path = test_output_dir / "stats.prom"
    mixer_configs = [{"name": mx.name, "label": mx.label} for mx in test_case.mixers]

    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=test_case.sample_rate,
        centerfreq_hz=test_case.centerfreq_hz,
        channels=config_channels,
        output_dir=test_output_dir,
        speedup_factor=speedup_factor,
        mode=test_case.mode,
        fft_size=test_case.fft_size,
        mixers=mixer_configs or None,
        mp3_tmp_dir=test_output_dir,
        stats_filepath=stats_path,
    )

    run_rtl_airband(binary_path, config_path, timeout_s=timeout_s)

    # Validate mixer outputs.
    # TODO: enable full duration validation for mixers in fast mode once rtl_airband
    # mixers support speedup_factor (currently audio is lost at high speedup rates).
    for mx in test_case.mixers:
        if speedup_factor != 1.0:
            output_validator.assert_mp3_present(
                mp3_dir=test_output_dir,
                filename_template=mx.label,
            )
        elif mx.expected_audio_s > 0:
            output_validator.validate_mp3(
                mp3_dir=test_output_dir,
                filename_template=mx.label,
                expected_duration_s=mx.expected_audio_s,
                tolerance=mp3_tolerance,
            )
        else:
            output_validator.assert_mp3_silent(
                mp3_dir=test_output_dir,
                filename_template=mx.label,
            )

    # Validate each channel — MP3 only
    for ch in test_case.channels:
        if ch.expected_audio_s > 0:
            output_validator.validate_mp3(
                mp3_dir=test_output_dir,
                filename_template=ch.label,
                expected_duration_s=ch.expected_audio_s,
                tolerance=mp3_tolerance,
            )
        else:
            output_validator.assert_mp3_silent(
                mp3_dir=test_output_dir,
                filename_template=ch.label,
            )

    # Basic stats validation
    stats = stats_validator.load(stats_path)
    assert (
        stats.device("buffer_overflow_count") == 0
    ), "Device buffer overflowed — increase speedup_factor or reduce channel count"
    stats_validator.assert_no_excessive_overruns(stats, max_overrun_count)
    for ch in test_case.channels:
        if ch.expected_audio_s > 0:
            assert (
                stats.channel("channel_activity_counter", ch.freq_hz) > 0
            ), f"Expected activity on {ch.freq_hz / 1e6:.3f} MHz but channel_activity_counter == 0"
