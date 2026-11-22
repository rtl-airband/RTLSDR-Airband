# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## This Is a Fork

This repo is a personal fork of [`rtl-airband/RTLSDR-Airband`](https://github.com/rtl-airband/RTLSDR-Airband).
Upstream is actively maintained; this fork tracks `main` and carries a small, deliberate delta.

Remotes to expect:

| Remote | Repo | Role |
|---|---|---|
| `origin` | `jschmall/RTLSDR-Airband` (canonical, self-hosted) | This fork; push here |
| `github` | `github.com/jschmall/RTLSDR-Airband` | Read-only push mirror of `origin` (local push URL disabled) |
| `upstream` | `rtl-airband/RTLSDR-Airband` | Source of truth; rebase target |
| `yegors` | `yegors/RTLSDR-Airband` | Reference only — source of the cherry-picked file-output features |

**GitHub is a push mirror, not the working repo.** `origin` is the canonical repo; the GitHub copy is force-pushed from it on every push. Never push, merge a PR, or tag on GitHub: anything done there is overwritten at the next sync. To take a GitHub PR, fetch its branch, merge it into `origin`, and let the mirror carry it back.

**The value of this fork is that it stays close to upstream and remains easy to rebase.**
Keep the local delta small and well understood.

### Local changes carried on top of upstream

Each entry below keeps its original number because other parts of this file, and README.md,
reference items by number (e.g. "fork-delta items 27–41"). Numbers are not resequenced even
though the groupings below are thematic, not chronological — item 1 and item 4 can sit under
the same heading despite being 3 apart. Item numbers use bold text rather than markdown's
auto-numbered list syntax specifically so they can't be silently renumbered by a renderer.

#### File & call-upload outputs (post_write_script, native rdio-scanner uploads)

**1. `post_write_script` + `min_rx_seconds`** — cherry-picked from `yegors` commit `bb36bb0`
("Rework file output options"). Touches `src/config.cpp`, `src/output.cpp`, `src/rtl_airband.h`.
Both options require `split_on_transmission = true`. `post_write_script` is used here to upload
completed transmission files to RDIO via API. Independent of upstream's `split_min_file_time`/
`split_max_file_time`/`split_max_idle_time` (added upstream in `v5.4.0`), which control *when* a
split file closes; `min_rx_seconds` then decides whether the already-closed file is kept. Lowering
`split_max_idle_time` below `min_rx_seconds` will discard transmissions that used to be saved.
Upstream's `split_tx_drop_shorter_than` (#580) is a second, independent drop rule in the same
`close_file()`. A file is deleted if either rule says so. `min_rx_seconds` measures
open-to-last-write and applies to every closed file. #580 measures first-to-last write, never
drops a part of a split transmission or a file closed at shutdown, and is the only rule counted
in `channel_split_tx_dropped_counter`. `post_write_script` and the rdio-scanner upload (item 4)
run only for kept files. Covered end to end by `system_tests/tests/test_min_rx_seconds.py`,
the first system test for either option. It was added when #580 was merged, because the merge
rewrote this path.

**4. Native rdio-scanner call-upload support** (`src/rdio_scanner.cpp`, new) — replaces the
`post_write_script` + external CSV lookup this fork previously used to push completed
transmissions to a [rdio-scanner](https://github.com/chuot/rdio-scanner) instance's
`/api/call-upload` endpoint. Adds a `rdio_scanner: { ... }` nested config group on `file` outputs
(requires `split_on_transmission = true`): `server`, `port`, `use_tls`, `api_key`, `system_id`,
`system_label`, `talkgroup_id`, `talkgroup_label`, `talkgroup_tag`, `talkgroup_group`,
`source_id`, `delete_after_upload`, `timeout_ms`, `max_retries` — system/talkgroup metadata is
declared per-channel in config directly, no CSV to keep in sync. Uploads are queued (bounded,
drop-oldest-and-log on overflow) and sent by a single background worker thread via libcurl, so a
slow/unreachable rdio-scanner instance never blocks the output thread; a completed local MP3 is
never deleted on a failed upload, even with `delete_after_upload = true`. New build dependency
`libcurl4-openssl-dev`, gated behind `-DRDIO_SCANNER=ON` (default ON) / `#ifdef
WITH_RDIO_SCANNER`, mirroring the `PULSEAUDIO` option pattern. Pure field-mapping logic is unit
tested (`test_rdio_scanner.cpp`); the worker thread/queue/libcurl path was validated manually
against a mock HTTP server. `R_SCAN` channels are not supported — the per-channel `talkgroup_id`
model only maps cleanly to `R_MULTICHANNEL` (enforced at parse time by item 18).

**5. `dateTime` instead of `timestamp` in rdio-scanner uploads** (`src/rdio_scanner.cpp`,
`rdio_scanner_build_fields()`) — the initial implementation sent rdio-scanner's `timestamp`
field (ms-epoch), which in production caused nearly every real, distinct transmission on a busy
talkgroup to be rejected as `duplicate call rejected` (roughly one upload succeeding per hour).
The pre-existing external `post_write_script` upload script — which sends `dateTime` (RFC3339)
instead — uploaded the same files successfully with calls only seconds apart, isolating the
field as the cause. Switched to `dateTime`, sent as a plain Unix epoch in **seconds** (`tv_sec`,
UTC — no hardcoded timezone, unlike the script it replaced). This is a hypothesis validated by
the script/native contrast, not a confirmed root cause inside rdio-scanner's own code — if
duplicate rejections recur, check whether this deployment's `/api/call-upload` implementation
has version-specific `timestamp` parsing before assuming a regression here.

**12. rdio-scanner config validation for `timeout_ms`/`max_retries`** (`src/config.cpp`) —
`timeout_ms = 0` passed straight through to libcurl's `CURLOPT_TIMEOUT_MS`, where 0 means "never
time out" (risking an indefinitely stuck worker thread); `max_retries < 0` silently skipped the
retry loop, reporting every job "failed" without a single upload attempt. Both are now rejected
at config parse time, matching this block's existing field validation.

**13. rdio-scanner worker shutdown is interruptible mid-retry** (`src/rdio_scanner.cpp`) — the
retry/backoff loop never checked the shutdown flag, so `rdio_scanner_shutdown()` could block for
the full duration of any in-progress retry — directly delaying this fork's one-at-a-time systemd
restarts. A `CURLOPT_XFERINFOFUNCTION` progress callback now aborts an in-flight
`curl_easy_perform()` once shutdown is requested, and the retry backoff waits on the same
condvar the shutdown path signals instead of sleeping the full interval. A job already in flight
at shutdown always gets its first attempt; only further retries are skipped.

**18. `rdio_scanner` rejected on `R_SCAN` channels at config time** (`src/config.cpp`) — an
`R_SCAN` channel's `talkgroup_id`/`system_id`/labels are fixed at config time, but its actual
frequency changes at runtime as it scans, so every scanned frequency's completed transmission
would silently upload under one static, near-certainly-wrong `talkgroup_id`. Already a
documented limitation (item 4); this makes the parser actually enforce it by threading
`dev->mode` through to `parse_outputs()`.

**25. Mixer file-output NULL-freqlist crash fix** (`src/output.cpp`) — `output_file_ready()`
dereferenced `channel->freqlist[channel->freq_idx]` unconditionally, but a mixer's own
`channel_t` (`mixer_t::channel`) has no `freqlist` at all — a mixed stream has no single source
frequency — leaving it `NULL` and crashing on the mixer's first file rotation. No automated test
caught this until two branches' own new mixer system tests hit it independently (items 26 and
27's test coverage). Fixed by computing the frequency once, guarded by `channel->freqlist !=
NULL`, defaulting to `0` for a mixer's own channel. Upstream #574 (`split_include_transmission_start`)
restructured `output_file_ready()` around a new `build_output_filename()` and again dereferenced
`freqlist` unconditionally, though now only when `include_freq` is set. The guard was re-applied
when that change was merged. Upstream #580 then added `fdata->freq = &channel->freqlist[...]`
(for its drop counter), also unguarded; it is set to `NULL` for a mixer's channel here, and the
counter increment checks it. Watch for this on future upstream changes to the function.

#### UDP stream output

**2. `sendto` byte-length fix** (`src/udp_stream.cpp`) — `sendto()` was being passed a float
*element* count where it needs a *byte* count. Corrected to `len * sizeof(float)`.

**3. UDP unit-mismatch follow-up fix** — the fix above changed `udp_stream_write()`'s internal
`len` convention from "already bytes" to "sample count, converted to bytes internally," but its
three call sites (`init_output()` in `rtl_airband.cpp`, the mono/stereo branches of
`process_outputs()` in `output.cpp`) still passed `WAVE_BATCH * sizeof(float)` — a byte count
under the old convention. Net effect: every UDP packet carried 4x the intended payload, reading
past the end of `channel->waveout`/`waveout_r` into adjacent `channel_t` memory. **This fully
explains the "~4x realtime throughput" and choppy-audio symptoms documented below under
"Resolved Issue" — it was never a pacing problem.** Fixed by making `len` a sample count at all
three call sites; regression-tested in `test_udp_stream.cpp`.
Upstream later fixed the same stereo over-read independently in #578, going the *opposite* way
(`len` is a byte count everywhere). When merging #578 this fork **kept its sample-count
convention**, because items 6/9/11's format-conversion, resampling, and bounds-check code is all
written in samples. It took #578's two independent fixes: `send()` on the already-`connect()`ed
socket in place of `sendto()` (which fails with `EISCONN` on macOS and truncated IPv6 addresses),
which removed `udp_stream_data::dest_sockaddr`, and raising `SO_SNDBUF` to fit one datagram, sized
for the actual wire bytes after `bit_depth`/`sample_rate`. The fork also took the subset of #578's
tests that adds coverage, converted to sample counts. **Expect this to conflict again** on any
future upstream change to `udp_stream.cpp`'s signatures.

**6. Configurable `bit_depth` for `udp_stream` output** (`src/udp_stream.cpp`, `src/config.cpp`,
`src/rtl_airband.h`) — optional `bit_depth` field on a `udp_stream` output: `32` (default,
unchanged float32), `16` (signed 16-bit LE PCM), or `8` (signed 8-bit PCM), to cut bandwidth for
consumers that don't need float precision. Samples are clamped to [-1.0, 1.0] before
scaling/rounding so AGC overshoot can't wrap/UB. Omitting `bit_depth` is fully backward
compatible. `bit_depth = 16` matches trunkrecorder/TwoToneDetect's documented UDP contract
(16-bit int PCM) — but the sample *rate* half of that contract needed item 9 to actually be
configurable. `bit_depth = 8` remains unit-tested only, never validated against a live consumer.

**9. Configurable `sample_rate` for `udp_stream` output** (`src/udp_stream.cpp`,
`src/config.cpp`, `src/rtl_airband.h`) — closes item 6's rate gap: the wire rate was
hard-coupled to the build's internal `WAVE_RATE` (8000 without NFM, 16000 with), so an NFM
build had no way to hand a fixed-rate consumer (e.g. trunkrecorder/TTD's 8000 Hz) the rate it
expects without separately reconfiguring the consumer. Adds an optional `sample_rate` field;
when set and different from `WAVE_RATE`, each channel (mono, or left/right independently before
stereo interleaving — resampling the already-interleaved buffer would blend channels together)
is linear-interpolation-resampled before the existing `bit_depth` conversion. Not
broadcast-quality, but correct and adequate for narrowband voice, and avoids a real DSP
resampling dependency for what's typically one fixed 16000→8000 conversion. Manually validated
end-to-end: a real binary + real UDP listener with `sample_rate` set to half `WAVE_RATE`
produced exactly the expected halved byte count on every received packet.

**11. UDP stream bounds checks are real, not `assert()`** (`src/udp_stream.cpp`) —
`udp_stream_write()`'s length-vs-buffer-size checks were `assert()`, which is stripped by
`-DNDEBUG` in the default Release build — the same bug class behind item 3's historical
4x-oversend incident had **no runtime protection in the binary that actually ships**. Replaced
with real conditionals that log and drop the packet on a mismatch instead of writing past the
buffer.

#### Icecast

**10. Icecast mountpoint buffer-overflow fix** (`src/output.cpp`, `src/helper_functions.{h,cpp}`)
— `shout_setup()` built the mountpoint into a fixed `char mp[100]` stack buffer via `sprintf()`,
with no length validation on the config-supplied value. Replaced with `std::string` via a new
`make_icecast_mountpoint()` helper, removing the fixed-size buffer (and the overflow risk)
entirely. Unit tested with a 500-char mountpoint that would have overflowed the old buffer.

**26. `send_tx_tags`: per-transmission Icecast metadata for non-scanning channels/mixers**
(`src/rtl_airband.h`, `src/config.cpp`, `src/output.cpp`, `src/mixer.cpp`,
`src/helper_functions.{h,cpp}`) — the pre-existing `send_scan_freq_tags` only tags Icecast
metadata when an `R_SCAN` device hops frequency; plain `R_MULTICHANNEL` channels and mixers had
no on-air indicator. Adds an independent `send_tx_tags` boolean: pushes the channel's `label` as
the Icecast "song" tag when a transmission starts (squelch opens) and clears it when it ends.
Rejected at parse time on `R_SCAN` channels (already owned by `send_scan_freq_tags`).
Self-contained inside `process_outputs()` (reads `channel->axcindicate`, an `std::atomic`) via
`icecast_tx_tag_step()`, replicating `shout_metadata_delay`'s buffering-compensation behavior
with a per-output deferred-apply state machine — a further flap while a change is pending
updates the pending value without pushing the deadline back; a revert to the already-applied
value cancels the change outright. For mixers, `mixinput_t` gained `source_device_idx`/
`source_channel_idx` (resolved at config-parse time, stored as indices since `dev->channels` is
realloc'd after parsing — a raw pointer wouldn't survive that) so a mixer's icecast output can
look up whichever source channel is currently talking; `mixer_select_active_tag_input()` breaks
ties by lowest index. `mixinput_t::has_signal` changed `bool` → `std::atomic<bool>` (a third
reader now exists, potentially on a different output thread). System-tested against an extended
`fake_icecast_server.py`, which surfaced two real libshout quirks: it needs `Connection:
Keep-Alive` echoed back to stream at all, and it always opens a second, independent `GET
/admin/metadata?...` connection for metadata regardless of the first request's outcome — the
fixture now accepts multiple concurrent connections instead of one blocking `accept()`.
Item 49 moved the metadata request itself off the output thread.

**49. Icecast metadata updates sent from a background thread** (`src/icecast_metadata.{h,cpp}`,
new; `src/output.cpp`, `src/config.cpp`, `src/rtl_airband.{h,cpp}`, `src/live_reconfig.cpp`).
The mixer output overruns that were still open after item 46 (042 ~73/h, 151 ~103/h, more by day)
came from `send_tx_tags`.
- **Found by measuring prod** (observe-only bpftrace, 2026-11-05). Each metadata update on the
  mixer's output thread did a DNS lookup (musl doesn't cache), a new TCP connection to
  Broadcastify's `/admin/metadata`, and libshout's `select()` loop at 1 ms until the reply came:
  ~350 ms per update. The mixer thread counts an overrun once a batch has waited ~125 ms, so
  every transmission start and end dropped 2–3 batches of the mixed feed. `send_scan_freq_tags`
  had the same blocking call.
- **The fix.** `process_outputs()` now only queues the value (`icecast_metadata_submit()`). One
  worker thread sends it. Each output holds a `std::shared_ptr<IcecastMetaTarget>` (created at
  parse time when either tag option is set) with a copy of the connection settings. The worker
  uses its own never-opened `shout_t` per target, never the output's streaming one, so the two
  threads share no libshout object. libshout 2.4.6's `shout_set_metadata()` builds its own
  connection from the `shout_t`'s settings. Its stray `shout_connection_set_callback(self->connection, ...)`
  returns early on a NULL connection. The copy also means a live channel removal can free
  `icecast_data` while a request is in flight.
- **Coalescing.** At most one pending value per target. A newer value replaces a waiting one
  (counted in `icecast_metadata_superseded_count`), since only the latest tag matters.
- **Shutdown** waits 2 s for an in-flight request, then detaches the worker (logged). A request
  to an unreachable server can block for a TCP connect timeout, and that shouldn't hold up a
  restart. The queue is heap-allocated and never freed, so a detached worker can't touch
  destroyed statics while the process exits.
- **Metrics.** `icecast_tx_tag_update_count` (name kept) now counts `send_scan_freq_tags` updates
  too. New `icecast_metadata_failure_count` and `icecast_metadata_superseded_count`.
- **Side fix.** Item 26 changed `icecast_data` to `new` (it gained a `std::string`), but
  `free_output_data()` (item 41) still released it with `free()`, so every live removal or edit
  of a channel with an Icecast output was a new/free mismatch. It now uses `delete`, as does the
  test fixture that copied the old pattern.
- **Tests.** `src/test_icecast_metadata.cpp` (queue order, coalescing, a value submitted mid-send
  is queued again, shutdown, target lifetime, settings copied). In
  `system_tests/tests/test_icecast_tx_tags.py`, a mixer with `send_tx_tags` runs in real time
  against `fake_icecast_server.py` answering metadata after 1 s. The old code overran the mixer
  21 times; the fix stays at ≤ 2.

#### Observability / metrics

**8. HTTP metrics endpoint** (`src/stats_http.cpp`, new) — `write_stats_file()` already writes
Prometheus-format stats to `stats_filepath` every `stats_update_interval` seconds (default 15s; configurable since upstream #579), but only to a file, requiring a textfile
collector per host across this fork's ~12 concurrent instances. New `stats_http_address`/
`stats_http_port` config options (must be set together, and require `stats_filepath`) start a
background thread that serves the *current* content of `stats_filepath` (read fresh off disk
per request) over plain HTTP to any request on that address:port, regardless of method/path.
Deliberately minimal — no request parsing, one connection at a time, shutdown polls a flag every
500ms rather than force-closing the listening socket from another thread (racy on Linux). No new
build dependency — plain POSIX sockets, unconditionally compiled in.

**19. Structured JSON logging (`-j`)** (`src/logging.{h,cpp}`) — opt-in flag that switches
`log()` from plain-text output to one JSON object per line (`{timestamp, level, pid, message}`),
to whichever destination was already configured. Makes correlating log lines across ~12
concurrent instances in a central pipeline (Loki/ELK/journald) a field lookup instead of a
regex. Plain text remains the default. `build_json_log_line()` is a pure function, unit tested
directly, including escaping of quotes/backslashes/newlines/control characters.

**22. `buffer_underrun_count` and `process_cpu_seconds_total` metrics** — added while
investigating whether output-overrun events are compute-bound or USB/host-bound;
`buffer_overflow_count` alone only shows *that* the demod thread fell behind, not why.
`buffer_underrun_count{device}` (`rtl_airband.cpp`, the "not enough data yet" branch; counter on
`input_t`, `input-common.h`) counts how often the demod thread found insufficient samples and
had to wait — increments frequently under healthy load, so read it as a trend: a device whose
underrun count goes flat while its overflow count climbs is the signature of a CPU-saturated
demod thread rather than USB starvation. `process_cpu_seconds_total` (`output.cpp`, via
`getrusage(RUSAGE_SELF, ...)`) is standard Prometheus cumulative process CPU time, so `rate()`
over it gives host CPU utilization correlatable against the buffer/overrun counters, without
cross-referencing external host monitoring.

**23. Failure/health counters for every output type** — extends item 22's "log it to the stats
file, not just syslog" treatment to the output side. New per-output counters, all
zero-initialized for free, exposed via `write_stats_file()` with `device`/`channel`/`output` or
`mixer`/`output` labels: `icecast_disconnect_count`/`icecast_backlog_exceeded_count` (icecast is
also usable on mixer outputs); `lame_encode_failure_count` (on `output_t` itself, shared by the
icecast/file encode paths — deliberately excludes the one-shot `LameTone` silence-marker encode
at file-open time, which uses its own throwaway `lame_t`); `file_write_failure_count`;
`udp_stream_dropped_packet_count` (at all three bounds-check drop sites from item 11 — this also
closed a real test gap, an oversized-length S8 case item 11 had left uncovered);
`pulse_underflow_count`/`pulse_overflow_count`/`pulse_disconnect_count` (guarded by
`WITH_PULSEAUDIO`); `rdio_scanner_queue_drop_count`/`rdio_scanner_upload_failure_count`
(process-wide atomics, since the upload queue/worker is already shared across every
`rdio_scanner`-configured output; guarded by `WITH_RDIO_SCANNER`).

**24. `-DRDIO_SCANNER=OFF` build fix** (`src/config.cpp`) — item 18's `dev_mode` parameter on
`parse_outputs()` is only read inside `#ifdef WITH_RDIO_SCANNER`, so it's unused (and a
`-Werror=unused-parameter` build failure) whenever `RDIO_SCANNER` is off. Since `RDIO_SCANNER`
defaults ON and CI doesn't build the OFF configuration, this went unnoticed until item 23. Fixed
with `(void)dev_mode;` guarded by `#ifndef WITH_RDIO_SCANNER`. **Worth remembering**:
`-DRDIO_SCANNER=OFF` is not exercised by default CI — build it manually
(`-DRDIO_SCANNER=OFF -DBUILD_UNITTESTS=TRUE`) after any change touching `parse_outputs()`'s
signature or the rdio_scanner code paths.

#### Correctness / reliability fixes

**14. `channel_t::axcindicate`/`freq_idx`/`state` made atomic** (`src/rtl_airband.h`) — each is
written by one thread and read by others (demod/controller/mixer/output) with no lock or atomic
— a data race under the C++ memory model, even though it never caused a torn read on this
project's target platforms. Switched to `std::atomic<T>`; the implicit `T` conversion meant
every existing call site needed zero changes.

**15. `init_output()` checks `airlame_init()`/`malloc()` failures** (`src/rtl_airband.cpp`) —
previously returned `true` unconditionally even when LAME init or the `lamebuf` allocation
failed, silently leaving `output->lame`/`lamebuf` null and crashing the *next* time the output
thread tried to encode, instead of failing cleanly at startup like both callers in `main()`
already expect.

**16. RTL-SDR tuning failures are now fatal** (`src/input-rtlsdr.cpp`) — `rtlsdr_init()` logged
failures from `rtlsdr_set_sample_rate()`/`rtlsdr_set_center_freq()`/
`rtlsdr_set_freq_correction()` but always returned success, so a device could report itself
initialized while silently sampling the wrong rate or frequency. Now returns `-1` on these three
failures so `input_init()`'s existing fail-fast path fires, matching how the runtime retune path
already handles the same failure.

**17. System tests for Icecast and rdio-scanner outputs**
(`system_tests/helpers/fake_icecast_server.py`, `fake_rdio_scanner_server.py`,
`tests/test_icecast_output.py`, `tests/test_rdio_scanner_output.py`) — the only two output types
this fork's production deployment actually uses had no end-to-end coverage before this. The
Icecast fixture surfaced the two libshout quirks documented under item 26.

**20. Unit tests for `util.cpp`/`mixer.cpp` pure functions** (`src/test_util.cpp`,
`src/test_mixer.cpp`) — `delta_sec()`, `atofs()`, the `sincosf` LUT, `dBFS_to_level()`/
`level_to_dBFS()`, and `mixer.cpp`'s `mix_waveforms()` had no coverage. Most of the rest of
these files (and `output.cpp`/`rtl_airband.cpp`/`config.cpp`) is tightly coupled to threading,
real sockets/files, or `libconfig::Setting` and isn't a good unit-test target without a larger
refactor — this extracts what was already genuinely pure. (Later dynamic_reload items reference
this as the reason new threading-coupled code is validated manually rather than unit tested.)

**21. Four small magic-number/fragility cleanups** — `rdio_scanner`'s hardcoded
`MAX_QUEUE_DEPTH = 64` is now a validated `rdio_scanner_queue_depth` config option; two
`memcpy()` calls in `output.cpp` used a hardcoded float size of `4` instead of `sizeof(float)`;
`LAMEBUF_SIZE` (`rtl_airband.h`) was a flat `22000` marked `// todo: calculate` — tracing actual
usage found the real worst case is `LameTone`'s 1-second silence marker, and per LAME's own
`1.25*num_samples + 7200` formula the old value was actually **insufficient** on NFM builds
(`1.25*16000+7200 = 27200 > 22000`) — now computed from `WAVE_RATE`; and the `getopt()`
optstring in `rtl_airband.cpp` was a manually-sized `char[16]` grown with `strcat()` per
conditional build flag, replaced with `std::string` to remove the silent-overflow risk.

#### SIGHUP config reload

**7. SIGHUP reload via re-exec** (`src/rtl_airband.cpp`) — SIGHUP previously mapped to the same
`do_exit` path as SIGINT/TERM/QUIT, meaning any config change across ~12 concurrent instances
required a manual, one-at-a-time restart. SIGHUP now runs the same clean-shutdown sequence, then
calls `execvp(argv[0], argv)` once shutdown completes, replacing the process image with a fresh
instance under the same PID that re-reads the same `-c` config path. This is a re-exec, not an
in-process hot reload — outputs still momentarily drop, same as a real restart — and a
non-foreground (self-daemonizing, no `-F`) invocation would double-fork again on every reload,
breaking PID-file tracking; this fork's deployment always uses `-F` (systemd manages the
process), so that path isn't hit. Manually validated: sent `SIGHUP`, confirmed the same PID
persisted through a full shutdown+restart log sequence; confirmed `SIGTERM` still exits normally
without reloading.

