---
name: ci-and-docker
description: How the CI workflows, self-hosted Raspberry Pi runners, and the Alpine Docker image are built and provisioned in this repo. Use when editing .github/workflows, the Dockerfile, the runner provisioning scripts, or debugging CI or container build failures.
---

# CI runners and Docker image

Four workflows run checks on open pull requests (`.github/workflows/`); `platform_build.yml` skips fork PRs. All of them except `code_formatting.yml` also run on pushes to `main`, tags, `workflow_dispatch`, and a daily schedule; `code_formatting.yml` runs on PRs and daily. A fifth, `version_bump.yml`, runs after a PR is merged — see Version Tagging in the root `CLAUDE.md`.

**`platform_build.yml`** — all self-hosted hardware. A single `airband-proxy` runner rsyncs the checkout to each Pi over SSH and runs build + unit tests + system tests there, so the Pis need no runner agent (32-bit ARM has no Node 24 after the Node 20 EOL):

| Target | Arch | `PLATFORM` | `BCM_VC` | `--sudo` |
|--------|------|------------|----------|----------|
| `airband-4b` | 64-bit ARM | `native` | OFF | no |
| `airband-3b` | 32-bit ARM | `rpiv2` | ON | yes |

The workflow passes `--sudo` exactly when `BCM_VC=ON` (the VideoCore GPU FFT needs root). Each target's work dir is `~/rtlsdr-airband-ci` in the CI account's home (not a tmpfs, which is too small for the build tree and uv venv), reused across runs so builds stay warm; this relies on there being a single `airband-proxy` runner, which runs one job at a time.

The proxy runner runs as user `airband-proxy` (`700` home, no sudo) and holds the targets' SSH key. On each Pi, CI logs in as `airband-build`, which has no password and either no sudo or, on BCM targets, a sudo rule for only the per-run `rtl_airband` binary. Build deps are **pre-installed** on the Pis, so the workflow never runs `install_dependencies` there. **Re-provision the Pis when `.github/install_dependencies` changes** — a stale target fails at CMake configure time when a requested library (SoapySDR, PulseAudio) is missing. The profiling build is skipped when libgoogle-perftools is absent.

Provisioning:
1. As `airband-proxy`, run `.github/setup_orchestrator_ssh` (creates the key, pins host keys, and writes the managed `~/.ssh/config.d/airband-ci`, prepending an `Include` for it to `~/.ssh/config`; don't hand-edit the managed file, it is regenerated on every run) and copy the `sudo PROXY_PUBKEY=… PROXY_FROM=… .github/setup_remote_test_target` command it prints for each target.
2. Run that command on each Pi (installs deps, the `airband-build` login with the `from=`-restricted key, a `/test_data` tmpfs, and uv). On `BCM_VC` targets (`airband-3b`) add `RTL_SUDO=1` after `sudo` (`sudo RTL_SUDO=1 PROXY_PUBKEY=…`, since `sudo` drops variables set before it) to grant the `rtl_airband` sudo rule; without it the account gets no sudo.
3. Re-run `.github/setup_orchestrator_ssh` to verify access.

**`build_docker_containers.yml`** — multi-arch image build (one job per platform via QEMU) with a smoke test per image. Pushes happen only on push/tag/schedule/`workflow_dispatch`, gated by the `Prepare` step's `push` output, because a fork PR's read-only `GITHUB_TOKEN` cannot push to the registry.

## Docker

The container image is built from a multi-stage `Dockerfile` based on `alpine:latest` (musl libc). Alpine is used because it ships a native `linux/arm/v6` image, so a single base covers every Raspberry Pi model (Pi 1/Zero armv6 through Pi 5 arm64) — Debian dropped the armel/`arm/v5` variant that the arm/v6 build previously relied on via fallback.

- Build/runtime dependencies come from `apk`. Note the Alpine-specific package names: `libconfig++` (Alpine splits the C++ binding into its own package), `libstdc++` (not in the base image), `fftw-single-libs`, `libpulse`, and `pkgconf` (provides `pkg-config`).
- The rtl-sdr-blog fork (for RTL-SDR Blog V4 support) and the SoapyAirspy module are compiled from source with CMake and installed to `/usr` — musl only searches `/lib` and `/usr/lib`, not `/usr/local/lib`. `ENV CMAKE_POLICY_VERSION_MINIMUM=3.5` lets their pre-3.5 `cmake_minimum_required` configure under Alpine's CMake 4.
- Both stages run `unittests`, so a broken build fails `docker build`. `scripts/find_version` is POSIX `sh` (Alpine has no bash).

```bash
# Build the image for the host architecture
docker build -t rtlsdr-airband .

# Build a specific architecture (requires QEMU/binfmt for cross-arch)
docker buildx build --platform linux/arm/v6 --load -t rtlsdr-airband:armv6 .
```
