/*
 * rtl_airband.h
 * Global declarations
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
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

#ifndef _RTL_AIRBAND_H
#define _RTL_AIRBAND_H 1
#include <lame/lame.h>
#include <netinet/in.h>  // sockaddr_in
#include <pthread.h>
#include <shout/shout.h>
#include <stdint.h>  // uint32_t
#include <sys/time.h>
#include <sys/un.h>  // sockaddr_un
#include <atomic>
#include <complex>
#include <cstdio>
#include <libconfig.h++>
#include <string>
#include <utility>
#include <vector>

#include "config.h"

#ifdef WITH_BCM_VC
#include "hello_fft/gpu_fft.h"
#else
#include <fftw3.h>
#endif /* WITH_BCM_VC */

#ifdef WITH_PULSEAUDIO
#include <pulse/context.h>
#include <pulse/stream.h>
#endif /* WITH_PULSEAUDIO */

#include "filters.h"
#include "input-common.h"  // input_t
#include "logging.h"
#include "squelch.h"

#define ALIGNED32 __attribute__((aligned(32)))
#define SLEEP(x) usleep(x * 1000)
#define THREAD pthread_t
#define GOTOXY(x, y) printf("%c[%d;%df", 0x1B, y, x)

#ifndef SYSCONFDIR
#define SYSCONFDIR "/usr/local/etc"
#endif /* SYSCONFDIR */

#define CFGFILE SYSCONFDIR "/rtl_airband.conf"
#define PIDFILE "/run/rtl_airband.pid"

#define MIN_BUF_SIZE 2560000
#define DEFAULT_SAMPLE_RATE 2560000

#ifdef NFM
#define WAVE_RATE 16000
#else
#define WAVE_RATE 8000
#endif /* NFM */

#define WAVE_BATCH WAVE_RATE / 8
#define AGC_EXTRA 100
#define WAVE_LEN 2 * WAVE_BATCH + AGC_EXTRA
#define MP3_RATE 8000
#define MAX_SHOUT_QUEUELEN 32768
#define TAG_QUEUE_LEN 16

#define MIN_FFT_SIZE_LOG 8
#define DEFAULT_FFT_SIZE_LOG 9
#define MAX_FFT_SIZE_LOG 13

// LAME's documented worst-case buffer requirement (lame.h): mp3buf_size = 1.25*num_samples
// + 7200. The largest single lame_encode_buffer_ieee_float() call in this codebase is
// LameTone's 1-second silence marker (output.cpp open_file(), used to fill gaps when
// resuming a continuous-mode file output) - it encodes WAVE_RATE samples in one call, not
// WAVE_BATCH, which only covers the smaller per-tick Icecast/streaming path. An extra
// +7200 covers the lame_encode_flush() call that follows into the same buffer (bounded by
// roughly one frame of still-buffered PCM plus final-frame padding - comparable to the
// encoder-state overhead LAME's own formula already allows for via its flat +7200 term).
#define LAMEBUF_SIZE ((5 * WAVE_RATE) / 4 + 14400)
#define MIX_DIVISOR 2

#ifdef WITH_BCM_VC
struct sample_fft_arg {
    size_t fft_size_by4;
    GPU_FFT_COMPLEX* dest;
};
extern "C" void samplefft(sample_fft_arg* a, unsigned char* buffer, float* window, float* levels);

#define FFT_BATCH 250
#else
#define FFT_BATCH 1
#endif /* WITH_BCM_VC */

// #define AFC_LOGGING

enum status { NO_SIGNAL = ' ', SIGNAL = '*', AFC_UP = '<', AFC_DOWN = '>' };
enum ch_states { CH_DIRTY, CH_WORKING, CH_READY };
enum mix_modes { MM_MONO, MM_STEREO };
enum udp_stream_format { STREAM_FORMAT_FLOAT32 = 0, STREAM_FORMAT_S16LE, STREAM_FORMAT_S8 };
enum output_type {
    O_ICECAST,
    O_FILE,
    O_RAWFILE,
    O_MIXER,
    O_UDP_STREAM,
    O_MIXER_REMOTE
#ifdef WITH_PULSEAUDIO
    ,
    O_PULSE
#endif /* WITH_PULSEAUDIO */
};

// Tracks the deferred-apply state machine for send_tx_tags (see icecast_tx_tag_step() in
// helper_functions.cpp). Once a change is pending, further changes update pending_value but
// do not push pending_deadline back - this bounds worst-case tag-update latency to one
// shout_metadata_delay window even if the source keeps flapping.
struct icecast_tx_tag_state {
    std::string applied;  // last value actually sent to Icecast ("" means cleared)
    bool pending = false;
    std::string pending_value;
    struct timeval pending_deadline;
};

