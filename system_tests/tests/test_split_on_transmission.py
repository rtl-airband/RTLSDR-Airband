"""
test_split_on_transmission.py — per-output split file time overrides.

Two file outputs on one channel see the same audio: two transmissions separated
by a GAP_S silence. The only difference is split_max_idle_time, so the file count
each produces shows whether the per-output override took effect.

  inherit  → global split_max_idle_time (> GAP_S) → stays open across the gap → 1 file
  override → per-output split_max_idle_time (< GAP_S) → closes in the gap  → 2 files

The split_include_transmission_start tests use the same audio with a short
split_max_file_time, so each transmission is cut into several parts that must be
named as one group.
"""

import subprocess
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

# Well under DURATION_A_S / DURATION_B_S so every transmission is split mid-way.
MAX_FILE_TIME_S = 2.0

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


def _scan_fixture(cache_dir: Path) -> Path:
    return iq_generator.get_or_generate_scan(
        duration_a_s=DURATION_A_S,
        gap_s=GAP_S,
        duration_b_s=DURATION_B_S,
        cache_dir=cache_dir,
    )


def _split_output(directory: Path, template: str, overrides: dict) -> dict:
    return {"directory": str(directory), "template": template, "overrides": overrides}


def _run_split_outputs(
    binary: Path,
    test_output_dir: Path,
    iq_file: Path,
    split_outputs: list[dict],
    global_settings: dict | None = None,
) -> None:
    """Replay the two-burst fixture in real time into the given split outputs."""
    config_path = test_output_dir / "rtl_airband.conf"
    config_writer.write_config(
        config_path=config_path,
        iq_filepath=iq_file,
        sample_rate=SAMPLE_RATE,
        centerfreq_hz=CENTERFREQ_HZ,
        channels=[
            {
                "freq_hz": CENTERFREQ_HZ + iq_generator.SCAN_DEMOD_OFFSET_HZ,
                "split_outputs": split_outputs,
            }
        ],
        output_dir=test_output_dir,
        speedup_factor=SPEEDUP_FACTOR,
        mode="multichannel",
        split_times=global_settings,
    )
    run_rtl_airband(binary, config_path, timeout_s=TIMEOUT_S)


def _assert_split_into_parts(
    test_output_dir: Path,
    template: str,
    freq_hz: int | None = None,
    transmissions: int = 2,
) -> None:
    """Every transmission is split, and every part is named under its start stamp."""
    standalone, parts = output_validator.split_file_names(
        test_output_dir, template, freq_hz
    )
    assert (
        not standalone
    ), f"{template}: split transmissions got whole names: {standalone}"
    assert (
        len(parts) == transmissions
    ), f"{template}: expected {transmissions} transmissions, got {sorted(parts)}"
    for transmission_stamp, part_stamps in parts.items():
        assert (
            len(part_stamps) >= 2
        ), f"{template}: {transmission_stamp} not split: {part_stamps}"
        assert (
            part_stamps[0] == transmission_stamp
        ), f"{template}: part 1 must repeat {transmission_stamp}"
        assert part_stamps == sorted(
            set(part_stamps)
        ), f"{template}: part stamps not increasing: {part_stamps}"


def _assert_standalone_only(
    test_output_dir: Path, template: str, min_files: int
) -> list[str]:
    standalone, parts = output_validator.split_file_names(test_output_dir, template)
    assert not parts, f"{template}: no file should get a part name: {parts}"
    assert (
        len(standalone) >= min_files
    ), f"{template}: expected at least {min_files} files: {standalone}"
    return standalone


