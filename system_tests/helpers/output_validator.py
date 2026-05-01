"""
Output validator for RTLSDR-Airband system tests.

Level 2 validation: verifies rawfile byte count against expected duration,
and MP3 duration / audio properties against expected values.

Rawfile format: interleaved float32 I/Q pairs (complex float, .cf32 extension).
Bytes per sample pair = 2 * sizeof(float32) = 8.

MP3 format: LAME-encoded, mono, 8000 Hz sample rate, VBR with LAME tag.
rtl_airband appends a date+hour timestamp to the filename_template, so MP3
files are found via glob. Each test writes to its own test_output/<name>/
subdirectory, so filenames are unique within a run.
"""

from pathlib import Path

from mutagen.mp3 import MP3

# rtl_airband hardcodes MP3_RATE = 8000 Hz for all MP3 output
_MP3_SAMPLE_RATE = 8000


def validate_rawfile(
    output_dir: Path,
    filename_template: str,
    expected_duration_s: float,
    wave_rate: int,
    tolerance: float = 0.15,
) -> None:
    """
    Assert that a rawfile output has a byte count within *tolerance* of expected.

    The rawfile extension is .cf32 (complex float32, interleaved I/Q pairs).
    Each sample pair is two float32 values = 8 bytes.

    Args:
        output_dir: Directory containing the output files.
        filename_template: Filename template used in the config (without extension).
        expected_duration_s: Expected audio duration in seconds.
        wave_rate: Output sample rate (8000 for non-NFM, 16000 for NFM).
        tolerance: Allowed fractional deviation from expected byte count (default ±15%).

    Raises:
        AssertionError: If no matching file is found or byte count is out of range.
    """
    matches = list(output_dir.glob(f"{filename_template}_[0-9]*.cf32"))
    assert (
        matches
    ), f"No .cf32 output file found matching '{filename_template}*.cf32' in {output_dir}"
    output_file = matches[0]
    actual_bytes = output_file.stat().st_size

    # expected_bytes = duration * wave_rate * 2 (I+Q pair) * 4 bytes per float32
    expected_bytes = expected_duration_s * wave_rate * 2 * 4

    deviation = abs(actual_bytes - expected_bytes) / expected_bytes
    print(
        f"rawfile {output_file.name}: "
        f"actual={actual_bytes} expected={expected_bytes:.0f} "
        f"deviation={deviation*100:.1f}% (limit ±{tolerance*100:.0f}%)"
    )
    assert deviation <= tolerance, (
        f"Rawfile byte count out of range: "
        f"actual={actual_bytes}, expected={expected_bytes:.0f} "
        f"(±{tolerance*100:.0f}%), deviation={deviation*100:.1f}%, "
        f"file={output_file}"
    )


def assert_output_silent(output_dir: Path, filename_template: str) -> None:
    """
    Assert that either no .cf32 file was created matching the template, or it is empty.

    This is used for squelch-closed and wrong-CTCSS tests where no audio should be written.

    Args:
        output_dir: Directory that would contain the output files.
        filename_template: Filename template used in the config (without extension).

    Raises:
        AssertionError: If a non-empty .cf32 file is found.
    """
    matches = list(output_dir.glob(f"{filename_template}_[0-9]*.cf32"))
    if not matches:
        return  # No file created — correct for squelch-closed

    for output_file in matches:
        actual_bytes = output_file.stat().st_size
        assert actual_bytes == 0, (
            f"Expected no audio output (squelch/CTCSS gate closed), but "
            f"{output_file} contains {actual_bytes} bytes"
        )