struct icecast_data {
    const char* hostname;
    int port;
#ifdef LIBSHOUT_HAS_TLS
    int tls_mode;
#endif /* LIBSHOUT_HAS_TLS */
    const char* username;
    const char* password;
    const char* mountpoint;
    const char* name;
    const char* genre;
    const char* description;
    bool send_scan_freq_tags;
    // send an on-air metadata tag (the channel's/mixer input's configured label) when a
    // transmission starts, cleared to an empty string when it ends. Independent of
    // send_scan_freq_tags - rejected at config parse time on R_SCAN channels, where
    // send_scan_freq_tags already covers frequency-change tagging.
    bool send_tx_tags;
    icecast_tx_tag_state tx_tag_state;
    shout_t* shout;
    // number of times this stream's connection was lost (network error or the
    // owning device failing) and had to be torn down for output_check_thread to reconnect
    size_t disconnect_count;
    // number of times shout_queuelen() exceeded MAX_SHOUT_QUEUELEN, forcing a disconnect -
    // a subset of disconnect_count, called out separately since it means the local
    // encode rate outpaced what Icecast could drain, not a network-level failure
    size_t backlog_exceeded_count;
    // number of times a send_tx_tags metadata update was successfully applied
    size_t tx_tag_update_count;
};

#ifdef WITH_RDIO_SCANNER
struct rdio_scanner_data {
    std::string server;
    int port;
    bool use_tls;
    std::string api_key;
    int system_id;  // -1 if unset
    std::string system_label;
    int talkgroup_id;
    std::string talkgroup_label;
    std::string talkgroup_tag;
    std::string talkgroup_group;
    int source_id;
    bool delete_after_upload;
    long timeout_ms;
    int max_retries;
};

// one completed transmission queued for upload; config is copied so the
// worker thread never touches file_data/rdio_scanner_data concurrently with
// the output thread that produced it
struct rdio_scanner_job {
    rdio_scanner_data config;
    std::string file_path;
    long long timestamp_sec;  // Unix epoch seconds, UTC - sent as the "dateTime" field
    int frequency;
};
#endif /* WITH_RDIO_SCANNER */
struct freq_t;

struct file_data {
    // set once by config
    enum output_type type;
    std::string basedir;
    std::string basename;
    bool dated_subdirectories;
    std::string suffix;
    bool continuous;
    bool append;
    bool split_on_transmission;
    bool include_freq;
    double split_min_file_time;
    double split_max_file_time;
    double split_max_idle_time;
    bool split_include_transmission_start;
    double min_rx_seconds;
    std::string post_write_script;
#ifdef WITH_RDIO_SCANNER
    rdio_scanner_data* rdio_scanner;  // NULL if not configured for this output
#endif                                /* WITH_RDIO_SCANNER */
    double split_tx_drop_shorter_than;

    // set per output file
    timeval transmission_start_time;
    timeval file_open_time;
#ifdef WITH_RDIO_SCANNER
    // frequency (Hz) captured at file-open time for rdio_scanner upload; unlike `frequency`
    // below, set regardless of include_freq
    int open_frequency;
#endif /* WITH_RDIO_SCANNER */
    timeval last_write_time;
    int frequency;
    freq_t* freq;  // channel frequency the file was opened on (scan mode can retune before it closes)
    std::string file_path;
    std::string file_path_tmp;
    FILE* f;
    // number of times fwrite() on this output came up short (disk full, I/O error) -
    // the output is disabled when this happens, so it should stay at 0 or 1 in practice
    size_t write_failure_count;
};

struct udp_stream_data {
    float* stereo_buffer;
    size_t stereo_buffer_len;  // number of float elements, including both channels

    bool continuous;
    const char* dest_address;
    const char* dest_port;

    udp_stream_format format;  // set by config parser, default STREAM_FORMAT_FLOAT32
    void* convert_buffer;      // NULL when format == STREAM_FORMAT_FLOAT32
    size_t convert_buffer_len;

    // set by config parser, default WAVE_RATE (no resampling). When different from
    // WAVE_RATE, each channel is linear-interpolation-resampled to this rate before
    // the format conversion/send above - decouples the wire rate from the build's
    // internal WAVE_RATE (8000 without NFM, 16000 with).
    int sample_rate;
    float* resample_buffer;  // mono resampling scratch space; NULL when sample_rate == WAVE_RATE
    size_t resample_buffer_len;
    float* resample_left_buffer;        // stereo resampling scratch space, one per channel;
    float* resample_right_buffer;       // NULL when sample_rate == WAVE_RATE
    size_t resample_stereo_buffer_len;  // capacity of each of the above two buffers

    int send_socket;
    // number of packets dropped by the bounds checks in udp_stream_write() (item 11 in
    // CLAUDE.md - real checks, not assert(), so this can actually increment in a Release build)
    size_t dropped_packet_count;
};

// Cross-instance mixer input: streams one channel's audio to a mixer input slot reserved by a
// remote_inputs entry in a DIFFERENT rtl_airband process instance's config, on the same host
// (see mixer_remote_wire.h for the wire protocol, mixer_remote.cpp for the socket I/O). Unlike
// a local `type = "mixer"` output, there is no ampfactor/balance here - those describe how the
// RECEIVING mixer should treat this input, so they live in the receiver's remote_inputs config
// entry instead (mirrors how a local mixer input already stores ampfactor/balance mixer-side,
// not on the source channel).
struct mixer_remote_send_data {
    const char* dest_path;  // strdup'd; the receiver's AF_UNIX SOCK_DGRAM listen path
    uint32_t stream_id;     // must match one of the receiver's remote_inputs[].stream_id entries

