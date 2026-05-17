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

When a test crosses an hour boundary, rtl_airband rotates the MP3 file. The
Xing/Info VBR header is written at file-open with a placeholder frame count
and is not rewritten on rotation, so mutagen's `info.length` over-reports
duration on rotated files. _measure_mp3_total_duration() scans frame
headers directly so duration is correct regardless of Xing state.
"""

from pathlib import Path

from mutagen.mp3 import MP3

# rtl_airband hardcodes MP3_RATE = 8000 Hz for all MP3 output
_MP3_SAMPLE_RATE = 8000

# MPEG audio frame parsing tables (Layer 3 only — the only layer LAME emits).
# Indexed by the 4-bit bitrate field in the frame header.
_MP3_BITRATES_V1L3 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
_MP3_BITRATES_V2L3 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160]
# Indexed by [version_field][sample_rate_field]
_MP3_SAMPLE_RATES = {
    3: [44100, 48000, 32000],  # MPEG-1
    2: [22050, 24000, 16000],  # MPEG-2 LSF (used by LAME for the 16 kHz NFM path)
    0: [11025, 12000, 8000],  # MPEG-2.5 (used by LAME for the 8 kHz AM path)
}


def _mp3_frames_duration(path: Path) -> float:
    """
    Compute MP3 duration by scanning every frame header.

    Does not rely on the Xing/Info VBR header's frame count, which rtl_airband
    leaves stale when rotating files at hour boundaries.
    """
    data = path.read_bytes()
    # Skip ID3v2 tag if present (LAME does not write one, but be defensive).
    offset = 0
    if len(data) >= 10 and data[:3] == b"ID3":
        tag_size = (
            ((data[6] & 0x7F) << 21)
            | ((data[7] & 0x7F) << 14)
            | ((data[8] & 0x7F) << 7)
            | (data[9] & 0x7F)
        )
        offset = 10 + tag_size

    total_s = 0.0
    n = len(data)
    while offset + 4 <= n:
        b0 = data[offset]
        b1 = data[offset + 1]
        # Frame sync: 11 consecutive 1-bits.
        if b0 != 0xFF or (b1 & 0xE0) != 0xE0:
            offset += 1
            continue

        b2 = data[offset + 2]
        version = (b1 >> 3) & 0x3  # 3=MPEG-1, 2=MPEG-2, 0=MPEG-2.5, 1=reserved
        layer = (b1 >> 1) & 0x3  # 1=Layer 3
        bitrate_idx = (b2 >> 4) & 0xF
        sr_idx = (b2 >> 2) & 0x3
        padding = (b2 >> 1) & 0x1

        if version == 1 or layer != 1 or sr_idx == 3 or bitrate_idx in (0, 15):
            offset += 1
            continue

        bitrates = _MP3_BITRATES_V1L3 if version == 3 else _MP3_BITRATES_V2L3
        bitrate_kbps = bitrates[bitrate_idx]
        sample_rate = _MP3_SAMPLE_RATES[version][sr_idx]
        samples_per_frame = 1152 if version == 3 else 576
        frame_bytes = (samples_per_frame // 8) * (
            bitrate_kbps * 1000
        ) // sample_rate + padding
        if frame_bytes < 4:
            offset += 1
            continue

        total_s += samples_per_frame / sample_rate
        offset += frame_bytes

    return total_s


def _measure_mp3_total_duration(
    mp3_dir: Path, filename_template: str
) -> tuple[float, list[Path]]:
    """
    Return (total_duration_s, sorted_matches) across all MP3 files matching the
    template. Each file's duration is measured by frame scanning so the result
    is correct even when an hour-boundary rotation produced multiple files.
    """
    matches = sorted(mp3_dir.glob(f"{filename_template}_[0-9]*.mp3"))
    total_s = sum(_mp3_frames_duration(p) for p in matches)
    return total_s, matches


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
    actual_s, matches = _measure_mp3_total_duration(mp3_dir, filename_template)
    assert (
        matches
    ), f"No .mp3 output file found matching '{filename_template}_[0-9]*.mp3' in {mp3_dir}"

    # Per-file sanity (size, sample rate, bitrate). Duration was already
    # measured by frame scanning above.
    for mp3_file in matches:
        assert mp3_file.stat().st_size > 0, f"MP3 file is empty: {mp3_file.name}"
        audio = MP3(mp3_file)
        assert audio.info.sample_rate == _MP3_SAMPLE_RATE, (
            f"Expected MP3 sample rate {_MP3_SAMPLE_RATE} Hz, "
            f"got {audio.info.sample_rate} Hz: {mp3_file.name}"
        )
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
