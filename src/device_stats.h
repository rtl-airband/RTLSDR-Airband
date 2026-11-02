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

// Carrier offset in Hz from a channel's summed phase increment between consecutive FFT frames
// (sum over n of z[n] * conj(z[n-1]) for the channel's bin output). A carrier sitting exactly on
// channel_freq advances by channel_freq-centerfreq cycles per second, sampled every
// round(sample_rate / WAVE_RATE) input samples - the same rotation compute_channel_dm_dphi()
// derotates for NFM/IQ channels - so whatever is left over is the carrier's offset from the
// configured frequency. Unambiguous to within +/- half the frame rate (WAVE_RATE / 2).
float freq_offset_from_phase_increment(double acc_re, double acc_im, int channel_freq, int centerfreq, int sample_rate);

// Batches skipped after a tuning change before the auto_ppm estimate (freq_t::ppm_offset_*)
// resumes: samples already buffered between the hardware and the demod thread were taken under
// the old tuning. 2 s worth (WAVE_BATCH is WAVE_RATE / 8).
#define FREQ_OFFSET_SETTLE_BATCHES 16

// Commits one demod batch's phase-increment sum into a frequency's smoothed offset estimate.
// open_samples is how many of the batch's samples had squelch open; batches below half open are
// dropped since the sum is then mostly noise. tuning_generation is the device's
// input_t::tuning_generation; when it changes, the auto_ppm estimate restarts after
// FREQ_OFFSET_SETTLE_BATCHES batches (freq_offset_hz itself is unaffected).
void freq_offset_commit(struct freq_t* fparms, double acc_re, double acc_im, int open_samples, int batch_samples, int centerfreq, int sample_rate, uint32_t tuning_generation = 0);

// JSON body of the control socket's read-only "status" command: one object per device with its
// current tuning, sample levels and counters, plus per-channel levels and offsets.
std::string build_status_json();

#endif /* _DEVICE_STATS_H */
