/*
 * live_reconfig.cpp
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

#include "live_reconfig.h"
#include <unistd.h>  // usleep
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iostream>
#include <libconfig.h++>
#include <sstream>
#include "input-common.h"
#include "logging.h"  // ConfigApplyError, config_error_is_recoverable
#include "rtl_airband.h"

using namespace std;

size_t compute_channel_bin(int channel_freq, int centerfreq, int sample_rate, size_t num_fft_bins) {
    return (size_t)ceil((channel_freq + sample_rate - centerfreq) / (double)(sample_rate / num_fft_bins) - 1.0) % num_fft_bins;
}

uint32_t compute_channel_dm_dphi(int channel_freq, int centerfreq, int sample_rate) {
    double dm_dphi = (double)(channel_freq - centerfreq);  // downmix freq in Hz

    // See config.cpp's original derivation of this formula for the full explanation - this must
    // stay bit-for-bit identical to the parse-time computation so a live retune produces exactly
    // the same result a restart with the new centerfreq would have.
    double decimation_factor = (double)sample_rate / (double)WAVE_RATE;
    double dm_dphi_correction = (double)WAVE_RATE / 2.0;
    dm_dphi_correction *= (decimation_factor - round(decimation_factor));
    dm_dphi_correction *= (double)(channel_freq - centerfreq) / ((double)sample_rate / 2.0);

    dm_dphi -= dm_dphi_correction;
    dm_dphi /= (double)WAVE_RATE;
    dm_dphi -= trunc(dm_dphi);
    dm_dphi *= 256.0 * 65536.0;
    return (uint32_t)((int)dm_dphi);
}

size_t compute_input_buf_size(int sample_rate, int bytes_per_sample) {
    // For the input buffer size use a base value and round it up to the nearest multiple of
    // FFT_BATCH blocks of input samples. ceil is required here because sample rate is not
    // guaranteed to be an integer multiple of WAVE_RATE. Must stay bit-for-bit identical to what
    // config.cpp's startup sizing computes - see this function's declaration comment
    // (live_reconfig.h) for why.
    size_t fft_batch_len = FFT_BATCH * (2 * (size_t)bytes_per_sample * (size_t)ceil((double)sample_rate / (double)WAVE_RATE));
    size_t buf_size = MIN_BUF_SIZE;
    if (buf_size % fft_batch_len != 0)
        buf_size += fft_batch_len - buf_size % fft_batch_len;
    return buf_size;
}

bool device_request_retune(device_t* dev, int new_centerfreq) {
    if (dev->mode != R_MULTICHANNEL) {
        // R_SCAN devices retune via their own controller_thread's fixed-offset scheme; accepting
        // a manual retune here would fight it and break the offset invariant it relies on.
        return false;
    }
    dev->pending_centerfreq_request.store(new_centerfreq, std::memory_order_release);
    return true;
}

bool device_confirm_retune(device_t* dev, int timeout_us, bool* timed_out) {
    // Not consumed via exchange(): a caller polling pending_centerfreq_request must not observe
    // "consumed" until centerfreq_apply_failed already reflects this attempt's actual result - see
    // both fields' comments (rtl_airband.h) and device_apply_retune()'s TOCTOU-avoiding
    // request/apply split (rtl_airband.cpp's demod loop).
    const int poll_interval_us = 5000;
    int waited_us = 0;
    while (dev->pending_centerfreq_request.load(std::memory_order_acquire) != -1 && waited_us < timeout_us) {
        usleep(poll_interval_us);
        waited_us += poll_interval_us;
    }
    if (dev->pending_centerfreq_request.load(std::memory_order_acquire) != -1) {
        *timed_out = true;
        return false;
    }
    *timed_out = false;
    return !dev->centerfreq_apply_failed.load(std::memory_order_acquire);
}

void reconnect_channel_outputs(channel_t* channel) {
    for (int k = 0; k < channel->output_count; k++) {
        output_t* output = channel->outputs + k;
        if (output->type == O_ICECAST) {
            icecast_data* icecast = (icecast_data*)(output->data);
            if (icecast->shout == NULL) {
                shout_setup(icecast, channel->mode);
            }
        } else if (output->type == O_UDP_STREAM) {
            udp_stream_data* sdata = (udp_stream_data*)(output->data);
            udp_stream_init(sdata, channel->mode, (size_t)WAVE_BATCH);
        } else if (output->type == O_MIXER) {
            mixer_data* mdata = (mixer_data*)(output->data);
            mixer_enable_input(mdata->mixer, mdata->input);
            // File/rawfile outputs need no action here - output_file_ready() (output.cpp)
            // lazily reopens on the next signal, same as it already does for a fresh channel.
#ifdef WITH_PULSEAUDIO
        } else if (output->type == O_PULSE) {
            pulse_data* pdata = (pulse_data*)(output->data);
            if (pdata->context == NULL) {
                pulse_init();
                pulse_setup(pdata, channel->mode);
            }
#endif /* WITH_PULSEAUDIO */
        }
        output->enabled = true;
    }
}

void channel_apply_enable(channel_t* channel) {
    channel->enabled = true;
    reconnect_channel_outputs(channel);
}

void channel_apply_disable(channel_t* channel) {
    channel->enabled = false;
    disable_channel_outputs(channel);
}