    // Never connect()-ed, unlike udp_stream_data's socket - an AF_UNIX SOCK_DGRAM connect()
    // requires the target socket file to already exist, but the receiving instance may start
    // (or reload_diff a matching remote_inputs entry into existence) after this one does. Every
    // send uses sendto() with dest_sockaddr explicitly instead - see mixer_remote_send_init()'s
    // comment (mixer_remote.h) for the full rationale.
    int send_socket;
    struct sockaddr_un dest_sockaddr;

    uint64_t seq;  // monotonically increasing, one per packet sent

    uint8_t* send_buf;  // scratch buffer sized for one full packet (header + configured sample count)
    size_t send_buf_len;

    // number of times a send failed - either the local bounds check (more samples than send_buf
    // was sized for) or the sendto() syscall itself (e.g. ENOENT because the receiver isn't
    // listening yet) - mirrors udp_stream_data::dropped_packet_count exactly (item 11/23 pattern)
    size_t dropped_packet_count;
};

#ifdef WITH_PULSEAUDIO
struct pulse_data {
    const char* server;
    const char* name;
    const char* sink;
    const char* stream_name;
    pa_context* context;
    pa_stream *left, *right;
    pa_channel_map lmap, rmap;
    mix_modes mode;
    bool continuous;
    size_t underflow_count;
    size_t overflow_count;
    // number of times the write path (latency check or pa_stream_write()) forced a
    // disconnect - a subset of what PulseAudio itself would call an underflow/overflow
    size_t disconnect_count;
};
#endif /* WITH_PULSEAUDIO */

struct mixer_data {
    struct mixer_t* mixer;
    int input;
};

struct output_t {
    enum output_type type;
    bool enabled;
    bool active;
    void* data;

    // set to true in order to initialize `lame` and `lamebuf` after config parsing
    // is complete
    bool has_mp3_output;

    // lame encoder and buffer for mp3 output. initialized after config parsing
    // if `uses_mp3_output` is true
    lame_t lame;
    unsigned char* lamebuf;

    // number of times lame_encode_buffer_ieee_float() returned a negative (error) code
    // for this output; only meaningful when has_mp3_output is true
    size_t lame_encode_failure_count;
};

struct freq_tag {
    int freq;
    struct timeval tv;
};

enum modulations {
    MOD_AM
#ifdef NFM
    ,
    MOD_NFM
#endif /* NFM */
};

class Signal {
   public:
    Signal(void) : pending_(false) {
        pthread_cond_init(&cond_, NULL);
        pthread_mutex_init(&mutex_, NULL);
    }
    // pending_ is the predicate so a send() that races ahead of wait() is not
    // lost: the next wait() returns immediately instead of blocking for the
    // *next* send. Without this the demod thread overruns its first batch
    // every time its initial send() arrives before the output thread reaches
    // its first wait().
    void send(void) {
        pthread_mutex_lock(&mutex_);
        pending_ = true;
        pthread_cond_signal(&cond_);
        pthread_mutex_unlock(&mutex_);
    }
    void wait(void) {
        pthread_mutex_lock(&mutex_);
        while (!pending_) {
            pthread_cond_wait(&cond_, &mutex_);
        }
        pending_ = false;
        pthread_mutex_unlock(&mutex_);
    }

   private:
    bool pending_;
    pthread_cond_t cond_;
    pthread_mutex_t mutex_;
};

