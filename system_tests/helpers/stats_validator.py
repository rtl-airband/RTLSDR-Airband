"""
Stats file validator for RTLSDR-Airband system tests.

Parses the Prometheus-format stats file written by rtl_airband on shutdown
and provides typed accessors for use in test assertions.

File format (tab-separated key/value, Prometheus text protocol):
    # HELP metric_name description
    # TYPE metric_name gauge|counter
    metric_name{label1="v1",label2="v2"}\tvalue
    ...

Channel metrics use freq="<MHz:.3f>" as the primary label, e.g. freq="120.025".
Device metrics use device="<index>".
"""

from __future__ import annotations

from pathlib import Path


class StatsFile:
    """Parsed rtl_airband stats file with typed accessors."""

    def __init__(self, metrics: dict[str, list[tuple[str, float]]]) -> None:
        self._metrics = metrics

    def channel(self, metric: str, freq_hz: int) -> float | None:
        """
        Return the value of a per-channel metric for the given frequency.

        freq_hz is in Hz; it is converted to the MHz string used as the label.
        Returns None if the metric or frequency is not present.
        """
        freq_str = f"{freq_hz / 1_000_000:.3f}"
        for labels_str, value in self._metrics.get(metric, []):
            if f'freq="{freq_str}"' in labels_str:
                return value
        return None

    def device(self, metric: str, index: int = 0) -> float | None:
        """
        Return the value of a per-device metric.

        Returns None if the metric or device index is not present.
        """
        for labels_str, value in self._metrics.get(metric, []):
            if f'device="{index}"' in labels_str:
                return value
        return None


def assert_no_excessive_overruns(stats: StatsFile, max_overrun_count: int) -> None:
    """Assert the output thread did not fall behind demod by more than max_overrun_count batches.

    Thorough mode requires zero overruns; fast mode allows a small budget to
    absorb CPU contention from -n auto. See the max_overrun_count fixture
    in conftest.py for the rationale behind the per-mode thresholds.
    """
    overruns = stats.device("output_overrun_count")
    assert overruns <= max_overrun_count, (
        f"Output thread fell behind demod by {overruns} batches "
        f"(allowed in this mode: <= {max_overrun_count}) — wave batches "
        "were overwritten before being read"
    )


def load(path: Path) -> StatsFile:
    """
    Parse a Prometheus-format rtl_airband stats file.

    Raises AssertionError if the file does not exist or is empty.
    """
    assert path.exists(), f"Stats file not found: {path}"
    assert path.stat().st_size > 0, f"Stats file is empty: {path}"

    metrics: dict[str, list[tuple[str, float]]] = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        tab_idx = line.rfind("\t")
        if tab_idx < 0:
            continue
        key_part = line[:tab_idx]
        val_part = line[tab_idx + 1 :]
        try:
            value = float(val_part)
        except ValueError:
            continue
        if "{" in key_part:
            name, rest = key_part.split("{", 1)
            labels = rest.rstrip("}")
        else:
            name = key_part
            labels = ""
        metrics.setdefault(name, []).append((labels, value))

    return StatsFile(metrics)