namespace {

// Mirrors, in reverse, every heap allocation parse_outputs() (config.cpp) makes for this output
// type - see reclaim_pending_channel_frees()'s comment below for why this only ever runs well
// after the output is provably no longer in use, never inline during teardown. output->lame/
// lamebuf are NOT this function's concern - channel_teardown_for_removal() already frees those
// immediately, since output_check_thread()/write_stats_file() never touch them.
void free_output_data(output_t* output) {
    switch (output->type) {
        case O_ICECAST: {
            // XCALLOC'd (config.cpp) - every string field is strdup()'d individually, plain
            // free() is correct. shout/shout_free() already ran in disable_channel_outputs().
            icecast_data* d = (icecast_data*)output->data;
            free(const_cast<char*>(d->hostname));
            free(const_cast<char*>(d->username));
            free(const_cast<char*>(d->password));
            free(const_cast<char*>(d->mountpoint));
            free(const_cast<char*>(d->name));
            free(const_cast<char*>(d->genre));
            free(const_cast<char*>(d->description));
            free(d);
            break;
        }
        case O_FILE:
        case O_RAWFILE: {
            // `new file_data()` (config.cpp) - std::string members free themselves via the
            // destructor delete runs; only the nested `new rdio_scanner_data()` needs its own
            // explicit delete first. close_file() (output.cpp) already closed/nulled `f`.
            file_data* d = (file_data*)output->data;
#ifdef WITH_RDIO_SCANNER
            delete d->rdio_scanner;
#endif /* WITH_RDIO_SCANNER */
            delete d;
            break;
        }
        case O_UDP_STREAM: {
            // XCALLOC'd (config.cpp); dest_address/dest_port strdup'd there, the four
            // conversion/resample buffers XCALLOC'd lazily by udp_stream_init() (possibly NULL if
            // init was never reached) - free() is safe on NULL. udp_stream_shutdown() (already run
            // by disable_channel_outputs()) only closes the socket, not these buffers.
            udp_stream_data* d = (udp_stream_data*)output->data;
            free(const_cast<char*>(d->dest_address));
            free(const_cast<char*>(d->dest_port));
            free(d->convert_buffer);
            free(d->resample_buffer);
            free(d->resample_left_buffer);
            free(d->resample_right_buffer);
            free(d->stereo_buffer);
            free(d);
            break;
        }
#ifdef WITH_PULSEAUDIO
        case O_PULSE: {
            // XCALLOC'd (config.cpp), strdup'd string fields - pulse_shutdown() (already run by
            // disable_channel_outputs()) tears down the PulseAudio-internal context/streams, not
            // these.
            pulse_data* d = (pulse_data*)output->data;
            free(const_cast<char*>(d->server));
            free(const_cast<char*>(d->name));
            free(const_cast<char*>(d->sink));
            free(const_cast<char*>(d->stream_name));
            free(d);
            break;
        }
#endif /* WITH_PULSEAUDIO */
        case O_MIXER_REMOTE: {
            // XCALLOC'd (config.cpp); dest_path strdup'd there. send_buf is already freed and
            // NULL'd by mixer_remote_send_shutdown() (disable_channel_outputs(), which always
            // runs before this) - nothing left to do for it here, only the strdup'd path.
            mixer_remote_send_data* d = (mixer_remote_send_data*)output->data;
            free(const_cast<char*>(d->dest_path));
            free(d);
            break;
        }
        case O_MIXER:
        default:
            // mixer_data::mixer points at the process-wide mixers[] array - not owned here.
            // XCALLOC'd, no other owned pointers.
            free(output->data);
            break;
    }
}

struct PendingChannelFree {
    output_t* outputs;
    int output_count;
    freq_t* freqlist;
    int freq_count;
    char* config_signature;
    struct timeval retired_at;
};

pthread_mutex_t pending_channel_free_mutex = PTHREAD_MUTEX_INITIALIZER;
std::vector<PendingChannelFree> pending_channel_frees;
// Lock-free fast-path check for reclaim_pending_channel_frees(), called once per output_thread()
// pass - relaxed is enough since it's only ever used to decide whether to take the mutex at all;
// the mutex itself (taken on both the rare push side and the drain side) is what actually
// synchronizes access to the vector.
std::atomic<size_t> pending_channel_free_count{0};

}  // namespace

// How long a torn-down channel's outputs/freqlist/config_signature sit in pending_channel_frees
// before reclaim_pending_channel_frees() actually frees them. Must comfortably exceed the widest
// window any reader could still be using a captured pointer from before teardown -
// write_stats_file() (src/output.cpp) is the riskiest unsynchronized reader of this memory (it has
// no `enabled` gate at all, unlike the demod thread's and output_check_thread's per-channel loops).
// What matters is how long one in-flight call can hold a stale pointer, not how often it runs
// (stats_update_interval): that function runs to completion in well under a second, so any call
// already in flight when a channel is torn down is long finished by the time this elapses. A plain
// externally-linked global rather than a file-local const, same rationale as
// mixer_capacity_finalized (rtl_airband.h): unit tests need to shrink this to something that
// doesn't make the test suite take 30+ seconds per case; production code never touches it.
double reclaim_grace_period_sec = 30.0;

size_t pending_channel_free_backlog() {
    return pending_channel_free_count.load(std::memory_order_relaxed);
}

// See channel_t::pending_remove_request's comment (rtl_airband.h) and channel_request_remove()'s
// declaration (live_reconfig.h) for the full rationale of what this does and does not free
// immediately vs. defer. `enabled = false` is set first (before anything is touched or freed) for
// the same reason channel_apply_disable() sets it first: output_check_thread() (src/output.cpp)
// gates its own per-channel output access on this flag - setting it first is what keeps that
// thread from reaching in while this teardown runs, same protection the pre-existing disable
// feature already relies on.
void channel_teardown_for_removal(channel_t* channel) {
    channel->enabled = false;
    disable_channel_outputs(channel, /*permanent=*/true);
    for (int k = 0; k < channel->output_count; k++) {
        output_t* output = channel->outputs + k;
        if (output->lame) {
            lame_close(output->lame);
            output->lame = NULL;
        }
        if (output->lamebuf) {
            free(output->lamebuf);
            output->lamebuf = NULL;
        }
    }
    // Queued, not freed here - see reclaim_grace_period_sec's comment above. channel->outputs/
    // freqlist/config_signature are deliberately left pointing at this (still-valid, not-yet-freed)
    // memory: if this slot gets reused by a later parse_channel() call (a live edit, not just a
    // removal), those fields are simply overwritten with fresh pointers - the old ones are already
    // safely captured here and will be freed on schedule regardless of what happens to the channel
    // struct itself afterward.
    PendingChannelFree pending;
    pending.outputs = channel->outputs;
    pending.output_count = channel->output_count;
    pending.freqlist = channel->freqlist;
    pending.freq_count = channel->freq_count;
    pending.config_signature = channel->config_signature;
    gettimeofday(&pending.retired_at, NULL);
    pthread_mutex_lock(&pending_channel_free_mutex);
    pending_channel_frees.push_back(pending);
    pending_channel_free_count.store(pending_channel_frees.size(), std::memory_order_relaxed);
    pthread_mutex_unlock(&pending_channel_free_mutex);

    // Permanent - see channel_t::removed's comment (rtl_airband.h) for why this must never be
    // reset except by parse_channel() populating a fresh channel into this slot.
    channel->removed.store(true, std::memory_order_release);
}