#### Dynamic reload / live reconfiguration (`dynamic_reload`, control socket)

Items 27–41 are one connected feature, built and hardened across a single extended effort: a
same-host-only Unix domain control socket that lets most day-to-day config changes reach an
already-running instance without a restart. README.md's "Live Reconfiguration" section is the
user-facing summary; these items are the engineering detail behind it.

**27. Foundation: control socket + request/apply/confirm pattern**
(`src/control_socket.{cpp,h}`, `src/live_reconfig.{cpp,h}`, new) — before this, the only reload
mechanism was SIGHUP's full re-exec (item 7): any config change dropped the whole feed for a
restart cycle. Adds a same-host-only (`0600`, `SO_PEERCRED`-checked against the daemon's own
`getuid()`) control socket, gated behind a new `control_socket_path` config option, accepting
one JSON object per line and returning one JSON response line: `retune`, `set_gain`,
`set_bandwidth`, `channel_enable`/`channel_disable`, `mixer_enable`/`mixer_disable`,
`reload_diff`.
- A root-run daemon (unset `User=`/`Group=` in its systemd unit) can only be controlled by
  root-run tooling, since `SO_PEERCRED` requires an exact UID match — document this in any unit
  file that sets `control_socket_path`.
- New `enabled = false/true` channel/mixer config keyword, distinct from the parse-time-permanent
  `disable` (which skips the array slot entirely). `enabled` still allocates everything but
  starts the channel/mixer skipped by the hot loops, so the control socket can toggle it live
  with no resize.