struct freq_t {
    int frequency;     // scan frequency
    char* label;       // frequency label
    float agcavgfast;  // average power, for AGC
    float ampfactor;   // multiplier to increase / decrease volume
    Squelch squelch;
    size_t active_counter;         // count of loops where channel has signal
    NotchFilter notch_filter;      // notch filter - good to remove CTCSS tones
    LowpassFilter lowpass_filter;  // lowpass filter, applied to I/Q after derotation, set at bandwidth/2 to remove out of band noise
    enum modulations modulation;
    // Smoothed carrier offset from this frequency while squelch was open (device_stats.cpp
    // freq_offset_commit()). Written by the demod thread, read by the control socket and output
    // threads; meaningless until freq_offset_updates is non-zero.
    std::atomic<float> freq_offset_hz;
    std::atomic<uint32_t> freq_offset_updates;
    // The same estimate restarted on every input_t::tuning_generation change, for auto_ppm
    // (auto_ppm.cpp): freq_offset_hz above still carries pre-change samples. ppm_offset_updates
    // is reset to 0 before ppm_offset_generation is published, so a reader that sees the current
    // generation never pairs it with a stale count. ppm_settle_batches is demod-thread only.
    std::atomic<float> ppm_offset_hz;
    std::atomic<uint32_t> ppm_offset_updates;
    std::atomic<uint32_t> ppm_offset_generation;
    int ppm_settle_batches;
    size_t split_tx_dropped_count;  // transmissions dropped for being shorter than split_tx_drop_shorter_than
};
struct channel_t {
    float wavein[WAVE_LEN];      // FFT output waveform
    float waveout[WAVE_LEN];     // waveform after squelch + AGC (left/center channel mixer output)
    float waveout_r[WAVE_LEN];   // right channel mixer output
    float iq_in[2 * WAVE_LEN];   // raw input samples for I/Q outputs and NFM demod
    float iq_out[2 * WAVE_LEN];  // raw output samples for I/Q outputs (FIXME: allocate only if required)
#ifdef NFM
    float pr;            // previous sample - real part
    float pj;            // previous sample - imaginary part
    float prev_waveout;  // previous sample - waveout before notch / ampfactor
    float alpha;
#endif                         /* NFM */
    uint32_t dm_dphi, dm_phi;  // derotation frequency and current phase value
    // Frequency offset estimator state (demod thread only): the previous raw bin output and this
    // batch's sum of freq_offset_step_cycles() over squelch-open samples.
    float fo_prev_re, fo_prev_im;
    double fo_expected_cycles;  // freq_offset_expected_cycles() for this batch's frequency
    double fo_step_sum;
    int fo_open_samples;
    enum mix_modes mode;  // mono or stereo
    // axcindicate, freq_idx, and state are each written by one thread (demod/mixer) and
    // read by others (output, controller, stats) with no lock between them - atomic so
    // that's well-defined rather than a data race, at no runtime cost over a plain
    // load/store on the platforms this project targets.
    std::atomic<status> axcindicate;
    unsigned char afc;  // 0 - AFC disabled; 1 - minimal AFC; 2 - more aggressive AFC and so on to 255
    struct freq_t* freqlist;
    int freq_count;
    std::atomic<int> freq_idx;
    int needs_raw_iq;
    int has_iq_outputs;
    std::atomic<ch_states> state;  // mixer channel state flag
    // Live-disable flag for the dynamic_reload control socket. Read by the demod/output/
    // controller threads with no lock, same rationale as axcindicate/freq_idx/state above - but
    // unlike those, this is only ever WRITTEN by the output thread that owns this channel's
    // outputs (device channels: the output thread serving that device; a mixer's own embedded
    // channel: never toggled independently, see mixer_t::pending_enable_request instead), never
    // directly by the control socket thread. Flipping it from another thread would race
    // output_thread()'s own concurrent reads/writes to channel->outputs - see
    // pending_enable_request below for how the control socket actually requests a change.
    std::atomic<bool> enabled;
    // Live enable/disable request from the dynamic_reload control socket: -1 = none pending,
    // 0 = disable requested, 1 = enable requested. The control socket thread only ever stores
    // into this (O(1), touches nothing else); output_thread() (src/output.cpp) polls and
    // consumes it once per pass, in the same thread that already owns this channel's `enabled`
    // flag and `outputs` array, so the actual channel_apply_enable()/channel_apply_disable()
    // (src/live_reconfig.cpp) - which tear down/re-arm icecast, UDP, and pulse connections -
    // never race that thread's own concurrent use of the same output_t structs.
    std::atomic<int> pending_enable_request;
    // Live removal request from the dynamic_reload control socket, posted only via reload_diff
    // detecting a pure tail decrease in a device's channel count (see compute_and_apply_diff(),
    // live_reconfig.cpp): -1 = none pending, 1 = removal requested. Same request/apply split and
    // single-owning-thread rationale as pending_enable_request above, but with one deliberate
    // difference: unlike enable/disable (which only flips a flag and reconnects/closes network
    // handles the output thread already exclusively owns), removal frees this channel's LAME
    // encoder. output_thread() (src/output.cpp) does NOT reset this to -1 the moment it observes
    // the request - only after channel_teardown_for_removal() (live_reconfig.cpp) has fully
    // finished, since a caller (try_remove_channels(), live_reconfig.cpp) that decrements
    // dev->channel_count as soon as it sees an early completion signal would be doing so while
    // frees are potentially still in flight.
    std::atomic<int> pending_remove_request;
    // Permanent tombstone, set by channel_teardown_for_removal() (live_reconfig.cpp) once a
    // removal has actually completed (LAME encoder freed) - reset to false only by parse_channel()
    // (config.cpp), when a fresh channel is parsed into this slot (startup, or a later live
    // append reusing this index after a successful removal). Exists because
    // try_remove_channels() (live_reconfig.cpp) can give up waiting on a slow/stuck output thread
    // and return without decrementing dev->channel_count, leaving this index looking "still live"
    // to anything bounds-checking against channel_count (get_device_and_channel(),
    // control_socket.cpp) even though the removal is either in flight or has already actually
    // completed. Checked together with pending_remove_request (!= -1 means a removal is currently
    // in flight) so a direct channel_enable/channel_disable command can never be issued against a
    // channel whose removal was requested, whether or not the output thread has gotten to it yet
    // - the alternative (letting it through) is a real crash: channel_apply_enable() never
    // reallocates the LAME encoder channel_teardown_for_removal() already freed, so the next
    // process_outputs() pass calls into LAME with a null encoder. Also checked by
    // compute_and_apply_diff()'s common-prefix signature walk (live_reconfig.cpp), so reverting a
    // config edit back to a tombstoned channel's exact original definition doesn't look like "no
    // change" and get silently skipped - a tombstoned index is always treated as diverged,
    // forcing it to be retried rather than left permanently dead.
    std::atomic<bool> removed;
    int output_count;
    output_t* outputs;
    int highpass;  // highpass filter cutoff
    int lowpass;   // lowpass filter cutoff
    // Canonical serialization of this channel's raw config block (everything except "enabled",
    // which is diffed/applied separately - see build_channel_identity_signature()'s comment,
    // config.cpp), set once by parse_channel() (shared by startup and dynamic_reload's live
    // append/replace path). compute_and_apply_diff() (live_reconfig.cpp) compares this against a
    // freshly re-read config's own signature for the same index to detect ANY change to an
    // existing channel - not just a count change - and replace (tear down + re-append) exactly
    // the channels that actually differ. NULL only for a mixer's own embedded channel (mixer->
    // channel never goes through parse_channel(), and is never itself replaceable this way).
    char* config_signature;
};