void reclaim_pending_channel_frees() {
    if (pending_channel_free_count.load(std::memory_order_relaxed) == 0) {
        return;
    }

    struct timeval now;
    gettimeofday(&now, NULL);

    std::vector<PendingChannelFree> ready;
    pthread_mutex_lock(&pending_channel_free_mutex);
    auto it = pending_channel_frees.begin();
    while (it != pending_channel_frees.end()) {
        if (delta_sec(&it->retired_at, &now) >= reclaim_grace_period_sec) {
            ready.push_back(*it);
            it = pending_channel_frees.erase(it);
        } else {
            ++it;
        }
    }
    pending_channel_free_count.store(pending_channel_frees.size(), std::memory_order_relaxed);
    pthread_mutex_unlock(&pending_channel_free_mutex);

    for (PendingChannelFree& pending : ready) {
        for (int k = 0; k < pending.output_count; k++) {
            free_output_data(pending.outputs + k);
        }
        free(pending.outputs);
        // Only ever queued by channel_teardown_for_removal(), which output_thread() only ever
        // calls on a real device channel (dev->channels[j], never mixer->channel) - freqlist is
        // always mk_freqlist()-allocated (config.cpp) and non-NULL here by construction.
        for (int k = 0; k < pending.freq_count; k++) {
            free(pending.freqlist[k].label);
        }
        free(pending.freqlist);
        free(pending.config_signature);
    }
}

namespace {

// Shared by every request-side function below: posts value into *request, then polls until the
// owning thread consumes it (resets to -1) or timeout_us elapses. Returns false on timeout -
// callers treat that as "request accepted but not confirmed applied," not a hard failure, since
// the request is still pending and will be applied on the owning thread's next pass.
bool post_request_and_wait(std::atomic<int>* request, int value, int timeout_us) {
    request->store(value, std::memory_order_release);
    const int poll_interval_us = 2000;
    int waited_us = 0;
    while (request->load(std::memory_order_acquire) != -1 && waited_us < timeout_us) {
        usleep(poll_interval_us);
        waited_us += poll_interval_us;
    }
    return request->load(std::memory_order_acquire) == -1;
}

}  // namespace

bool channel_request_enable(channel_t* channel, int timeout_us) {
    return post_request_and_wait(&channel->pending_enable_request, 1, timeout_us);
}

bool channel_request_disable(channel_t* channel, int timeout_us) {
    return post_request_and_wait(&channel->pending_enable_request, 0, timeout_us);
}

bool channel_request_remove(channel_t* channel, int timeout_us) {
    return post_request_and_wait(&channel->pending_remove_request, 1, timeout_us);
}

bool mixer_request_enable(mixer_t* mixer, int timeout_us) {
    return post_request_and_wait(&mixer->pending_enable_request, 1, timeout_us);
}

bool mixer_request_disable(mixer_t* mixer, int timeout_us) {
    return post_request_and_wait(&mixer->pending_enable_request, 0, timeout_us);
}

namespace {

// Recomputes every channel's bins/base_bins/dm_dphi in place for the given (centerfreq,
// sample_rate) pair - shared by device_apply_retune() (only centerfreq varies) and
// device_apply_sample_rate() (only sample_rate varies) below. Runs inside the demod thread that
// exclusively owns dev - same thread as AFC's own per-bin adjustments (see the AFC class in
// rtl_airband.cpp), so plain assignments here are safe without any lock: there is only ever one
// writer to bins/base_bins/dm_dphi at a time. Callers are responsible for only invoking this
// after confirming the hardware actually reached (centerfreq, sample_rate) - recomputing bins for
// a value the hardware never reached would leave the demod math and the actual RF/sampling state
// disagreeing (see device_apply_retune()'s original ordering-discipline comment, preserved below).
void recompute_channel_bins(device_t* dev, int centerfreq, int sample_rate) {
    for (int j = 0; j < dev->channel_count; j++) {
        channel_t* channel = dev->channels + j;
        int channel_freq = channel->freqlist[0].frequency;
        dev->base_bins[j] = dev->bins[j] = compute_channel_bin(channel_freq, centerfreq, sample_rate, fft_size);
        if (channel->needs_raw_iq) {
            channel->dm_dphi = compute_channel_dm_dphi(channel_freq, centerfreq, sample_rate);
            channel->dm_phi = 0;
        }
    }
}

}  // namespace

bool device_apply_retune(device_t* dev, int new_centerfreq) {
    // Hardware retune is attempted FIRST, before touching bins/base_bins/dm_dphi: on failure the
    // radio is still physically on the old centerfreq (input_set_centerfreq() only updates
    // input->centerfreq on success), so recomputing bins for new_centerfreq first would leave the
    // demod math and the actual RF tuning disagreeing - every channel on this device would
    // demodulate the wrong bin offset relative to what the hardware is really receiving, until the
    // next successful retune. Bins are only ever recomputed for a frequency the hardware actually
    // reached.
    if (input_set_centerfreq(dev->input, new_centerfreq) != 0) {
        return false;
    }
    recompute_channel_bins(dev, new_centerfreq, dev->input->sample_rate);
    return true;
}

