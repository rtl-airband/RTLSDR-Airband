# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Working Norms

- **Never commit directly to `main`.** Check `git branch --show-current` before committing; if it is `main`, create a branch first. All changes reach `main` through a pull request. Name branches `<prefix>/<short_description>`, where the prefix is the contributor's GitHub username or a category such as `dev` or `fix` (e.g. `dev/drop_macos14_ci`).
- **Keep this file updated** as changes are made to the codebase.
- **Never guess or make assumptions** — ask clarifying questions when requirements are unclear.
- **All code changes should include unit tests.** When possible, write tests first and implement code to make them pass (TDD).
- **After writing code, review comments** and remove any that don't explain non-obvious behavior — don't comment what the code already says.

## Development Environment

The repo is set up for development in VS Code using a devcontainer (`.devcontainer/`). When working inside the devcontainer, all compile, test, and run commands must be executed inside the container.

Large system test cases for manual runs can be kept outside the repo in `../RTLSDR-Airband_test_cases/` (a sibling of the checkout, so several checkouts can share one copy). The devcontainer mounts it read-only at `system_tests/test_cases/` (gitignored); the pytest suite does not read it. `initializeCommand` creates an empty folder on the host if it is missing, so the container starts without it. This needs a POSIX host shell (macOS, Linux, WSL) and a container rebuild to take effect.

## Wiki Documentation

User-facing documentation lives in a separate repo: https://github.com/rtl-airband/RTLSDR-Airband/wiki

Flag when code changes require wiki updates and provide suggested content — do not edit the wiki directly.

## Code Review Guidelines

When reviewing code:
- Reference specific files and line numbers (`src/foo.cpp:42`)
- Start with architecture-level concerns before line-level feedback
- Consider SDR/DSP domain context (signal processing constraints, real-time threading, buffer management)
- Verify the testing approach covers the behavior being changed
- Structure feedback clearly: separate blocking issues from suggestions
- Be pragmatic — prefer working correct code over theoretical perfection
- Check for consistency with surrounding code style and conventions

## Project Overview

RTLSDR-Airband is a C++ SDR (Software-Defined Radio) application that receives analog radio voice channels from SDR devices (RTL-SDR, SoapySDR) and produces MP3 audio streams for Icecast, file recording, UDP, and PulseAudio.

## Build Commands

