/*
 * input-common.h
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

#ifndef _INPUT_COMMON_H
#define _INPUT_COMMON_H 1
#include <pthread.h>
#include <atomic>
#include <libconfig.h++>
#include <mutex>

#if __GNUC__ >= 4
#define MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#else
#define MODULE_EXPORT extern "C"
#endif /* __GNUC__ */

typedef enum { SFMT_UNDEF = 0, SFMT_U8, SFMT_S8, SFMT_S16, SFMT_F32 } sample_format_t;
#define SAMPLE_FORMAT_CNT 5

typedef enum { INPUT_UNKNOWN = 0, INPUT_INITIALIZED, INPUT_RUNNING, INPUT_FAILED, INPUT_STOPPED, INPUT_DISABLED } input_state_t;
#define INPUT_STATE_CNT 6

typedef struct input_t input_t;

// Current tuner settings as the driver last applied them, for the control socket "status"
// command. gain_known is false when the driver can't express its gain as one number (SoapySDR's
// per-element "name=value,..." gain form).
typedef struct {
    float gain;
    bool gain_known;
    float correction;
    int bandwidth;  // tuner bandwidth in Hz, 0 = driver default / automatic
} input_settings_t;

struct input_t {
    // Set once by each driver's own <type>_input_new(), e.g. "rtlsdr"/"soapysdr"/"file"
    // - matches the config file's "type" string. Only consumed by the dynamic_reload
    // reload_diff path (live_reconfig.cpp) to detect a device's driver type changing across a
    // config edit, which is out of v1 scope and needs a restart rather than a live apply.
    const char* driver_type;
    unsigned char* buffer;
    void* dev_data;
    size_t buf_size, bufs, bufe;
    size_t overflow_count;
    size_t underrun_count;
    // Failed input_set_centerfreq() calls (transient hardware errors on an already-running
    // device) - does not affect input->state, unlike overflow/underrun which are informational
    // only. See input_set_centerfreq()'s comment (input-common.cpp).
    size_t centerfreq_retune_failure_count;
    // Bumped after every successful centerfreq, correction or sample_rate change. Lets the
    // per-frequency offset estimate used by auto_ppm (freq_t::ppm_offset_*) restart on fresh
    // samples instead of averaging across the change.
    std::atomic<uint32_t> tuning_generation;
    input_state_t state;
    sample_format_t sfmt;
    float fullscale;
    int bytes_per_sample;
    int sample_rate;
    int centerfreq;
    int (*parse_config)(input_t* const input, libconfig::Setting& cfg);
    int (*init)(input_t* const input);
    void* (*run_rx_thread)(void* input_ptr);  // to be launched via pthread_create()
    int (*set_centerfreq)(input_t* const input, int const centerfreq);
    // set_gain/set_bandwidth/set_correction are nullable, unlike set_centerfreq - not every driver
    // has a live hook for these (e.g. the file driver has none of the three). input_set_gain()/
    // input_set_bandwidth()/input_set_correction() return -1/ENOTSUP when the driver's pointer is
    // NULL, so callers get a clear "not supported by this driver" result instead of a silent
    // no-op.
    int (*set_gain)(input_t* const input, float const gain);
    int (*set_bandwidth)(input_t* const input, int const bandwidth);
    int (*set_correction)(input_t* const input, int const correction);
    int (*stop)(input_t* const input);
    // Nullable. Call it with input_control_mutex() held, the lock every live set_* goes through,
    // so the driver's dev_data fields it reads are never written concurrently.
    int (*get_settings)(input_t* const input, input_settings_t* out);
    // ADC level measurements (device_stats.cpp). peak_level is the largest |I|/|Q| over the last
    // complete ~1 s window as a fraction of full scale, valid once peak_valid is set; written by
    // the demod thread, read by the control socket and output threads. The window fields are
    // demod-thread only.
    std::atomic<float> peak_level;
    std::atomic<bool> peak_valid;
    std::atomic<size_t> clipped_count;
    float peak_window_max;
    size_t peak_window_samples;
    pthread_t rx_thread;
    pthread_mutex_t buffer_lock;
};

input_t* input_new(char const* const type);
int input_init(input_t* const input);
int input_parse_config(input_t* const input, libconfig::Setting& cfg);
int input_start(input_t* const input);
int input_set_centerfreq(input_t* const input, int const centerfreq);
int input_set_gain(input_t* const input, float const gain);
int input_set_bandwidth(input_t* const input, int const bandwidth);
int input_set_correction(input_t* const input, int const correction);
int input_stop(input_t* const input);

// Serializes every live driver control call (input_set_centerfreq/gain/bandwidth/correction,
// get_settings, a live sample_rate reopen) across the threads that issue them: control socket,
// R_SCAN controller, demod (live retune) and auto_ppm. Process-wide rather than per device
// because these calls are rare and a single function-local static needs no per-input setup,
// which the hand-built input_t test fixtures would otherwise have to remember. Recursive so a
// holder can call the input_set_* wrappers, which take it themselves.
std::recursive_mutex& input_control_mutex();

// RTL-SDR USB transfer sizing for input-rtlsdr.cpp, kept here because the unit tests don't link
// the rtlsdr driver. A fixed 320000-byte transfer is 156 ms of samples at 1.024 Msps, longer than
// one 125 ms audio batch, so one transfer could complete two batches at once and the second
// overran the single demod->output slot. The auto size is half a batch.
#define RTLSDR_MAX_AUTO_USB_BUF_LEN 320000  // the old fixed size, still used at >= 2.56 Msps
#define RTLSDR_MAX_USB_BUF_LEN 16777216     // upper bound for an explicit buffer_size
#define RTLSDR_DEFAULT_LIBUSB_BUFFER_COUNT 10
size_t rtlsdr_auto_usb_buf_len(int sample_rate);
// The buffer count that keeps total USB buffering at the old 10 x 320000 bytes, so a smaller
// transfer doesn't shrink the cushion against RX-thread stalls (or grow usbfs memory use).
int rtlsdr_auto_usb_buf_count(size_t buf_len);
bool rtlsdr_valid_usb_buf_len(long long buf_len);

#endif /* _INPUT_COMMON_H */