bool device_request_sample_rate(device_t* dev, int new_sample_rate) {
    if (dev->mode != R_MULTICHANNEL) {
        // Same rationale as device_request_retune() above - R_SCAN devices retune via their own
        // controller_thread's fixed-offset scheme, and R_SCAN live reconfiguration is out of
        // scope entirely.
        return false;
    }
    if (dev->input->stop == NULL) {
        // Structurally can't pause/reopen this driver's device - see device_apply_sample_rate()'s
        // comment (live_reconfig.h) for why that's required.
        return false;
    }
    dev->pending_sample_rate_request.store(new_sample_rate, std::memory_order_release);
    return true;
}

bool device_confirm_sample_rate(device_t* dev, int timeout_us, bool* timed_out) {
    // Mirrors device_confirm_retune() exactly - see both functions' declaration comments
    // (live_reconfig.h) for the shared "publish result before clearing the request" rationale.
    const int poll_interval_us = 5000;
    int waited_us = 0;
    while (dev->pending_sample_rate_request.load(std::memory_order_acquire) != -1 && waited_us < timeout_us) {
        usleep(poll_interval_us);
        waited_us += poll_interval_us;
    }
    if (dev->pending_sample_rate_request.load(std::memory_order_acquire) != -1) {
        *timed_out = true;
        return false;
    }
    *timed_out = false;
    return !dev->sample_rate_apply_failed.load(std::memory_order_acquire);
}

namespace {

// Wraps dev->input->init() (e.g. rtlsdr_init()) so a startup-only fatal error() call becomes a
// catchable failure instead of killing the whole process - the same mechanism try_append_channels()
// above already established for parse_channel()'s error() calls (config_error_is_recoverable,
// src/logging.cpp). Deliberately does NOT call the generic input_init() wrapper
// (src/input-common.cpp): that function unconditionally re-runs pthread_mutex_init() on
// buffer_lock (undefined behavior on an already-live mutex) and unconditionally overwrites
// input->state, neither of which is correct for reopening an already-running device - both are
// left to device_apply_sample_rate() below.
bool device_reopen_recoverable(device_t* dev, string* error_text) {
    config_error_is_recoverable = true;
    ostringstream captured;
    std::streambuf* old_cerr_buf = std::cerr.rdbuf(captured.rdbuf());
    bool ok = true;
    string exception_text;
    try {
        if (dev->input->init(dev->input) < 0) {
            ok = false;
        }
    } catch (const std::exception& e) {
        // Catches ConfigApplyError (what recoverable-mode error() throws) - unlike
        // try_append_channels()'s use of this same pattern, a driver's init() hook doesn't walk a
        // libconfig::Setting tree, so a raw libconfig::ConfigException isn't expected here, but
        // catching std::exception broadly costs nothing and matches the established precedent.
        ok = false;
        exception_text = e.what();
    }
    std::cerr.rdbuf(old_cerr_buf);
    config_error_is_recoverable = false;
    if (!ok) {
        string captured_text = captured.str();
        *error_text = captured_text.empty() ? exception_text : captured_text;
        return false;
    }
    return true;
}

}  // namespace

bool device_apply_sample_rate(device_t* dev, int new_sample_rate) {
    input_t* input = dev->input;
    // Held across the whole stop/reopen: the driver's dev_data->dev handle is closed and replaced
    // in here, so no other thread may issue a control call against it meanwhile.
    std::lock_guard<std::recursive_mutex> control(input_control_mutex());
    int old_sample_rate = input->sample_rate;

    // Compute and allocate the new buffer FIRST, before touching the RX thread at all - pure
    // computation (XCALLOC either succeeds or the process is out of memory, an existing fatal
    // condition throughout this codebase via xrealloc()'s own convention, not a new failure mode
    // to handle here), and keeping the old buffer untouched until the new rate is confirmed
    // working is what makes rollback cheap: on failure there's nothing to reallocate, just reopen
    // and restart against the buffer that's already there.
    size_t new_buf_size = compute_input_buf_size(new_sample_rate, input->bytes_per_sample);
    unsigned char* new_buffer = (unsigned char*)XCALLOC(sizeof(unsigned char), new_buf_size + 2 * (size_t)input->bytes_per_sample * fft_size);

    // Stop the RX thread directly via the driver hook (NOT input_stop() - that sets
    // input->state = INPUT_STOPPED and is written as a one-shot, process-exit-only call, see its
    // comment, src/input-common.cpp) and join it ourselves.
    if (input->stop(input) != 0) {
        // The driver couldn't cleanly stop - e.g. a failed cancel of the in-flight async read. The
        // device is presumably still fine at the old rate (nothing was actually torn down), so
        // this is a rejected request, not a device failure. Deliberately does NOT pthread_join()
        // here: if the driver's stop() failed to actually cancel the blocking read, joining could
        // hang forever waiting for a thread that will never exit on its own.
        free(new_buffer);
        log(LOG_ERR, "Device: failed to stop RX thread for sample_rate change, request rejected\n");
        return false;
    }
    pthread_join(input->rx_thread, NULL);

    input->sample_rate = new_sample_rate;
    string error_text;
    bool reopened = device_reopen_recoverable(dev, &error_text);

    if (reopened) {
        free(input->buffer);
        input->buffer = new_buffer;
        input->buf_size = new_buf_size;
        input->bufs = input->bufe = 0;
        pthread_create(&input->rx_thread, NULL, input->run_rx_thread, input);
        recompute_channel_bins(dev, input->centerfreq, new_sample_rate);
        input->tuning_generation.fetch_add(1, std::memory_order_release);
        return true;
    }

    log(LOG_ERR, "Failed to apply sample_rate %d: %s - rolling back to %d\n", new_sample_rate, error_text.c_str(), old_sample_rate);
    input->sample_rate = old_sample_rate;
    string rollback_error_text;
    bool rolled_back = device_reopen_recoverable(dev, &rollback_error_text);

    free(new_buffer);  // never adopted, whether rollback succeeded or not - old buffer is untouched

    if (rolled_back) {
        pthread_create(&input->rx_thread, NULL, input->run_rx_thread, input);
        input->tuning_generation.fetch_add(1, std::memory_order_release);
        // bins/dm_dphi are already correct for old_sample_rate - nothing about this device's
        // tuning actually changed, so no recompute is needed.
        return false;
    }

    log(LOG_ERR, "Rollback to previous sample_rate %d also failed: %s - device is down\n", old_sample_rate, rollback_error_text.c_str());
    input->state = INPUT_FAILED;
    return false;
}