def validate_mp3(
    mp3_dir: Path,
    filename_template: str,
    expected_duration_s: float,
    tolerance: float,
) -> Path:
    """
    Assert that an MP3 output exists with expected duration and valid audio properties.

    rtl_airband appends a date+hour timestamp to the filename_template, so this
    function uses a glob to locate the file. Returns the path to the validated file.

    Expected audio properties (from rtl_airband's hardcoded LAME settings):
      - Sample rate: 8000 Hz (MP3_RATE)
      - Mode: mono
      - Encoding: VBR with LAME tag (accurate duration in metadata)

    Args:
        mp3_dir: Directory containing the MP3 output files.
        filename_template: Filename template used in the config (without extension).
        expected_duration_s: Expected audio duration in seconds.
        tolerance: Allowed fractional deviation from expected duration (default ±20%).

    Returns:
        Path to the validated MP3 file.

    Raises:
        AssertionError: If no file is found, duration is out of range, or audio
                        properties (sample rate, bitrate) are invalid.
    """
    matches = list(mp3_dir.glob(f"{filename_template}_[0-9]*.mp3"))
    assert (
        matches
    ), f"No .mp3 output file found matching '{filename_template}_[0-9]*.mp3' in {mp3_dir}"

    # Validate each file and sum durations (multiple files arise from hour-boundary splits)
    actual_s = 0.0
    for mp3_file in matches:
        assert mp3_file.stat().st_size > 0, f"MP3 file is empty: {mp3_file.name}"
        audio = MP3(mp3_file)
        actual_s += audio.info.length

        # Sample rate — hardcoded to 8000 Hz in rtl_airband (MP3_RATE)
        assert audio.info.sample_rate == _MP3_SAMPLE_RATE, (
            f"Expected MP3 sample rate {_MP3_SAMPLE_RATE} Hz, "
            f"got {audio.info.sample_rate} Hz: {mp3_file.name}"
        )

        # Bitrate — VBR so this is the average; must be positive (file has audio content)
        assert (
            audio.info.bitrate > 0
        ), f"MP3 average bitrate is 0 kbps (empty or corrupt file): {mp3_file.name}"

    label = (
        matches[0].name
        if len(matches) == 1
        else f"{len(matches)} files (hour boundary)"
    )
    deviation = abs(actual_s - expected_duration_s) / expected_duration_s
    print(
        f"mp3 {label}: "
        f"actual={actual_s:.2f}s expected={expected_duration_s:.2f}s "
        f"deviation={deviation*100:.1f}% (limit ±{tolerance*100:.0f}%)"
    )
    assert deviation <= tolerance, (
        f"MP3 duration {actual_s:.2f}s deviates {deviation:.1%} from "
        f"expected {expected_duration_s:.2f}s (±{tolerance:.0%}): {label}"
    )

    return matches[0]


def assert_mp3_present(mp3_dir: Path, filename_template: str) -> None:
    """
    Assert that at least one non-empty MP3 file exists for this template.

    Used when duration validation is skipped (e.g. mixer output at high speedup).

    Raises:
        AssertionError: If no matching file is found or all matching files are empty.
    """
    matches = list(mp3_dir.glob(f"{filename_template}_[0-9]*.mp3"))
    assert (
        matches
    ), f"No .mp3 output file found matching '{filename_template}_[0-9]*.mp3' in {mp3_dir}"
    for mp3_file in matches:
        assert mp3_file.stat().st_size > 0, f"MP3 file is empty: {mp3_file.name}"


def assert_mp3_silent(mp3_dir: Path, filename_template: str) -> None:
    """
    Assert that no non-empty MP3 file was created for this template.

    With non-continuous MP3 output (rtl_airband default), no file is created when
    the squelch stays closed for the entire run. Used for squelch-closed and
    wrong-CTCSS tests.

    Args:
        mp3_dir: Directory that would contain MP3 output files.
        filename_template: Filename template used in the config (without extension).

    Raises:
        AssertionError: If a non-empty MP3 file is found.
    """
    matches = list(mp3_dir.glob(f"{filename_template}_[0-9]*.mp3"))
    if not matches:
        return  # No file created — correct for squelch-closed

    for mp3_file in matches:
        assert mp3_file.stat().st_size == 0, (
            f"Expected no MP3 output (squelch/CTCSS gate closed), but "
            f"{mp3_file.name} contains {mp3_file.stat().st_size} bytes"
        )
