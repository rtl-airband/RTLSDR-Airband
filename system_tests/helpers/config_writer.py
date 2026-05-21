"""
Config writer for RTLSDR-Airband system tests.

Generates minimal libconfig++-format .conf files for the rtl_airband binary.
"""

from pathlib import Path


def write_config(
    config_path: Path,
    iq_filepath: Path,
    sample_rate: int,
    centerfreq_hz: int,
    channels: list[dict],
    output_dir: Path,
    speedup_factor: float = 1.0,
    mode: str = "multichannel",
    fft_size: int | None = None,
    mixers: list[dict] | None = None,
    mp3_tmp_dir: Path | None = None,
    stats_filepath: Path | None = None,
) -> None:
    """
    Write a minimal libconfig++-format .conf file for rtl_airband.

    Args:
        config_path: Where to write the config file.
        iq_filepath: Path to the IQ fixture file (file input driver).
        sample_rate: IQ sample rate in Hz.
        centerfreq_hz: Center frequency in Hz.
        channels: List of channel dicts with keys:
            - freq_hz (int): Channel center frequency in Hz.
            - modulation (str|None): "am" or "nfm", omitted if None (binary default).
            - squelch (float): Squelch threshold (0 = disabled).
            - ctcss (float|None): CTCSS tone Hz, omitted if None.
            - bandwidth (int|None): NFM demodulation bandwidth in Hz, omitted if None.
            - notch (float|None): Notch filter frequency in Hz, omitted if None.
            - output_filename_template (str): Template for MP3 output filename
              (used only when mp3_tmp_dir is provided).
            - mixer_output (dict|None): {"name": str, "balance": float}, omitted if None.
            - scan_freqs_hz (list[int]): Scan mode only — list of frequencies in Hz.
        output_dir: Directory where mixer MP3 outputs are written. Unused when
            mixers is empty/None.
        speedup_factor: IQ replay speed factor (1.0 = real-time).
        mode: "multichannel" or "scan".
        fft_size: FFT size for the device, omitted if None (binary default).
        mixers: List of mixer dicts with keys:
            - name (str): Mixer name referenced by channel mixer_output entries.
            - label (str): Filename template for the mixer's MP3 output.
            Output files are written to output_dir.
        mp3_tmp_dir: If provided, each channel gets a "file" (MP3) output
            written to this directory using output_filename_template. The
            directory must already exist. rtl_airband appends a date+hour
            timestamp to the filename; use output_validator.validate_mp3()
            to find and validate the resulting file.
        stats_filepath: If provided, rtl_airband writes a Prometheus-format stats
            file to this path on shutdown.
    """
    lines = []
    if fft_size is not None:
        lines.append(f"fft_size = {fft_size};")

    if mixers:
        lines.append("mixers:")
        lines.append("{")
        for mx in mixers:
            lines.append(f"  {mx['name']}:")
            lines.append("  {")
            lines.append("    outputs:")
            lines.append("    (")
            lines.append("      {")
            lines.append('        type = "file";')
            lines.append(f'        directory = "{output_dir}";')
            lines.append(f'        filename_template = "{mx["label"]}";')
            lines.append("        continuous = false;")
            lines.append("      }")
            lines.append("    );")
            lines.append("  };")
        lines.append("};")

    lines.append("devices:")
    lines.append("({")
    lines.append('  type = "file";')
    lines.append(f'  filepath = "{iq_filepath}";')
    lines.append(f"  sample_rate = {sample_rate};")
    lines.append(f"  centerfreq = {centerfreq_hz};")
    lines.append(f"  speedup_factor = {speedup_factor:.6f};")
    if mode == "scan":
        lines.append('  mode = "scan";')
    lines.append("  channels:")
    lines.append("  (")

    for i, ch in enumerate(channels):
        is_last = i == len(channels) - 1
        lines.append("    {")

        if mode == "scan" and "scan_freqs_hz" in ch:
            # Scan mode: freqs list in MHz
            freq_mhz_list = ", ".join(f"{f / 1e6:.6f}" for f in ch["scan_freqs_hz"])
            lines.append(f"      freqs = ({freq_mhz_list});")
        else:
            # Normal mode: single freq in MHz
            freq_mhz = ch["freq_hz"] / 1e6
            lines.append(f"      freq = {freq_mhz:.6f};")

        if ch.get("modulation") is not None:
            lines.append(f'      modulation = "{ch["modulation"]}";')

        if ch.get("ctcss") is not None:
            lines.append(f"      ctcss = {ch['ctcss']:.1f};")

        if ch.get("bandwidth") is not None:
            lines.append(f"      bandwidth = {int(ch['bandwidth'])};")

        if ch.get("notch") is not None:
            lines.append(f"      notch = {ch['notch']:.2f};")

        if ch.get("squelch") is not None:
            lines.append(f"      squelch_snr_threshold = {ch['squelch']:.1f};")

        # Build output entries: file outputs use directory+template+append,
        # mixer outputs use name+balance.
        output_entries: list[dict] = []
        if mp3_tmp_dir is not None:
            output_entries.append(
                {
                    "type": "file",
                    "directory": str(mp3_tmp_dir),
                    "template": ch["output_filename_template"],
                }
            )
        if ch.get("mixer_output") is not None:
            output_entries.append(
                {
                    "type": "mixer",
                    "name": ch["mixer_output"]["name"],
                    "balance": ch["mixer_output"]["balance"],
                }
            )

        lines.append("      outputs: (")
        for j, entry in enumerate(output_entries):
            is_last_out = j == len(output_entries) - 1
            lines.append("        {")
            lines.append(f'          type = "{entry["type"]}";')
            if entry["type"] == "mixer":
                lines.append(f'          name = "{entry["name"]}";')
                lines.append(f'          balance = {entry["balance"]:.1f};')
            else:
                lines.append(f'          directory = "{entry["directory"]}";')
                lines.append(f'          filename_template = "{entry["template"]}";')
                lines.append("          append = false;")
            lines.append("        }" + ("" if is_last_out else ","))
        lines.append("      );")

        lines.append("    }" + ("" if is_last else ","))

    lines.append("  );")
    lines.append("});")

    if stats_filepath is not None:
        lines.append(f'stats_filepath = "{stats_filepath}";')

    config_path.write_text("\n".join(lines) + "\n")