- Live centerfreq retune is `R_MULTICHANNEL`-only (`R_SCAN` keeps its fixed-offset
  controller-thread scheme). The control socket only ever *posts a request*
  (`device_t::pending_centerfreq_request`, sentinel `-1`); the demod thread that already
  exclusively owns `bins`/`base_bins`/`dm_dphi` polls and applies it in-thread, keeping the
  recompute single-writer — the same invariant AFC's own adjustments already relied on.
- Channel/mixer enable/disable uses the same request/apply split, for a documented reason: an
  earlier version called `mixer_disable()` directly from the control-socket thread and a system
  test reproducibly segfaulted it — a real data race against the owning output thread's
  concurrent `process_outputs()`/`close_file()`. Known gap, not fixed: the pre-existing "last
  input died" auto-cascade (`mixer_disable_input()` → `mixer_disable()`) can in principle still
  run from a *different* output thread than the one owning the target mixer when
  `multiple_output_threads = true` — not hit in this fork's default single-output-thread
  deployment.
- `set_gain`/`set_bandwidth` are new nullable `input_t` vtable hooks (mirroring
  `set_centerfreq`), returning `ENOTSUP` when a driver leaves the pointer null.
- `reload_diff` re-reads the same `-c` config file into a read-only snapshot and applies
  whatever's in scope through the same primitives a single command would use. Device/channel/
  mixer count changes, driver type, and mode changes are detected and reported under
  `skipped_requires_restart`, never attempted.