enum rec_modes { R_MULTICHANNEL, R_SCAN };
struct device_t {
    input_t* input;
#ifdef NFM
    float alpha;
#endif /* NFM */
    // Number of live channels. Grown at runtime (dynamic channel add via reload_diff) up to
    // channel_capacity by writing a new channel into an already-allocated, already-zeroed slot
    // and then publishing this count - see compute_and_apply_diff() (live_reconfig.cpp). Every
    // existing "for (int j = 0; j < dev->channel_count; j++)" loop re-reads this fresh each pass
    // (never caches it across passes), so the implicit atomic->int conversion means no call site
    // needs to change.
    std::atomic<int> channel_count;
    // Allocated size of channels/bins/base_bins - channel_count plus the device's configured
    // reserve_channels headroom (config.cpp). Fixed once at startup in parse_devices() and never
    // changed again; channel_count can grow up to this value without ever reallocating the
    // arrays below, which is what makes runtime growth safe with no lock: a demod/output thread
    // reading through channels/bins/base_bins never sees the block move.
    int channel_capacity;
    size_t *base_bins, *bins;
    channel_t* channels;
    // FIXME: size_t
    int waveend;
    int waveavail;
    THREAD controller_thread;
    struct freq_tag tag_queue[TAG_QUEUE_LEN];
    int tq_head, tq_tail;
    int last_frequency;
    pthread_mutex_t tag_queue_lock;
    int row;
    int failed;
    // Live centerfreq retune request from the dynamic_reload control socket thread. -1 means no
    // pending request. The demod thread that owns this device (and already exclusively owns
    // bins/base_bins/dm_dphi for AFC's own in-place adjustments, see the AFC class above) polls
    // and consumes this once per pass, so the actual bins/base_bins/dm_dphi recompute happens
    // in-thread with plain assignments - no cross-thread synchronization needed on those fields.
    std::atomic<int> pending_centerfreq_request;
    // Set by the demod thread (device_apply_retune()'s caller, rtl_airband.cpp) to the actual
    // outcome of the request above - true on failure, false on success - and published BEFORE
    // pending_centerfreq_request is reset to -1, not concurrently with it. Mirrors why
    // channel_t::pending_remove_request is a field separate from pending_enable_request (see its
    // comment below): a control-socket thread polling for "request consumed" must see a result
    // that already reflects whether the hardware call actually succeeded, not just that the
    // demod thread picked the request up.
    std::atomic<bool> centerfreq_apply_failed;
    // Live sample_rate change request, same request/apply split as pending_centerfreq_request
    // above but heavier: the demod thread must stop this device's RX thread, reopen the hardware
    // at the new rate, resize the input buffer, and recompute every channel's
    // bins/base_bins/dm_dphi - see device_apply_sample_rate() (live_reconfig.cpp) for the full
    // sequence, including its rollback-to-the-old-rate-on-failure behavior. -1 means no pending
    // request.
    std::atomic<int> pending_sample_rate_request;
    // Set by the demod thread to the actual outcome - true if the requested rate was rejected
    // (regardless of whether rollback to the old rate succeeded), published BEFORE
    // pending_sample_rate_request is reset to -1, same ordering discipline as
    // centerfreq_apply_failed above. A caller that wants to know whether the device survived a
    // rejected request (rollback succeeded, still running at the old rate) vs. is now genuinely
    // down (rollback also failed) should check input->state separately - only the latter case
    // sets INPUT_FAILED.
    std::atomic<bool> sample_rate_apply_failed;
    enum rec_modes mode;
    size_t output_overrun_count;
    struct auto_ppm_device* auto_ppm;  // NULL unless the device block has an auto_ppm key (auto_ppm.h)
};

