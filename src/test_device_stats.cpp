/*
 * test_device_stats.cpp
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

#include "test_base_class.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "control_socket.h"
#include "device_stats.h"
#include "live_reconfig.h"
#include "rtl_airband.h"

using namespace std;

// Normally generated into version.cpp, which the unittests target doesn't compile.
char const* RTL_AIRBAND_VERSION = "test-version";

class DeviceStatsTest : public TestBaseClass {};

TEST_F(DeviceStatsTest, u8_quiet_signal_has_low_peak_and_no_clipping) {
    vector<unsigned char> buf = {127, 128, 120, 135, 128, 127};
    SampleLevels levels;
    scan_sample_levels(buf.data(), 3, SFMT_U8, 126.5f, &levels);
    EXPECT_EQ(levels.clipped, 0u);
    EXPECT_NEAR(levels.peak, 7.5f / 127.5f, 1e-6);
}

TEST_F(DeviceStatsTest, u8_counts_each_iq_sample_once_when_either_component_clips) {
    vector<unsigned char> buf = {255, 0, 128, 255, 128, 128};
    SampleLevels levels;
    scan_sample_levels(buf.data(), 3, SFMT_U8, 126.5f, &levels);
    EXPECT_EQ(levels.clipped, 2u);
    EXPECT_FLOAT_EQ(levels.peak, 1.0f);
}

TEST_F(DeviceStatsTest, s8_clipping_at_either_rail) {
    vector<signed char> buf = {-128, 0, 10, -20, 127, 5};
    SampleLevels levels;
    scan_sample_levels((const unsigned char*)buf.data(), 3, SFMT_S8, 126.5f, &levels);
    EXPECT_EQ(levels.clipped, 2u);
    EXPECT_FLOAT_EQ(levels.peak, 1.0f);
}

TEST_F(DeviceStatsTest, s16_uses_the_devices_native_full_scale) {
    // A 12-bit ADC in a CS16 container: 2047 is the rail, not 32767.
    vector<int16_t> buf = {1024, -512, 2047, 0};
    SampleLevels levels;
    scan_sample_levels((const unsigned char*)buf.data(), 2, SFMT_S16, 2048.0f, &levels);
    EXPECT_EQ(levels.clipped, 1u);
    EXPECT_NEAR(levels.peak, 2047.0f / 2048.0f, 1e-6);
}

TEST_F(DeviceStatsTest, f32_peak_relative_to_full_scale) {
    vector<float> buf = {0.25f, -0.5f, 0.1f, 0.0f};
    SampleLevels levels;
    scan_sample_levels((const unsigned char*)buf.data(), 2, SFMT_F32, 1.0f, &levels);
    EXPECT_EQ(levels.clipped, 0u);
    EXPECT_FLOAT_EQ(levels.peak, 0.5f);
}

TEST_F(DeviceStatsTest, peak_is_published_per_window_and_clipping_accumulates) {
    input_t input = {};
    input_account_sample_levels(&input, SampleLevels{0.5f, 3}, 600, 1000);
    EXPECT_FALSE(input.peak_valid.load()) << "window isn't complete yet";
    EXPECT_EQ(input.clipped_count.load(), 3u);

    input_account_sample_levels(&input, SampleLevels{0.25f, 1}, 600, 1000);
    ASSERT_TRUE(input.peak_valid.load());
    EXPECT_FLOAT_EQ(input.peak_level.load(), 0.5f) << "the window's max, not the latest scan";
    EXPECT_EQ(input.clipped_count.load(), 4u);

    input_account_sample_levels(&input, SampleLevels{0.1f, 0}, 1000, 1000);
    EXPECT_FLOAT_EQ(input.peak_level.load(), 0.1f) << "a new window starts from scratch";
}

TEST_F(DeviceStatsTest, consumed_samples_wrap_around_the_circular_buffer) {
    vector<unsigned char> buf(8, 128);
    buf[0] = 255;  // first sample after the wrap
    buf[6] = 0;    // last sample before the wrap
    input_t input = {};
    input.buffer = buf.data();
    input.buf_size = buf.size();
    input.bufs = 6;
    input.bytes_per_sample = 1;
    input.sfmt = SFMT_U8;
    input.sample_rate = 1000000;
    input_measure_consumed_samples(&input, 4);
    EXPECT_EQ(input.clipped_count.load(), 2u);
    EXPECT_FLOAT_EQ(input.peak_window_max, 1.0f);
    EXPECT_EQ(input.peak_window_samples, 2u);
}

// The demod loop's 7-term Blackman-Harris window (rtl_airband.cpp). It matters here:
// compute_channel_bin() picks the bin *below* a channel that lands exactly on a bin center, and
// only the window's main lobe makes the carrier visible in that neighbouring bin at all.
static double fft_window(size_t i, size_t n) {
    const double a[] = {0.27105140069342, 0.43329793923448, 0.21812299954311, 0.06592544638803, 0.01081174209837, 0.00077658482522, 0.00001388721735};
    double x = 0.0;
    for (int k = 0; k < 7; k++) {
        x += (k % 2 ? -a[k] : a[k]) * cos(2.0 * M_PI * k * (double)i / (double)(n - 1));
    }
    return x;
}

// What the demod loop sees for one channel: the bin of a windowed fft_size-point DFT taken every
// round(sample_rate / WAVE_RATE) input samples, of a carrier at carrier_hz, optionally frequency
// modulated voice-like: nine incommensurate 300-2900 Hz tones, fm_tone_deviation_hz each, so the
// spectrum is dense rather than a few Bessel lines one bin could lock onto. Returns the mean of
// freq_offset_step_cycles() over frames, as the demod loop accumulates it.
static double simulate_mean_step(int channel_freq, int centerfreq, int sample_rate, double carrier_hz, int frames, double fm_tone_deviation_hz = 0.0) {
    size_t const n_fft = 512;
    size_t const hop = (size_t)round((double)sample_rate / (double)WAVE_RATE);
    size_t const bin = compute_channel_bin(channel_freq, centerfreq, sample_rate, n_fft);
    double const expected = freq_offset_expected_cycles(channel_freq, centerfreq, sample_rate);
    double const w = 2.0 * M_PI * (carrier_hz - centerfreq) / sample_rate;
    const double tones[] = {311.0, 523.0, 787.0, 1033.0, 1291.0, 1597.0, 1931.0, 2389.0, 2903.0};
    const double tone_phase[] = {0.3, 1.9, 4.2, 2.7, 5.5, 0.9, 3.6, 6.0, 1.4};
    complex<double> prev;
    double sum = 0.0;
    for (int f = 0; f < frames; f++) {
        complex<double> z;
        for (size_t m = 0; m < n_fft; m++) {
            double const t = (double)(f * hop + m);
            double phase = w * t;
            if (fm_tone_deviation_hz != 0.0) {
                for (int k = 0; k < 9; k++) {
                    phase += fm_tone_deviation_hz / tones[k] * sin(2.0 * M_PI * tones[k] * t / sample_rate + tone_phase[k]);
                }
            }
            z += fft_window(m, n_fft) * polar(1.0, phase) * polar(1.0, -2.0 * M_PI * (double)(bin * m) / (double)n_fft);
        }
        if (f > 0) {
            complex<double> const p = z * conj(prev);
            sum += freq_offset_step_cycles((float)p.real(), (float)p.imag(), expected);
        }
        prev = z;
    }
    return sum / (frames - 1);
}

struct OffsetCase {
    int sample_rate;
    int centerfreq;
    int channel_freq;
    double offset_hz;
};

TEST_F(DeviceStatsTest, freq_offset_recovers_known_carrier_offsets_through_the_real_dft) {
    vector<OffsetCase> cases = {
        {2560000, 120000000, 120250000, 0.0},  {2560000, 120000000, 120250000, 350.0}, {2560000, 120000000, 119725000, -1200.0},
        {2400000, 133000000, 133912500, 75.0}, {2048000, 450000000, 449600000, -40.0}, {1000000, 118000000, 118300000, 2500.0},  // decimation 125 (AM) / 62.5 (NFM) - exercises the non-integer path
    };
    for (const OffsetCase& c : cases) {
        float const got = freq_offset_from_mean_step(simulate_mean_step(c.channel_freq, c.centerfreq, c.sample_rate, c.channel_freq + c.offset_hz, 64), c.sample_rate);
        EXPECT_NEAR(got, c.offset_hz, 0.5) << "sample_rate=" << c.sample_rate << " channel=" << c.channel_freq << " offset=" << c.offset_hz;
    }
}

TEST_F(DeviceStatsTest, freq_offset_is_not_pulled_by_the_bin_response_on_wide_fm) {
    // NFM voice deviates up to +/-5 kHz (here nine tones of 600 Hz each), wider than a 512-point
    // bin at 2.4 MS/s (4.7 kHz), and this channel sits ~1.5 kHz from its bin's center. Weighting phase steps by power through that
    // bin read a NOAA weather carrier several hundred Hz off on real hardware.
    int const fs = 2400000, cf = 162200000, ch = 162550000;
    // Within the +/-2 ppm range where auto_ppm settles. Further out, this noise-free multi-tone
    // signal reads up to ~8% low (it keeps sweeping past the bin edge), which only slows
    // convergence and never overshoots.
    for (double offset : {-300.0, -72.0, 0.0, 250.0}) {
        float const got = freq_offset_from_mean_step(simulate_mean_step(ch, cf, fs, ch + offset, 2000, 600.0), fs);
        EXPECT_NEAR(got, offset, 15.0) << "offset=" << offset;
    }
}

TEST_F(DeviceStatsTest, freq_offset_commit_drops_mostly_closed_batches_and_smooths) {
    freq_t f = {};
    f.frequency = 120250000;
    int const fs = 2560000, cf = 120000000;
    double const step = simulate_mean_step(f.frequency, cf, fs, f.frequency + 1000.0, 16);

    freq_offset_commit(&f, step * (WAVE_BATCH / 4), WAVE_BATCH / 4, WAVE_BATCH, fs);
    EXPECT_EQ(f.freq_offset_updates.load(), 0u);

    freq_offset_commit(&f, step * WAVE_BATCH, WAVE_BATCH, WAVE_BATCH, fs);
    EXPECT_EQ(f.freq_offset_updates.load(), 1u);
    EXPECT_NEAR(f.freq_offset_hz.load(), 1000.0f, 0.5f) << "first estimate is taken as-is";

    double const step2 = simulate_mean_step(f.frequency, cf, fs, f.frequency + 2000.0, 16);
    freq_offset_commit(&f, step2 * WAVE_BATCH, WAVE_BATCH, WAVE_BATCH, fs);
    EXPECT_NEAR(f.freq_offset_hz.load(), 1200.0f, 1.0f) << "later estimates are smoothed";
}

TEST_F(DeviceStatsTest, tuning_change_restarts_the_ppm_estimate_after_settling) {
    freq_t f = {};
    f.frequency = 120250000;
    int const fs = 2560000, cf = 120000000;
    double const before = simulate_mean_step(f.frequency, cf, fs, f.frequency + 1000.0, 16);
    double const after = simulate_mean_step(f.frequency, cf, fs, f.frequency + 100.0, 16);

    for (int i = 0; i < 5; i++) {
        freq_offset_commit(&f, before * WAVE_BATCH, WAVE_BATCH, WAVE_BATCH, fs, 0);
    }
    EXPECT_EQ(f.ppm_offset_updates.load(), 5u);
    EXPECT_NEAR(f.ppm_offset_hz.load(), 1000.0f, 0.5f);

    // Generation 1: the first FREQ_OFFSET_SETTLE_BATCHES batches may still be pre-change
    // samples, so even ones that look like the old tuning must not reach the ppm estimate.
    for (int i = 0; i < FREQ_OFFSET_SETTLE_BATCHES; i++) {
        freq_offset_commit(&f, before * WAVE_BATCH, WAVE_BATCH, WAVE_BATCH, fs, 1);
        EXPECT_EQ(f.ppm_offset_generation.load(), 1u);
        EXPECT_EQ(f.ppm_offset_updates.load(), 0u) << i;
    }
    freq_offset_commit(&f, after * WAVE_BATCH, WAVE_BATCH, WAVE_BATCH, fs, 1);
    EXPECT_EQ(f.ppm_offset_updates.load(), 1u);
    EXPECT_NEAR(f.ppm_offset_hz.load(), 100.0f, 0.5f) << "restarted, not smoothed with the old value";

    // freq_offset_hz keeps its original meaning: every open batch, smoothed straight through.
    EXPECT_EQ(f.freq_offset_updates.load(), 5u + FREQ_OFFSET_SETTLE_BATCHES + 1u);
    EXPECT_GT(f.freq_offset_hz.load(), 500.0f);
}

TEST_F(DeviceStatsTest, settling_counts_closed_squelch_batches_too) {
    freq_t f = {};
    f.frequency = 120250000;
    int const fs = 2560000, cf = 120000000;
    double const step = simulate_mean_step(f.frequency, cf, fs, f.frequency + 300.0, 16);
    for (int i = 0; i < FREQ_OFFSET_SETTLE_BATCHES; i++) {
        freq_offset_commit(&f, 0.0, 0, WAVE_BATCH, fs, 3);
    }
    freq_offset_commit(&f, step * WAVE_BATCH, WAVE_BATCH, WAVE_BATCH, fs, 3);
    EXPECT_EQ(f.ppm_offset_updates.load(), 1u);
}

static int fake_get_settings(input_t* const, input_settings_t* out) {
    out->gain = 33.8f;
    out->gain_known = true;
    out->correction = 2.0f;
    out->bandwidth = 0;
    return 0;
}

class DeviceStatusJsonTest : public TestBaseClass {
   protected:
    void SetUp() override {
        TestBaseClass::SetUp();
        saved_devices = devices;
        saved_device_count = device_count;
    }
    void TearDown() override {
        devices = saved_devices;
        device_count = saved_device_count;
        TestBaseClass::TearDown();
    }
    device_t* saved_devices;
    int saved_device_count;
};

TEST_F(DeviceStatusJsonTest, no_devices_is_an_empty_list) {
    devices = nullptr;
    device_count = 0;
    string response = control_socket_dispatch_command({{"cmd", "status"}});
    EXPECT_EQ(response, "{\"ok\":true,\"version\":\"test-version\",\"devices\":[]}");
}

TEST_F(DeviceStatusJsonTest, reports_tuning_levels_counters_and_channels) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.state = INPUT_RUNNING;
    input.centerfreq = 120000000;
    input.sample_rate = 2560000;
    input.get_settings = &fake_get_settings;
    input.peak_level = 0.5f;
    input.peak_valid = true;
    input.clipped_count = 7;
    input.overflow_count = 3;

    freq_t* freqs = (freq_t*)calloc(2, sizeof(freq_t));
    freqs[0].frequency = 120250000;
    freqs[0].label = strdup("Tower \"A\"");
    freqs[0].freq_offset_hz = 42.0f;
    freqs[0].freq_offset_updates = 5;
    freqs[1].frequency = 120500000;

    unique_ptr<channel_t[]> channels(new channel_t[2]());
    channels[0].freqlist = &freqs[0];
    channels[0].freq_count = 1;
    channels[0].enabled = true;
    channels[0].axcindicate = SIGNAL;
    channels[1].freqlist = &freqs[1];
    channels[1].freq_count = 1;
    channels[1].enabled = false;
    channels[1].axcindicate = NO_SIGNAL;

    unique_ptr<device_t> dev(new device_t());
    dev->input = &input;
    dev->mode = R_MULTICHANNEL;
    dev->channels = channels.get();
    dev->channel_count = 2;

    devices = dev.get();
    device_count = 1;
    string json = build_status_json();

    EXPECT_NE(json.find("\"driver\":\"rtlsdr\",\"state\":\"running\",\"mode\":\"multichannel\",\"centerfreq\":120000000,\"sample_rate\":2560000"), string::npos) << json;
    EXPECT_NE(json.find("\"gain\":33.8,\"correction\":2,\"bandwidth\":0"), string::npos) << json;
    EXPECT_NE(json.find("\"peak_dbfs\":-6.02,\"clipped_samples\":7,\"buffer_overflows\":3"), string::npos) << json;
    EXPECT_NE(json.find("{\"index\":0,\"enabled\":true,\"freq_idx\":0,\"squelch_open\":true"), string::npos) << json;
    EXPECT_NE(json.find("\"label\":\"Tower \\\"A\\\"\""), string::npos) << json;
    EXPECT_NE(json.find("\"freq_offset_hz\":42.0,\"freq_offset_updates\":5"), string::npos) << json;
    EXPECT_NE(json.find("{\"index\":1,\"enabled\":false,\"freq_idx\":0,\"squelch_open\":false"), string::npos) << json;
    EXPECT_NE(json.find("\"freq\":120500000,\"label\":null"), string::npos) << json;
    EXPECT_NE(json.find("\"freq_offset_hz\":null,\"freq_offset_updates\":0"), string::npos) << json;

    free(freqs[0].label);
    free(freqs);
}

TEST_F(DeviceStatusJsonTest, unmeasured_peak_and_missing_settings_hook_are_null) {
    input_t input = {};
    input.driver_type = "file";
    input.state = INPUT_RUNNING;
    unique_ptr<device_t> dev(new device_t());
    dev->input = &input;
    dev->channel_count = 0;
    devices = dev.get();
    device_count = 1;
    string json = build_status_json();
    EXPECT_NE(json.find("\"gain\":null,\"correction\":null,\"bandwidth\":null,\"peak_dbfs\":null"), string::npos) << json;
}