def test_split_include_transmission_start(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    cache_dir: Path,
) -> None:
    """
    Parts of a split transmission share its start stamp; whole ones keep today's names.

    The option is set globally, so parts/whole/freqparts inherit it and plain checks
    that a per-output false overrides it.
    """
    freq_hz = CENTERFREQ_HZ + iq_generator.SCAN_DEMOD_OFFSET_HZ
    _run_split_outputs(
        binary_under_test.path,
        test_output_dir,
        _scan_fixture(cache_dir),
        [
            _split_output(
                test_output_dir, "parts", {"split_max_file_time": MAX_FILE_TIME_S}
            ),
            _split_output(
                test_output_dir,
                "plain",
                {
                    "split_max_file_time": MAX_FILE_TIME_S,
                    "split_include_transmission_start": False,
                },
            ),
            _split_output(test_output_dir, "whole", {}),
            _split_output(
                test_output_dir,
                "freqparts",
                {"split_max_file_time": MAX_FILE_TIME_S, "include_freq": True},
            ),
        ],
        global_settings={"split_include_transmission_start": True},
    )

    _assert_split_into_parts(test_output_dir, "parts")
    # include_freq puts the frequency after both stamps
    _assert_split_into_parts(test_output_dir, "freqparts", freq_hz)
    _assert_standalone_only(test_output_dir, "plain", min_files=4)
    whole = _assert_standalone_only(test_output_dir, "whole", min_files=2)
    assert len(whole) == 2, f"Expected 2 whole transmissions: {whole}"

    # splitting must not drop or duplicate audio
    whole_s = output_validator.total_mp3_duration_s(test_output_dir, "whole")
    parts_s = output_validator.total_mp3_duration_s(test_output_dir, "parts")
    assert (
        abs(parts_s - whole_s) <= 0.1 * whole_s
    ), f"Split parts total {parts_s:.2f}s should match the unsplit {whole_s:.2f}s"


def _run_config(
    binary: Path, config_path: Path, text: str
) -> subprocess.CompletedProcess:
    config_path.write_text(text)
    return run_rtl_airband(binary, config_path, timeout_s=TIMEOUT_S, check=False)


def _validation_config(
    iq_file: Path, directory: Path, output: str, top: str = "", mixer: str = ""
) -> str:
    """A one-channel config with extra settings on its file output, top level or a mixer."""
    mixers = ""
    if mixer:
        mixers = f"""mixers: {{ mix1: {{ outputs: ( {{ type = "file"; directory = "{directory}";
  filename_template = "mix"; {mixer} }} ); }} }};"""
    return f"""{top}
{mixers}
devices: ({{
  type = "file"; filepath = "{iq_file}"; sample_rate = {SAMPLE_RATE};
  centerfreq = {CENTERFREQ_HZ}; speedup_factor = 20.0;
  channels: ({{
    freq = {(CENTERFREQ_HZ + iq_generator.SCAN_DEMOD_OFFSET_HZ) / 1e6:.6f};
    outputs: ({{ type = "file"; directory = "{directory}"; filename_template = "out";
      {output} }});
  }});
}});
"""


def test_split_include_transmission_start_config_validation(
    binary_under_test: BinaryUnderTest,
    test_output_dir: Path,
    cache_dir: Path,
) -> None:
    """Bad values and mixer outputs are errors; an output without splitting only warns."""
    iq_file = _scan_fixture(cache_dir)
    split = "split_on_transmission = true;"
    errors = {
        "mixer": _validation_config(
            iq_file,
            test_output_dir,
            split,
            mixer="split_include_transmission_start = true;",
        ),
        "per_output_int": _validation_config(
            iq_file, test_output_dir, f"{split} split_include_transmission_start = 1;"
        ),
        "global_string": _validation_config(
            iq_file,
            test_output_dir,
            split,
            top='split_include_transmission_start = "yes";',
        ),
    }
    for name, text in errors.items():
        result = _run_config(
            binary_under_test.path, test_output_dir / f"{name}.conf", text
        )
        assert result.returncode != 0, f"{name}: config should be rejected"
        assert (
            "Configuration error" in result.stderr
            and "split_include_transmission_start" in result.stderr
        ), f"{name}: unexpected stderr:\n{result.stderr}"

    ignored = _run_config(
        binary_under_test.path,
        test_output_dir / "ignored.conf",
        _validation_config(
            iq_file, test_output_dir, "split_include_transmission_start = true;"
        ),
    )
    assert ignored.returncode == 0, f"Warning only, got:\n{ignored.stderr}"
    assert (
        "split_include_transmission_start is ignored without split_on_transmission"
        in ignored.stderr
    ), f"Expected a warning, got:\n{ignored.stderr}"

    global_only = _run_config(
        binary_under_test.path,
        test_output_dir / "global_only.conf",
        _validation_config(
            iq_file, test_output_dir, "", top="split_include_transmission_start = true;"
        ),
    )
    assert global_only.returncode == 0, global_only.stderr
    assert (
        "split_include_transmission_start is ignored" not in global_only.stderr
    ), f"The global value alone should not warn:\n{global_only.stderr}"