struct mixinput_t {
    float* wavein;
    float ampfactor;
    float ampl, ampr;
    bool ready;
    // written by mixer_put_samples() (device output thread), read by mixer_thread() under
    // mutex and also by process_outputs() for send_tx_tags (a third reader, on a possibly
    // different output thread when multiple_output_threads=true) - atomic for the same
    // reason channel_t::axcindicate is, see the comment there.
    std::atomic<bool> has_signal;
    pthread_mutex_t mutex;
    size_t input_overrun_count;
    // index into devices[]/devices[].channels[] this input was configured from, set to -1 by
    // mixer_connect_input() and resolved by config.cpp's parse_outputs() right after; read
    // live by process_outputs() to look up the source channel's current label/freq_idx for
    // send_tx_tags on a mixer's icecast output. mixer->inputs is XCALLOC/XREALLOC'd (not
    // placement-new'd), so these are set explicitly in mixer_connect_input() rather than via
    // an in-class initializer - the same reason has_signal is set there too instead of relying
    // on XCALLOC's zero-init.
    int source_device_idx;
    int source_channel_idx;
    // strdup'd label for a remote_inputs-connected slot (source_device_idx/source_channel_idx
    // are -1 for these - there is no local channel to look up), or NULL if this slot has no
    // label configured (or is a local input). Set by config.cpp's parse_mixers() right after
    // mixer_connect_input() returns, mirroring how source_device_idx/source_channel_idx are set
    // post-connect for a local `type = "mixer"` output. Read by mixer_tx_tag() (output.cpp) as
    // the send_tx_tags fallback when source_device_idx/source_channel_idx are unresolved. Must be
    // freed and reset to NULL whenever a slot is reused (mixer_connect_input()'s tombstone-reuse
    // branch) or zeroed on fresh growth (mixer_finalize_capacity()) - same discipline
    // source_device_idx/source_channel_idx already follow, for the same reason (item 40's
    // merge-time fix): a reused slot must never transiently expose the previous occupant's label.
    char* remote_label;
};

struct mixer_t {
    const char* name;
    bool enabled;
    // Config-file "enabled = false" intent for the dynamic_reload control socket. `enabled`
    // above is auto-managed by mixer_connect_input()/mixer_disable_input() and always becomes
    // true as soon as any input connects, so a config-time "start disabled" request has to be
    // applied as a post-parse step, after all channels' mixer outputs have connected - see
    // main()'s call to mixer_disable() for every mixer with config_wants_disabled set, right
    // after parse_devices() returns.
    bool config_wants_disabled;
    // Live enable/disable request from the dynamic_reload control socket: -1 = none pending,
    // 0 = disable requested, 1 = enable requested. Same rationale as
    // channel_t::pending_enable_request - `enabled`, `inputs_todo`/`input_mask`, and this
    // mixer's own outputs are all touched by whichever output thread owns this mixer's range
    // (output_thread(), src/output.cpp), so a mixer_enable()/mixer_disable() call from the
    // control socket thread itself would race that thread's concurrent use of the same state.
    // The control socket only ever stores into this; output_thread() polls and consumes it.
    std::atomic<int> pending_enable_request;
    int interval;
    size_t output_overrun_count;
    // Number of connected inputs. Grown at runtime (dynamic channel add via reload_diff, when an
    // appended channel's output is `type = "mixer"`) up to input_capacity by writing into an
    // already-allocated, already-zeroed slot and then publishing this count - see
    // mixer_connect_input() (mixer.cpp) and device_t::channel_count's comment above for the same
    // pattern applied to device channels. Every existing "for (j=0;j<mixer->input_count;j++)"
    // loop re-reads this fresh each pass, so the implicit atomic->int conversion means no call
    // site needs to change.
    std::atomic<int> input_count;
    // Allocated size of inputs/inputs_todo/input_mask - input_count plus this mixer's configured
    // reserve_inputs headroom. Unlike device_t::channel_capacity (sized upfront from a known
    // config list length), a mixer's inputs are discovered incrementally as parse_devices() walks
    // every device channel's outputs, so this is only fixed once, by mixer_finalize_capacity()
    // (mixer.cpp), called from main() after parse_devices() returns and before any thread starts.
    // From that point on, input_count can grow up to this value without ever reallocating the
    // arrays below, which is what makes runtime growth safe with no lock.
    int input_capacity;
    // reserve_inputs from this mixer's config block (default 0), set once by parse_mixers() and
    // consumed once by mixer_finalize_capacity() - not read again after that.
    int reserve_inputs;
    mixinput_t* inputs;
    bool* inputs_todo;
    bool* input_mask;
    // Parallel to input_mask, but permanent-removal-only: set by mixer_disable_input(...,
    // permanent=true) (a channel actually being torn down, not just disabled) and cleared only
    // when mixer_connect_input() reclaims that slot for a new input. input_mask alone can't carry
    // this distinction - it's also flipped false/true by ordinary temporary disable/re-enable
    // (mixer_disable()/mixer_enable(), channel_apply_disable()/channel_apply_enable()), which must
    // NOT free the slot, since the same channel is expected to reconnect to the same index later.
    // Without this, every live-edited mixer-connected channel (item 30) permanently burned a new
    // reserve_inputs slot instead of reusing the one its previous definition vacated.
    bool* input_removed;
    // Canonical serialization of this mixer's remote_inputs config block (build_mixer_remote_
    // inputs_signature(), config.cpp), set once by parse_mixers() - NULL only in hand-built test
    // fixtures that predate this field (parse_mixers() always sets it in production), never in a
    // live-running process. remote_inputs has no live-apply primitive of its own (unlike
    // name/enabled, which reload_diff already diffs field-by-field): a route's listen_path/
    // stream_id/ampfactor/balance/label are all fixed at parse time, since mixer_connect_input()
    // is only ever called for them during startup config parsing. Without this signature,
    // compute_and_apply_diff() (live_reconfig.cpp) had no way to even notice a remote_inputs edit
    // - it wasn't rejected as skipped_requires_restart, it was silently never inspected at all.
    char* remote_inputs_signature;
    channel_t channel;
};

