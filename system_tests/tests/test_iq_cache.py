"""
test_iq_cache.py — unit tests for the LRU byte-budget cache in iq_generator
and the cache/output helpers in conftest (size parser, dir resolution, the
xdist-controller guard, and the directory-wipe helper).

These exercise pure helper logic and do not run the rtl_airband binary.
"""

import os
from pathlib import Path

import numpy as np
import pytest
from conftest import (
    DEFAULT_CACHE_DIR,
    DEFAULT_TEST_OUTPUT_DIR,
    _is_xdist_controller,
    _resolve_cache_dir,
    _resolve_test_output_dir,
    _wipe_dir_contents,
    parse_size,
)
from helpers import iq_generator


class _FakeConfig:  # pylint: disable=too-few-public-methods
    """Minimal pytest.Config stand-in for testing option resolvers.

    workerinput is set only when provided, mirroring pytest-xdist (the attribute
    exists on workers, is absent on the controller).
    """

    def __init__(self, options: dict, workerinput: dict | None = None):
        self._options = options
        if workerinput is not None:
            self.workerinput = workerinput

    def getoption(self, name: str):
        if name in self._options:
            return self._options[name]
        raise ValueError(f"unknown option {name}")


def _write(cache_dir: Path, name: str, nbytes: int) -> Path:
    """Write an .iq fixture of exactly nbytes through the budgeted writer."""
    assert nbytes % 2 == 0, "iq files are 2 bytes per sample"
    n = nbytes // 2
    path = cache_dir / name
    iq_generator._write_iq(path, np.zeros(n, np.uint8), np.zeros(n, np.uint8))
    return path


def _total(cache_dir: Path) -> int:
    return sum(f.stat().st_size for f in cache_dir.glob("*.iq"))


@pytest.fixture(autouse=True)
def _isolate_cache_state():
    """Give each test a clean cache state, then restore the session's.

    iq_generator holds the budget and LRU as module globals. These tests mutate
    them, so we snapshot on entry and restore on exit — otherwise this module
    would clobber the session-wide budget that pytest_configure set for the
    real system tests that sort after it.
    """
    saved_budget = iq_generator._cache_max_bytes
    saved_lru = iq_generator._lru.copy()
    saved_seeded = set(iq_generator._seeded_dirs)
    iq_generator.set_cache_budget(None)
    yield
    iq_generator._cache_max_bytes = saved_budget
    iq_generator._lru.clear()
    iq_generator._lru.update(saved_lru)
    iq_generator._seeded_dirs.clear()
    iq_generator._seeded_dirs.update(saved_seeded)


# ---------------------------------------------------------------------------
# parse_size
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "value,expected",
    [
        (None, None),
        ("", None),
        ("  ", None),
        ("100", 100),
        ("1K", 1024),
        ("2M", 2 * 1024**2),
        ("1G", 1024**3),
        ("1.5G", int(1.5 * 1024**3)),
        ("90m", 90 * 1024**2),  # suffix is case-insensitive
        ("0", 0),
    ],
)
def test_parse_size_valid(value, expected):
    assert parse_size(value) == expected


@pytest.mark.parametrize("value", ["abc", "10X", "1.2.3", "-5", "-1M", "K", "M"])
def test_parse_size_invalid(value):
    with pytest.raises(ValueError):
        parse_size(value)


# ---------------------------------------------------------------------------
# cache dir resolution (--generated-input-dir)
# ---------------------------------------------------------------------------


def test_resolve_cache_dir_default():
    cfg = _FakeConfig({"--generated-input-dir": None})
    assert _resolve_cache_dir(cfg) == DEFAULT_CACHE_DIR


def test_resolve_cache_dir_override(tmp_path):
    cfg = _FakeConfig({"--generated-input-dir": str(tmp_path)})
    assert _resolve_cache_dir(cfg) == tmp_path


def test_resolve_cache_dir_option_absent():
    # Option not registered (e.g. plugin loading) → fall back to default.
    assert _resolve_cache_dir(_FakeConfig({})) == DEFAULT_CACHE_DIR


def test_resolve_test_output_dir_default():
    cfg = _FakeConfig({"--test-output-dir": None})
    assert _resolve_test_output_dir(cfg) == DEFAULT_TEST_OUTPUT_DIR


def test_resolve_test_output_dir_override(tmp_path):
    cfg = _FakeConfig({"--test-output-dir": str(tmp_path)})
    assert _resolve_test_output_dir(cfg) == tmp_path


# ---------------------------------------------------------------------------
# xdist controller guard + directory wipe
# ---------------------------------------------------------------------------


def test_is_xdist_controller_when_no_workerinput():
    # No workerinput attr → controller (or a non-xdist run).
    assert _is_xdist_controller(_FakeConfig({})) is True