namespace {

// Mirrors config.cpp's static parse_anynum2int() exactly (int Hz, float treated as MHz*1e6, or a
// suffixed string via atofs()) - duplicated rather than exported because that function is
// deliberately internal to the startup parser; this is the read-only diff path's own copy.
int snapshot_parse_anynum2int(libconfig::Setting& f) {
    if (f.getType() == libconfig::Setting::TypeInt) {
        return (int)f;
    } else if (f.getType() == libconfig::Setting::TypeFloat) {
        return (int)((double)f * 1e6);
    } else if (f.getType() == libconfig::Setting::TypeString) {
        char* s = strdup((char const*)f);
        int ret = (int)atofs(s);
        free(s);
        return ret;
    }
    return 0;
}

// Only numeric "gain" (plain dB, int or float) is diffable against a single live gain value -
// SoapySDR's per-element string form ("key=value,...") has no single number to compare or apply
// via input_set_gain(), so it's left out of the snapshot entirely (never reported as a diff).
bool snapshot_get_numeric_gain(libconfig::Setting& dev_setting, float* gain) {
    if (!dev_setting.exists("gain")) {
        return false;
    }
    libconfig::Setting& g = dev_setting["gain"];
    if (g.getType() == libconfig::Setting::TypeInt) {
        *gain = (float)(int)g;
        return true;
    } else if (g.getType() == libconfig::Setting::TypeFloat) {
        *gain = (float)(double)g;
        return true;
    }
    return false;
}

// Attempts to parse and append snap.channel_count - dev->channel_count new channels (the caller
// has already confirmed this fits within dev->channel_capacity) from their raw config Settings.
// All-or-nothing: on any single new channel's parse failure, nothing is published - dev's
// channel_count is only ever advanced on full success, and until then nothing at index >=
// dev->channel_count is visible to any other thread (demod/output), so there's nothing to unwind
// there. A channel that individually parsed fine earlier in a batch that later fails is simply
// leaked (its heap allocations - strdup'd labels, XCALLOC'd outputs - are never freed) rather
// than unwound: this is a rare, operator-triggered path (editing a config file and retrying), so
// a few hundred bytes of leak on a failed attempt is an acceptable tradeoff against a real
// rollback's complexity. On failure, *error_text carries the best available diagnostic text.
bool try_append_channels(device_t* dev, int dev_idx, const DeviceConfigSnapshot& snap, string* error_text) {
    int old_count = dev->channel_count;
    int new_count = old_count;

    config_error_is_recoverable = true;
    ostringstream captured;
    std::streambuf* old_cerr_buf = std::cerr.rdbuf(captured.rdbuf());
    bool ok = true;
    string exception_text;
    try {
        for (int k = old_count; k < snap.channel_count; k++) {
            libconfig::Setting& chan_setting = (*snap.raw_channels_setting)[snap.raw_channel_indices[k]];
            channel_t* channel = dev->channels + k;
            if (!parse_channel(chan_setting, dev, dev_idx, k, channel)) {
                // parse_channel() returning false here is the same pre-existing legacy-value
                // quirk documented on parse_channel() itself (config.cpp) - this one channel is
                // silently not counted, matching startup behavior for the same input. Nothing to
                // connect for it.
                continue;
            }
            // Mirrors main()'s startup call to init_output() for every output right after
            // parse_devices() returns (rtl_airband.cpp): a freshly parsed channel_t's output_t
            // structs are allocated by parse_outputs() (inside parse_channel() above), but none
            // of them have a LAME encoder or an open connection until this runs - unlike
            // re-enabling an already-live channel (channel_apply_enable() -> reconnect_channel_
            // outputs()), which can assume that part already happened once at startup.
            for (int o = 0; o < channel->output_count; o++) {
                if (!init_output(channel, channel->outputs + o)) {
                    throw std::runtime_error("failed to initialize output " + to_string(o) + " for appended channel " + to_string(k));
                }
            }
            new_count = k + 1;
        }
    } catch (const std::exception& e) {
        // Catches both ConfigApplyError (what recoverable-mode error() throws - config.cpp's
        // call sites print a human-readable message to cerr immediately before calling error(),
        // captured above) and any raw libconfig::ConfigException (e.g. SettingNotFoundException
        // from indexing a required-but-missing key like chan_setting["outputs"] - config.cpp's
        // parser assumes many required keys exist rather than calling error() for every one of
        // them, exactly as a startup config missing the same key would already fail today).
        // Either way the process must survive; e.what() is the fallback when nothing was ever
        // written to cerr on this particular failure path.
        ok = false;
        exception_text = e.what();
    }
    std::cerr.rdbuf(old_cerr_buf);
    config_error_is_recoverable = false;

    if (!ok) {
        string captured_text = captured.str();
        *error_text = captured_text.empty() ? exception_text : captured_text;
        return false;
    }
    dev->channel_count.store(new_count, std::memory_order_release);
    return true;
}

// Attempts to remove dev->channel_count - target_count channels from the tail (the caller has
// already confirmed the surviving prefix [0, target_count) matches - see compute_and_apply_diff()
// 's replace/decrease handling). target_count is an explicit parameter rather than derived from
// a DeviceConfigSnapshot because a tail *replace* (not just a pure decrease) needs to remove down
// to the common-prefix length, which can be less than the snapshot's own final channel_count when
// channels are also being appended afterward in the same call. Unlike try_append_channels(), this
// is NOT all-or-nothing: dev->channel_count is decremented by one immediately after each
// individual channel's teardown is confirmed complete, so a mid-batch failure
// (channel_request_remove() timing out - e.g. the output thread is stuck or unusually slow)
// leaves dev->channel_count at a valid, still-tail-consistent smaller value rather than either an
// inconsistent one or losing an already-confirmed removal. Returns the number of channels
// actually removed; on a partial result (< old dev->channel_count - target_count), *error_text
// explains why the remainder was left in place.
int try_remove_channels(device_t* dev, int target_count, string* error_text) {
    int removed = 0;
    for (int k = dev->channel_count - 1; k >= target_count; k--) {
        channel_t* channel = dev->channels + k;
        if (!channel_request_remove(channel)) {
            *error_text = "channel " + to_string(k) + " removal request timed out waiting for the output thread";
            return removed;
        }
        dev->channel_count.store(k, std::memory_order_release);
        removed++;
    }
    return removed;
}

}  // namespace

