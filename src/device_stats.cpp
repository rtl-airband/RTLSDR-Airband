/*
 * device_stats.cpp
 * Per-device hardware health measurements and the control socket "status" snapshot
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

#include "device_stats.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "auto_ppm.h"
#include "rtl_airband.h"

using namespace std;

void scan_sample_levels(const unsigned char* buf, size_t n_iq_samples, sample_format_t sfmt, float fullscale, SampleLevels* out) {
    out->peak = 0.0f;
    out->clipped = 0;
    size_t const n = 2 * n_iq_samples;

    if (sfmt == SFMT_U8) {
        unsigned char lo = 255, hi = 0;
        for (size_t i = 0; i < n; i += 2) {
            unsigned char const a = buf[i], b = buf[i + 1];
            lo = min(lo, min(a, b));
            hi = max(hi, max(a, b));
            if (a == 0 || a == 255 || b == 0 || b == 255) {
                out->clipped++;
            }
        }
        if (n > 0) {
            out->peak = max(fabsf((float)lo - 127.5f), fabsf((float)hi - 127.5f)) / 127.5f;
        }
    } else if (sfmt == SFMT_S8) {
        const signed char* s = (const signed char*)buf;
        int peak = 0;
        for (size_t i = 0; i < n; i += 2) {
            int const a = s[i], b = s[i + 1];
            peak = max(peak, max(abs(a), abs(b)));
            if (a == -128 || a == 127 || b == -128 || b == 127) {
                out->clipped++;
            }
        }
        out->peak = (float)peak / 128.0f;
    } else if (sfmt == SFMT_S16) {
        const int16_t* s = (const int16_t*)buf;
        float const limit = fullscale - 1.0f;
        int peak = 0;
        for (size_t i = 0; i < n; i += 2) {
            int const a = abs((int)s[i]), b = abs((int)s[i + 1]);
            peak = max(peak, max(a, b));
            if (a >= limit || b >= limit) {
                out->clipped++;
            }
        }
        out->peak = (float)peak / fullscale;
    } else if (sfmt == SFMT_F32) {
        const float* s = (const float*)buf;
        float const limit = fullscale * 0.999f;
        float peak = 0.0f;
        for (size_t i = 0; i < n; i += 2) {
            float const a = fabsf(s[i]), b = fabsf(s[i + 1]);
            peak = max(peak, max(a, b));
            if (a >= limit || b >= limit) {
                out->clipped++;
            }
        }
        out->peak = peak / fullscale;
    }
}

void input_account_sample_levels(input_t* input, const SampleLevels& levels, size_t n_iq_samples, size_t window_samples) {
    if (levels.clipped > 0) {
        input->clipped_count.fetch_add(levels.clipped, memory_order_relaxed);
    }
    input->peak_window_max = max(input->peak_window_max, levels.peak);
    input->peak_window_samples += n_iq_samples;
    if (input->peak_window_samples >= window_samples) {
        input->peak_level.store(input->peak_window_max, memory_order_relaxed);
        input->peak_valid.store(true, memory_order_release);
        input->peak_window_max = 0.0f;
        input->peak_window_samples = 0;
    }
}

void input_measure_consumed_samples(input_t* input, size_t nbytes) {
    size_t const sample_bytes = 2 * input->bytes_per_sample;
    size_t const window = (size_t)input->sample_rate;  // ~1 s of I/Q samples
    size_t const first = min(nbytes, input->buf_size - input->bufs);
    SampleLevels levels;
    scan_sample_levels(input->buffer + input->bufs, first / sample_bytes, input->sfmt, input->fullscale, &levels);
    input_account_sample_levels(input, levels, first / sample_bytes, window);
    if (first < nbytes) {
        size_t const rest = (nbytes - first) / sample_bytes;
        scan_sample_levels(input->buffer, rest, input->sfmt, input->fullscale, &levels);
        input_account_sample_levels(input, levels, rest, window);
    }
}

float freq_offset_from_phase_increment(double acc_re, double acc_im, int channel_freq, int centerfreq, int sample_rate) {
    double const hop = round((double)sample_rate / (double)WAVE_RATE);
    double expected = (double)(channel_freq - centerfreq) * hop / (double)sample_rate;
    expected -= floor(expected);
    double delta = atan2(acc_im, acc_re) / (2.0 * M_PI) - expected;
    delta -= floor(delta + 0.5);  // wrap to [-0.5, 0.5) cycles per frame
    return (float)(delta * (double)sample_rate / hop);
}

namespace {

void smooth_offset(std::atomic<float>& value, std::atomic<uint32_t>& updates, float offset) {
    uint32_t const n = updates.load(memory_order_relaxed);
    value.store(n == 0 ? offset : 0.8f * value.load(memory_order_relaxed) + 0.2f * offset, memory_order_relaxed);
    updates.store(n + 1, memory_order_release);
}

}  // namespace

void freq_offset_commit(freq_t* fparms, double acc_re, double acc_im, int open_samples, int batch_samples, int centerfreq, int sample_rate, uint32_t tuning_generation) {
    if (fparms->ppm_offset_generation.load(memory_order_relaxed) != tuning_generation) {
        fparms->ppm_offset_updates.store(0, memory_order_relaxed);
        fparms->ppm_settle_batches = FREQ_OFFSET_SETTLE_BATCHES;
        fparms->ppm_offset_generation.store(tuning_generation, memory_order_release);
    }
    // Counted per batch whether or not squelch is open, so settling takes the same wall time either way.
    bool const settling = fparms->ppm_settle_batches > 0;
    if (settling) {
        fparms->ppm_settle_batches--;
    }
    if (open_samples * 2 < batch_samples || (acc_re == 0.0 && acc_im == 0.0)) {
        return;
    }
    float const offset = freq_offset_from_phase_increment(acc_re, acc_im, fparms->frequency, centerfreq, sample_rate);
    smooth_offset(fparms->freq_offset_hz, fparms->freq_offset_updates, offset);
    if (!settling) {
        smooth_offset(fparms->ppm_offset_hz, fparms->ppm_offset_updates, offset);
    }
}

namespace {

const char* input_state_name(input_state_t state) {
    switch (state) {
        case INPUT_INITIALIZED:
            return "initialized";
        case INPUT_RUNNING:
            return "running";
        case INPUT_FAILED:
            return "failed";
        case INPUT_STOPPED:
            return "stopped";
        case INPUT_DISABLED:
            return "disabled";
        default:
            return "unknown";
    }
}

string json_str(const char* s) {
    string out = "\"";
    for (const char* p = s; *p; p++) {
        unsigned char const c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += (char)c;
        }
    }
    return out + "\"";
}

// JSON has no NaN/Infinity, so a level of exactly zero (dBFS -inf) or an unmeasured value is null.
string json_num(double v, const char* fmt = "%.2f") {
    if (!isfinite(v)) {
        return "null";
    }
    char buf[48];
    snprintf(buf, sizeof(buf), fmt, v);
    return buf;
}

string channel_status_json(channel_t* channel, int index, uint32_t channel_generation) {
    string out = "{\"index\":" + to_string(index) + ",\"enabled\":" + (channel->enabled ? "true" : "false");
    int const freq_idx = channel->freq_idx;
    out += ",\"freq_idx\":" + to_string(freq_idx);
    out += ",\"squelch_open\":" + string(channel->axcindicate != NO_SIGNAL ? "true" : "false");
    out += ",\"freqs\":[";
    for (int k = 0; k < channel->freq_count; k++) {
        freq_t* f = channel->freqlist + k;
        if (k > 0) {
            out += ",";
        }
        out += "{\"freq\":" + to_string(f->frequency);
        out += ",\"label\":" + (f->label != NULL ? json_str(f->label) : string("null"));
        out += ",\"modulation\":" + string(f->modulation == MOD_AM ? "\"am\"" : "\"nfm\"");
        out += ",\"noise_dbfs\":" + json_num(level_to_dBFS(f->squelch.noise_level()));
        out += ",\"signal_dbfs\":" + json_num(level_to_dBFS(f->squelch.signal_level()));
        uint32_t const updates = f->freq_offset_updates.load(memory_order_acquire);
        out += ",\"freq_offset_hz\":" + (updates > 0 ? json_num(f->freq_offset_hz.load(memory_order_relaxed), "%.1f") : string("null"));
        out += ",\"freq_offset_updates\":" + to_string(updates);
        bool const ppm_current = f->ppm_offset_generation.load(memory_order_acquire) == channel_generation;
        uint32_t const ppm_updates = ppm_current ? f->ppm_offset_updates.load(memory_order_acquire) : 0;
        out += ",\"ppm_offset_hz\":" + (ppm_updates > 0 ? json_num(f->ppm_offset_hz.load(memory_order_relaxed), "%.1f") : string("null"));
        out += ",\"ppm_offset_updates\":" + to_string(ppm_updates);
        out += "}";
    }
    return out + "]}";
}

string device_status_json(device_t* dev, int index) {
    input_t* input = dev->input;
    string out = "{\"index\":" + to_string(index);
    out += ",\"driver\":" + (input->driver_type != NULL ? json_str(input->driver_type) : string("null"));
    out += ",\"state\":" + json_str(input_state_name(input->state));
    out += ",\"mode\":" + string(dev->mode == R_SCAN ? "\"scan\"" : "\"multichannel\"");
    out += ",\"centerfreq\":" + to_string(input->centerfreq);
    out += ",\"sample_rate\":" + to_string(input->sample_rate);

    input_settings_t settings;
    bool have_settings = false;
    if (input->get_settings != NULL) {
        lock_guard<recursive_mutex> control(input_control_mutex());
        have_settings = input->get_settings(input, &settings) == 0;
    }
    out += ",\"gain\":" + (have_settings && settings.gain_known ? json_num(settings.gain, "%.1f") : string("null"));
    out += ",\"correction\":" + (have_settings ? json_num(settings.correction, "%g") : string("null"));
    out += ",\"bandwidth\":" + (have_settings ? to_string(settings.bandwidth) : string("null"));

    bool const peak_valid = input->peak_valid.load(memory_order_acquire);
    out += ",\"peak_dbfs\":" + (peak_valid ? json_num(20.0 * log10((double)input->peak_level.load(memory_order_relaxed))) : string("null"));
    out += ",\"clipped_samples\":" + to_string(input->clipped_count.load(memory_order_relaxed));
    out += ",\"buffer_overflows\":" + to_string(input->overflow_count);
    out += ",\"buffer_underruns\":" + to_string(input->underrun_count);
    out += ",\"retune_failures\":" + to_string(input->centerfreq_retune_failure_count);
    out += ",\"output_overruns\":" + to_string(dev->output_overrun_count);
    out += ",\"auto_ppm\":" + auto_ppm_status_json(dev, auto_ppm_now());
    uint32_t const generation = input->tuning_generation.load(memory_order_acquire);

    out += ",\"channels\":[";
    int const channel_count = dev->channel_count;
    for (int j = 0; j < channel_count; j++) {
        if (j > 0) {
            out += ",";
        }
        out += channel_status_json(dev->channels + j, j, generation);
    }
    return out + "]}";
}

}  // namespace

string build_status_json() {
    string out = "{\"ok\":true,\"version\":" + json_str(RTL_AIRBAND_VERSION) + ",\"devices\":[";
    for (int i = 0; i < device_count; i++) {
        if (i > 0) {
            out += ",";
        }
        out += device_status_json(devices + i, i);
    }
    return out + "]}";
}