def test_is_xdist_controller_false_on_worker():
    cfg = _FakeConfig({}, workerinput={"workerid": "gw3"})  # xdist sets this on workers
    assert _is_xdist_controller(cfg) is False


def test_wipe_dir_contents_removes_children_keeps_dir(tmp_path):
    (tmp_path / "a.txt").write_text("x")
    sub = tmp_path / "sub"
    sub.mkdir()
    (sub / "b.txt").write_text("y")

    _wipe_dir_contents(tmp_path)

    assert tmp_path.exists()  # dir itself preserved (may be a mount point)
    assert not list(tmp_path.iterdir())


def test_wipe_dir_contents_noop_on_missing_dir(tmp_path):
    missing = tmp_path / "does_not_exist"
    _wipe_dir_contents(missing)  # must not raise
    assert not missing.exists()


# ---------------------------------------------------------------------------
# LRU eviction
# ---------------------------------------------------------------------------


def test_unlimited_never_evicts(tmp_path):
    iq_generator.set_cache_budget(None)
    for i in range(5):
        _write(tmp_path, f"f{i}.iq", 100)
    assert len(list(tmp_path.glob("*.iq"))) == 5
    assert _total(tmp_path) == 500


def test_evicts_least_recently_used(tmp_path):
    iq_generator.set_cache_budget(250)  # room for ~2 files of 100 bytes
    _write(tmp_path, "a.iq", 100)
    _write(tmp_path, "b.iq", 100)
    _write(tmp_path, "c.iq", 100)  # forces eviction of the oldest, a
    assert not (tmp_path / "a.iq").exists()
    assert (tmp_path / "b.iq").exists()
    assert (tmp_path / "c.iq").exists()
    assert _total(tmp_path) <= 250


def test_touch_protects_recently_used(tmp_path):
    iq_generator.set_cache_budget(250)
    a = _write(tmp_path, "a.iq", 100)
    _write(tmp_path, "b.iq", 100)
    iq_generator._register_hit(a)  # a is now most-recently-used
    _write(tmp_path, "c.iq", 100)  # should evict b, not a
    assert (tmp_path / "a.iq").exists()
    assert not (tmp_path / "b.iq").exists()
    assert (tmp_path / "c.iq").exists()


def test_reused_fixture_not_regenerated(tmp_path):
    """A cache hit keeps the file warm so it survives later eviction pressure."""
    iq_generator.set_cache_budget(250)
    a = _write(tmp_path, "a.iq", 100)
    a_mtime = a.stat().st_mtime_ns
    _write(tmp_path, "b.iq", 100)
    iq_generator._register_hit(a)
    _write(tmp_path, "c.iq", 100)  # evicts b
    assert a.exists()
    assert a.stat().st_mtime_ns == a_mtime  # untouched on disk, never rewritten


def test_file_larger_than_budget_still_written(tmp_path):
    iq_generator.set_cache_budget(50)
    _write(tmp_path, "a.iq", 40)
    big = _write(tmp_path, "big.iq", 200)  # exceeds budget by itself
    assert big.exists()
    assert not (tmp_path / "a.iq").exists()  # everything else evicted to make room


def test_hit_before_first_write_preserves_lru_order(tmp_path):
    """A cache hit before the session's first write must not invert LRU order.

    Regression: _touch must seed pre-existing files so a hit can't jump ahead
    of older, untouched fixtures in the LRU. Without seeding in _touch, the
    just-hit file would be evicted before the older one.
    """
    iq_generator.set_cache_budget(None)
    old1 = _write(tmp_path, "old1.iq", 100)
    old2 = _write(tmp_path, "old2.iq", 100)
    os.utime(old1, (1000, 1000))  # old1 strictly older than old2 on disk
    os.utime(old2, (2000, 2000))

    # Fresh session with a budget; LRU not yet seeded.
    iq_generator.set_cache_budget(250)
    iq_generator._register_hit(old2)  # hit before any write
    _write(tmp_path, "new.iq", 100)  # 300 > 250 → one eviction

    assert old2.exists()  # recently hit → survives
    assert not (tmp_path / "old1.iq").exists()  # oldest, untouched → evicted
    assert (tmp_path / "new.iq").exists()


def test_seeds_from_preexisting_files(tmp_path):
    """Files already on disk (no --clean) are eligible for eviction."""
    # Create pre-existing files directly, unlimited so nothing is tracked.
    iq_generator.set_cache_budget(None)
    _write(tmp_path, "old.iq", 100)
    # Now impose a budget; the writer must account for the pre-existing file.
    iq_generator.set_cache_budget(150)
    _write(tmp_path, "new.iq", 100)  # old + new = 200 > 150 → old evicted
    assert not (tmp_path / "old.iq").exists()
    assert (tmp_path / "new.iq").exists()
    assert _total(tmp_path) <= 150