bool parse_config_snapshot(const string& config_path, ConfigSnapshot* out, string* error) {
    out->raw_config = std::make_shared<libconfig::Config>();
    libconfig::Config& cfg = *out->raw_config;
    try {
        cfg.readFile(config_path.c_str());
    } catch (const libconfig::FileIOException&) {
        *error = "could not read config file '" + config_path + "'";
        return false;
    } catch (const libconfig::ParseException& pe) {
        *error = string("parse error at ") + pe.getFile() + ":" + to_string(pe.getLine()) + ": " + pe.getError();
        return false;
    }
    libconfig::Setting& root = cfg.getRoot();

    out->mixers.clear();
    if (root.exists("mixers")) {
        libconfig::Setting& mx = root["mixers"];
        for (int i = 0; i < mx.getLength(); i++) {
            if (mx[i].exists("disable") && (bool)mx[i]["disable"] == true) {
                continue;
            }
            MixerConfigSnapshot m;
            const char* name = mx[i].getName();
            m.name = name ? name : "";
            m.enabled = mx[i].exists("enabled") ? (bool)mx[i]["enabled"] : true;
            m.remote_inputs_signature = build_mixer_remote_inputs_signature(mx[i]);
            out->mixers.push_back(m);
        }
    }

    out->devices.clear();
    if (!root.exists("devices")) {
        *error = "config has no 'devices' section";
        return false;
    }
    libconfig::Setting& devs = root["devices"];
    for (int i = 0; i < devs.getLength(); i++) {
        if (devs[i].exists("disable") && (bool)devs[i]["disable"] == true) {
            continue;
        }
        DeviceConfigSnapshot dev;
        dev.type = devs[i].exists("type") ? (const char*)devs[i]["type"] : "rtlsdr";
        if (devs[i].exists("mode") && strncmp((const char*)devs[i]["mode"], "scan", 4) == 0) {
            dev.mode = R_SCAN;
        } else {
            dev.mode = R_MULTICHANNEL;
        }
        dev.sample_rate = devs[i].exists("sample_rate") ? snapshot_parse_anynum2int(devs[i]["sample_rate"]) : 0;
        dev.centerfreq = (dev.mode == R_MULTICHANNEL && devs[i].exists("centerfreq")) ? snapshot_parse_anynum2int(devs[i]["centerfreq"]) : 0;
        dev.has_gain = snapshot_get_numeric_gain(devs[i], &dev.gain);
        dev.has_bandwidth = devs[i].exists("bandwidth");
        dev.bandwidth = dev.has_bandwidth ? snapshot_parse_anynum2int(devs[i]["bandwidth"]) : 0;
        dev.has_correction = devs[i].exists("correction");
        dev.correction = dev.has_correction ? snapshot_parse_anynum2int(devs[i]["correction"]) : 0;
        dev.has_auto_ppm = devs[i].exists("auto_ppm");
        dev.auto_ppm = dev.has_auto_ppm && (bool)devs[i]["auto_ppm"];
        auto_ppm_parse_params(devs[i], &dev.auto_ppm_params, &dev.auto_ppm_params_error);

        dev.channel_enabled.clear();
        dev.channel_signature.clear();
        dev.raw_channel_indices.clear();
        dev.raw_channels_setting = nullptr;
        if (devs[i].exists("channels")) {
            libconfig::Setting& chans = devs[i]["channels"];
            dev.raw_channels_setting = &chans;
            for (int j = 0; j < chans.getLength(); j++) {
                if (chans[j].exists("disable") && (bool)chans[j]["disable"] == true) {
                    continue;
                }
                dev.channel_enabled.push_back(chans[j].exists("enabled") ? (bool)chans[j]["enabled"] : true);
                dev.channel_signature.push_back(build_channel_identity_signature(chans[j]));
                dev.raw_channel_indices.push_back(j);
            }
        }
        dev.channel_count = (int)dev.channel_enabled.size();

        out->devices.push_back(dev);
    }
    return true;
}

