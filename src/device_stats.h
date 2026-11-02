/*
 * device_stats.h
 * Per-device hardware health measurements (ADC peak level / clipping, per-channel carrier
 * frequency offset) and the control socket "status" snapshot that reports them.
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

#ifndef _DEVICE_STATS_H
#define _DEVICE_STATS_H 1

#include <cmath>
#include <cstddef>
#include <string>
#include "input-common.h"

struct SampleLevels {
    float peak;      // largest |I| or |Q| seen, as a fraction of the format's full scale (1.0 = full scale)
    size_t clipped;  // I/Q samples where either component sat at the format's full-scale limit
};

// Scans n_iq_samples interleaved I/Q samples of the given format. U8/S8 ignore fullscale (their
// limits are fixed by the format); S16/F32 use it, since SoapySDR devices report a native full
// scale that can be smaller than the container (a 12-bit ADC in CS16 tops out at 2048).
void scan_sample_levels(const unsigned char* buf, size_t n_iq_samples, sample_format_t sfmt, float fullscale, SampleLevels* out);

// Folds one scan into the device's running window and publishes the window's peak (and resets
// it) once at least window_samples I/Q samples have been seen. Demod-thread only, except for the
// atomics it publishes into.
void input_account_sample_levels(input_t* input, const SampleLevels& levels, size_t n_iq_samples, size_t window_samples);

// Measures the nbytes of input the demod loop is about to consume starting at input->bufs,
// wrapping at buf_size. Called once per pass with exactly the bytes that pass advances bufs by, so
// every input sample is counted once even though consecutive FFT windows overlap.
void input_measure_consumed_samples(input_t* input, size_t nbytes);

// Phase advance, in cycles per FFT frame wrapped to [0, 1), of a carrier sitting exactly on
// channel_freq in a channel's bin output: it advances by channel_freq-centerfreq cycles per
// second, sampled every round(sample_rate / WAVE_RATE) input samples - the same rotation
// compute_channel_dm_dphi() derotates for NFM/IQ channels.
double freq_offset_expected_cycles(int channel_freq, int centerfreq, int sample_rate);

// One frame's phase step of the bin output (re/im of z[n] * conj(z[n-1])) minus the expected
// advance, wrapped to [-0.5, 0.5) cycles. The estimate is the plain mean of these steps:
// weighting them by signal power (the angle of the summed products) lets the FFT bin's response,
// narrower than an NFM signal's deviation, pull the result by hundreds of Hz.
inline double freq_offset_step_cycles(float re, float im, double expected_cycles) {
    double d = atan2((double)im, (double)re) * (0.5 / M_PI) - expected_cycles;
    return d - floor(d + 0.5);
}

// Carrier offset in Hz from the mean per-frame step (cycles) above. Unambiguous to within +/- half
// the frame rate (WAVE_RATE / 2).
float freq_offset_from_mean_step(double mean_step_cycles, int sample_rate);

// Batches skipped after a tuning change before the auto_ppm estimate (freq_t::ppm_offset_*)
// resumes: samples already buffered between the hardware and the demod thread were taken under
// the old tuning. 2 s worth (WAVE_BATCH is WAVE_RATE / 8).
#define FREQ_OFFSET_SETTLE_BATCHES 16

// Commits one demod batch's summed freq_offset_step_cycles() (step_sum_cycles, over open_samples
// squelch-open samples) into a frequency's smoothed offset estimate. Batches below half open are
// dropped since the sum is then mostly noise. tuning_generation is the device's
// input_t::tuning_generation; when it changes, the auto_ppm estimate restarts after
// FREQ_OFFSET_SETTLE_BATCHES batches (freq_offset_hz itself is unaffected).
void freq_offset_commit(struct freq_t* fparms, double step_sum_cycles, int open_samples, int batch_samples, int sample_rate, uint32_t tuning_generation = 0);

// JSON body of the control socket's read-only "status" command: one object per device with its
// current tuning, sample levels and counters, plus per-channel levels and offsets.
std::string build_status_json();

#endif /* _DEVICE_STATS_H */