- **Documented, still-open follow-up**: a live-retune system test specifically confirming
  retuning one channel doesn't corrupt a sibling channel's bins on the same device (see
  `test_control_socket.py`'s module docstring).

**Known limitation across all of items 27-41**: `reload_diff` re-reads only the `devices` and
`mixers` sections of the config file (`src/live_reconfig.cpp`, `compute_and_apply_diff()`), never
the top-level globals. A change to a *global* setting — `shout_metadata_delay`, `stats_filepath`,
`fft_size`, or upstream's `split_min_file_time`/`split_max_file_time`/`split_max_idle_time` —
is silently ignored with no `skipped_requires_restart` entry. The same goes for the newer upstream
globals `stats_update_interval` (#579), `split_include_transmission_start` (#574) and
`split_tx_drop_shorter_than` (#580). Any of them needs a restart (or SIGHUP,
item 7). *Per-output* `split_*` values are covered for free: item 31's `config_signature` is a
full `serialize_setting()` of the raw channel block, so editing one rebuilds that channel like any
other output change.

**28. Dynamic channel add via `reload_diff`** (`src/config.cpp`, `src/live_reconfig.{cpp,h}`,
`src/rtl_airband.h`) — the config file stays the single source of truth: append a channel to a
device's `channels` list and call `reload_diff` (no new wire command); a pure tail append is
detected and applied live. Any other `channel_count` change still required a restart until item
31 generalized this.
- New `reserve_channels` device-level config int (default 0) — `dev->channels`/`bins`/
  `base_bins` are allocated with this much extra headroom at startup and never resized again;
  `dev->channel_count` (now `std::atomic<int>`) can grow up to `channel_capacity` by writing
  into an already-allocated slot and publishing the new count. Real runtime array growth
  (`realloc` while threads are mid-iteration) was rejected as an unjustified hazard — an operator
  sets `reserve_channels` once (one restart) before dynamic add works on a device. Rejected at
  parse time on `R_SCAN` (always exactly one channel).
- `parse_channel()` — the startup per-channel body, extracted so the startup and live-append
  paths share one implementation (including `parse_outputs()`, so an appended channel supports
  the exact same `outputs` block a config-file channel would).
- A batch of appended channels is all-or-nothing: `channel_count` only publishes after every new
  channel parses and connects; a channel that individually succeeded earlier in a failed batch is
  leaked, not unwound (acceptable for a rare, operator-triggered failure path).
- `config.cpp`'s `error()` calls `_Exit(1)` unconditionally — correct for startup but fatal to a
  *running* process if reused verbatim. A `thread_local` gate (`config_error_is_recoverable`)
  makes `error()` throw `ConfigApplyError` instead when set; the live-append path sets it and
  catches broadly (`std::exception`, not just `ConfigApplyError` — some required-but-missing
  config keys throw a raw `libconfig::SettingNotFoundException` instead of going through
  `error()`).

**29. Mixer-input live-append data race fix (`reserve_inputs`)** (`src/mixer.cpp`,
`src/rtl_airband.{cpp,h}`, `src/config.cpp`) — a dynamically appended channel with a `type =
"mixer"` output reached `mixer_connect_input()`, which grew `mixer_t::inputs`/`inputs_todo`/
`input_mask` via an unconditional `XREALLOC` — safe only "at startup," a comment that was no
longer true once threads were already running and reading that pointer with no synchronization.
A real use-after-free, not theoretical.
- Mirrors item 28's fix: `reserve_inputs` mixer-level config int sizes headroom, finalized once
  by `mixer_finalize_capacity()` (called from `main()` right after `parse_devices()`, the same
  single-threaded window). `mixer_capacity_finalized` is a plain global, not a `mixer.cpp`
  file-static — the unit test binary links every `test_*.cpp` into one process, and a
  file-static would leak `true` across unrelated tests. After finalization, exceeding
  `input_capacity` is rejected with a clear error instead of reallocating, surfacing via the same
  `config_error_is_recoverable` path as an ordinary `skipped_requires_restart`.
- Two adjacent bugs found and fixed alongside: a mixer declared with zero startup-connected
  inputs (the intended shape for a `reserve_inputs`-only mixer) never had its own output
  initialized (`main()`'s startup loop skipped `init_output()` for any mixer still `enabled ==
  false`) — fixed by always initializing a mixer's own outputs at startup, matching device
  channels. And `control_socket.cpp`'s `json_escape()` didn't escape control characters, only
  `"`/`\` — since nearly every `error_response()` message ends in `"\n"` and the wire protocol is
  one JSON object per line, an embedded newline silently truncated the response before the
  closing brace on *any* config-parse error during a live append, not just this one.

**30. Live channel removal via `reload_diff`** (`src/live_reconfig.{cpp,h}`, `src/config.cpp`,
`src/output.cpp`, `src/rtl_airband.h`) — the inverse of item 28: deleting a channel from config
and calling `reload_diff` tears it down live. `dev->channels`/`bins`/`base_bins` never shrink or
reallocate (item 28's `reserve_channels`/`channel_capacity` already solved slot safety), but
removal is destructive, unlike append, which raises two problems append never had:
- **Confirmation ordering**: `channel_t::pending_remove_request` is a separate atomic field from
  `pending_enable_request` because `output_thread()` only resets it to `-1` *after*
  `channel_teardown_for_removal()` fully completes, not at the moment the request is observed —
  item 27's enable/disable pattern lets a waiter see "consumed" before the apply function
  finishes, which is wrong here since the caller decrements `dev->channel_count` on confirmation.
- **Deliberately narrow teardown**: `channel_teardown_for_removal()` sets `enabled = false`
  first, calls `disable_channel_outputs()`, then frees each output's LAME encoder (`lame_close()`
  + `free(lamebuf)`) — the one resource that scales meaningfully with channel count.
  `channel->outputs`/`freqlist` and each output's `data` struct are deliberately left allocated
  (leaked) at this point, since freeing them would touch memory `output_check_thread()` reads
  with no synchronization beyond the `enabled` check — safely reclaimed later by item 41.
- Not all-or-nothing unlike append: `dev->channel_count` decrements immediately per confirmed
  channel, so a mid-batch timeout leaves a valid, still-tail-consistent smaller count.
- Constructing a real `freq_t` in a unit test (needed to give a channel a real freq) runs
  `Squelch`'s constructor, which unconditionally calls `debug_print()` — fine in production, but
  segfaults in the unit test binary under `-DDEBUG`, since `debugf` is only ever `fopen()`'d by
  the real startup path. Worked around with `calloc`-constructed test fixtures instead of real
  C++ construction (item 36 later needed the same workaround) — not fixed at the source.
- Original position/frequency-based matching, and its "non-tail deletion is rejected" behavior,
  were **superseded by item 31**'s signature-based common-prefix diff, below.

**31. Live channel edit via `reload_diff` (tail-replace generalization)** (`src/config.cpp`,
`src/live_reconfig.{cpp,h}`, `src/rtl_airband.h`) — items 28/30 could add or remove a channel
live, but not edit one (`freq`/`modulation`/`bandwidth`/filters/`outputs` changes were silently
ignored). Turned out to need no new mechanism: running item 30's removal and item 28's append
back to back on the same index *is* an edit.
- `channel_t` gains `config_signature`, a canonical serialization of the channel's entire raw
  config block (`build_channel_identity_signature()` → `serialize_setting()`, which recursively
  walks any `libconfig::Setting` — scalar, group, list, array — so nested structure like a mixer
  connection's `balance` is captured automatically, with no per-field enumeration to maintain as
  new options are added). Set unconditionally at the end of `parse_channel()`. `enabled` is
  deliberately excluded (it already has a cheap live-apply path via item 27; folding it in would
  make toggling it alone trigger a full rebuild).
- `compute_and_apply_diff()` now finds the longest common signature-matching prefix, removes
  whatever's live past it (`try_remove_channels()`, now taking an explicit `target_count`), and
  appends whatever the snapshot has past it. A pure count increase/decrease is just the case
  where one side of that gap is empty.
- **Behavior change from item 30**: a non-tail deletion (removing a channel from the middle) now
  succeeds instead of being flatly rejected — the signature diff finds *where* file and live
  state first disagree by content, not just by count, so any sibling channel after the divergence
  point gets rebuilt too (a brief interruption, not data loss). **Known limitation**: the
  comparison is a positional common *prefix*, not a set match — reordering two unchanged channels
  in the config causes both to be rebuilt.
- A caveat originally flagged here — editing a mixer-connected channel permanently burned a
  `reserve_inputs` slot on every edit, since `mixer_disable_input()` never released a slot back
  to capacity — is **fixed by item 40**, no longer a live concern.

**32. Live retune/gain/bandwidth failures no longer take down the whole process**
(`src/input-common.{cpp,h}`, `src/live_reconfig.{cpp,h}`, `src/control_socket.cpp`,
`src/rtl_airband.{cpp,h}`, `src/config.cpp`, `src/output.cpp`) — `input_set_centerfreq()`/
`set_gain()`/`set_bandwidth()` unconditionally set `input->state = INPUT_FAILED` on any nonzero
driver return, including a single transient hardware error (e.g. an i2c write failure) on an
otherwise-healthy device. `demodulate()`'s main loop treats `INPUT_FAILED` as fatal for that
device, exiting the whole process if it was the last one running — found by hitting exactly this
against real hardware. `R_SCAN`'s `controller_thread()` had the same problem, permanently
abandoning a scan on the same failure.
- Fixed by no longer setting `INPUT_FAILED` from these three functions — that state now means
  only what it should: the RX stream itself died. `controller_thread()` now logs and retries the
  hop next cycle instead of abandoning the device.
- Second bug found in the same area: `handle_retune()`'s poll loop consumed
  `pending_centerfreq_request` (via `exchange()`) *before* `device_apply_retune()` (and the
  hardware call inside it) actually ran — a caller could observe "consumed" before the result
  existed. Fixed the same way item 30 established for `pending_remove_request`: a new
  `centerfreq_apply_failed` field is set from the real result, and the pending field only resets
  after that.
- New `input_t::centerfreq_retune_failure_count` metric, exposed alongside `buffer_underrun_count`
  (item 22).

**33. Live retune bins/dm_dphi correctness + `reload_diff` failure reporting fix**
(`src/live_reconfig.{cpp,h}`, `src/control_socket.cpp`) — found investigating a production
report of "the same failure" persisting after item 32. Extensive fault-injection and
real-hardware stress testing never reproduced a crash or race in the request/apply/confirm
machinery (items 27/29/32) — the actual bugs were correctness/reporting issues:
- `device_apply_retune()` recomputed every channel's `bins`/`base_bins`/`dm_dphi` for the *new*
  centerfreq unconditionally, before attempting the hardware retune. On a failed
  `input_set_centerfreq()` call the radio stays on its old centerfreq, but the demod math had
  already moved on — every channel on that device demodulated the wrong bin offset until the
  next successful retune. Fixed by reordering: attempt the hardware call first, only recompute
  on success.
- `compute_and_apply_diff()`'s centerfreq branch reported "applied" based solely on *posting* the
  request, never checking whether the demod thread's hardware call actually succeeded — unlike
  the standalone `retune` command, which already polled for real confirmation. Fixed by
  extracting that poll-and-check logic into a shared `device_confirm_retune()`, used by both; a
  real hardware failure now surfaces under `skipped_requires_restart` with wording clarifying no
  restart is actually needed (retry `reload_diff`) — see item 37.
- **Worth remembering as a negative result**: this design held up under 9,500+ forced retune
  failures and 700+ real add/remove/retune cycles. If a genuine *crash* (not a wrong-frequency or
  under-reported-failure symptom) shows up again in this area, check the OS/systemd level first
  (OOM killer, watchdog, resource limits) before re-auditing this machinery.
- Also noted, not fixed: a pre-existing UBSan finding at `src/ctcss.h:68` ("load of value 240,
  which is not a valid value for type 'bool'" — an uninitialized-read UB, unrelated to this fix).

**34. Live RTL-SDR tuner bandwidth control** (`src/input-rtlsdr.{cpp,h}`, `src/input-common.h`,
`src/live_reconfig.{cpp,h}`) — `input->set_bandwidth` was hardcoded NULL for rtlsdr with a
comment claiming no tuner-bandwidth API exists — verified false against the actual installed
`librtlsdr2 2.0.1`: `rtlsdr_set_tuner_bandwidth()` exists and works. New optional `bandwidth`
device-level config key (integer Hz, 0 = automatic), applied at `rtlsdr_init()`; omitting it is
fully backward compatible. `rtlsdr_set_bandwidth()` implements the pre-existing nullable
`set_bandwidth` hook, so the pre-existing standalone control-socket command now works for rtlsdr
with zero changes there. `DeviceConfigSnapshot` gains `has_bandwidth`/`bandwidth`, reapplied
unconditionally on every `reload_diff` (no live-readable "current bandwidth" to diff against —
harmless/idempotent, same tradeoff as gain). Deliberately rtlsdr-only (this fork's deployment is
exclusively RTL-SDR); soapysdr already had the live hook. Verified end-to-end against real
hardware for all three paths (startup config, standalone command, `reload_diff`).

**35. Live RTL-SDR frequency correction (PPM) control** (`src/input-rtlsdr.cpp`,
`src/input-common.{cpp,h}`, `src/control_socket.cpp`, `src/live_reconfig.{cpp,h}`) — same
three-layer template as item 34 (driver hook, standalone command, `reload_diff` wiring), applied
to `correction`, which already had startup config support. `rtlsdr_set_correction()` treats
`rtlsdr_set_freq_correction()`'s `-2` return ("already at this value") as success, matching the
startup path's existing handling. Verified end-to-end against real hardware.

**36. Live device `sample_rate` change** (`src/live_reconfig.{cpp,h}`, `src/rtl_airband.{cpp,h}`,
`src/config.cpp`, `src/control_socket.cpp`, `src/input-rtlsdr.cpp`) — the most expensive item on
this to-do list: genuinely requires stopping/restarting the device's RX thread, resizing
`input_t::buffer`, and recomputing bins/`dm_dphi`, not just one API call.
- The FFTW plan/`fft_size` is a process-wide global with zero dependency on any device's
  `sample_rate` — changing one device's rate can't touch another device's FFT processing.
- `device_apply_sample_rate()` runs synchronously within this device's own turn in the demod
  thread's round-robin (the same single-writer-thread invariant `device_apply_retune()` relies
  on): allocates the new buffer *first* (old buffer untouched until the new rate is confirmed
  working — cheap rollback), stops the RX thread via the driver's `stop` hook directly and joins
  it (aborting without joining if `stop()` itself fails, since a join could hang forever),
  reopens via a new `device_reopen_recoverable()` helper (wraps the driver's `init()` hook with
  the same `config_error_is_recoverable` mechanism item 28 established), and on success frees the
  old buffer, restarts the RX thread, and recomputes bins/`dm_dphi`. On failure, attempts
  rollback to the old rate before giving up (restores service for a transient hiccup); only if
  rollback *also* fails does it mark `INPUT_FAILED`.
- `rtlsdr_init()` was refactored (extracting `rtlsdr_configure_opened_device()`) after
  real-hardware testing found a failure partway through the original straight-line function left
  `dev_data->dev` open, making the rollback's own reopen collide with the still-claimed USB
  interface.
- Also required explicit `pending_sample_rate_request = -1` initialization in `parse_devices()`
  — missing this was a second real bug caught only by real-hardware testing: a freshly
  zero-initialized `device_t` defaults the field to `0`, which the demod thread's `if
  (pending_sample_rate >= 0)` check read as an immediate request for `sample_rate = 0` — a
  guaranteed SIGFPE at startup that no unit test caught because every test fixture set the field
  explicitly.
- **Known limitation**: in the non-default `multiple_demod_threads=false` topology with more than
  one device, this device's turn in the round-robin blocks synchronously for the whole
  stop/reopen/restart sequence (potentially hundreds of ms), pausing every other device that
  thread services. Accepted for this fork's one-device-per-instance deployment; worth
  reconsidering before use in a genuinely multi-device-per-thread deployment.
- Verified end-to-end against real hardware, including both bugs above and a fault-injected
  reopen-failure rollback.

**37. `reload_diff` wording consistency** (`src/live_reconfig.cpp`) — found while checking a
consumer (rtl-airband-panel) that classifies `skipped_requires_restart` entries by matching a
"no restart needed" substring, added for items 33/36's centerfreq/sample_rate wording.
`gain`/`bandwidth`/`correction` failures (items 27/34/35) are the same kind of transient,
retryable, no-restart-needed failure but lacked that wording — a caller had no way to distinguish
them from a genuinely restart-required entry other than by field name. Appended the same "- no
restart needed, retry reload_diff" suffix to all three.

**38. Control socket: per-connection recv timeout + buffer cap** (`src/control_socket.{cpp,h}`)
— `handle_connection()` had no timeout on its `recv()` loop and no cap on how much it would
buffer waiting for a newline. Since `control_main()`'s accept loop is single-threaded, a single
stalled or misbehaving client blocked *every* other control-socket operation indefinitely,
including delaying clean shutdown (only re-checked between `recv()` calls). Now takes
`timeout_sec`/`max_buffered_bytes` parameters (defaulted to 10s/16KiB for production), sets
`SO_RCVTIMEO`, and disconnects a client that exceeds the buffer cap without a newline. Does
**not** make the socket handle multiple clients concurrently — still one connection at a time,
unchanged. Verified against real hardware: a stuck first client now delays a second client's
request only up to the configured timeout, not forever.

**39. Channel removal: tombstone to prevent a real crash on a stale index**
(`src/rtl_airband.h`, `src/config.cpp`, `src/live_reconfig.cpp`, `src/control_socket.cpp`) — the
highest-severity finding from a pre-merge review pass across the whole branch. If
`try_remove_channels()`'s confirmation from the output thread times out, the removal request is
still live and gets processed asynchronously with no further correlation back to the caller —
`dev->channel_count` is never decremented for that index. In that window, the channel looks
"still live" to `get_device_and_channel()` (the bounds check backing `channel_enable`/
`channel_disable`), but the removal is in flight or already complete (LAME encoder already
freed). A `channel_enable` landing on that index reopens connections but never reallocates LAME
(only `init_output()` does that) — the next encode call crashes with a null encoder, **taking
down the entire `output_thread()`, every feed on that instance, not just one channel**.
- A second, quieter bug from the same root cause: `channel_teardown_for_removal()` never updated
  `config_signature`, so reverting a channel deletion in the config file would leave item 31's
  signature match seeing "unchanged" and never re-diff that index — the channel stays
  permanently, silently dead with no restart-free way to revive it.
- Fixed with a permanent tombstone, `channel_t::removed`, set once a removal completes and reset
  only by `parse_channel()` populating a fresh channel into a reused slot.
  `get_device_and_channel()` now rejects any index where a removal is in flight
  (`pending_remove_request != -1`) or already complete (`removed == true`); item 31's
  common-prefix walk now treats a tombstoned channel as always diverged, regardless of signature
  match, so a reverted delete is correctly revived instead of silently skipped forever.

**40. Mixer: reclaim an input slot on permanent channel removal, not just temporary disable**
(`src/rtl_airband.h`, `src/mixer.cpp`, `src/output.cpp`, `src/live_reconfig.cpp`) — closes item
31's flagged-but-unfixed caveat: `mixer_disable_input()` only ever masked a slot off, never
releasing it back to `input_capacity` — correct for an *ordinary* temporary disable (the channel
is expected to reconnect to the same index), but item 31's live channel *edit* tears down and
re-appends, so every edit of a mixer-connected channel permanently burned a new `reserve_inputs`
slot.
- `disable_channel_outputs()` and `mixer_disable_input()` both gained a `permanent` bool (default
  `false`, every pre-existing call site unchanged); `channel_teardown_for_removal()` is the only
  caller that passes `true`. A new parallel array, `mixer_t::input_removed`, is tombstoned on
  permanent disable and checked first in `mixer_connect_input()` — a tombstoned slot is reused
  before any capacity check, so reuse never touches `reserve_inputs`. The slot's mutex is
  deliberately never re-initialized or destroyed on reuse — `mixer_thread()` locks every slot's
  mutex unconditionally every pass regardless of `input_mask`, so it must stay valid for the
  mixer's entire lifetime.
- **Merge-time fix**: item 26's `mixinput_t::source_device_idx`/`source_channel_idx` (added on a
  separate line of history) weren't reset to `-1` on the reuse path the way the fresh-growth path
  already did, briefly exposing the previous occupant's stale-but-not-unsafe source indices to
  `mixer_tx_tag()` for one tick. Fixed to match the fresh-growth path's discipline.

**41. Channel teardown: safely reclaim leaked outputs/freqlist memory**
(`src/live_reconfig.{cpp,h}`, `src/output.cpp`) — the last of the pre-merge-review findings.
Item 30 deliberately left `channel->outputs`/`freqlist`/`config_signature` and each output's
`data` struct allocated forever after a removal, since immediately freeing them risked a
use-after-free against readers gated only on `channel->enabled` (no real synchronization).
Investigating surfaced a **third, unguarded reader**: `write_stats_file()`'s per-channel metrics
loop iterates `channel->freqlist` with no `enabled` check at all, every `stats_update_interval` seconds.
- Rejected a generation/epoch-counter approach (correct in principle, but would touch multiple
  hot-path loops with different cadences for a fix whose whole point is closing a memory-safety
  gap, not adding a performance-critical primitive).
- Instead: a deferred, time-based reclamation queue, private to `live_reconfig.cpp`, drained once
  per `output_thread()` pass (the same thread that already exclusively owns this memory).
  Teardown captures the old pointers into a timestamped `PendingChannelFree` entry instead of
  freeing them immediately — a slot reused by a later `parse_channel()` call just overwrites the
  pointers with fresh ones, the old values already safely captured. Anything past
  `reclaim_grace_period_sec` (30s in production) gets actually freed. The safety margin is set
  by how long one `write_stats_file()` call spends reading this state (well under a second), not
  by the stats cycle, so a `stats_update_interval` (upstream #579) above 15s doesn't weaken it.
  Since item 51 that reading happens in memory, before the file write that can block. `lame`/`lamebuf` are unaffected — those still free immediately,
  since neither reader ever touches them.
- `free_output_data()` mirrors, in reverse, every heap allocation `parse_outputs()` makes per
  output type — `udp_stream_data`/`pulse_data` are `XCALLOC`'d with `strdup()`'d
  string fields (`free()`); `icecast_data` is `new`'d with `strdup()`'d strings (`delete`, since
  item 49 — item 26 had changed it to `new` and this was still `free()`); `file_data` (and nested `rdio_scanner_data`) are `new`'d with
  `std::string` members (`delete`, not `free()` — a mismatch here would itself be the
  heap-corruption bug this fix exists to prevent); `mixer_data` owns no nested pointers.
- Confirmed clean under ASan/UBSan **with LeakSanitizer specifically enabled** for these two
  tests (the full suite otherwise runs with `detect_leaks=0` due to unrelated pre-existing
  fixture leaks) — positively confirming the fix, not just "doesn't crash."

#### Cross-instance remote mixer input

**42. Cross-instance remote mixer input** (`src/mixer_remote_wire.{h,cpp}`,
`src/mixer_remote.{h,cpp}`, new; `src/rtl_airband.h`, `src/config.cpp`, `src/output.cpp`,
`src/rtl_airband.cpp`, `src/mixer.cpp`) — a mixer in one instance can absorb a live audio input
streamed from a channel in a *different* instance's process, same host only. Extends each
instance's own mixer (any mixer can absorb a remote input alongside local channels) rather than
a standalone relay-hub process, since this fork's ~12 instances already share one host.
- **Wire protocol** (`mixer_remote_wire.h`) — a fixed 32-byte header (magic/version/format/rate/
  stream_id/seq/sample_count/flags) followed by raw mono float32 PCM, one datagram per
  `WAVE_BATCH` tick. Only `MIXER_REMOTE_FORMAT_FLOAT32` is implemented; 16/8-bit variants are
  deferred (same-host loopback has no real bandwidth constraint).
- **Transport**: `AF_UNIX SOCK_DGRAM`, separate from the `dynamic_reload` control socket
  (different framing/latency needs). The sender deliberately never `connect()`s — unlike UDP, an
  `AF_UNIX SOCK_DGRAM` `connect()` requires the target socket file to already exist, and the
  receiving instance may start later; every packet uses `sendto()` with the destination supplied
  explicitly, and a missing/refused receiver is counted (`dropped_packet_count`) and non-fatal.
- **Trust model**: `SCM_CREDENTIALS`/`SO_PASSCRED`, the connectionless analog of the control
  socket's `SO_PEERCRED` check — a datagram is rejected unless its sender's UID matches the
  receiver's own. Any two instances sharing a mixer must run under the same service account.
- **Config**: a `mixer_remote` output type (`type = "mixer_remote"; dest_path = "..."; stream_id
  = N;`) on any channel, dispatched **unconditionally every tick** (not gated on squelch, unlike
  `udp_stream`) — load-bearing, since the receiver's silence-fill logic only correctly
  distinguishes "sender alive, channel quiet" from "sender gone" if packets keep arriving every
  tick. Receiving side: a mixer-level `remote_inputs` block (`listen_path`, `stream_id`,
  `ampfactor`, `balance`, optional `label`), parsed at startup via the existing
  `mixer_connect_input()` — no new capacity-safety mechanism needed. No live creation (matches
  item 27's "toggle only, no add/remove" precedent for mixers/devices).
- **Threading**: one receive thread per unique `listen_path`, calling `mixer_put_samples()`
  directly — no request/apply split, unlike `device_t`/`channel_t`. `mixinput_t` already has its
  own per-slot mutex specifically so any producer thread can call `mixer_put_samples()` safely
  (already proven by `multiple_output_threads = true`); the request/apply pattern exists for
  fields with no independent lock of their own, which doesn't apply here.
- **Tag support**: `mixinput_t::remote_label` lets `mixer_tx_tag()` (item 26) fall back to a
  configured label when there's no local `source_device_idx`/`source_channel_idx` to resolve —
  needed the same slot-reuse zeroing discipline item 40 established, applied proactively here.
- **Observability**: per-route counters (`rate_mismatch_count`, `sample_count_mismatch_count`,
  `malformed_payload_count`, `seq_gap_count`, `seq_reorder_or_duplicate_count`,
  `last_packet_time`) and per-listener counters (`rejected_uid_count`, `malformed_header_count`,
  `unknown_stream_count` — split from route-level since a malformed header can't be attributed to
  a route). `last_packet_time` specifically distinguishes a quiet-but-alive sender from a dead
  one — confirmed against a real `kill -9` on the sender during validation.
- **Deviation from plan**: a listener `bind()` failure is non-fatal (logs and skips that
  listener) rather than fatal like `init_output()` — matches the existing precedent of
  `control_socket_start()`/`stats_http_start()`, both optional background listeners whose return
  values aren't even checked in `main()`.
- **Gotchas found during real-hardware validation**: `AF_UNIX` socket paths are capped at 107
  bytes (`sizeof(sockaddr_un::sun_path) - 1`) — both send and receive sides correctly reject an
  over-length path rather than truncating or crashing, but keep `dest_path`/`listen_path` short
  (`/run/rtl_airband/*.sock`-style). Separately, `-F` (foreground) does **not** disable syslog —
  `do_syslog` defaults on regardless, so `-e` is also needed to see this feature's own log output
  on stdout/stderr instead of syslog; caused a long detour chasing a phantom "listener never
  starts" hypothesis.
- **Real bug found and fixed**: `free_output_data()` (item 41) had no `case O_MIXER_REMOTE:`, so
  it fell into the generic `default:` branch and leaked the `strdup()`'d `dest_path` on every
  live removal/edit of a `mixer_remote`-connected channel. Fixed by adding the missing case;
  confirmed via a revert-then-reproduce under LeakSanitizer.
- **Second gap found and fixed**: `MixerConfigSnapshot` only ever captured a mixer's `name`/
  `enabled` — `remote_inputs` was invisible to `reload_diff` entirely, so editing a
  `remote_inputs` entry silently did nothing with no signal either way. Fixed the same way item
  31 solved the analogous channel problem: a `remote_inputs_signature` (same `serialize_setting()`
  machinery as `config_signature`) is compared on every `reload_diff`; a mismatch reports
  `"mixer[i]: remote_inputs changed"` under `skipped_requires_restart` instead of silently doing
  nothing.
- **Confirmed independently**: disabling/removing a mixer or its `remote_inputs` on the
  *receiving* instance has zero effect on the *sending* instance under every tested scenario
  (mixer disable, config-rejected mixer-count change, full restart) — the sender's `sendto()`
  just starts returning `ECONNREFUSED`, handled the same as "nobody listening yet."
- **Still not done**: no two-instance system test exists yet (every current system test drives
  exactly one instance; this needs a `Popen()`-based two-process helper). Per-remote-input live
  enable/disable and 16/8-bit wire formats remain out of scope unless a concrete need emerges.

Anything outside those areas should match upstream. If a diff against `upstream/main` shows
changes elsewhere, treat it as unintended drift and flag it.

**43. Device health: ADC peak/clipping, per-channel frequency offset, control socket `status`**
(`src/device_stats.{h,cpp}`, new) — adds the hardware-side numbers a control panel needs to judge
an SDR's setup (headroom before clipping, how far off-frequency the dongle's oscillator is, what
gain it's actually running at), which nothing exposed before: the stats file had per-channel noise
and signal levels and per-device buffer counters only, and the control socket had no read command.
- **ADC peak and clipping.** Once per demod pass, `input_measure_consumed_samples()` scans exactly
  the input bytes that pass advances `bufs` by (wrap-safe), so every sample is counted once even
  though FFT windows overlap. `peak_level` is the largest |I|/|Q| over the last complete ~1 s
  window as a fraction of full scale (U8/S8 by format rails, S16/F32 by the driver's `fullscale`,
  since SoapySDR CS16 devices can report a 12-bit full scale); `clipped_count` counts I/Q samples
  where either component sat on a rail. New `input_t` fields, atomics published by the demod
  thread. Covers the BCM_VC path too, since it reads the raw buffer rather than the FFT input.
- **Per-channel carrier offset.** `iq_in` is now filled for *every* channel, not just
  `needs_raw_iq` ones (two stores per channel-sample, plus a memmove per batch). In the batch loop,
  while squelch is open, the power-weighted sum of z[n]·conj(z[n-1]) over the raw bin output is
  accumulated (**replaced by item 45** with the mean of per-frame phase steps, since the
  power-weighted form is biased by hundreds of Hz on NFM); at the end of the batch
  `freq_offset_from_phase_increment()` subtracts the rotation
  a carrier exactly on the channel frequency would have (the same quantity
  `compute_channel_dm_dphi()` derotates) and converts the remainder to Hz. Sub-Hz precision, where
  FFT bin resolution (~4-5 kHz) would be useless for ppm. Batches less than half open are dropped,
  R_SCAN batches that hopped mid-batch are dropped, and the result is EMA-smoothed (0.2) into
  `freq_t::freq_offset_hz`. Works whether `afc` is on or off. AM sidebands bias the estimate
  slightly (a 0.8-modulated 1 kHz test tone reads ~3% low: ~0.2 ppm at 120 MHz).
  **Gotcha found while testing:** `compute_channel_bin()` picks the bin *below* a channel that
  lands exactly on a bin center; only the 7-term Blackman-Harris window's main lobe makes the
  carrier visible there, so the unit test's simulated DFT has to apply the same window.
- **Driver settings readback.** New nullable `input_t::get_settings` hook (rtlsdr,
  soapysdr; also mirisdr until upstream v7.0.0 removed that driver) reports gain / correction / tuner bandwidth from the driver's `dev_data`. Originally
  called only from the control socket thread; since item 45 every caller holds
  `input_control_mutex()`, the lock the live `set_*` wrappers take, so there's no concurrent
  writer. rtlsdr now stores the gain the tuner actually took (nearest supported step)
  at init, as `rtlsdr_set_gain` already did.
- **Control socket `status` command** (read-only): `{"ok":true,"version":...,"devices":[{index,
  driver,state,mode,centerfreq,sample_rate,gain,correction,bandwidth,peak_dbfs,clipped_samples,
  buffer_overflows,buffer_underruns,retune_failures,output_overruns,channels:[{index,enabled,
  freq_idx,squelch_open,freqs:[{freq,label,modulation,noise_dbfs,signal_dbfs,freq_offset_hz,
  freq_offset_updates}]}]}]}`. Unmeasured values are `null` (JSON has no NaN/inf). Unlike the stats
  file's channel series, channels are addressed by device/channel index.
- **Stats file**: `device_peak_dbfs{device}` (gauge, omitted until the first window completes),
  `device_clipped_sample_count{device}` (counter), `channel_freq_offset_hz{freq,label}` (gauge,
  omitted for a frequency with no transmission yet). Existing series are unchanged.
- Tests: `src/test_device_stats.cpp` (level scans per sample format, window publishing, buffer wrap,
  offset recovery through a windowed DFT at several sample rates incl. a non-integer decimation,
  commit gating/smoothing, status JSON shape) and `system_tests/tests/test_device_status.py`
  (a 700 Hz off-frequency AM carrier and a clipped fixture through the real binary).

**44. `rtl_airband -v` prints a `Features:` line** (`fork_features()` in
`src/helper_functions.cpp`, printed from `main()`). The version line is unchanged; a second line
reads `Features: fork control_socket device_status reserve mixer_remote send_tx_tags stats_http
json_log post_write_script auto_ppm usb_chunk_sizing auto_ppm_voting` (`auto_ppm` added by item 45, `usb_chunk_sizing` by item 46, `auto_ppm_voting` by item 47), followed by whichever optional build components are compiled in
(`rdio_scanner`, `soapysdr`, `pulseaudio`, `nfm`). `mirisdr` was also in that list until
upstream v7.0.0 removed the driver. It was a build-component name, not a fork capability, so its
disappearance doesn't break the never-remove rule below, which covers the fork capability names.
- **Why:** upstream tags in the same `vN.x` range as this fork, and `v6.3.0`/`v6.3.1` exist in
  both with different meanings (see "Version tagging on this fork"), so a version number can no
  longer tell the two apart. rtl-airband-panel reads this line to decide
  whether an instance runs the fork, and soft-locks fork-only settings and Apply live when it
  doesn't.
- **The names are an interface.** Add new names, but never rename or remove one. The line
  always starts with `fork`. Unit tested in `test_helper_functions.cpp`.

**45. Automatic PPM correction (`auto_ppm`)** (`src/auto_ppm.{h,cpp}`, new; `src/device_stats.{h,cpp}`,
`src/input-common.{h,cpp}`, `src/config.cpp`, `src/live_reconfig.{h,cpp}`, `src/control_socket.cpp`,
`src/output.cpp`, `src/rtl_airband.{cpp,h}`) — closes the loop between item 43's per-frequency
carrier offsets and item 35's live correction. Device keys: `auto_ppm` (bool), `auto_ppm_interval`
(s, default 30, ≥5), `auto_ppm_max_ppm` (default 100, 1..100), `auto_ppm_max_step` (default 5).
- **Drivers:** only those with both `set_correction` and `get_settings` hooks, i.e. rtlsdr today;
  parse rejects the key elsewhere (`system_tests/tests/test_auto_ppm.py`). Adding soapysdr needs a
  float-ppm `set_correction` hook.
- **Math:** residual ppm = −offset_hz / channel_freq × 1e6 (tuner LO and ADC share one rtlsdr
  crystal, so the error scales with the channel frequency, not centerfreq); new correction =
  current + residual. Matches librtlsdr's convention (`corr` = how fast the crystal runs);
  confirmed on hardware (setting +20 ppm moved a carrier +3.2 kHz at 162.55 MHz).
- **Decision** (`auto_ppm_decide()`, pure, unit tested in `src/test_auto_ppm.cpp`): a frequency votes
  once it has ≥20 offset updates since the last tuning change. Estimates implying an absolute
  correction beyond ±100 ppm (`AUTO_PPM_HARD_LIMIT`) are dropped individually. The rest are
  combined by median: anything over 1.5 ppm from it is an outlier (offset-carrier ATC sites are
  ±2.5–7.5 kHz, ~20–60 ppm). It holds when the survivors spread over 2 ppm, or when there are
  more than two voters but fewer than two survivors. Then a 0.75 ppm deadband (1 ppm since item
  48), a step limit, and a clamp to ±`auto_ppm_max_ppm`.
  **Divergence guard:** if the residual grows (by >0.25 ppm) after two consecutive steps, it
  reverts to the configured correction and trips (`disabled_reason` in status) until a
  `reload_diff` or restart.
- **Estimator reset:** item 43's `freq_offset_hz` EMA keeps pre-change samples, so it can't feed a
  closed loop directly. `input_t::tuning_generation` is bumped after every successful centerfreq,
  correction or sample_rate change. `freq_offset_commit()` keeps a second per-frequency EMA
  (`freq_t::ppm_offset_hz/updates/generation`) that restarts on a generation change after
  skipping `FREQ_OFFSET_SETTLE_BATCHES` (16, 2 s) batches of already-buffered old-tuning samples.
  `freq_offset_hz` itself is unchanged. R_SCAN bumps the generation on every hop, so only the
  current scan frequency ever votes.
- **Threading:** one `auto_ppm` thread (started only if a device has the key), ticking every 1 s,
  joined in `main()` before devices stop. New process-wide `input_control_mutex()` (recursive,
  function-local static, so hand-built test `input_t`s need no setup) serializes every live driver
  control call: the `input_set_*` wrappers, `get_settings` callers, and `device_apply_sample_rate()`
  (held across its whole stop/reopen, since `dev_data->dev` is replaced there). It also closes a
  pre-existing, never-hit overlap between control-socket `set_*` and the demod thread's live retune.
  Lock order: `auto_ppm_device::lock`, then `input_control_mutex()`.
- **reload_diff:** `auto_ppm` toggles live (and re-arms a tripped device) only when the device
  started with the key present. Turning it on for a device that didn't, or changing
  interval/max_ppm/max_step, goes under `skipped_requires_restart`. While it stays on, an unchanged
  config `correction` is no longer reapplied (that would discard the learned value). It's reported
  as `correction unchanged in config, left to auto_ppm`. A changed one is applied and becomes the
  new revert target; one outside the limit is refused. `set_correction` refuses values outside
  `auto_ppm_max_ppm` while auto_ppm is on.
- **Observability:** status JSON `auto_ppm` object per device (`null` if unconfigured) and
  `ppm_offset_hz`/`ppm_offset_updates` per frequency; stats file `device_ppm_correction`,
  `device_auto_ppm_residual_ppm`, `device_auto_ppm_adjustment_count` (auto_ppm devices only);
  LOG_INFO per adjustment, LOG_WARNING once per clamp episode, LOG_ERR on a trip.
- **No persistence:** each start begins from config `correction`.
- **Decision rule hardened by items 47 and 48** (minimum voters, deadband in Hz, two-reading
  confirmation; same-direction divergence, lone-voter distrust, 1 ppm floor) after prod hunting
  on low-band and mobile-heavy devices.
- **Estimator change (affects item 43's `freq_offset_hz` too):** hardware testing showed item 43's
  power-weighted estimate, the angle of Σ z[n]·conj(z[n−1]), is badly biased on NFM. The
  FFT-bin channelizer's response is curved across a ±5 kHz-deviation signal, and weighting by
  power lets it pull the result. On NOAA 162.550 it read −340 Hz at fft_size 512 (true −72 Hz),
  and anything from −140 to +700 Hz across fft_size 256..2048. Now the plain mean of each frame's
  wrapped, derotated phase step (`freq_offset_step_cycles()`, one atan2 per squelch-open sample),
  accumulated as `channel_t::fo_step_sum`. That reads −64..−104 Hz against a true −89 Hz on the
  same signal at fft_size 256–1024. A Python replay of captured IQ through the same windowed-bin
  model matched both behaviours.
  **Remaining limitation:** carrierless digital signals (P25 C4FM control channels) still read
  ~1.2–1.4 ppm high when the channel sits off its bin's center (checked against a spectral
  centroid), because the bin response tilts their flat spectrum. AM and analog FM voice are
  carrier-dominated and unaffected. At very large offsets (≳1 kHz), wide FM reads up to ~8% low,
  which only slows convergence.
- **Hardware validation** (R820T dongle, NOAA 162.550 reference, true error ≈ +0.5 ppm):
  - From `correction = −20`, it converged −20→−15→−10→−5→−1→0 at a 10 s interval and held
    (residual +0.4).
  - A channel mistuned 3.5 kHz with `auto_ppm_max_ppm = 10` stepped 0→5→10 and held at the limit
    with one warning. `set_correction 11` was refused.
  - A sign-flipped scratch build went 3→6→11, then tripped and reverted to 3.
  - reload_diff: an unchanged config kept the learned value; `auto_ppm = false` restored the
    config correction; `true` resumed adjusting.
  - Continuous transmitters (NOAA, P25 control channels) need an explicit `squelch_threshold`.
    The auto squelch learns a carrier that never drops as its noise floor and never opens, and
    offsets are only measured while squelch is open.
- Side fix: `DeviceConfigSnapshot::has_gain`/`gain` are now default-initialized. Hand-built test
  snapshots copied them uninitialized (UBSan "invalid bool"), the same trap the struct's other
  defaulted fields already document.

**46. RTL-SDR USB transfer size follows the sample rate** (`src/input-common.{h,cpp}`,
`src/input-rtlsdr.{h,cpp}`) — upstream always asked librtlsdr for 320000-byte transfers. At
1.024 Msps that is 156 ms of samples, longer than one 125 ms audio batch (`WAVE_BATCH`), and the
demod loop emits batches unpaced, so roughly one transfer in four completed two batches within
0–10 ms. The demod→output handoff is a single slot (`device_t::waveavail`), so the second batch
overran whenever the output pass hadn't finished.
- **Found by measuring production**, not by guessing. Observe-only bpftrace on the prod host
  showed NFS file I/O (worst 15 ms) and scheduler delay (worst 6 ms) nowhere near the 125 ms
  budget, while the overrun rate tracked sample rate: 1.024 Msps instances 57–248/h, 2.56 Msps
  instances (whose transfer was already 62.5 ms) 6.5–10.5/h despite having the most file
  outputs. The output thread's wakeup gaps were ~156 ms or 0–10 ms, never ~125 ms.
- **The fix**: `rtlsdr_auto_usb_buf_len()` sizes each transfer to half a batch, rounded down to
  librtlsdr's 512-byte multiple and clamped to 16384..320000 (1.024 M → 128000, 1.4 M → 174592,
  2.0 M → 249856, 2.56 M and up → 320000, unchanged). `rtlsdr_auto_usb_buf_count()` raises the
  default buffer count so total buffering stays at the old 10 × 320000 bytes (1.024 M → 25), so
  neither the cushion against RX stalls nor usbfs memory use changes. Both are computed in
  `rtlsdr_rx_thread()`, not at parse time, so item 36's live `sample_rate` change gets a size for
  the new rate. The file driver already does the same thing (`src/input-file.cpp`, one batch per
  read).
- **Config**: new optional device key `buffer_size` (bytes, positive multiple of 512, at most
  16 MiB) overrides the auto length; `buffer_size = 320000; buffers = 10;` restores the old
  behavior exactly. An explicit `buffers` is still honored as given. Like `buffers`, it applies
  only at RX start and `reload_diff` ignores it.
- **Hardware validation** (dev box, two Nooelec NESDR SMArt v5, R820T, 1.024 Msps, six NFM
  channels with continuous file outputs): old sizing showed 483–488 back-to-back output wakeups
  in 5 min; auto sizing showed 1, with gaps centered on 125 ms. Confirmed sequentially on one
  dongle and simultaneously on two, same signal. No input buffer overflows; recorded audio
  length matched wall time.
- **CPU**: `perf stat` showed the same work (instructions +0.6%), but reported CPU time rose
  ~35% (5.0% → 6.8% of a core on the dev box) because the evenly spread load lets the CPU clock
  stay lower (1.63 vs 2.12 GHz average). Expect the same reading on prod; it's frequency
  scaling, not extra cost.
- Doesn't touch SoapySDR. The planned demod→output ring buffer would absorb bursts from any
  driver.
- Tests: `src/test_input_common.cpp` (sizing and validation); `system_tests/tests/test_rtlsdr_buffer_size.py`
  (parser rejects bad `buffer_size` through the real binary, before any device is opened). The
  system test caught a real bug: libconfig throws on `(long long)` of a plain int setting.

**47. auto_ppm: minimum voters, frequency-scaled deadband, two-reading confirmation**
(`src/auto_ppm.{h,cpp}`, `src/config.cpp`, `src/live_reconfig.cpp`). Item 45's decision rule
hunted in production on the first day of v7.2.1 (2026-11-03), and the divergence guard tripped on
two instances. Neither dongle was actually diverging:
- **042 (42 MHz):** 17 adjustments in 45 min between −1 and +3 ppm, with only 1–2 voters
  (residuals +2.80, +0.92, −4.03, +2.28, … ppm). At low band 1 ppm is only 42 Hz, so transmitter
  tolerance (Red 2 Base −88 Hz vs Red 2 Mobile +57 Hz) plus estimator noise is several ppm. The
  fixed 0.75 ppm deadband (32 Hz) and 0.25 ppm growth tolerance (10 Hz) sat below that noise.
- **172:** steps of −0.76, +1.65, −1.17, −0.78, +2.27 ppm, mostly on one voter. Different mobiles
  on the same frequency disagree by 1–2 ppm.

Three changes, all inside the still-pure `auto_ppm_decide()`:
- **`auto_ppm_min_voters`** (device key, default 1, 1..32): surviving voters, counted after
  outlier rejection, must reach it, or the evaluation holds with `"N voter(s), need K"` and no
  residual. Parse rejects >1 in scan mode (only the current frequency's generation votes) and
  anything above `channels + reserve_channels`.
- **`auto_ppm_deadband_hz`** (default 100, 0..1000): deadband = max(0.75 ppm, Hz / median voter
  frequency). That gives 2.36 ppm at 42.44 MHz, 0.83 at 120 MHz, and the unchanged 0.75 from
  133 MHz up. 0 restores item 45's deadband. The divergence guard's growth tolerance is now
  deadband / 3, which is 0.25 at the floor as before. (Item 48 raised the floor to 1 ppm.)
- **Persistence (fixed, 2 readings):** an out-of-deadband residual steps only when the previous
  valid reading had the same sign **and** the contributing voters' total update count grew in
  between. Without that second condition, a stale EMA re-read 30 s later would confirm itself.
  - A sign flip restarts the count.
  - A reading inside the deadband clears it.
  - Evaluations with no residual leave it as is.
  - The divergence check still runs first, on the first reading after a step.
  - **Every step now takes at least two intervals.**
- Status JSON gains `min_voters`, `deadband_hz`, `deadband_ppm` and `pending_count`. The
  adjustment log line includes the deadband. A changed key goes under `skipped_requires_restart`.
- Tests (`src/test_auto_ppm.cpp`):
  - Replays of the two prod sequences. 042 drops from 17 adjustments to 4 at defaults and to 1
    with `min_voters = 2`. 172 makes 0 with `min_voters = 2`.
  - The dev-box NOAA single-reference case still converges −20 → 0, in 8 evaluations.
  - Persistence edge cases, low-band deadband, and growth-tolerance scaling.
  - `system_tests/tests/test_auto_ppm.py` covers the parse-time rejections on an rtlsdr config,
    with no dongle needed.

**48. auto_ppm: same-direction divergence, lone-voter distrust, 1 ppm floor** (`src/auto_ppm.{h,cpp}`).
The first overnight run of item 47 on prod (2026-11-04, 10.5 h) fixed 042 and 172 (1 and 0
adjustments with `auto_ppm_min_voters = 2`), but showed three more faults:
- **154 tripped without diverging.** SC Fire Tac 2 (153.950) reads a steady −5048 Hz (+32.8 ppm).
  Voting alone 4 min after start, it stepped the dongle −5 ppm; persistence didn't stop it,
  because it's wrong the same way every time. Later the guard counted a +1 step at 03:23 and a −1
  step at 06:31 as "two consecutive adjustments", and tripped when the first reading after the
  second step came from Tac 2 alone.
- **169 flipped 16 ↔ 17 ppm** four times on residuals of 0.77–0.94 ppm: a dongle drifting around
  n + 0.5 against a 0.75 ppm floor, with a whole-ppm driver.
- **119** (one AM airband channel) hunted between −5 and +2 ppm, alternating between tower and
  aircraft transmitters. No rule fixes a one-channel device: auto_ppm was turned off there in
  config.

Three changes, all inside `auto_ppm_decide()`:
- **Divergence counts only same-direction steps.** A step opposite to the previous one resets
  `growing_steps` (new `AutoPpmState::last_step_sign`). A real sign error steps the same way
  repeatedly while the error grows; alternating steps are hunting, which persistence handles.
- **Lone-voter distrust after convergence.** `AutoPpmState::converged` is set by the first
  in-deadband reading with ≥2 contributing voters. After that, an evaluation with exactly one
  contributor and |residual| > max(5 ppm, deadband) (`LONE_VOTER_LIMIT_PPM`) holds with `"one
  channel +N ppm off; waiting for another channel to agree"`. It has no residual and leaves
  persistence and the pending post-step check alone. Before convergence a lone voter is trusted,
  so a single reference (the dev-box NOAA case) still pulls in a large startup error.
  `reset_tracking()` (enable toggle, re-arm) clears it. This wouldn't have stopped Tac 2's first
  lone step, which came before any multi-voter agreement; `auto_ppm_min_voters = 2` (now set on
  154) and the planned per-channel opt-out (#24) cover that.
- **Deadband floor 0.75 → 1 ppm** (`DEADBAND_PPM`), with growth tolerance 0.33 ppm at the floor.
  The Hz term now only widens it below 100 MHz at the default 100 Hz.
- Status JSON gains `converged`. No new config keys, so no new `Features:` name.
- Tests: replays of 154's trip and 169's toggling; opposite-direction steps; lone voter held after
  convergence, trusted before it, and two agreeing channels still stepping on a large error. They
  fail against item 47's rules. The NOAA single-reference case still converges in ≤10 evaluations.

**50. auto_ppm: hysteresis on the deadband** (`src/auto_ppm.{h,cpp}`). The first night on item 48
(2026-11-04/05, 23.5 h) had no trips, but two pairs of steps that cancelled out:
- 154 stepped −3 → −2 on +1.16 ppm (6/8 voters) and back to −3 an hour later on −1.00. The check
  was `fabs(residual) < deadband`, so exactly 1.00 counted as outside it.
- 042 stepped 1 → −2 and back 12 h apart, on −2.64 and +2.55 against a 2.36 ppm deadband.

Both are a dongle sitting near n + 0.5 ppm with a driver that only takes whole ppm, as on 169
(item 48). `AutoPpmState::settled` is set by the first reading inside the deadband. From then on a
residual must reach deadband × 1.25 (`HYSTERESIS_FACTOR`: 1.25 ppm at the floor, 2.95 at 42 MHz)
before persistence counts it. Readings in between hold as `"within deadband (hysteresis)"` and
reset persistence like any in-deadband reading. A step doesn't clear `settled`, so a reading just
over the deadband the other way doesn't undo it. Before the first in-deadband reading, the
deadband alone decides, so startup convergence is unchanged. `reset_tracking()` clears it, and
status JSON gains `settled`. The cost is that a sustained error between 1 and 1.25 ppm (about
200 Hz at 160 MHz) is left alone. Tests replay both prod pairs, check that a settled device still
steps past the band, and that the hysteresis only applies once settled.

#### Output thread isolation

**51. The stats file is written from its own thread** (`src/output.cpp`, `src/rtl_airband.{h,cpp}`).
`write_stats_file()` ran on the output thread (the one owning device 0), so a stats write that
blocked stalled every output on the instance. On prod (2026-11-05) the nightly VM backup calls
qemu-ga `guest-fsfreeze` at ~01:07 PST. The VM's ext4 freezes for ~12 s, the stats `fopen()`/write
blocks, and all 12 instances lose ~12 s of audio together: 48–149 output overruns each, 1158 in one
minute. The disk itself was 1–4% busy, so it was never contention. Recordings go to NFS, which
the freeze doesn't touch.
- `stats_writer_start()` (called from `main()` after the output threads start) runs a thread that
  writes immediately, then every `stats_update_interval`. It waits on a condition variable, so
  shutdown is prompt. `stats_writer_shutdown()` runs after the output threads are joined and
  writes once more, so the final counters cover all output, as the old post-loop write did.
- The text is built with `open_memstream()` and written to `stats_filepath` only afterwards. The
  part that reads channel and device state therefore stays bounded, which item 41's reclaim grace
  period relies on. Only the file write can block.
- File semantics are unchanged: same path, truncated and rewritten in place.
- Not unit tested: `output.cpp` isn't linked into `unittests` (item 20).
  `system_tests/tests/test_stats_writer_stall.py` points `stats_filepath` at a FIFO with no reader
  for 5 s, so `fopen()` blocks the way a frozen filesystem does. Over a 10 s transmission the old
  binary recorded 6.3 s, and this one records all of it. `test_stats_update_interval.py` still
  passes.
- The freeze still blocks anything else that writes to local disk. On prod that is only the stats
  thread and logging. The output threads write recordings to NFS and the network.

### Do not import from `yegors`

The `yegors` fork also contains SRT output support and its own UDP path. The UDP path there is
non-functional — its `O_UDP_STREAM` branch in `process_outputs()` checks squelch and then never
calls `udp_stream_write()`. Neither the SRT work nor that UDP code should be pulled in.

### Upstreaming

Bug fixes that are not personal preference — the `sendto` fix in particular — are candidates for
PRs to `rtl-airband/RTLSDR-Airband`. Open an issue describing the bug before sending a PR.
The `post_write_script` / `min_rx_seconds` features originated with `yegors`; credit accordingly
if they are ever proposed upstream.

## Working Norms

- **Never commit directly to `main`.** Check `git branch --show-current` before committing; if it is `main`, create a branch first. All changes reach `main` through a pull request. Name branches `<prefix>/<short_description>`, where the prefix is the contributor's GitHub username or a category such as `dev` or `fix` (e.g. `dev/drop_macos14_ci`).
- **Keep this file updated** as changes are made to the codebase.
- **Never guess or make assumptions** — ask clarifying questions when requirements are unclear.
- **All code changes should include unit tests.** When possible, write tests first and implement code to make them pass (TDD).
- **After writing code, review comments** and remove any that don't explain non-obvious behavior — don't comment what the code already says.
- **This fork feeds live public-safety audio.** Changes here affect production Broadcastify feeds.
  Test on a low-priority SDR instance before rolling out.

## Development Environment

The repo is set up for development in VS Code using a devcontainer (`.devcontainer/`). When working
inside the devcontainer, all compile, test, and run commands must be executed inside the container.

Note: the devcontainer has failed to build under GitHub Codespaces (`docker buildx build` exits 1
during container creation). Local builds on Ubuntu without the devcontainer work fine — install the
dependencies directly and build with CMake as below.

Large system test cases for manual runs can be kept outside the repo in `../RTLSDR-Airband_test_cases/` (a sibling of the checkout, so several checkouts can share one copy). The devcontainer mounts it read-only at `system_tests/test_cases/` (gitignored); the pytest suite does not read it. `initializeCommand` creates an empty folder on the host if it is missing, so the container starts without it. This needs a POSIX host shell (macOS, Linux, WSL) and a container rebuild to take effect.

## Wiki Documentation

User-facing documentation lives in this fork's wiki, a separate git repo:
`git@git.schmall.me:jschmall/RTLSDR-Airband.wiki.git` (Forgejo). The GitHub wiki
(github.com/jschmall/RTLSDR-Airband/wiki) is a copy carried by the push mirror; never edit or
push it directly. Upstream's wiki (github.com/rtl-airband/RTLSDR-Airband/wiki) is a different
repo that this one merges from.

- **Edit the fork wiki as part of fork changes.** A change that adds or changes a config key, a
  control socket command, a stats metric, a `Features:` name or a CMake option isn't finished until
  the relevant wiki page documents it (key name, type, default, range, first fork version). The
  README's "Fork-Specific Features" section is the summary; the wiki pages carry the detail.
- **Where things go:** device keys on `Configuring-<driver>-devices`, channel keys on
  `Configuring-channels-for-*`, output keys on `Configuring-<type>-outputs`, globals on
  `Configuring-Global-settings`, metrics on `Channel-usage-statistics`, control socket commands on
  `Live-reconfiguration-via-control-socket`, build options on `Installation`. Larger features get
  their own page (as `Automatic-PPM-correction`, `Device-health-status`,
  `Cross-instance-mixer-input` do), listed in `_Sidebar.md` with "(fork)" and on `Home`'s
  "This fork" list.
- **Style:** match the existing pages (a Syntax block with optional keys commented out at their
  defaults, then "Explanation of keywords"); mark fork-only keys **fork-only**.
- **Links:** keep each `[[...]]` link on one line (one that wraps renders as raw text), and give a
  link to a section a label (`[[Text|Page#anchor]]`).
- **Upstream wiki changes:** merge upstream's wiki master into the fork wiki when bumping to a new
  upstream release, keeping fork sections on conflict.
- Edits to *upstream's* wiki are still only suggested, never made directly.

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

RTLSDR-Airband is a C++ SDR (Software-Defined Radio) application that receives analog radio voice
channels from SDR devices (RTL-SDR, SoapySDR) and produces MP3 audio streams for Icecast,
file recording, UDP, and PulseAudio.

## Build Commands

Dependencies: libconfig++, libmp3lame, libshout, libfftw3f, librtlsdr, libsoapysdr, libpulse, libcurl (fork-only, for `RDIO_SCANNER`), plus optional libgoogle-perftools for `-DPROFILING=TRUE` (`install_dependencies` installs it only where apt can resolve it; `scripts/build_all_combos` skips the profiling build when `libprofiler` isn't found). Install via `.github/install_dependencies`.

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
| `RDIO_SCANNER` | ON | Native rdio-scanner call-upload output (fork-only, needs libcurl) |
| `BUILD_UNITTESTS` | OFF | Build Google Test unit tests |
| `BCM_VC` | OFF | Broadcom VideoCore GPU FFT (RPi v2 only; requires `PLATFORM=rpiv2`, which also turns it on) |

`RTLSDR`, `SOAPYSDR`, `PULSEAUDIO`, and `PROFILING` are hard requirements when ON: if the library is not found, CMake fails at configure time. Pass `-D<FLAG>=OFF` to build without it. The fork-only `RDIO_SCANNER` is the exception: it is still auto-detected, and a build without libcurl silently leaves rdio_scanner out (check the `-v` Features line).

The flag names are `NFM` and `PULSEAUDIO`, **not** `WITH_NFM` / `WITH_PULSEAUDIO`. A `-DWITH_NFM=ON`
typo silently defines an unused cache variable and leaves NFM off. Confirm from the build rather
than from the command line that was typed:

```bash
grep -n "NFM" builds/Release/CMakeCache.txt
```

### `NFM` matters for anything consuming the UDP stream

`WAVE_RATE` is 16000 when NFM is enabled and 8000 when it is not. Any downstream decoder of the raw
UDP PCM must match, or audio plays back at the wrong rate. This fork's deployed builds use `NFM` on.
A config containing `modulation = "nfm"` fails to parse on an AM-only build, which is a useful
implicit confirmation that the flag took effect.

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

**Note**: as of this writing, `gh api repos/.../actions/runs` shows zero workflow runs have ever
executed on this fork's repo despite these workflows being enabled and correctly triggered on
`pull_request`/`push` — none of the ~19 merged PRs so far actually had CI run as a gate. Treat a
local build + `unittests` pass across the relevant configurations as the real pre-merge check
until this is investigated; don't assume a green PR check exists.

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

### Version tagging on *this fork*

The section above describes how the tag is produced **upstream**. On this fork `version_bump.yml`
exists but has never actually run (see the CI note above — no workflow run has ever executed on
this repo), so **fork tags are created by hand** on the merge commit and pushed by name:

```bash
git tag -a v7.1.0 -m "..." && git push origin v7.1.0
```

This fork tags its own releases in the same `vN.N.N` namespace upstream uses, so its numbers must
stay **above** upstream's latest to be unambiguous — `v7.1.0` sits above upstream `v7.0.0` (#582), leaving
room for upstream `v7.0.x` patch releases. Also
check whether any upstream commit being merged is still **untagged** and carries a bump keyword:
upstream will tag it later, and the fork must leave room for that. `v6.3.0` skips `v6.2.0` because
upstream #574 (`#minor`, merged here) had no tag yet at merge time.

**This rule was broken more than once, and three names now clash.** Upstream later tagged its own `v6.3.0`
(#580) and `v6.3.1` (#581), which point at different commits from the fork's tags of the same
names. The same is true of `v5.3.0`: the fork's `v5.3.0` (`86d25de`) exists only in the local clone,
not on `origin`, and upstream's `v5.3.0` is a different commit. Neither name tells you which project a build came from, so use the `-v` `Features:` line
(item 44) to identify a fork build. Because of the clash, `remote.upstream.tagOpt` is set to
`--no-tags` in the local clone, and upstream tags are fetched under a separate namespace instead:

```bash
git config remote.upstream.tagOpt --no-tags
git config --add remote.upstream.fetch '+refs/tags/*:refs/tags-upstream/*'
git log -1 refs/tags-upstream/v7.0.0   # inspect an upstream tag without clashing with a fork tag
```

Older upstream tags fetched before that change (through `v7.0.0`, minus the three clashing names)
still sit in the local `refs/tags/` namespace. They are deliberately **not pushed to `origin`**, which is why `git push --tags` must
never be used here — the README's fork-version badge reads `origin`'s tags and would otherwise
start reporting upstream's release as this fork's. `scripts/find_version` (and therefore
`rtl_airband -v`) reports `git describe` against whichever tag is nearest in local history, so it
can name an upstream tag on an untagged working tree; that's expected, not drift.

`origin` mirrors to GitHub, and the mirror copies **every** tag on `origin`, so an upstream tag pushed to
`origin` by mistake lands on GitHub (and in the badge) within seconds. If that happens, delete it from
`origin` (`git push origin --delete refs/tags/<tag>`) and force a mirror sync. Tag and branch
*deletions* don't trigger a sync on their own; they otherwise reach GitHub only at the next scheduled
sync (up to 8h). The fork's own tags on `origin` are exactly: `v5.5.0`, `v6.3.0`, `v6.3.1`, `v6.4.0`,
`v6.5.0`, `v6.6.0`, `v6.7.0`, `v7.1.0`, plus any created since.

`CHANGELOG.md` is fork-only (upstream has none) and is **not** currently kept in sync — it stops
at 2026-07-23 and predates fork-delta items 27-42 entirely. Treat this file's numbered fork-delta
list, not `CHANGELOG.md`, as the authoritative record of what the fork carries.

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

Tests use Google Test (fetched via CMake FetchContent). In this fork the `unittests` binary links
`config.cpp` but not `rtl_airband.cpp`. Every global that `config.cpp` reads therefore needs a
test-side definition, with the same default, in `src/test_live_reconfig.cpp`. Upstream's tests
don't link `config.cpp`, so an upstream merge that adds a global (e.g.
`global_split_tx_drop_shorter_than`, #580) builds fine upstream and fails to link here. Test files in `src/test_*.cpp` cover filters, squelch, CTCSS, helper functions, and signal generation. `src/test_base_class.h` provides test utilities.

## Resolved Issue: UDP Stream "Pacing" Was a Byte-Length Bug

Previously tracked here as an open pacing mystery: a downstream `ffmpeg -f null` read of the
`udp_stream` output reported ~4x realtime throughput, and every Liquidsoap consumption strategy
tried — `input.ffmpeg` directly on the UDP socket, the same wrapped in `buffer()`, with and
without `self_sync`, and MP3 transcoded into `input.harbor` — produced choppy audio or a latency
snowball ending in a broken Icecast pipe.

**Root cause found without needing the tcpdump diagnostic**: item 3 under "Local changes carried
on top of upstream" above. The send side was emitting exactly 4x the correct byte count per
buffer (confirmed by code inspection, not packet capture), which fully accounts for the "~4x
realtime throughput" reading and the downstream jitter/latency symptoms — there was no pacing
gap to diagnose. Fixed in `src/output.cpp`, `src/rtl_airband.cpp`, and `src/udp_stream.cpp`.

If choppy audio or throughput anomalies reappear after this fix, re-open the tcpdump-based
inter-packet-timing diagnostic (`tcpdump -i eth0 -n -ttt udp port 9001`, not `-i any` — that
didn't capture this traffic on the deployment host) before assuming a new pacing bug and writing
send-side pacing code; confirm with a wire capture first.

## Deployment Context

Deployment-specific notes (build/SDR host, audio consumers, addresses, per-instance setup) live in
`CLAUDE.local.md`, which is gitignored. Keep anything that identifies the production network there,
not here: this repo is public.

General lessons that apply to any deployment:

- `sudo make install` places the binary at `/usr/local/bin/rtl_airband`. Running instances pick up
  a new binary on their next restart; restart them one at a time and confirm each comes back clean.
- `rtl_airband -v` can print a git hash rather than a semantic version on untagged builds; that is expected.
- Per-instance channel configs live outside this repo and are not committed.
- Once an instance's config sets `control_socket_path` (`dynamic_reload`, item 27), its systemd
  unit must set `User=`/`Group=` to a non-root service account (the control socket's `SO_PEERCRED`
  check rejects clients whose UID differs from the daemon's).
- **That non-root account also needs USB device access**: add it to `plugdev`
  (`sudo usermod -aG plugdev <service-account>`), the group librtlsdr's udev rule grants access to.
  Without it the failure is a misleading `RTLSDR device with serial number N not found`, because
  `rtlsdr_find_device_by_serial()` (`src/input-rtlsdr.cpp`) discards the return code of
  `rtlsdr_get_device_usb_strings()`, a known robustness gap not yet fixed. Also documented in
  `init.d/rtl_airband.service`.