DiffResult compute_and_apply_diff(const ConfigSnapshot& snapshot) {
    DiffResult result;

    if ((int)snapshot.devices.size() != device_count) {
        result.skipped_requires_restart.push_back("device count changed (" + to_string(device_count) + " -> " + to_string(snapshot.devices.size()) + ")");
    } else {
        for (int i = 0; i < device_count; i++) {
            device_t* dev = devices + i;
            const DeviceConfigSnapshot& snap = snapshot.devices[i];
            string label = "device[" + to_string(i) + "]";

            if (dev->input->driver_type != nullptr && snap.type != string(dev->input->driver_type)) {
                result.skipped_requires_restart.push_back(label + ": driver type changed");
                continue;  // nothing else about this device is safely comparable
            }
            if (snap.mode != dev->mode) {
                result.skipped_requires_restart.push_back(label + ": mode changed");
                continue;
            }
            // sample_rate is live-appliable now (device_apply_sample_rate() below) - see that
            // block, placed after the centerfreq diff further down, for the actual diff/apply
            // logic. No longer an automatic skipped_requires_restart entry.

            // Longest common prefix, by raw config signature (build_channel_identity_signature(),
            // config.cpp), between what's live and what the file now says - see DeviceConfig
            // Snapshot::channel_signature's comment. Channels from L onward differ in SOME way
            // (a pure count change, a reordering, or an in-place field edit - freq/modulation/
            // bandwidth/squelch/notch/ctcss/outputs - all look the same at this level: "the
            // channel that used to be at this index isn't there anymore").
            int compare_limit = min((int)dev->channel_count, snap.channel_count);
            int common_prefix = 0;
            while (common_prefix < compare_limit && !dev->channels[common_prefix].removed.load(std::memory_order_acquire) && dev->channels[common_prefix].config_signature != nullptr &&
                   snap.channel_signature[common_prefix] == dev->channels[common_prefix].config_signature) {
                common_prefix++;
            }
            bool tail_diverges = common_prefix < dev->channel_count || common_prefix < snap.channel_count;

            if (tail_diverges && dev->mode != R_MULTICHANNEL) {
                // Structurally shouldn't happen for a real R_SCAN config (validated at parse time
                // to always have exactly one channel) - but kept as an explicit, cheap guard
                // matching the documented non-goal: R_SCAN's single channel holds a "freqs" list
                // and its own controller-thread fixed-offset retune scheme (see item 2's "Device
                // Modes"), neither of which this tail-replace mechanism (or item 27/29's add/
                // remove it's built from) has ever supported.
                result.skipped_requires_restart.push_back(label + ": channel count or definition changed (R_SCAN devices don't support live channel changes)");
            } else if (tail_diverges) {
                bool ok = true;
                if (common_prefix < dev->channel_count) {
                    int to_remove = dev->channel_count - common_prefix;
                    string apply_error;
                    int removed = try_remove_channels(dev, common_prefix, &apply_error);
                    if (removed > 0) {
                        result.applied.push_back(label + ": removed " + to_string(removed) + " channel(s) from the tail" + (common_prefix < snap.channel_count ? " (replacing)" : ""));
                    }
                    if (removed < to_remove) {
                        result.skipped_requires_restart.push_back(label + ": " + to_string(to_remove - removed) + " channel(s) could not be removed: " + apply_error);
                        ok = false;
                    }
                }
                if (ok && dev->channel_count < snap.channel_count) {
                    int to_add = snap.channel_count - dev->channel_count;
                    if (dev->channel_count + to_add > dev->channel_capacity) {
                        result.skipped_requires_restart.push_back(label + ": " + to_string(to_add) + " new/replacement channel(s) declared but only " +
                                                                  to_string(dev->channel_capacity - dev->channel_count) + " reserve_channels slot(s) available");
                    } else {
                        int added_from = dev->channel_count;
                        string apply_error;
                        if (try_append_channels(dev, i, snap, &apply_error)) {
                            result.applied.push_back(label + ": added " + to_string(dev->channel_count - added_from) + " channel(s) (index " + to_string(added_from) + "-" +
                                                     to_string(dev->channel_count - 1) + ")");
                        } else {
                            result.skipped_requires_restart.push_back(label + ": " + to_string(to_add) + " new/replacement channel(s) failed to parse and were not added: " + apply_error +
                                                                      " (fix the config and retry reload_diff - no restart needed)");
                        }
                    }
                }
            }

            // Untouched common-prefix channels can still have had just "enabled" toggled - the
            // one field intentionally excluded from the signature above, so it never forces a
            // tear-down/replace of its own. Runs regardless of whether a tail replace also
            // happened above.
            for (int j = 0; j < common_prefix; j++) {
                channel_t* channel = dev->channels + j;
                bool live_enabled = channel->enabled.load();
                if (snap.channel_enabled[j] != live_enabled) {
                    bool confirmed = snap.channel_enabled[j] ? channel_request_enable(channel) : channel_request_disable(channel);
                    result.applied.push_back(label + " channel[" + to_string(j) + "]: enabled -> " + (snap.channel_enabled[j] ? "true" : "false") +
                                             (confirmed ? "" : " (request posted, not yet confirmed applied)"));
                }
            }

            if (dev->mode == R_MULTICHANNEL && snap.centerfreq != 0 && snap.centerfreq != dev->input->centerfreq) {
                if (device_request_retune(dev, snap.centerfreq)) {
                    // Wait for the demod thread's real outcome instead of just reporting "request
                    // posted" as success - a failed hardware retune (e.g. a transient i2c error)
                    // otherwise looked identical to a successful one in this response, with no way
                    // for a caller to know the device was left on its old centerfreq.
                    bool timed_out = false;
                    bool ok = device_confirm_retune(dev, /*timeout_us=*/500000, &timed_out);
                    if (ok) {
                        result.applied.push_back(label + ": centerfreq -> " + to_string(snap.centerfreq));
                    } else if (timed_out) {
                        result.applied.push_back(label + ": centerfreq -> " + to_string(snap.centerfreq) + " (request posted, not yet confirmed applied)");
                    } else {
                        result.skipped_requires_restart.push_back(
                            label + ": centerfreq change failed (transient hardware error, device still on its previous centerfreq, see logs) - no restart needed, retry reload_diff");
                    }
                } else {
                    result.skipped_requires_restart.push_back(label + ": centerfreq changed but retune request was rejected");
                }
            }

            if (dev->mode == R_MULTICHANNEL && snap.sample_rate != 0 && snap.sample_rate != dev->input->sample_rate) {
                // Unlike gain/bandwidth/correction below, sample_rate DOES have a live-readable
                // current value (dev->input->sample_rate), so this is a real diff, not an
                // unconditional reapply - deliberately so, since this operation is expensive (RX
                // thread stop/reopen/restart), not a cheap idempotent call.
                if (device_request_sample_rate(dev, snap.sample_rate)) {
                    // Budget a longer timeout than a plain retune - the apply side includes a full
                    // RX-thread join and hardware device reopen, not just one API call. See
                    // device_confirm_sample_rate()'s declaration comment (live_reconfig.h).
                    bool timed_out = false;
                    bool ok = device_confirm_sample_rate(dev, /*timeout_us=*/3000000, &timed_out);
                    if (ok) {
                        result.applied.push_back(label + ": sample_rate -> " + to_string(snap.sample_rate));
                    } else if (timed_out) {
                        result.applied.push_back(label + ": sample_rate -> " + to_string(snap.sample_rate) + " (request posted, not yet confirmed applied)");
                    } else {
                        result.skipped_requires_restart.push_back(label +
                                                                  ": sample_rate change failed and was rolled back to the previous rate (or the device is now down - check logs and device health) - "
                                                                  "no restart needed if rolled back, retry reload_diff");
                    }
                } else {
                    result.skipped_requires_restart.push_back(label + ": sample_rate changed but the request was rejected (not R_MULTICHANNEL, or driver does not support live rate changes)");
                }
            }

            if (snap.has_gain) {
                // No live-readable "current gain" exists (each driver's dev_data is private to
                // its own .cpp), so this reapplies unconditionally rather than truly diffing -
                // harmless (idempotent), just not a precise "only report a real change" signal.
                errno = 0;
                if (input_set_gain(dev->input, snap.gain) == 0) {
                    result.applied.push_back(label + ": gain -> " + to_string(snap.gain));
                } else if (errno != ENOTSUP) {
                    result.skipped_requires_restart.push_back(label + ": gain present in config but failed to apply live, see logs - no restart needed, retry reload_diff");
                }
            }

            if (snap.has_bandwidth) {
                // Same "no live-readable current value" situation as gain above - input_t has no
                // generic bandwidth field (each driver's tuner-bandwidth state, where tracked at
                // all, is private to its own dev_data), so this reapplies unconditionally rather
                // than truly diffing. Unlike centerfreq, input_set_bandwidth() is synchronous (no
                // pending_*/apply-thread split - see input-common.cpp), so its return value is
                // already the real outcome, not just "request posted".
                errno = 0;
                if (input_set_bandwidth(dev->input, snap.bandwidth) == 0) {
                    result.applied.push_back(label + ": bandwidth -> " + to_string(snap.bandwidth));
                } else if (errno != ENOTSUP) {
                    result.skipped_requires_restart.push_back(label + ": bandwidth present in config but failed to apply live, see logs - no restart needed, retry reload_diff");
                }
            }

            // While auto_ppm stays on, an unchanged config correction is left alone: reapplying it
            // would throw away what auto_ppm has learned on every reload_diff.
            bool apply_correction = true;
            if (dev->auto_ppm == nullptr) {
                if (snap.auto_ppm) {
                    result.skipped_requires_restart.push_back(label + ": auto_ppm enabled in config, but the device started without it");
                }
            } else if (!snap.auto_ppm_params_error.empty()) {
                result.skipped_requires_restart.push_back(label + ": auto_ppm not changed, " + snap.auto_ppm_params_error);
                apply_correction = !auto_ppm_active(dev);
            } else if (snap.auto_ppm && snap.has_correction && abs(snap.correction) > snap.auto_ppm_params.max_ppm) {
                result.skipped_requires_restart.push_back(label + ": auto_ppm not changed, correction " + to_string(snap.correction) + " is outside auto_ppm_max_ppm");
                apply_correction = false;
            } else {
                if (snap.auto_ppm_params != dev->auto_ppm->params) {
                    result.skipped_requires_restart.push_back(label + ": auto_ppm_interval/auto_ppm_max_ppm/auto_ppm_max_step changed");
                }
                vector<string> changes;
                apply_correction = auto_ppm_reload(dev, snap.auto_ppm, snap.correction, &changes);
                for (const string& change : changes) {
                    result.applied.push_back(label + ": " + change);
                }
                if (!apply_correction && snap.has_correction) {
                    result.applied.push_back(label + ": correction unchanged in config, left to auto_ppm");
                }
            }

            if (snap.has_correction && apply_correction) {
                // Same pattern as bandwidth immediately above - no live-readable current value,
                // reapplies unconditionally, input_set_correction() is synchronous so its return
                // value is already the real outcome.
                errno = 0;
                if (input_set_correction(dev->input, snap.correction) == 0) {
                    result.applied.push_back(label + ": correction -> " + to_string(snap.correction));
                } else if (errno != ENOTSUP) {
                    result.skipped_requires_restart.push_back(label + ": correction present in config but failed to apply live, see logs - no restart needed, retry reload_diff");
                }
            }
        }
    }

    if ((int)snapshot.mixers.size() != mixer_count) {
        result.skipped_requires_restart.push_back("mixer count changed");
    } else {
        for (int i = 0; i < mixer_count; i++) {
            mixer_t* mixer = mixers + i;
            const MixerConfigSnapshot& snap = snapshot.mixers[i];
            if (mixer->name != nullptr && snap.name != string(mixer->name)) {
                result.skipped_requires_restart.push_back("mixer[" + to_string(i) + "]: name changed");
                continue;
            }
            // remote_inputs has no live-apply primitive at all (every route's listen_path/
            // stream_id/ampfactor/balance/label is fixed by mixer_connect_input() calls made only
            // during startup parsing) - previously this was silently never inspected here at all,
            // so an operator editing remote_inputs and calling reload_diff got no signal either
            // way. mixer->remote_inputs_signature is only ever null in a hand-built test fixture
            // that predates this field (parse_mixers() always sets it in production) - skip the
            // check rather than reporting a spurious mismatch against every such fixture.
            if (mixer->remote_inputs_signature != nullptr && snap.remote_inputs_signature != string(mixer->remote_inputs_signature)) {
                result.skipped_requires_restart.push_back("mixer[" + to_string(i) + "]: remote_inputs changed");
                continue;
            }
            if (snap.enabled != mixer->enabled) {
                bool confirmed = snap.enabled ? mixer_request_enable(mixer) : mixer_request_disable(mixer);
                result.applied.push_back(string("mixer '") + (mixer->name ? mixer->name : "") + "': enabled -> " + (snap.enabled ? "true" : "false") +
                                         (confirmed ? "" : " (request posted, not yet confirmed applied)"));
            }
        }
    }

    return result;
}
