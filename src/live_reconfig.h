/*
 * live_reconfig.h
 * Live retune/reconfiguration primitives for the dynamic_reload control socket
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#ifndef _LIVE_RECONFIG_H
#define _LIVE_RECONFIG_H 1

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "auto_ppm.h"
#include "rtl_airband.h"

// Pure math, shared with config.cpp's parse-time setup so both paths agree by construction.
size_t compute_channel_bin(int channel_freq, int centerfreq, int sample_rate, size_t num_fft_bins);
uint32_t compute_channel_dm_dphi(int channel_freq, int centerfreq, int sample_rate);

// Pure math, shared with config.cpp's startup input_t::buffer sizing (parse_devices()) so both
// paths agree by construction - same rationale as compute_channel_bin() above. A mismatch here
// between startup and a live sample_rate change would be a real correctness bug (inconsistent
// buf_size/bps/available arithmetic), unlike e.g. snapshot_parse_anynum2int()'s deliberate
// duplication of a startup-only convenience parser (live_reconfig.cpp) where drift is harmless.
size_t compute_input_buf_size(int sample_rate, int bytes_per_sample);

// Called by the control socket thread. Validates dev->mode (only R_MULTICHANNEL devices are
// retunable this way - R_SCAN devices already have their own controller-thread retune loop) and,
// if valid, posts the request; returns false without touching any state otherwise. The actual
// hardware retune and bins/base_bins/dm_dphi recompute happen later, in the demod thread that
// owns this device - see device_apply_retune().
bool device_request_retune(device_t* dev, int new_centerfreq);

// Polls dev->pending_centerfreq_request until the demod thread consumes it (see
// device_apply_retune()) or timeout_us elapses, then reports the real outcome. Returns true only
// if the request was consumed AND the hardware retune actually succeeded
// (!centerfreq_apply_failed). Sets *timed_out and returns false if the request is still pending
// at the timeout - callers should treat that as "posted, not yet confirmed" (the same semantics
// post_request_and_wait() already uses for enable/disable/remove requests), not a hard failure.
// Shared by handle_retune() (control_socket.cpp) and compute_and_apply_diff() below - both post
// via device_request_retune() and need the same wait-and-check logic to report a real result
// instead of just "request accepted".
bool device_confirm_retune(device_t* dev, int timeout_us, bool* timed_out);

// Called from demodulate() (rtl_airband.cpp), in the demod thread that exclusively owns dev, once
// it observes a pending request. Recomputes bins/base_bins/dm_dphi for every channel on this
// device in place (plain assignments - no locking needed, same single-writer-thread invariant
// AFC's own per-bin adjustments already rely on) and retunes the hardware via
// input_set_centerfreq(). Returns true on success, false if input_set_centerfreq() failed (e.g. a
// transient hardware error) - the caller is responsible for publishing this into
// dev->centerfreq_apply_failed before clearing dev->pending_centerfreq_request, so a control
// socket thread waiting on the request sees the real outcome, not just "request consumed". A
// failure here does not mark the device dead - see input_set_centerfreq()'s comment
// (input-common.cpp).
bool device_apply_retune(device_t* dev, int new_centerfreq);

// Live sample_rate change - same request/apply/confirm shape as retune above
// (device_request_retune/device_confirm_retune/device_apply_retune), but device_apply_sample_rate()
// is substantially heavier: it stops this device's RX thread, reopens the hardware at the new
// rate, resizes the input buffer, and recomputes every channel's bins/base_bins/dm_dphi, all
// synchronously within this device's own turn in the demod thread's round-robin (see
// demodulate(), rtl_airband.cpp) - the same single-writer-thread invariant device_apply_retune()
// already relies on. Also unlike retune/gain/bandwidth/correction, a failure here isn't free to
// walk away from: the RX pipeline was already torn down for the attempt, so
// device_apply_sample_rate() rolls back to the OLD rate (against the still-intact old buffer,
// never freed until the new rate is confirmed working) rather than leaving the device stopped.
// Only marks dev->input->state = INPUT_FAILED if that rollback ALSO fails - a genuine "the
// device won't come back up at any rate" case, which demodulate()'s existing
// INPUT_FAILED -> INPUT_DISABLED handling already knows how to fold into devices_running/the
// "all receivers failed" exit path with no new machinery needed there.
//
// device_request_sample_rate() additionally rejects (returns false, posts nothing) if
// dev->input->stop is NULL - some drivers may have nothing to pause/reopen, and structurally
// cannot support this operation at all.
//
// device_confirm_sample_rate() mirrors device_confirm_retune() exactly (same polling/reporting
// contract) - callers should budget a longer timeout_us than a plain retune, since the apply
// side includes a full RX-thread join and hardware device reopen, not just one API call.
bool device_request_sample_rate(device_t* dev, int new_sample_rate);
bool device_confirm_sample_rate(device_t* dev, int timeout_us, bool* timed_out);
bool device_apply_sample_rate(device_t* dev, int new_sample_rate);

// Channel enable/disable, split into request (control socket thread) / apply (output thread)
// halves - see channel_t::pending_enable_request's comment (rtl_airband.h) for why: a channel's
// `enabled` flag and `outputs` are owned by whichever output thread processes that channel, so
// applying the change from any other thread (the control socket's) would race that thread's own
// concurrent reads/writes to the same output_t structs. channel_request_enable()/
// channel_request_disable() just post the request (O(1), touch nothing else) and are the only
// half safe to call from the control socket thread; channel_apply_enable()/
// channel_apply_disable() do the real work and must only be called from within output_thread()
// (src/output.cpp), which polls and consumes pending_enable_request once per pass.
bool channel_request_enable(channel_t* channel, int timeout_us = 500000);
bool channel_request_disable(channel_t* channel, int timeout_us = 500000);
void channel_apply_enable(channel_t* channel);
void channel_apply_disable(channel_t* channel);

// Channel removal (tail-only - see compute_and_apply_diff()'s decrease branch), same request/
// apply split as enable/disable above, but with a stronger completion guarantee - see
// channel_t::pending_remove_request's comment (rtl_airband.h). channel_request_remove() is safe
// to call from the control socket thread; channel_teardown_for_removal() must only run inside the
// output thread that owns this channel. Closes connections (disable_channel_outputs()) and frees
// each output's LAME encoder/lamebuf immediately (the one resource that scales meaningfully with
// channel count - see LAMEBUF_SIZE), then hands channel->outputs/freqlist/config_signature and
// each output's own `data` struct off to reclaim_pending_channel_frees() (below) rather than
// freeing them immediately: output_check_thread() (src/output.cpp), the demod thread, and
// write_stats_file() (src/output.cpp) all read this memory with no synchronization beyond, at
// best, the same `enabled` check this teardown already sets first - write_stats_file() has no
// gate on it at all. Freeing on a delay closes that window without touching any of those threads'
// hot-path code.
bool channel_request_remove(channel_t* channel, int timeout_us = 500000);
void channel_teardown_for_removal(channel_t* channel);

// Actually frees channel->outputs/freqlist/config_signature (and each output's own `data`
// struct) for every channel_teardown_for_removal() call whose reclaim_grace_period_sec (see
// live_reconfig.cpp) has elapsed since teardown - see channel_teardown_for_removal()'s comment
// above for why teardown itself only queues rather than frees directly. Called once per pass from
// output_thread() (src/output.cpp), the same thread that already owns this memory via
// channel_teardown_for_removal() - a fast, lock-free size check makes this effectively free when
// nothing is queued, which is true for the overwhelming majority of passes on any instance that
// isn't actively being live-edited.
void reclaim_pending_channel_frees();

// Test-only hooks (live_reconfig.cpp): reclaim_grace_period_sec defaults to 30.0 in production and
// is never touched by production code - tests shrink it (e.g. to 0.0) so
// reclaim_pending_channel_frees() can be exercised deterministically without a real 30-second
// sleep. pending_channel_free_backlog() exposes the otherwise-encapsulated queue depth so a test
// can assert a teardown queued exactly what it expected, and that a reclaim pass actually drained
// it.
extern double reclaim_grace_period_sec;
size_t pending_channel_free_backlog();

// Connection-establishing half of init_output() (rtl_airband.cpp), redone for every output on an
// already-initialized channel (lame/lamebuf already allocated once at startup, not touched again
// here). Shared by channel_apply_enable() and mixer_enable() (mixer.cpp) - a mixer's own outputs
// are just outputs on mixer->channel, so the same per-output-type logic applies. Only safe to
// call from the output thread that owns the channel/mixer being reconnected, same as the
// apply-side functions above.
void reconnect_channel_outputs(channel_t* channel);

// Mixer enable/disable, same request/apply split and rationale as the channel functions above -
// see mixer_t::pending_enable_request's comment (rtl_airband.h). mixer_request_enable()/
// mixer_request_disable() are safe to call from the control socket thread; mixer_enable()/
// mixer_disable() (mixer.cpp) must only be called from within output_thread().
bool mixer_request_enable(mixer_t* mixer, int timeout_us = 500000);
bool mixer_request_disable(mixer_t* mixer, int timeout_us = 500000);

// A read-only snapshot of the fields reload_diff can act on - deliberately not a full mirror of
// parse_devices()/parse_channels()/parse_mixers(); it only captures what has a live-apply
// primitive (centerfreq, numeric gain, bandwidth, correction, channel/mixer enabled) plus what's
// needed to detect out-of-v1-scope changes (counts, type, mode, sample_rate). Parsing this never
// touches devices[]/mixers[] or any other live state - re-invoking the real parse_* functions against
// already-running state would corrupt it (they assume a pristine, XCALLOC'd array).
struct DeviceConfigSnapshot {
    std::string type;
    rec_modes mode;
    int channel_count;
    int centerfreq;
    int sample_rate;
    bool has_gain = false;  // false if "gain" is absent, or present as a non-numeric (per-element) form
    float gain = 0.0f;
    // Defaulted (unlike most of the scalar fields above) so existing hand-built DeviceConfigSnapshot
    // test fixtures that predate this field don't leave it as indeterminate garbage - a real
    // regression this exact addition hit: several pre-existing tests construct a bare
    // DeviceConfigSnapshot and only set the fields they care about.
    bool has_bandwidth = false;  // false if "bandwidth" is absent from the device's config block
    int bandwidth = 0;
    bool has_correction = false;  // false if "correction" is absent from the device's config block
    int correction = 0;
    bool has_auto_ppm = false;  // false if "auto_ppm" is absent
    bool auto_ppm = false;
    AutoPpmParams auto_ppm_params;
    std::string auto_ppm_params_error;  // non-empty when the auto_ppm_* values are out of range
    std::vector<bool> channel_enabled;  // size == channel_count
    // Per-channel canonical config signature (build_channel_identity_signature(), config.cpp),
    // built from the same raw Setting parse_channel() itself would see. compute_and_apply_diff()
    // compares this against each live channel's own channel_t::config_signature to find the
    // longest common prefix between what's live and what the file now says - channels past that
    // point are torn down (if live) and/or freshly parsed and appended (if in the file), which is
    // how an edit to an EXISTING channel's freq/modulation/bandwidth/squelch/notch/ctcss/outputs
    // gets picked up live, not just a pure count change - see that function's comment for why a
    // wrong guess about "is this the same channel" is worse here than on the append side (a wrong
    // guess there can only fail to add something new; here it could tear down and free the WRONG
    // channel's live audio feed).
    std::vector<std::string> channel_signature;  // size == channel_count

    // Together, let compute_and_apply_diff() locate a newly-appended channel's full raw
    // definition (freq/label/modulation/outputs/...) when channel_count grows, without
    // re-deriving the disable=true skip logic a second time: raw_channel_indices[k] is this
    // device's compacted channel k's position within *raw_channels_setting (same size/order as
    // channel_enabled). raw_channels_setting is NULL when this device has no raw "channels"
    // setting at all (channel_count is then 0 too). Only valid as long as the ConfigSnapshot's
    // own raw_config below is kept alive - these are raw pointers into that Config's tree.
    std::vector<int> raw_channel_indices;
    libconfig::Setting* raw_channels_setting = nullptr;
};

struct MixerConfigSnapshot {
    std::string name;
    bool enabled;
    // build_mixer_remote_inputs_signature() (config.cpp) of the freshly re-read config -
    // compared against the live mixer's own mixer_t::remote_inputs_signature in
    // compute_and_apply_diff(). Defaulted to the same "absent" marker build_mixer_remote_
    // inputs_signature() itself uses for a mixer with no remote_inputs block, so a hand-built
    // MixerConfigSnapshot test fixture that predates this field compares as unchanged against a
    // live mixer_t that also has no remote_inputs, rather than as indeterminate garbage.
    std::string remote_inputs_signature = "<absent>";
};

struct ConfigSnapshot {
    std::vector<DeviceConfigSnapshot> devices;
    std::vector<MixerConfigSnapshot> mixers;

    // Keeps the freshly re-read config file's parsed tree alive past parse_config_snapshot()
    // returning, since DeviceConfigSnapshot::raw_channels_setting points into it - libconfig
    // Setting references/pointers are only valid while their owning Config is alive. Everything
    // else in this struct is copied out by value and doesn't need this; only the live
    // channel-append path in compute_and_apply_diff() dereferences it.
    std::shared_ptr<libconfig::Config> raw_config;
};

// Re-reads cfgfile from disk into a snapshot. Returns false and sets *error on a file/parse
// error (mirrors the read-only nature of this path - never calls error()/exit() the way the
// startup parser does).
bool parse_config_snapshot(const std::string& config_path, ConfigSnapshot* out, std::string* error);

// Diffs snapshot against the live devices[]/mixers[] and applies whatever v1 supports through the
// same primitives a single control-socket command would use (device_request_retune/
// input_set_gain/channel_request_enable/channel_request_disable/mixer_request_enable/
// mixer_request_disable). Anything outside v1 scope (device/channel/mixer count changes,
// sample_rate, type, mode) is reported in skipped_requires_restart and never attempted. Runs on
// the control socket thread, so - like the single-item commands - only ever posts requests, never
// calls the apply-side functions directly.
struct DiffResult {
    std::vector<std::string> applied;
    std::vector<std::string> skipped_requires_restart;
};
DiffResult compute_and_apply_diff(const ConfigSnapshot& snapshot);

#endif /* _LIVE_RECONFIG_H */
