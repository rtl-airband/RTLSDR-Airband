"""
conftest.py — shared fixtures and helpers for RTLSDR-Airband system tests.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest

CACHE_DIR = Path(__file__).parent / ".generated_input"
DEFAULT_TEST_OUTPUT_DIR = Path(__file__).parent / "test_output"


def _resolve_test_output_dir(config: pytest.Config) -> Path:
    """Return the base directory under which each test creates its own subdir.

    Defaults to system_tests/test_output; can be overridden with
    --test-output-dir, useful for pointing at a tmpfs on hosts where SD-card
    writeback stalls perturb timing-sensitive tests.
    """
    try:
        override = config.getoption("--test-output-dir")
    except ValueError:
        override = None
    return Path(override) if override else DEFAULT_TEST_OUTPUT_DIR


_use_sudo: bool = False


# ---------------------------------------------------------------------------
# CLI options
# ---------------------------------------------------------------------------


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--binary", required=True, help="Path to non-NFM rtl_airband binary"
    )
    parser.addoption(
        "--nfm-binary",
        default=None,
        help="Path to NFM rtl_airband binary",
    )
    parser.addoption(
        "--mode",
        choices=["fast", "thorough"],
        default="thorough",
        help=(
            "Test mode: 'fast' (replay speedup, allows some output overruns, "
            "use with -n auto for parallel) or 'thorough' (no speedup, "
            "first-batch warm-up overrun budget only, serial)."
        ),
    )
    parser.addoption(
        "--sudo",
        action="store_true",
        default=False,
        help="Invoke rtl_airband via sudo (required when BCM VideoCore GPU FFT is enabled)",
    )
    parser.addoption(
        "--clean",
        action="store_true",
        default=False,
        help="Delete the .generated_input cache before running tests",
    )
    parser.addoption(
        "--test-output-dir",
        default=None,
        help=(
            "Override the base directory used for per-test output (default: "
            "system_tests/test_output). Point this at a tmpfs to remove disk-"
            "writeback jitter from timing-sensitive tests on slow-storage hosts."
        ),
    )


# ---------------------------------------------------------------------------
# Binary descriptor
# ---------------------------------------------------------------------------


@dataclass
class BinaryUnderTest:
    path: Path
    wave_rate: int  # 8000 for non-NFM, 16000 for NFM
    label: str  # "non-nfm" or "nfm" — used as the pytest parametrize ID


# ---------------------------------------------------------------------------
# Early stash of am_binaries list for pytest_generate_tests hooks
# Called during collection, before session fixtures run.
# ---------------------------------------------------------------------------


def pytest_configure(config: pytest.Config) -> None:
    """
    Stash the list of BinaryUnderTest on the config object so that
    pytest_generate_tests hooks in individual test modules can access it
    during collection (before session fixtures run).
    """
    global _use_sudo  # pylint: disable=global-statement
    try:
        _use_sudo = bool(config.getoption("--sudo"))
    except ValueError:
        pass

    try:
        if config.getoption("--clean") and CACHE_DIR.exists():
            shutil.rmtree(CACHE_DIR)
    except ValueError:
        pass

    test_output_base = _resolve_test_output_dir(config)
    if test_output_base.exists():
        # Clean only contents, not the directory itself — the caller may have
        # set up the path as a mount point (e.g. tmpfs) we shouldn't unlink.
        for child in test_output_base.iterdir():
            if child.is_dir():
                shutil.rmtree(child)
            else:
                child.unlink()

    binary_val = None
    nfm_val = None
    try:
        binary_val = config.getoption("--binary")
    except ValueError:
        # Option not registered yet (e.g., during plugin loading)
        pass

    if binary_val is None:
        # Not yet available; tests will fail at fixture time with a clear message.
        config._rtlsdr_am_binaries = []
    else:
        bins: list[BinaryUnderTest] = [
            BinaryUnderTest(path=Path(binary_val), wave_rate=8000, label="non-nfm")
        ]
        try:
            nfm_val = config.getoption("--nfm-binary")
        except ValueError:
            pass

        if nfm_val is not None:
            bins.append(
                BinaryUnderTest(path=Path(nfm_val), wave_rate=16000, label="nfm")
            )

        config._rtlsdr_am_binaries = bins

    # Reject --mode thorough with -n. Thorough mode caps output overruns at 1
    # (first-batch warm-up only); parallel pytest-xdist workers contending for
    # CPU produce more than that and would fail the assertion.
    try:
        mode = config.getoption("--mode")
        numprocesses = getattr(config.option, "numprocesses", None)
    except ValueError:
        mode = None
        numprocesses = None

    if mode == "thorough" and numprocesses not in (None, 0, "0"):
        pytest.exit(
            "ERROR: --mode thorough is incompatible with -n / --numprocesses. "
            "Thorough mode runs serially so the tight overrun budget stays "
            "meaningful. Use --mode fast for parallel runs.",
            returncode=4,
        )


# ---------------------------------------------------------------------------
# Session-scoped fixtures
# ---------------------------------------------------------------------------


@pytest.fixture(scope="session")
def binary(request: pytest.FixtureRequest) -> Path:
    p = Path(request.config.getoption("--binary")).resolve()
    assert p.exists(), f"Binary not found: {p}"
    return p


@pytest.fixture(scope="session")
def nfm_binary(request: pytest.FixtureRequest) -> Path | None:
    val = request.config.getoption("--nfm-binary")
    if val is None:
        return None
    p = Path(val).resolve()
    assert p.exists(), f"NFM binary not found: {p}"
    return p


@pytest.fixture(scope="session", autouse=True)
def ensure_cache_dir() -> None:
    CACHE_DIR.mkdir(exist_ok=True)


@pytest.fixture(scope="session")
def _test_output_dir(request: pytest.FixtureRequest) -> Path:
    base = _resolve_test_output_dir(request.config)
    base.mkdir(parents=True, exist_ok=True)
    return base


@pytest.fixture
def test_output_dir(request: pytest.FixtureRequest, _test_output_dir: Path) -> Path:
    """Per-test subdirectory under test_output/ for all generated files (config + audio)."""
    test_name = re.sub(r"[^\w.-]", "_", request.node.name)
    d = _test_output_dir / test_name
    d.mkdir(parents=True)
    return d


@pytest.fixture(scope="session")
def am_binaries(request: pytest.FixtureRequest) -> list[BinaryUnderTest]:
    """
    Returns a list of BinaryUnderTest instances for parametrizing AM tests.
    Reads the list built by pytest_configure to avoid duplicating that logic.
    """
    return request.config._rtlsdr_am_binaries


@pytest.fixture(scope="session")
def mp3_tolerance(request: pytest.FixtureRequest) -> float:
    """MP3 duration tolerance: 10% in both modes.

    Fast mode (10x speedup) adds some demod-thread timing jitter — measured
    worst-case is ~6% on multichannel non-nfm under load. 10% gives a ~1.5x
    margin over that; tightening further risks intermittent CI failures.
    """
    return 0.10


@pytest.fixture(scope="session")
def max_overrun_count(request: pytest.FixtureRequest) -> int:
    """Allowed output_overrun_count threshold per mode.

    Thorough mode allows 1 overrun to absorb a first-batch warm-up race:
    on the very first batch the output thread does LAME init + file
    create + marker-tone padding before clearing dev->waveavail, and on
    slow hosts this can occasionally exceed the 125 ms batch period —
    the demod thread then produces a second batch with waveavail still
    set and increments the counter by one. Anything beyond 1 is a real
    regression in the producer/consumer pipeline.

    Fast mode (10x speedup) plus parallel pytest-xdist workers
    contending for CPU does occasionally trip the same race by a few
    additional batches; allow up to 5 to absorb that without losing the
    assertion's value as a regression sentinel.
    """
    return 5 if request.config.getoption("--mode") == "fast" else 1


@pytest.fixture(scope="session")
def speedup_factor(request: pytest.FixtureRequest) -> float:
    """IQ playback speedup: 10x in fast mode, 1x in thorough mode."""
    return 10.0 if request.config.getoption("--mode") == "fast" else 1.0


# ---------------------------------------------------------------------------
# Binary runner
# ---------------------------------------------------------------------------


def run_rtl_airband(
    binary: Path,
    config_path: Path,
    timeout_s: float,
) -> subprocess.CompletedProcess:
    """
    Run: <binary> -F -e -c <config_path>

    Captures stdout and stderr. On timeout, re-raises with captured stderr.
    Asserts returncode == 0, including stderr in the assertion message on failure.
    """
    cmd = (["sudo"] if _use_sudo else []) + [
        str(binary),
        "-F",
        "-e",
        "-c",
        str(config_path),
    ]
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout_s,
        )
    except subprocess.TimeoutExpired as exc:
        stderr_so_far = (
            exc.stderr.decode("utf-8", errors="replace") if exc.stderr else ""
        )
        raise subprocess.TimeoutExpired(
            cmd,
            timeout_s,
            output=exc.output,
            stderr=f"Process timed out after {timeout_s}s.\nStderr so far:\n{stderr_so_far}",
        ) from exc

    assert result.returncode == 0, (
        f"rtl_airband exited with code {result.returncode}.\n"
        f"Command: {' '.join(cmd)}\n"
        f"Stderr:\n{result.stderr}\n"
        f"Stdout:\n{result.stdout}"
    )
    return result


# ---------------------------------------------------------------------------
# User-provided test cases
# ---------------------------------------------------------------------------

_NAME_RE = re.compile(r"^[A-Za-z0-9_]+$")

_VALID_BINARIES = {"non-nfm", "nfm", "both"}
_VALID_MODES = {"multichannel", "scan"}


@dataclass
class UserMixerCase:
    name: str  # mixer name referenced by channel mixer_output entries
    label: str  # output filename template
    expected_audio_s: float


@dataclass
class UserChannelCase:
    freq_hz: int
    modulation: str | None
    squelch: float | None
    ctcss: float | None
    bandwidth: int | None
    notch: float | None
    mixer_output: dict | None  # {"name": str, "balance": float} or None
    label: str
    expected_audio_s: float
    scan_freqs_hz: list[int] | None  # scan mode only


@dataclass
class UserTestCase:
    name: str
    description: str
    binary: str  # "non-nfm" or "nfm"
    iq_file: Path  # resolved absolute path
    sample_rate: int
    centerfreq_hz: int
    mode: str
    fft_size: int | None
    mixers: list[UserMixerCase]
    channels: list[UserChannelCase]


def _parse_channel(raw: dict[str, Any], index: int, mode: str) -> UserChannelCase:
    """Parse and validate a channel dict from the JSON schema."""
    freq_hz = raw.get("freq_hz")
    if freq_hz is None:
        raise ValueError(f"Channel {index}: missing required field 'freq_hz'")
    if not isinstance(freq_hz, int):
        raise ValueError(f"Channel {index}: 'freq_hz' must be an integer")

    expected_audio_s = raw.get("expected_audio_s")
    if expected_audio_s is None:
        raise ValueError(f"Channel {index}: missing required field 'expected_audio_s'")

    modulation_raw = raw.get("modulation", None)
    if modulation_raw is not None and modulation_raw not in ("am", "nfm"):
        raise ValueError(
            f"Channel {index}: 'modulation' must be 'am' or 'nfm', got {modulation_raw!r}"
        )
    modulation = modulation_raw
    squelch_raw = raw.get("squelch", None)
    squelch = float(squelch_raw) if squelch_raw is not None else None
    ctcss_raw = raw.get("ctcss", None)
    ctcss = float(ctcss_raw) if ctcss_raw is not None else None
    bandwidth_raw = raw.get("bandwidth", None)
    bandwidth = int(bandwidth_raw) if bandwidth_raw is not None else None
    notch_raw = raw.get("notch", None)
    notch = float(notch_raw) if notch_raw is not None else None
    mixer_output_raw = raw.get("mixer_output", None)
    mixer_output: dict | None = None
    if mixer_output_raw is not None:
        mo_name = mixer_output_raw.get("name")
        mo_balance = mixer_output_raw.get("balance")
        if mo_name is None:
            raise ValueError(
                f"Channel {index}: mixer_output missing required field 'name'"
            )
        if mo_balance is None:
            raise ValueError(
                f"Channel {index}: mixer_output missing required field 'balance'"
            )
        mixer_output = {"name": str(mo_name), "balance": float(mo_balance)}
    label = raw.get("label", f"ch{index}")

    scan_freqs_hz: list[int] | None = None
    if mode == "scan":
        scan_freqs_raw = raw.get("scan_freqs_hz")
        if scan_freqs_raw is None:
            raise ValueError(f"Channel {index}: 'scan_freqs_hz' required in scan mode")
        scan_freqs_hz = [int(f) for f in scan_freqs_raw]

    return UserChannelCase(
        freq_hz=int(freq_hz),
        modulation=modulation,
        squelch=squelch,
        ctcss=ctcss,
        bandwidth=bandwidth,
        notch=notch,
        mixer_output=mixer_output,
        label=label,
        expected_audio_s=float(expected_audio_s),
        scan_freqs_hz=scan_freqs_hz,
    )


def load_extra_test_cases(json_path: Path) -> list[UserTestCase]:
    """
    Parse and validate the user-provided test case JSON file.

    IQ file paths in the JSON are resolved relative to the JSON file's directory.
    Raises ValueError with a clear message if the file is malformed, a name is
    invalid/duplicate, or a required field is missing.
    """
    try:
        raw_data = json.loads(json_path.read_text())
    except json.JSONDecodeError as exc:
        raise ValueError(f"Failed to parse JSON from {json_path}: {exc}") from exc

    if not isinstance(raw_data, dict) or "test_cases" not in raw_data:
        raise ValueError(
            f"{json_path}: top-level JSON must be an object with 'test_cases' key"
        )

    raw_cases = raw_data["test_cases"]
    if not isinstance(raw_cases, list):
        raise ValueError(f"{json_path}: 'test_cases' must be an array")

    json_dir = json_path.parent
    seen_names: set[str] = set()
    test_cases: list[UserTestCase] = []

    for i, raw in enumerate(raw_cases):
        # name
        name = raw.get("name")
        if name is None:
            raise ValueError(f"Test case {i}: missing required field 'name'")
        if not _NAME_RE.match(name):
            raise ValueError(
                f"Test case {i}: 'name' must be alphanumeric+underscores only, got {name!r}"
            )
        if name in seen_names:
            raise ValueError(f"Test case {i}: duplicate name {name!r}")
        seen_names.add(name)

        # binary
        binary_field = raw.get("binary")
        if binary_field is None:
            raise ValueError(f"Test case {name!r}: missing required field 'binary'")
        if binary_field not in _VALID_BINARIES:
            raise ValueError(
                f"Test case {name!r}: 'binary' must be one of {_VALID_BINARIES}, got {binary_field!r}"
            )

        # iq_file
        iq_file_raw = raw.get("iq_file")
        if iq_file_raw is None:
            raise ValueError(f"Test case {name!r}: missing required field 'iq_file'")
        iq_file = (json_dir / iq_file_raw).resolve()

        # sample_rate
        sample_rate = raw.get("sample_rate")
        if sample_rate is None:
            raise ValueError(
                f"Test case {name!r}: missing required field 'sample_rate'"
            )
        sample_rate = int(sample_rate)
        if sample_rate <= 16000:
            raise ValueError(f"Test case {name!r}: 'sample_rate' must be > 16000")

        # centerfreq_hz
        centerfreq_hz = raw.get("centerfreq_hz")
        if centerfreq_hz is None:
            raise ValueError(
                f"Test case {name!r}: missing required field 'centerfreq_hz'"
            )

        # mode
        mode = raw.get("mode", "multichannel")
        if mode not in _VALID_MODES:
            raise ValueError(
                f"Test case {name!r}: 'mode' must be one of {_VALID_MODES}, got {mode!r}"
            )

        # channels
        channels_raw = raw.get("channels")
        if channels_raw is None:
            raise ValueError(f"Test case {name!r}: missing required field 'channels'")
        if not isinstance(channels_raw, list) or len(channels_raw) == 0:
            raise ValueError(
                f"Test case {name!r}: 'channels' must be a non-empty array"
            )

        channels = [_parse_channel(ch, j, mode) for j, ch in enumerate(channels_raw)]

        fft_size_raw = raw.get("fft_size", None)
        fft_size = int(fft_size_raw) if fft_size_raw is not None else None

        mixers: list[UserMixerCase] = []
        for k, mx in enumerate(raw.get("mixers", [])):
            mx_name = mx.get("name")
            mx_label = mx.get("label")
            mx_expected = mx.get("expected_audio_s")
            if mx_name is None:
                raise ValueError(
                    f"Test case {name!r}: mixer {k} missing required field 'name'"
                )
            if mx_label is None:
                raise ValueError(
                    f"Test case {name!r}: mixer {k} missing required field 'label'"
                )
            if mx_expected is None:
                raise ValueError(
                    f"Test case {name!r}: mixer {k} missing required field 'expected_audio_s'"
                )
            mixers.append(
                UserMixerCase(
                    name=str(mx_name),
                    label=str(mx_label),
                    expected_audio_s=float(mx_expected),
                )
            )

        test_cases.append(
            UserTestCase(
                name=name,
                description=raw.get("description", ""),
                binary=binary_field,
                iq_file=iq_file,
                sample_rate=sample_rate,
                centerfreq_hz=int(centerfreq_hz),
                mode=mode,
                fft_size=fft_size,
                mixers=mixers,
                channels=channels,
            )
        )

    return test_cases