struct demod_params_t {
    Signal* mp3_signal;
    int device_start;
    int device_end;

#ifndef WITH_BCM_VC
    fftwf_plan fft;
    fftwf_complex* fftin;
    fftwf_complex* fftout;
#endif /* WITH_BCM_VC */
};

struct output_params_t {
    Signal* mp3_signal;
    int device_start;
    int device_end;
    int mixer_start;
    int mixer_end;
};

// version.cpp
extern char const* RTL_AIRBAND_VERSION;

// output.cpp
lame_t airlame_init(mix_modes mixmode, int highpass, int lowpass);
void shout_setup(icecast_data* icecast, mix_modes mixmode);
void disable_device_outputs(device_t* dev);
// permanent=true means the owning channel is being torn down for good (channel_teardown_for_
// removal(), live_reconfig.cpp), not just temporarily disabled - see mixer_disable_input()'s
// comment (mixer.cpp) for why that distinction matters for its O_MIXER-type outputs.
void disable_channel_outputs(channel_t* channel, bool permanent = false);
void* output_check_thread(void* params);
void* output_thread(void* params);

// rtl_airband.cpp
extern bool use_localtime;
extern bool multiple_demod_threads;
extern bool multiple_output_threads;
extern char* stats_filepath;
extern double stats_update_interval;
extern double global_split_min_file_time;
extern double global_split_max_file_time;
extern double global_split_max_idle_time;
extern char* stats_http_address;
extern int stats_http_port;
extern char* control_socket_path;
extern char* cfgfile;
#ifdef WITH_RDIO_SCANNER
extern int rdio_scanner_queue_depth;
#endif /* WITH_RDIO_SCANNER */
extern bool global_split_include_transmission_start;
extern double global_split_tx_drop_shorter_than;
extern size_t fft_size, fft_size_log;
extern int device_count, mixer_count;
extern int shout_metadata_delay;
extern volatile int do_exit, device_opened;
extern float alpha;
extern device_t* devices;
extern mixer_t* mixers;
// Allocates output->lame/lamebuf and connects output->type-specific state (shout_setup()/
// udp_stream_init()/pulse_setup()) - the "make this output actually ready to encode/send" step
// that's otherwise only ever run once, at startup, right after parse_devices()/parse_mixers().
// Shared with the dynamic_reload live channel-append path (live_reconfig.cpp), which must run
// this for a brand-new channel's outputs before publishing it - unlike channel_apply_enable()'s
// reconnect_channel_outputs() (live_reconfig.h), which assumes lame/lamebuf already exist from
// this call and only redoes the connection.
bool init_output(channel_t* channel, output_t* output);

// util.cpp
int atomic_inc(volatile int* pv);
int atomic_dec(volatile int* pv);
int atomic_get(volatile int* pv);
double atofs(char* s);
double delta_sec(const timeval* start, const timeval* stop);
void log(int priority, const char* format, ...);
void tag_queue_put(device_t* dev, int freq, struct timeval tv);
void tag_queue_get(device_t* dev, struct freq_tag* tag);
void tag_queue_advance(device_t* dev);
void sincosf_lut_init();
void sincosf_lut(uint32_t phi, float* sine, float* cosine);
void* xcalloc(size_t nmemb, size_t size, const char* file, const int line, const char* func);
void* xrealloc(void* ptr, size_t size, const char* file, const int line, const char* func);
#define XCALLOC(nmemb, size) xcalloc((nmemb), (size), __FILE__, __LINE__, __func__)
#define XREALLOC(ptr, size) xrealloc((ptr), (size), __FILE__, __LINE__, __func__)
float dBFS_to_level(const float& dBFS);
float level_to_dBFS(const float& level);