Dependencies: libconfig++, libmp3lame, libshout, libfftw3f, librtlsdr, libsoapysdr, libpulse, plus optional libgoogle-perftools for `-DPROFILING=TRUE` (`install_dependencies` installs it only where apt can resolve it; `scripts/build_all_combos` skips the profiling build when `libprofiler` isn't found). Install via `.github/install_dependencies`.

```bash
# Standard debug build with unit tests
cmake -B builds/Debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_UNITTESTS=TRUE
cmake --build builds/Debug -j4

# Release build with NFM and SoapySDR
cmake -B builds/Release -DCMAKE_BUILD_TYPE=Release -DNFM=TRUE -DSOAPYSDR=ON
cmake --build builds/Release -j4

# Run unit tests
./builds/Debug/src/unittests
./builds/Release/src/unittests

# Run the binary
./builds/Debug/src/rtl_airband -c /path/to/config.conf
./builds/Release/src/rtl_airband -c /path/to/config.conf
```

Key CMake flags (all in `src/CMakeLists.txt`):

| Flag | Default | Purpose |
|------|---------|---------|
| `NFM` | OFF | Enable Narrow FM demodulation |
| `PLATFORM` | `native` | Optimization target: `native`, `generic`, `rpiv2` |
| `RTLSDR` | ON | RTL-SDR driver |
| `SOAPYSDR` | ON | SoapySDR (vendor-neutral) driver |
| `PULSEAUDIO` | ON | PulseAudio output |
| `BUILD_UNITTESTS` | OFF | Build Google Test unit tests |
| `BCM_VC` | OFF | Broadcom VideoCore GPU FFT (RPi v2 only; requires `PLATFORM=rpiv2`, which also turns it on) |

`RTLSDR`, `SOAPYSDR`, `PULSEAUDIO`, and `PROFILING` are hard requirements when ON: if the library is not found, CMake fails at configure time. Pass `-D<FLAG>=OFF` to build without it.

## Code Formatting and Pre-commit

Uses clang-format with Chromium style (indent=4, column limit=200, config in `.clang-format`). The version is pinned only in the `dev` group of `pyproject.toml` (locked in `uv.lock`); the pre-commit hook, the `code_formatting.yml` workflow, and VS Code all run that copy via `uv`, never a distro `clang-format-N`. The dependency is skipped on armv6l (no wheel), so the hook cannot run there. To change the version, edit the pin, run `uv lock`, then `pre-commit run clang-format --all-files`.

```bash
# Install pre-commit hooks (once, after cloning)
pre-commit install

# Run all pre-commit hooks manually
pre-commit run --all-files

# Format C++ source files manually (also used by CI)
pre-commit run clang-format --all-files
```

Hooks are defined in `.pre-commit-config.yaml`. The build/unit-test hook runs when `src/*.cpp`, `src/*.h`, or `CMakeLists.txt` change; the system-test hook also runs when `system_tests/` changes, and only if the build/unit-test step passes.

## CI and Pull Request Checks

Workflows live in `.github/workflows/`. The self-hosted Pi runners, their provisioning, and the Docker image are covered by the `ci-and-docker` skill (`.claude/skills/ci-and-docker/`).

Self-hosted runner security (public repo): `platform_build` is the only self-hosted job and is gated with `if: github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository` so **fork** PRs never execute on the hardware — keep this guard on any self-hosted job. This complements the repo setting requiring approval for outside collaborators, network-segmented runners, and keeping repo secrets out of these workflows.

**Before submitting a PR**, the pre-commit hooks cover most checks automatically. For build system or config changes not touching `src/`, verify all four cmake configurations build cleanly by hand (Debug and Release, each with and without `-DNFM=TRUE`, all with `-DBUILD_UNITTESTS=TRUE`).

## Version Tagging

`version_bump.yml` runs when a PR is **merged into `main`** and tags the merge commit with [`anothrNick/github-tag-action@1.64.0`](https://github.com/anothrNick/github-tag-action). Configured with `WITH_V: true` (tags look like `v5.3.0`) and `DEFAULT_BUMP: patch`.

The action scans the **full commit message body of every commit between the previous tag and the merge commit** (`git log "$tag_commit".."$commit" --format=%B`) for a bump keyword, so the keyword can live in any commit in the PR — it does not have to be in the merge commit. "Previous tag" means the highest semver tag, not the most recent one by date:

| Keyword in a commit message | Result |
|------|--------|
| `#major` | `v5.3.0` → `v6.0.0` |
| `#minor` | `v5.3.0` → `v5.4.0` |
| `#patch` | `v5.3.0` → `v5.3.1` |
| `#none` | no tag is created |
| none of the above | `DEFAULT_BUMP: patch` applies → `v5.3.1` |

Rules when writing commit messages:

- **Every merged PR creates a tag** unless a commit says `#none`. With `DEFAULT_BUMP: patch`, doing nothing still bumps the patch version, so add a keyword only to ask for something other than a patch.
- **Add `#minor` for a new user-facing feature or a new config option.** Add `#major` for a breaking change — a removed or renamed config key, or changed default behavior.
- **Highest keyword wins**, checked in the order `#major` → `#minor` → `#patch` → `#none`. One `#major` anywhere in the PR's commits bumps major even if other commits say `#minor`.
- **Matching is a plain substring search over the whole message**, body included. Never write these tokens in prose (for example "fixed a #minor issue") — it will bump the version. Refer to them as "the #minor keyword" only outside commit messages.
- **`#minor` and `#major` also publish a GitHub Release** with generated notes; `#patch` and the default bump only create the tag (`if: steps.tag.outputs.part != 'patch'`).
- Squash-merging collapses the PR's commits into one message, so make sure the keyword survives into the squash message.

After tagging, the workflow re-runs `ci_build.yml`, `platform_build.yml`, and `build_docker_containers.yml` against the new tag (a tag pushed with `GITHUB_TOKEN` does not fire their own `tags: ['v*']` triggers, so they have to be dispatched explicitly).

Note that those three dispatch steps are unguarded: on a `#none` merge the action leaves `new_tag` at the **existing** tag, so they re-run against the previous release and republish its container images.

## System Tests

End-to-end tests live in `system_tests/`. They run the actual binary against generated IQ files and validate the audio output (MP3 duration, rawfile size). Managed with [uv](https://docs.astral.sh/uv/).

```bash
# Run system tests (requires Release binaries — run scripts/run_unit_tests first)
scripts/run_system_tests
```

Python tooling (formatter, import sorter, linter) is configured in the repo-root `pyproject.toml` under `[tool.black]`, `[tool.isort]`, and `[tool.pylint]`. Run them manually from the repo root:

```bash
uv run black system_tests/
uv run isort system_tests/
uv run pylint system_tests/conftest.py system_tests/helpers/ system_tests/tests/
```

## Architecture

### Reception Pipeline

```
SDR device (input-*.cpp)
  → RX thread → circular sample buffer
  → demod thread: FFT (FFTW3) → demod (AM/NFM) → filter → CTCSS → squelch → AGC
  → channel output handlers
  → output thread: MP3 encode (lame) → Icecast / file / UDP / PulseAudio
```

Device modes, the threading model, and the config format are documented in `src/CLAUDE.md` (loaded when working under `src/`).

### Unit Tests

Tests use Google Test (fetched via CMake FetchContent). Test files in `src/test_*.cpp` cover filters, squelch, CTCSS, helper functions, and signal generation. `src/test_base_class.h` provides test utilities.