// mixer.cpp
// Set once by mixer_finalize_capacity(), called from main() after parse_devices() returns and
// before any thread starts. Before this point, mixer_connect_input() may still grow
// inputs/inputs_todo/input_mask via realloc (safe - single-threaded startup); after this point it
// never does, so a live-append attempt that would require growing past input_capacity is rejected
// instead. A plain global (not a mixer.cpp file-static) so unit tests can reset it explicitly -
// the unittests binary links every test_*.cpp into one process, and a file-static would leak
// `true` across unrelated tests in link/registration order.
extern bool mixer_capacity_finalized;
void mixer_finalize_capacity();
mixer_t* getmixerbyname(const char* name);
int mixer_connect_input(mixer_t* mixer, float ampfactor, float balance);
// permanent=true (channel_teardown_for_removal(), via disable_channel_outputs()) tombstones the
// slot in input_removed so a later mixer_connect_input() can reclaim it instead of consuming new
// reserve_inputs headroom; permanent=false (ordinary disable) leaves the slot bound to the same
// channel for a later mixer_enable_input() to re-arm.
void mixer_disable_input(mixer_t* mixer, int input_idx, bool permanent = false);
void mixer_enable_input(mixer_t* mixer, int input_idx);
// mixer_disable()/mixer_enable() do the real work behind the dynamic_reload control socket's
// mixer_disable/mixer_enable commands (and the shutdown-time/auto "all inputs died" path) - only
// call these from the output thread that owns this mixer's range (output_thread(), output.cpp),
// never directly from the control socket thread. See mixer_t::pending_enable_request and
// live_reconfig.h's mixer_request_enable()/mixer_request_disable() for the thread-safe entry
// point the control socket actually uses.
void mixer_disable(mixer_t* mixer);
void mixer_enable(mixer_t* mixer);
void mixer_put_samples(mixer_t* mixer, int input_idx, const float* samples, bool has_signal, unsigned int len);
void* mixer_thread(void* params);
const char* mixer_get_error();
// pure - no I/O or global state - so it can be unit tested directly
void mix_waveforms(float* sum, const float* in, float mult, int size);
// pure - lowest-index input with has_signal true, or -1 if none; used by send_tx_tags to
// pick a single tag when several mixer inputs are transmitting simultaneously
int mixer_select_active_tag_input(const mixinput_t* inputs, int input_count);

// config.cpp
int parse_devices(libconfig::Setting& devs);
int parse_mixers(libconfig::Setting& mx);
// Shared by parse_devices()'s startup path and dynamic_reload's live channel-append path
// (live_reconfig.cpp) - see parse_channel()'s definition in config.cpp for the full contract,
// including why it returns false for two pre-existing legacy-value quirks.
bool parse_channel(libconfig::Setting& chan_setting, device_t* dev, int dev_idx, int chan_idx, channel_t* channel);
// Recursively serializes chan_setting's raw config fields (skipping "enabled" at the top level)
// into a canonical string - see channel_t::config_signature's comment for why a whole-subtree
// signature, rather than a hand-maintained list of individually-diffed fields, is what backs
// dynamic_reload's live channel-replace detection: it captures every field a channel config can
// have (freq, modulation, bandwidth, squelch, notch, ctcss, highpass/lowpass, outputs - including
// arbitrarily nested output-type-specific blocks) with no risk of a future config option being
// silently left out of the comparison.
std::string build_channel_identity_signature(const libconfig::Setting& chan_setting);
// Same idea as build_channel_identity_signature(), scoped to just a mixer's remote_inputs
// block (or a distinct "absent" marker if the mixer has none) - see mixer_t::
// remote_inputs_signature's comment (rtl_airband.h) for why reload_diff needs this: unlike
// every other mixer field it already diffs (name, enabled), remote_inputs has no live-apply
// primitive at all, so a changed remote_inputs block must be positively detected and reported
// under skipped_requires_restart rather than silently doing nothing.
std::string build_mixer_remote_inputs_signature(const libconfig::Setting& mixer_setting);

// udp_stream.cpp
bool udp_stream_init(udp_stream_data* sdata, mix_modes mode, size_t len);
void udp_stream_write(udp_stream_data* sdata, const float* data, size_t len);
void udp_stream_write(udp_stream_data* sdata, const float* data_left, const float* data_right, size_t len);
void udp_stream_shutdown(udp_stream_data* sdata);
// pure - no I/O - so it can be unit tested directly. Returns the number of samples
// written to out (capped at out_capacity).
size_t resample_linear(const float* in, size_t len_in, int rate_in, int rate_out, float* out, size_t out_capacity);

// stats_http.cpp
void stats_http_start();
void stats_http_shutdown();

#ifdef WITH_RDIO_SCANNER
// rdio_scanner.cpp
extern bool rdio_scanner_enabled;
// process-wide, not per-output: the upload queue and its worker thread are shared by
// every rdio_scanner-configured output, so these counters are too
extern std::atomic<size_t> rdio_scanner_queue_drop_count;
extern std::atomic<size_t> rdio_scanner_upload_failure_count;
void rdio_scanner_start();
void rdio_scanner_shutdown();
void rdio_scanner_enqueue(rdio_scanner_data* config, const std::string& file_path, const timeval& open_time, int frequency);
// exposed for unit testing - pure mapping from a job to the multipart form
// fields that would be sent, with no network I/O
std::vector<std::pair<std::string, std::string>> rdio_scanner_build_fields(const rdio_scanner_job& job);
#endif /* WITH_RDIO_SCANNER */

#ifdef WITH_PULSEAUDIO
#define PULSE_STREAM_LATENCY_LIMIT 10000000UL
// pulse.cpp
void pulse_init();
int pulse_setup(pulse_data* pdata, mix_modes mixmode);
void pulse_start();
void pulse_shutdown(pulse_data* pdata);
void pulse_write_stream(pulse_data* pdata, mix_modes mode, const float* data_left, const float* data_right, size_t len);
#endif /* WITH_PULSEAUDIO */

#endif /* _RTL_AIRBAND_H */
