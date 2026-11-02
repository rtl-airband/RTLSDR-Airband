/*
 * test_live_reconfig.cpp
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

#include <thread>

#include "live_reconfig.h"
#include "rtl_airband.h"

using namespace std;

// live_reconfig.cpp's reconnect_channel_outputs() references these as extern; normally defined
// in output.cpp/pulse.cpp, neither of which can be linked here for the same reasons documented
// in test_mixer.cpp. Not exercised by the tests below - all test fixtures use output_count=0,
// so these are only needed to satisfy the linker.
void shout_setup(icecast_data*, mix_modes) {}
#ifdef WITH_PULSEAUDIO
void pulse_init() {}
int pulse_setup(pulse_data*, mix_modes) {
    return 0;
}
#endif /* WITH_PULSEAUDIO */
// config.cpp's parse_split_file_times() reads these as extern for the per-output > global >
// default fallback; normally defined in rtl_airband.cpp, which can't be linked here for the same
// reasons documented above. Set to the same defaults rtl_airband.cpp uses, so a test fixture's
// file output parses with realistic split settings rather than zeros.
double global_split_min_file_time = 1.0;
double global_split_max_file_time = 60.0 * 60.0;
double global_split_max_idle_time = 0.5;
bool global_split_include_transmission_start = false;
#ifdef NFM
// config.cpp's parse_devices() references this as extern under NFM; normally defined in
// rtl_airband.cpp, which can't be linked here for the same reasons documented above. None of the
// tests below call parse_devices() itself (only parse_channel()/compute_and_apply_diff(), which
// don't need it) - only needed to satisfy the linker.
float alpha = 0.0f;
#endif /* NFM */
// try_append_channels() (via this file) calls this as the "make a freshly appended channel's
// outputs actually ready to encode/send" step; normally defined in rtl_airband.cpp (calls
// airlame_init() from output.cpp), neither of which can be linked here for the same reasons
// documented above. The ChannelAppendTest cases below test the append/publish logic itself, not
// real LAME/icecast/udp connection setup - that's exercised by system_tests/tests/
// test_channel_add.py against a real binary instead. Always "succeeds" here so the append logic
// under test isn't gated on something this stub can't meaningfully do.
bool init_output(channel_t*, output_t*) {
    return true;
}

class LiveReconfigTest : public TestBaseClass {};

TEST_F(LiveReconfigTest, compute_channel_bin_matches_hand_computed_value) {
    // sample_rate=2000000, fft_size=2048 -> bin width = 976.5625 Hz (truncated to 976 by the
    // integer division baked into the original formula, see live_reconfig.cpp's comment)
    int const sample_rate = 2000000;
    size_t const num_bins = 2048;
    int const centerfreq = 100000000;
    int const channel_freq = 100050000;  // +50kHz from center

    size_t bin = compute_channel_bin(channel_freq, centerfreq, sample_rate, num_bins);

    // bin_width = sample_rate / fft_size = 976 (integer division)
    // bin = ceil((channel_freq + sample_rate - centerfreq) / (double)bin_width - 1.0) % fft_size
    //     = ceil((100050000 + 2000000 - 100000000) / 976.0 - 1.0) % 2048
    //     = ceil(2050000 / 976.0 - 1.0) % 2048
    double const bin_width = (double)(sample_rate / (int)num_bins);
    size_t const expected = (size_t)ceil((channel_freq + sample_rate - centerfreq) / bin_width - 1.0) % num_bins;
    EXPECT_EQ(bin, expected);
}

TEST_F(LiveReconfigTest, compute_channel_bin_at_center_frequency) {
    // A channel exactly at the tuned center should land near bin fft_size-1 (the formula's -1.0
    // term), not bin 0 - this pins the "off by one" convention rather than deriving it.
    int const sample_rate = 2000000;
    size_t const num_bins = 2048;
    int const centerfreq = 100000000;

    size_t bin = compute_channel_bin(centerfreq, centerfreq, sample_rate, num_bins);
    double const bin_width = (double)(sample_rate / (int)num_bins);
    size_t const expected = (size_t)ceil(sample_rate / bin_width - 1.0) % num_bins;
    EXPECT_EQ(bin, expected);
}

TEST_F(LiveReconfigTest, compute_channel_dm_dphi_is_zero_when_channel_equals_center) {
    // No downmix needed when the channel sits exactly on the tuned center frequency.
    uint32_t dphi = compute_channel_dm_dphi(100000000, 100000000, 2000000);
    EXPECT_EQ(dphi, 0u);
}

TEST_F(LiveReconfigTest, compute_channel_dm_dphi_is_deterministic) {
    // Same inputs must always produce the same output - this is the property the live-retune
    // path depends on to stay in sync with config.cpp's parse-time computation.
    uint32_t a = compute_channel_dm_dphi(100050000, 100000000, 2000000);
    uint32_t b = compute_channel_dm_dphi(100050000, 100000000, 2000000);
    EXPECT_EQ(a, b);
}

TEST_F(LiveReconfigTest, compute_channel_dm_dphi_sign_flips_with_offset_direction) {
    // A channel above center and the mirror-image channel below center should downmix in
    // opposite directions.
    uint32_t above = compute_channel_dm_dphi(100050000, 100000000, 2000000);
    uint32_t below = compute_channel_dm_dphi(99950000, 100000000, 2000000);
    EXPECT_NE(above, below);
}

TEST_F(LiveReconfigTest, device_request_retune_accepts_r_multichannel) {
    device_t dev = {};
    dev.mode = R_MULTICHANNEL;
    dev.pending_centerfreq_request = -1;

    EXPECT_TRUE(device_request_retune(&dev, 123456789));
    EXPECT_EQ(dev.pending_centerfreq_request.load(), 123456789);
}

TEST_F(LiveReconfigTest, device_request_retune_rejects_r_scan) {
    // R_SCAN devices retune via their own controller_thread's fixed-offset scheme; this path
    // must never fight it.
    device_t dev = {};
    dev.mode = R_SCAN;
    dev.pending_centerfreq_request = -1;

    EXPECT_FALSE(device_request_retune(&dev, 123456789));
    EXPECT_EQ(dev.pending_centerfreq_request.load(), -1);
}

int fake_set_centerfreq_ok(input_t* const, int const) {
    return 0;
}

int fake_set_centerfreq_fails(input_t* const, int const) {
    return -1;
}

TEST_F(LiveReconfigTest, device_apply_retune_succeeds_and_updates_centerfreq) {
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_centerfreq = &fake_set_centerfreq_ok;
    input.dev_data = &fake_dev_data;

    freq_t freq = {};
    freq.frequency = 120050000;
    channel_t channel = {};
    channel.freqlist = &freq;

    device_t dev = {};
    dev.input = &input;
    dev.channel_count = 1;
    dev.channels = &channel;
    size_t bins[1] = {0}, base_bins[1] = {0};
    dev.bins = bins;
    dev.base_bins = base_bins;

    bool ok = device_apply_retune(&dev, 121000000);

    EXPECT_TRUE(ok);
    EXPECT_EQ(input.centerfreq, 121000000);
    EXPECT_EQ(input.state, INPUT_RUNNING);
}

TEST_F(LiveReconfigTest, device_apply_retune_returns_false_and_does_not_mark_input_failed_on_hardware_error) {
    // The regression this test guards against: a single failed retune (e.g. a transient i2c
    // error) used to unconditionally set input->state = INPUT_FAILED, which demodulate()
    // (rtl_airband.cpp) treats identically to "device never came up" - on the last running
    // device, that cascaded into exiting the whole process. See input_set_centerfreq()'s comment
    // (input-common.cpp).
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_centerfreq = &fake_set_centerfreq_fails;
    input.dev_data = &fake_dev_data;
    input.centerfreq_retune_failure_count = 0;

    freq_t freq = {};
    freq.frequency = 120050000;
    channel_t channel = {};
    channel.freqlist = &freq;

    device_t dev = {};
    dev.input = &input;
    dev.channel_count = 1;
    dev.channels = &channel;
    size_t bins[1] = {0}, base_bins[1] = {0};
    dev.bins = bins;
    dev.base_bins = base_bins;

    bool ok = device_apply_retune(&dev, 121000000);

    EXPECT_FALSE(ok);
    EXPECT_EQ(input.state, INPUT_RUNNING);
    EXPECT_EQ(input.centerfreq, 120000000);  // unchanged on failure
    EXPECT_EQ(input.centerfreq_retune_failure_count, 1u);
    // Bins/base_bins must stay at their pre-retune values on a failed hardware call - the radio is
    // still physically on the OLD centerfreq (input.centerfreq above proves that), so recomputing
    // them for the new, never-reached centerfreq would make the demod math and the actual RF
    // tuning disagree until the next successful retune.
    EXPECT_EQ(bins[0], 0u);
    EXPECT_EQ(base_bins[0], 0u);
}

TEST_F(LiveReconfigTest, retune_consumption_publishes_failure_before_clearing_request) {
    // Mirrors demodulate()'s consumption block (rtl_airband.cpp) - a control socket thread
    // polling pending_centerfreq_request must never observe "consumed" (-1) before
    // centerfreq_apply_failed already reflects the real result. This is the TOCTOU fix itself
    // (a separate bug from the INPUT_FAILED removal above): the old code cleared the request via
    // a plain exchange() before device_apply_retune() had even run.
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_centerfreq = &fake_set_centerfreq_fails;
    input.dev_data = &fake_dev_data;

    freq_t freq = {};
    freq.frequency = 120050000;
    channel_t channel = {};
    channel.freqlist = &freq;

    device_t dev = {};
    dev.input = &input;
    dev.channel_count = 1;
    dev.channels = &channel;
    size_t bins[1] = {0}, base_bins[1] = {0};
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    dev.centerfreq_apply_failed = false;

    std::atomic<bool> stop_consumer{false};
    std::thread consumer([&]() {
        while (!stop_consumer.load()) {
            int pending = dev.pending_centerfreq_request.load(std::memory_order_acquire);
            if (pending >= 0) {
                bool ok = device_apply_retune(&dev, pending);
                dev.centerfreq_apply_failed.store(!ok, std::memory_order_release);
                dev.pending_centerfreq_request.store(-1, std::memory_order_release);
            }
            std::this_thread::yield();
        }
    });

    dev.pending_centerfreq_request.store(121000000, std::memory_order_release);
    // Poll the same way handle_retune() does (control_socket.cpp).
    while (dev.pending_centerfreq_request.load(std::memory_order_acquire) != -1) {
        std::this_thread::yield();
    }
    bool failed = dev.centerfreq_apply_failed.load(std::memory_order_acquire);

    stop_consumer.store(true);
    consumer.join();

    EXPECT_TRUE(failed);
}

TEST_F(LiveReconfigTest, device_confirm_retune_reports_success) {
    device_t dev = {};
    dev.pending_centerfreq_request = -1;  // already consumed
    dev.centerfreq_apply_failed = false;

    bool timed_out = true;  // pre-set to a non-default value to confirm it's actually written
    bool ok = device_confirm_retune(&dev, /*timeout_us=*/50000, &timed_out);

    EXPECT_TRUE(ok);
    EXPECT_FALSE(timed_out);
}

TEST_F(LiveReconfigTest, device_confirm_retune_reports_hardware_failure) {
    device_t dev = {};
    dev.pending_centerfreq_request = -1;  // already consumed
    dev.centerfreq_apply_failed = true;

    bool timed_out = false;
    bool ok = device_confirm_retune(&dev, /*timeout_us=*/50000, &timed_out);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(timed_out);
}

TEST_F(LiveReconfigTest, device_confirm_retune_reports_timeout_when_never_consumed) {
    device_t dev = {};
    dev.pending_centerfreq_request = 121000000;  // never consumed by anything in this test

    bool timed_out = false;
    bool ok = device_confirm_retune(&dev, /*timeout_us=*/20000, &timed_out);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(timed_out);
    EXPECT_EQ(dev.pending_centerfreq_request.load(), 121000000);  // untouched
}

TEST(ComputeInputBufSizeTest, matches_the_startup_sizing_formula_config_cpp_used_to_compute_inline) {
    // Fixed known-good values, independent of WAVE_RATE (which differs between NFM and non-NFM
    // builds) - MIN_BUF_SIZE is always a multiple of any realistic fft_batch_len, so buf_size
    // should always come back as exactly MIN_BUF_SIZE for these inputs regardless of build.
    EXPECT_EQ(compute_input_buf_size(2000000, 1), (size_t)MIN_BUF_SIZE);
    EXPECT_GT(compute_input_buf_size(3000000, 1), 0u);
}

TEST(ComputeInputBufSizeTest, larger_sample_rate_produces_larger_or_equal_buf_size) {
    size_t small = compute_input_buf_size(1000000, 1);
    size_t large = compute_input_buf_size(3000000, 1);
    EXPECT_GE(large, small);
}

TEST_F(LiveReconfigTest, device_confirm_sample_rate_reports_success) {
    device_t dev = {};
    dev.pending_sample_rate_request = -1;  // already consumed
    dev.sample_rate_apply_failed = false;

    bool timed_out = true;
    bool ok = device_confirm_sample_rate(&dev, /*timeout_us=*/50000, &timed_out);

    EXPECT_TRUE(ok);
    EXPECT_FALSE(timed_out);
}

TEST_F(LiveReconfigTest, device_confirm_sample_rate_reports_failure) {
    device_t dev = {};
    dev.pending_sample_rate_request = -1;
    dev.sample_rate_apply_failed = true;

    bool timed_out = false;
    bool ok = device_confirm_sample_rate(&dev, /*timeout_us=*/50000, &timed_out);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(timed_out);
}

TEST_F(LiveReconfigTest, device_confirm_sample_rate_reports_timeout_when_never_consumed) {
    device_t dev = {};
    dev.pending_sample_rate_request = 3000000;  // never consumed by anything in this test

    bool timed_out = false;
    bool ok = device_confirm_sample_rate(&dev, /*timeout_us=*/20000, &timed_out);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(timed_out);
    EXPECT_EQ(dev.pending_sample_rate_request.load(), 3000000);
}

int fake_stop_ok_for_sample_rate_test(input_t* const) {
    return 0;
}

TEST_F(LiveReconfigTest, device_request_sample_rate_rejects_r_scan) {
    input_t input = {};
    input.stop = &fake_stop_ok_for_sample_rate_test;
    device_t dev = {};
    dev.input = &input;
    dev.mode = R_SCAN;
    dev.pending_sample_rate_request = -1;

    bool ok = device_request_sample_rate(&dev, 3000000);

    EXPECT_FALSE(ok);
    EXPECT_EQ(dev.pending_sample_rate_request.load(), -1);
}

TEST_F(LiveReconfigTest, device_request_sample_rate_rejects_driver_without_stop_hook) {
    input_t input = {};
    input.stop = nullptr;
    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.pending_sample_rate_request = -1;

    bool ok = device_request_sample_rate(&dev, 3000000);

    EXPECT_FALSE(ok);
    EXPECT_EQ(dev.pending_sample_rate_request.load(), -1);
}

TEST_F(LiveReconfigTest, device_request_sample_rate_accepts_r_multichannel_with_stop_hook) {
    input_t input = {};
    input.stop = &fake_stop_ok_for_sample_rate_test;
    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.pending_sample_rate_request = -1;

    bool ok = device_request_sample_rate(&dev, 3000000);

    EXPECT_TRUE(ok);
    EXPECT_EQ(dev.pending_sample_rate_request.load(), 3000000);
}

namespace {

// Fake driver hooks for device_apply_sample_rate() - a real, joinable RX thread is needed
// (device_apply_sample_rate() calls pthread_join() on it directly, matching the real driver's
// contract), so fake_run_rx_thread() blocks until fake_stop() signals it, mirroring
// rtlsdr_rx_thread()'s real blocking-until-canceled behavior closely enough for this to exercise
// the actual stop/join/reopen/restart sequence rather than mocking it away.
struct FakeReconfigurableDevData {
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> thread_running{false};
    std::atomic<int> init_call_count{0};
    // 0 = init always succeeds; N>0 = the Nth call (1-indexed) fails, every other call succeeds -
    // lets one test express "new rate fails, rollback succeeds" (fail_on_call=1) distinctly from
    // "every attempt fails" (fail_on_call=1 combined with never calling again, or see
    // fail_forever below).
    int fail_on_call = 0;
    bool fail_forever = false;
};

int fake_reconfig_init(input_t* const input) {
    FakeReconfigurableDevData* d = (FakeReconfigurableDevData*)input->dev_data;
    int call = d->init_call_count.fetch_add(1) + 1;
    if (d->fail_forever || call == d->fail_on_call) {
        return -1;
    }
    return 0;
}

int fake_reconfig_stop(input_t* const input) {
    FakeReconfigurableDevData* d = (FakeReconfigurableDevData*)input->dev_data;
    d->stop_requested.store(true, std::memory_order_release);
    return 0;
}

int fake_reconfig_stop_fails(input_t* const) {
    return -1;
}

void* fake_reconfig_run_rx_thread(void* arg) {
    input_t* input = (input_t*)arg;
    FakeReconfigurableDevData* d = (FakeReconfigurableDevData*)input->dev_data;
    d->thread_running.store(true, std::memory_order_release);
    while (!d->stop_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    d->thread_running.store(false, std::memory_order_release);
    return nullptr;
}

// Spawns fake_reconfig_run_rx_thread and waits for it to actually start, matching what
// input_start() does at real startup - device_apply_sample_rate() assumes a live, already-running
// RX thread exists to stop and join.
void start_fake_reconfig_rx_thread(input_t* input, FakeReconfigurableDevData* dev_data) {
    dev_data->stop_requested.store(false, std::memory_order_release);
    pthread_create(&input->rx_thread, NULL, input->run_rx_thread, input);
    while (!dev_data->thread_running.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

}  // namespace

class SampleRateApplyTest : public TestBaseClass {
   protected:
    FakeReconfigurableDevData dev_data;
    input_t input = {};
    channel_t channel = {};
    // freq_t embeds a Squelch, which - unlike this fixture's other plain-struct members - has a
    // real constructor that unconditionally calls debug_print() (Squelch::Squelch() ->
    // set_squelch_snr_threshold(), squelch.cpp). See ChannelRemoveTest's identical comment above
    // for the full explanation of why a stack-constructed `freq_t freq = {};` segfaults/corrupts
    // memory in this Debug-build unittest binary (debugf is never fopen()'d here) - calloc, not a
    // real C++ construction, sidesteps it the same way mk_freqlist() (config.cpp) already does.
    freq_t* freq = nullptr;
    device_t dev = {};
    unsigned char* old_buffer = nullptr;
    const size_t kOldBufSize = 4096;

    void SetUp() override {
        TestBaseClass::SetUp();
        input.driver_type = "rtlsdr";
        input.sample_rate = 2000000;
        input.centerfreq = 120000000;
        input.bytes_per_sample = 1;
        input.state = INPUT_RUNNING;
        input.init = &fake_reconfig_init;
        input.stop = &fake_reconfig_stop;
        input.run_rx_thread = &fake_reconfig_run_rx_thread;
        input.dev_data = &dev_data;
        old_buffer = (unsigned char*)XCALLOC(1, kOldBufSize);
        input.buffer = old_buffer;
        input.buf_size = kOldBufSize;
        input.bufs = 7;
        input.bufe = 42;

        freq = (freq_t*)calloc(1, sizeof(freq_t));
        freq->frequency = 120050000;
        channel.freqlist = freq;

        dev.input = &input;
        dev.mode = R_MULTICHANNEL;
        dev.channel_count = 1;
        dev.channels = &channel;
        static size_t bins[1];
        static size_t base_bins[1];
        bins[0] = base_bins[0] = 0;
        dev.bins = bins;
        dev.base_bins = base_bins;

        start_fake_reconfig_rx_thread(&input, &dev_data);
    }

    void TearDown() override {
        free(freq);
        TestBaseClass::TearDown();
    }

    // Only call when a real RX thread is still expected to be running (the success and
    // rollback-succeeded paths restart one; the "stop failed" path never actually stopped the
    // original one) - the "rollback also failed" path leaves nothing running to join.
    void stopAndJoinFinalRxThread() {
        dev_data.stop_requested.store(true, std::memory_order_release);
        pthread_join(input.rx_thread, NULL);
    }
};

TEST_F(SampleRateApplyTest, succeeds_reopens_swaps_buffer_and_recomputes_bins) {
    bool ok = device_apply_sample_rate(&dev, 3000000);

    EXPECT_TRUE(ok);
    EXPECT_EQ(input.sample_rate, 3000000);
    EXPECT_EQ(input.state, INPUT_RUNNING);
    EXPECT_EQ(input.bufs, 0u);
    EXPECT_EQ(input.bufe, 0u);
    EXPECT_EQ(input.buf_size, compute_input_buf_size(3000000, 1));
    EXPECT_NE(input.buffer, old_buffer);  // swapped to the newly allocated buffer
    // Bins recomputed for the new sample_rate, matching what parse-time startup would compute.
    EXPECT_EQ(dev.bins[0], compute_channel_bin(120050000, 120000000, 3000000, fft_size));

    stopAndJoinFinalRxThread();
    free(input.buffer);
}

TEST_F(SampleRateApplyTest, rolls_back_to_old_rate_when_new_rate_reopen_fails) {
    dev_data.fail_on_call = 1;  // first init() call (new rate) fails; second (rollback) succeeds

    bool ok = device_apply_sample_rate(&dev, 3000000);

    EXPECT_FALSE(ok);
    EXPECT_EQ(input.sample_rate, 2000000);  // rolled back
    EXPECT_EQ(input.state, INPUT_RUNNING);  // device survived - not marked failed
    EXPECT_EQ(input.buffer, old_buffer);    // old buffer never touched/freed
    EXPECT_EQ(input.buf_size, kOldBufSize);
    // Bins untouched - old_sample_rate's tuning never actually changed, so no recompute needed.
    EXPECT_EQ(dev.bins[0], 0u);

    stopAndJoinFinalRxThread();
    free(input.buffer);
}

TEST_F(SampleRateApplyTest, marks_input_failed_when_rollback_also_fails) {
    dev_data.fail_forever = true;

    bool ok = device_apply_sample_rate(&dev, 3000000);

    EXPECT_FALSE(ok);
    EXPECT_EQ(input.state, INPUT_FAILED);
    // No RX thread is running to restart - both reopen attempts failed - nothing to join here.
    free(input.buffer);  // old buffer was never freed by device_apply_sample_rate() in this path
}

TEST_F(SampleRateApplyTest, rejects_without_touching_state_when_stop_fails) {
    input.stop = &fake_reconfig_stop_fails;

    bool ok = device_apply_sample_rate(&dev, 3000000);

    EXPECT_FALSE(ok);
    EXPECT_EQ(input.sample_rate, 2000000);  // unchanged
    EXPECT_EQ(input.state, INPUT_RUNNING);
    EXPECT_EQ(dev_data.init_call_count.load(), 0);  // never even attempted - stop() failed first
    EXPECT_EQ(input.buffer, old_buffer);

    // The original RX thread was never actually stopped (stop() failed before touching it) - stop
    // and join it directly for cleanup, bypassing the fake stop() hook this test replaced.
    dev_data.stop_requested.store(true, std::memory_order_release);
    pthread_join(input.rx_thread, NULL);
    free(input.buffer);
}

TEST_F(LiveReconfigTest, channel_apply_disable_sets_enabled_false) {
    // channel_apply_disable()/channel_apply_enable() are the apply-side halves, exercised
    // directly here as pure functions (see channel_t::pending_enable_request's comment,
    // rtl_airband.h, for why these are only safe to call for real from the output thread).
    channel_t channel = {};
    channel.enabled = true;
    channel.output_count = 0;

    channel_apply_disable(&channel);
    EXPECT_FALSE(channel.enabled.load());
}

TEST_F(LiveReconfigTest, channel_apply_enable_sets_enabled_true) {
    channel_t channel = {};
    channel.enabled = false;
    channel.output_count = 0;

    channel_apply_enable(&channel);
    EXPECT_TRUE(channel.enabled.load());
}

TEST_F(LiveReconfigTest, channel_request_enable_posts_pending_request) {
    channel_t channel = {};
    channel.enabled = false;
    channel.output_count = 0;
    channel.pending_enable_request = -1;

    // A short timeout since nothing consumes the request in this unit test - just confirming
    // the request/apply split's request side does its one job (post the value) without
    // touching `enabled` itself.
    channel_request_enable(&channel, /*timeout_us=*/1000);
    EXPECT_EQ(channel.pending_enable_request.load(), 1);
    EXPECT_FALSE(channel.enabled.load());  // not applied - no output thread consumed it here
}

TEST_F(LiveReconfigTest, channel_request_disable_posts_pending_request) {
    channel_t channel = {};
    channel.enabled = true;
    channel.output_count = 0;
    channel.pending_enable_request = -1;

    channel_request_disable(&channel, /*timeout_us=*/1000);
    EXPECT_EQ(channel.pending_enable_request.load(), 0);
    EXPECT_TRUE(channel.enabled.load());
}

TEST_F(LiveReconfigTest, channel_request_confirms_when_consumed_promptly) {
    channel_t channel = {};
    channel.enabled = false;
    channel.output_count = 0;
    channel.pending_enable_request = -1;

    // Simulate the output thread consuming the request concurrently.
    std::thread consumer([&channel]() {
        while (channel.pending_enable_request.load(std::memory_order_acquire) == -1) {
            std::this_thread::yield();
        }
        channel_apply_enable(&channel);
        channel.pending_enable_request.store(-1, std::memory_order_release);
    });

    bool confirmed = channel_request_enable(&channel, /*timeout_us=*/500000);
    consumer.join();

    EXPECT_TRUE(confirmed);
    EXPECT_TRUE(channel.enabled.load());
}

class ConfigSnapshotTest : public TestBaseClass {
   protected:
    std::string write_config(const std::string& contents) {
        std::string path = temp_dir + "/test.conf";
        FILE* f = fopen(path.c_str(), "w");
        fwrite(contents.data(), 1, contents.size(), f);
        fclose(f);
        return path;
    }
};

TEST_F(ConfigSnapshotTest, parses_basic_multichannel_device) {
    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  gain = 30;
  centerfreq = 120000000;
  sample_rate = 2048000;
  bandwidth = 2000000;
  correction = 80;
  channels: ( { freq = 120.000000; enabled = true; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); } );
});
)");
    ConfigSnapshot snapshot;
    std::string error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &error)) << error;

    ASSERT_EQ(snapshot.devices.size(), 1u);
    EXPECT_EQ(snapshot.devices[0].type, "rtlsdr");
    EXPECT_EQ(snapshot.devices[0].mode, R_MULTICHANNEL);
    EXPECT_EQ(snapshot.devices[0].centerfreq, 120000000);
    EXPECT_EQ(snapshot.devices[0].sample_rate, 2048000);
    ASSERT_TRUE(snapshot.devices[0].has_gain);
    EXPECT_FLOAT_EQ(snapshot.devices[0].gain, 30.0f);
    ASSERT_TRUE(snapshot.devices[0].has_bandwidth);
    EXPECT_EQ(snapshot.devices[0].bandwidth, 2000000);
    ASSERT_TRUE(snapshot.devices[0].has_correction);
    EXPECT_EQ(snapshot.devices[0].correction, 80);
    ASSERT_EQ(snapshot.devices[0].channel_enabled.size(), 1u);
    EXPECT_TRUE(snapshot.devices[0].channel_enabled[0]);
}

TEST_F(ConfigSnapshotTest, bandwidth_absent_from_config_reports_has_bandwidth_false) {
    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  gain = 30;
  centerfreq = 120000000;
  channels: ( { freq = 120.000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); } );
});
)");
    ConfigSnapshot snapshot;
    std::string error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &error)) << error;

    ASSERT_EQ(snapshot.devices.size(), 1u);
    EXPECT_FALSE(snapshot.devices[0].has_bandwidth);
    EXPECT_FALSE(snapshot.devices[0].has_correction);
}

TEST_F(ConfigSnapshotTest, channel_enabled_defaults_true_when_absent) {
    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  gain = 30;
  centerfreq = 120000000;
  channels: ( { freq = 120.000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); } );
});
)");
    ConfigSnapshot snapshot;
    std::string error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &error)) << error;
    ASSERT_EQ(snapshot.devices[0].channel_enabled.size(), 1u);
    EXPECT_TRUE(snapshot.devices[0].channel_enabled[0]);
}

TEST_F(ConfigSnapshotTest, skips_disabled_devices_and_channels) {
    std::string path = write_config(R"(
devices:
(
{
  type = "rtlsdr";
  index = 0;
  gain = 30;
  centerfreq = 120000000;
  disable = true;
  channels: ( { freq = 120.000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); } );
},
{
  type = "rtlsdr";
  index = 1;
  gain = 30;
  centerfreq = 121000000;
  channels: (
    { freq = 121.000000; disable = true; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); },
    { freq = 121.100000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "y"; } ); }
  );
}
);
)");
    ConfigSnapshot snapshot;
    std::string error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &error)) << error;

    // Only the non-disabled device shows up, matching parse_devices()'s own indexing convention.
    ASSERT_EQ(snapshot.devices.size(), 1u);
    EXPECT_EQ(snapshot.devices[0].centerfreq, 121000000);
    // Only the non-disabled channel shows up within it.
    ASSERT_EQ(snapshot.devices[0].channel_enabled.size(), 1u);
}

TEST_F(ConfigSnapshotTest, r_scan_mode_has_no_centerfreq_and_string_gain_is_not_numeric) {
    std::string path = write_config(R"(
devices:
({
  type = "soapysdr";
  device_string = "driver=fake";
  mode = "scan";
  gain = "LNA=10,VGA=5";
  channels: ( { freqs = ( 120.000000, 121.000000 ); outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); } );
});
)");
    ConfigSnapshot snapshot;
    std::string error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &error)) << error;

    ASSERT_EQ(snapshot.devices.size(), 1u);
    EXPECT_EQ(snapshot.devices[0].mode, R_SCAN);
    EXPECT_EQ(snapshot.devices[0].centerfreq, 0);  // not captured for R_SCAN
    EXPECT_FALSE(snapshot.devices[0].has_gain);    // string form isn't a single diffable number
}

TEST_F(ConfigSnapshotTest, mixer_enabled_keyword_and_disable_keyword) {
    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  gain = 30;
  centerfreq = 120000000;
  channels: ( { freq = 120.000000; outputs: ( { type = "mixer"; name = "mix1"; } ); } );
});
mixers: {
  mix1: { enabled = false; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); };
  mix2: { disable = true; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "y"; } ); };
};
)");
    ConfigSnapshot snapshot;
    std::string error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &error)) << error;

    // mix2 is "disable"d at parse time - never shows up, same as a disabled device/channel.
    ASSERT_EQ(snapshot.mixers.size(), 1u);
    EXPECT_EQ(snapshot.mixers[0].name, "mix1");
    EXPECT_FALSE(snapshot.mixers[0].enabled);
}

TEST_F(ConfigSnapshotTest, reports_error_on_missing_file) {
    ConfigSnapshot snapshot;
    std::string error;
    EXPECT_FALSE(parse_config_snapshot(temp_dir + "/does_not_exist.conf", &snapshot, &error));
    EXPECT_FALSE(error.empty());
}

TEST_F(ConfigSnapshotTest, reports_error_on_malformed_config) {
    std::string path = write_config("devices: ( { this is not valid libconfig");
    ConfigSnapshot snapshot;
    std::string error;
    EXPECT_FALSE(parse_config_snapshot(path, &snapshot, &error));
    EXPECT_FALSE(error.empty());
}

class DiffApplyTest : public LiveReconfigTest {
   protected:
    void SetUp() override {
        LiveReconfigTest::SetUp();
        devices = nullptr;
        device_count = 0;
        mixers = nullptr;
        mixer_count = 0;
        // A single process-wide flag (see its declaration in rtl_airband.h) - must be reset per
        // test or a mixer capacity test earlier in the binary leaks "finalized" into an unrelated
        // later test that expects startup-style (pre-finalize) mixer_connect_input() growth.
        mixer_capacity_finalized = false;
    }
};

TEST_F(DiffApplyTest, device_count_mismatch_is_requires_restart_and_applies_nothing_else) {
    device_t dev = {};
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;  // zero devices - mismatches live device_count of 1
    DiffResult result = compute_and_apply_diff(snapshot);

    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("device count changed"), std::string::npos);
    EXPECT_TRUE(result.applied.empty());
}

TEST_F(DiffApplyTest, channel_enabled_diff_calls_channel_enable_disable) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    // channel_t has non-copyable std::atomic members, so construct the array in place rather
    // than copy-initializing from locals.
    channel_t chans[2] = {};
    chans[0].enabled = true;
    chans[0].config_signature = strdup("chan0-signature");
    chans[1].enabled = false;
    chans[1].config_signature = strdup("chan1-signature");
    // "No request pending", as config.cpp sets it. Left zero-initialized, 0 reads as a pending
    // disable: the consumer thread below would disable chans[0] before compute_and_apply_diff()
    // looks at it, leaving only one change to report (a race; consistently lost under QEMU).
    chans[0].pending_enable_request = -1;
    chans[1].pending_enable_request = -1;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 2;
    dev.channels = chans;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 2;
    dsnap.centerfreq = 120000000;  // unchanged - no retune expected
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = false;
    dsnap.channel_enabled = {false, true};  // flip both
    // Definitions unchanged - only "enabled" differs (excluded from the signature, see
    // build_channel_identity_signature()'s comment, config.cpp), so this must stay on the cheap
    // enable/disable path below rather than triggering a tear-down/replace.
    dsnap.channel_signature = {"chan0-signature", "chan1-signature"};
    snapshot.devices.push_back(dsnap);

    // compute_and_apply_diff() only posts requests (see live_reconfig.h's comment on why - the
    // apply side must run on the owning output thread, not here) and then polls for
    // consumption; simulate that thread so this test stays fast and still exercises the full
    // request -> apply round trip rather than just the posting half.
    std::atomic<bool> stop_consumer{false};
    std::thread consumer([&]() {
        while (!stop_consumer.load()) {
            for (int k = 0; k < 2; k++) {
                int pending = chans[k].pending_enable_request.exchange(-1, std::memory_order_acq_rel);
                if (pending == 1) {
                    channel_apply_enable(&chans[k]);
                } else if (pending == 0) {
                    channel_apply_disable(&chans[k]);
                }
            }
            std::this_thread::yield();
        }
    });

    DiffResult result = compute_and_apply_diff(snapshot);
    stop_consumer.store(true);
    consumer.join();

    EXPECT_FALSE(dev.channels[0].enabled.load());
    EXPECT_TRUE(dev.channels[1].enabled.load());
    EXPECT_EQ(result.applied.size(), 2u);
    EXPECT_EQ(dev.pending_centerfreq_request.load(), -1);  // centerfreq unchanged, no request posted
}

TEST_F(DiffApplyTest, centerfreq_diff_posts_a_retune_request_and_waits_for_confirmation) {
    // A background thread stands in for the demod thread's consumption loop
    // (demodulate(), rtl_airband.cpp), so this exercises the full request -> apply round trip
    // rather than just the posting half, matching the enable/disable tests above.
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_centerfreq = &fake_set_centerfreq_ok;
    input.dev_data = &fake_dev_data;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    dev.pending_centerfreq_request = -1;
    dev.centerfreq_apply_failed = false;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120050000;  // changed
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    std::atomic<bool> stop_consumer{false};
    std::thread consumer([&]() {
        while (!stop_consumer.load()) {
            int pending = dev.pending_centerfreq_request.load(std::memory_order_acquire);
            if (pending != -1) {
                bool ok = device_apply_retune(&dev, pending);
                dev.centerfreq_apply_failed.store(!ok, std::memory_order_release);
                dev.pending_centerfreq_request.store(-1, std::memory_order_release);
            }
            std::this_thread::yield();
        }
    });

    DiffResult result = compute_and_apply_diff(snapshot);
    stop_consumer.store(true);
    consumer.join();

    EXPECT_EQ(input.centerfreq, 120050000);
    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("centerfreq -> 120050000"), std::string::npos);
    EXPECT_EQ(result.applied[0].find("not yet confirmed"), std::string::npos);
    EXPECT_TRUE(result.skipped_requires_restart.empty());
}

TEST_F(DiffApplyTest, centerfreq_diff_reports_transient_hardware_failure_instead_of_claiming_success) {
    // Regression test for the reload_diff under-reporting bug: previously this branch reported
    // "applied: centerfreq -> X" purely because device_request_retune() posted the request, with
    // no check of whether the demod thread's actual hardware call succeeded - so a real, ongoing
    // i2c retune failure (matching production symptoms) looked identical to success in the
    // response, and the device was silently left on its old centerfreq.
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_centerfreq = &fake_set_centerfreq_fails;
    input.dev_data = &fake_dev_data;
    input.centerfreq_retune_failure_count = 0;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    dev.pending_centerfreq_request = -1;
    dev.centerfreq_apply_failed = false;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120050000;  // changed
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    std::atomic<bool> stop_consumer{false};
    std::thread consumer([&]() {
        while (!stop_consumer.load()) {
            int pending = dev.pending_centerfreq_request.load(std::memory_order_acquire);
            if (pending != -1) {
                bool ok = device_apply_retune(&dev, pending);
                dev.centerfreq_apply_failed.store(!ok, std::memory_order_release);
                dev.pending_centerfreq_request.store(-1, std::memory_order_release);
            }
            std::this_thread::yield();
        }
    });

    DiffResult result = compute_and_apply_diff(snapshot);
    stop_consumer.store(true);
    consumer.join();

    EXPECT_EQ(input.centerfreq, 120000000);  // still on the old centerfreq
    EXPECT_TRUE(result.applied.empty());
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("centerfreq"), std::string::npos);
    EXPECT_NE(result.skipped_requires_restart[0].find("no restart needed"), std::string::npos);
}

TEST_F(DiffApplyTest, centerfreq_diff_reports_pending_when_not_yet_confirmed) {
    // No consumer thread here - the request is never picked up, so this exercises
    // device_confirm_retune()'s timeout branch. Deliberately slow (~500ms, the default timeout)
    // rather than mocked away, matching how the equivalent channel-enable/disable timeout paths
    // are already exercised elsewhere in this file.
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120050000;  // changed
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.pending_centerfreq_request.load(), 120050000);  // still pending, never consumed
    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("centerfreq -> 120050000"), std::string::npos);
    EXPECT_NE(result.applied[0].find("not yet confirmed"), std::string::npos);
}

TEST_F(DiffApplyTest, driver_type_change_is_requires_restart) {
    input_t input = {};
    input.driver_type = "rtlsdr";

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "soapysdr";  // different from live "rtlsdr"
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("driver type changed"), std::string::npos);
    EXPECT_TRUE(result.applied.empty());
}

TEST_F(DiffApplyTest, mode_change_is_requires_restart) {
    input_t input = {};
    input.driver_type = "rtlsdr";

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_SCAN;  // different from live R_MULTICHANNEL
    dsnap.channel_count = 0;
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("mode changed"), std::string::npos);
}

// sample_rate is live-appliable now (device_apply_sample_rate(), see the SampleRateApplyTest
// fixture above for direct coverage of that function) - these exercise the same fake
// init/stop/run_rx_thread hooks and a real background thread standing in for the demod thread's
// consumption loop, matching the centerfreq_diff_* tests' pattern above.
TEST_F(DiffApplyTest, sample_rate_diff_posts_request_and_reports_success) {
    FakeReconfigurableDevData fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.bytes_per_sample = 1;
    input.state = INPUT_RUNNING;
    input.init = &fake_reconfig_init;
    input.stop = &fake_reconfig_stop;
    input.run_rx_thread = &fake_reconfig_run_rx_thread;
    input.dev_data = &fake_dev_data;
    input.buffer = (unsigned char*)XCALLOC(1, 4096);
    input.buf_size = 4096;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    dev.pending_sample_rate_request = -1;
    devices = &dev;
    device_count = 1;

    start_fake_reconfig_rx_thread(&input, &fake_dev_data);

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;
    dsnap.sample_rate = 2400000;  // changed
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    std::atomic<bool> stop_consumer{false};
    std::thread consumer([&]() {
        while (!stop_consumer.load()) {
            int pending = dev.pending_sample_rate_request.load(std::memory_order_acquire);
            if (pending != -1) {
                bool ok = device_apply_sample_rate(&dev, pending);
                dev.sample_rate_apply_failed.store(!ok, std::memory_order_release);
                dev.pending_sample_rate_request.store(-1, std::memory_order_release);
            }
            std::this_thread::yield();
        }
    });

    DiffResult result = compute_and_apply_diff(snapshot);
    stop_consumer.store(true);
    consumer.join();

    EXPECT_EQ(input.sample_rate, 2400000);
    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("sample_rate -> 2400000"), std::string::npos);
    EXPECT_TRUE(result.skipped_requires_restart.empty());

    fake_dev_data.stop_requested.store(true, std::memory_order_release);
    pthread_join(input.rx_thread, NULL);
    free(input.buffer);
}

TEST_F(DiffApplyTest, sample_rate_diff_reports_rollback_message_on_failure) {
    FakeReconfigurableDevData fake_dev_data;
    fake_dev_data.fail_forever = true;  // both the new-rate attempt and the rollback fail

    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.bytes_per_sample = 1;
    input.state = INPUT_RUNNING;
    input.init = &fake_reconfig_init;
    input.stop = &fake_reconfig_stop;
    input.run_rx_thread = &fake_reconfig_run_rx_thread;
    input.dev_data = &fake_dev_data;
    input.buffer = (unsigned char*)XCALLOC(1, 4096);
    input.buf_size = 4096;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    dev.pending_sample_rate_request = -1;
    devices = &dev;
    device_count = 1;

    start_fake_reconfig_rx_thread(&input, &fake_dev_data);

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;
    dsnap.sample_rate = 2400000;  // changed
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    std::atomic<bool> stop_consumer{false};
    std::thread consumer([&]() {
        while (!stop_consumer.load()) {
            int pending = dev.pending_sample_rate_request.load(std::memory_order_acquire);
            if (pending != -1) {
                bool ok = device_apply_sample_rate(&dev, pending);
                dev.sample_rate_apply_failed.store(!ok, std::memory_order_release);
                dev.pending_sample_rate_request.store(-1, std::memory_order_release);
            }
            std::this_thread::yield();
        }
    });

    DiffResult result = compute_and_apply_diff(snapshot);
    stop_consumer.store(true);
    consumer.join();

    EXPECT_EQ(input.state, INPUT_FAILED);  // both attempts failed - genuinely down
    EXPECT_TRUE(result.applied.empty());
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("sample_rate"), std::string::npos);
    EXPECT_NE(result.skipped_requires_restart[0].find("no restart needed if rolled back"), std::string::npos);

    free(input.buffer);  // rx_thread never restarted - nothing to join
}

TEST_F(DiffApplyTest, sample_rate_unchanged_does_not_post_a_request) {
    // Unlike gain/bandwidth/correction (no live-readable current value, always reapplied), this
    // is a real diff - re-requesting the CURRENT sample_rate on every reload_diff would be a real
    // cost (a full RX-thread stop/reopen/restart cycle) for zero benefit.
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    dev.pending_sample_rate_request = -1;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;
    dsnap.sample_rate = 2000000;  // unchanged
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.pending_sample_rate_request.load(), -1);
    EXPECT_TRUE(result.applied.empty());
    EXPECT_TRUE(result.skipped_requires_restart.empty());
}

TEST_F(DiffApplyTest, gain_not_supported_by_driver_is_silently_skipped_not_reported) {
    // set_gain == NULL (the rtlsdr driver leaves it unset in this fixture on purpose) -
    // input_set_gain() returns ENOTSUP, which reload_diff treats as "nothing to report", not a
    // failure worth flagging on every single reload.
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_gain = nullptr;
    input.dev_data = &fake_dev_data;  // input_set_gain() asserts this non-null before the set_gain-null check

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = true;
    dsnap.gain = 30.0f;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_TRUE(result.applied.empty());
    EXPECT_TRUE(result.skipped_requires_restart.empty());
}

int fake_set_gain_ok(input_t* const, float const) {
    return 0;
}

TEST_F(DiffApplyTest, gain_applied_via_input_set_gain_is_reported) {
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_gain = &fake_set_gain_ok;
    input.dev_data = &fake_dev_data;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = true;
    dsnap.gain = 30.0f;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("gain"), std::string::npos);
}

int fake_set_gain_fails(input_t* const, float const) {
    return -1;
}

TEST_F(DiffApplyTest, gain_set_hardware_failure_does_not_mark_input_failed) {
    // The regression this test guards against - see input_set_gain()'s comment
    // (input-common.cpp): a failed gain change must not mark the device dead, unlike the old
    // behavior that set input->state = INPUT_FAILED on any driver error.
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_gain = &fake_set_gain_fails;
    input.dev_data = &fake_dev_data;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = true;
    dsnap.gain = 30.0f;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(input.state, INPUT_RUNNING);
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("gain"), std::string::npos);
    // Same "retryable, not a real restart requirement" wording centerfreq/sample_rate use -
    // consumers (e.g. rtl-airband-panel's LiveApplyBanner) key off this exact substring to
    // separate genuinely-restart-required entries from ones that just need a retry.
    EXPECT_NE(result.skipped_requires_restart[0].find("no restart needed"), std::string::npos);
}

TEST_F(DiffApplyTest, bandwidth_not_supported_by_driver_is_silently_skipped_not_reported) {
    // set_bandwidth == NULL - input_set_bandwidth() returns ENOTSUP, which reload_diff treats as
    // "nothing to report", not a failure worth flagging on every single reload. Mirrors the
    // equivalent gain test above.
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_bandwidth = nullptr;
    input.dev_data = &fake_dev_data;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;
    dsnap.sample_rate = 2000000;
    dsnap.has_bandwidth = true;
    dsnap.bandwidth = 2000000;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_TRUE(result.applied.empty());
    EXPECT_TRUE(result.skipped_requires_restart.empty());
}

int fake_set_bandwidth_ok(input_t* const, int const) {
    return 0;
}

TEST_F(DiffApplyTest, bandwidth_applied_via_input_set_bandwidth_is_reported) {
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_bandwidth = &fake_set_bandwidth_ok;
    input.dev_data = &fake_dev_data;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;
    dsnap.sample_rate = 2000000;
    dsnap.has_bandwidth = true;
    dsnap.bandwidth = 2000000;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("bandwidth -> 2000000"), std::string::npos);
}

int fake_set_bandwidth_fails(input_t* const, int const) {
    return -1;
}

TEST_F(DiffApplyTest, bandwidth_set_hardware_failure_does_not_mark_input_failed) {
    // Mirrors gain_set_hardware_failure_does_not_mark_input_failed above - a failed bandwidth
    // change must not mark the device dead (input_set_bandwidth()'s comment, input-common.cpp).
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_bandwidth = &fake_set_bandwidth_fails;
    input.dev_data = &fake_dev_data;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;
    dsnap.sample_rate = 2000000;
    dsnap.has_bandwidth = true;
    dsnap.bandwidth = 2000000;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(input.state, INPUT_RUNNING);
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("bandwidth"), std::string::npos);
    EXPECT_NE(result.skipped_requires_restart[0].find("no restart needed"), std::string::npos);
}

TEST_F(DiffApplyTest, correction_not_supported_by_driver_is_silently_skipped_not_reported) {
    // Mirrors bandwidth_not_supported_by_driver_is_silently_skipped_not_reported above.
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_correction = nullptr;
    input.dev_data = &fake_dev_data;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;
    dsnap.sample_rate = 2000000;
    dsnap.has_correction = true;
    dsnap.correction = 80;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_TRUE(result.applied.empty());
    EXPECT_TRUE(result.skipped_requires_restart.empty());
}

int fake_set_correction_ok(input_t* const, int const) {
    return 0;
}

TEST_F(DiffApplyTest, correction_applied_via_input_set_correction_is_reported) {
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_correction = &fake_set_correction_ok;
    input.dev_data = &fake_dev_data;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;
    dsnap.sample_rate = 2000000;
    dsnap.has_correction = true;
    dsnap.correction = 80;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("correction -> 80"), std::string::npos);
}

namespace {
int auto_ppm_fake_set_calls = 0;
int auto_ppm_fake_set_correction(input_t* const, int const) {
    auto_ppm_fake_set_calls++;
    return 0;
}
}  // namespace

class AutoPpmDiffTest : public DiffApplyTest {
   protected:
    void SetUp() override {
        DiffApplyTest::SetUp();
        auto_ppm_fake_set_calls = 0;
        input.driver_type = "rtlsdr";
        input.sample_rate = 2000000;
        input.centerfreq = 120000000;
        input.state = INPUT_RUNNING;
        input.set_correction = &auto_ppm_fake_set_correction;
        input.dev_data = &fake_dev_data;
        dev.input = &input;
        dev.mode = R_MULTICHANNEL;
        dev.channel_count = 0;
        devices = &dev;
        device_count = 1;

        dsnap.type = "rtlsdr";
        dsnap.mode = R_MULTICHANNEL;
        dsnap.channel_count = 0;
        dsnap.centerfreq = 120000000;
        dsnap.sample_rate = 2000000;
        dsnap.has_correction = true;
        dsnap.correction = 4;
        dsnap.has_auto_ppm = true;
        dsnap.auto_ppm = true;
    }
    void TearDown() override {
        delete dev.auto_ppm;
        DiffApplyTest::TearDown();
    }
    DiffResult run() {
        ConfigSnapshot snapshot;
        snapshot.devices.push_back(dsnap);
        return compute_and_apply_diff(snapshot);
    }
    void configure_auto_ppm(bool enabled) {
        dev.auto_ppm = new auto_ppm_device();
        dev.auto_ppm->enabled = enabled;
        dev.auto_ppm->state.base_correction = 4;
    }
    int fake_dev_data = 0;
    input_t input{};
    device_t dev{};
    DeviceConfigSnapshot dsnap;
};

TEST_F(AutoPpmDiffTest, unchanged_correction_is_left_to_auto_ppm) {
    configure_auto_ppm(true);
    DiffResult result = run();
    EXPECT_EQ(auto_ppm_fake_set_calls, 0);
    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("left to auto_ppm"), std::string::npos);
    EXPECT_TRUE(result.skipped_requires_restart.empty());
}

TEST_F(AutoPpmDiffTest, changed_correction_is_applied_as_the_new_base) {
    configure_auto_ppm(true);
    dsnap.correction = 9;
    DiffResult result = run();
    EXPECT_EQ(auto_ppm_fake_set_calls, 1);
    EXPECT_EQ(dev.auto_ppm->state.base_correction, 9);
    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("correction -> 9"), std::string::npos);
}

TEST_F(AutoPpmDiffTest, turning_auto_ppm_off_reapplies_the_config_correction) {
    configure_auto_ppm(true);
    dsnap.auto_ppm = false;
    DiffResult result = run();
    EXPECT_FALSE(dev.auto_ppm->enabled);
    EXPECT_EQ(auto_ppm_fake_set_calls, 1);
    ASSERT_EQ(result.applied.size(), 2u);
    EXPECT_NE(result.applied[0].find("auto_ppm -> false"), std::string::npos);
    EXPECT_NE(result.applied[1].find("correction -> 4"), std::string::npos);
}

TEST_F(AutoPpmDiffTest, enabling_auto_ppm_live_needs_it_configured_at_startup) {
    DiffResult result = run();
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("auto_ppm"), std::string::npos);
}

TEST_F(AutoPpmDiffTest, changed_parameters_need_a_restart) {
    configure_auto_ppm(true);
    dsnap.auto_ppm_params.max_step = 2;
    DiffResult result = run();
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("auto_ppm_max_step"), std::string::npos);
    EXPECT_EQ(dev.auto_ppm->params.max_step, 5);
}

TEST_F(AutoPpmDiffTest, correction_outside_the_limit_is_not_applied) {
    configure_auto_ppm(true);
    dsnap.correction = 150;
    DiffResult result = run();
    EXPECT_EQ(auto_ppm_fake_set_calls, 0);
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("outside auto_ppm_max_ppm"), std::string::npos);
}

TEST_F(ConfigSnapshotTest, auto_ppm_keys_are_captured) {
    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  centerfreq = 120000000;
  auto_ppm = true;
  auto_ppm_max_ppm = 40;
  channels: ( { freq = 120.100000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); } );
});
)");
    ConfigSnapshot snapshot;
    std::string error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &error)) << error;
    ASSERT_EQ(snapshot.devices.size(), 1u);
    EXPECT_TRUE(snapshot.devices[0].has_auto_ppm);
    EXPECT_TRUE(snapshot.devices[0].auto_ppm);
    EXPECT_EQ(snapshot.devices[0].auto_ppm_params.max_ppm, 40);
    EXPECT_TRUE(snapshot.devices[0].auto_ppm_params_error.empty());
}

int fake_set_correction_fails(input_t* const, int const) {
    return -1;
}

TEST_F(DiffApplyTest, correction_set_hardware_failure_does_not_mark_input_failed) {
    // Mirrors bandwidth_set_hardware_failure_does_not_mark_input_failed above - a failed
    // correction change must not mark the device dead (input_set_correction()'s comment,
    // input-common.cpp).
    int fake_dev_data;
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;
    input.state = INPUT_RUNNING;
    input.set_correction = &fake_set_correction_fails;
    input.dev_data = &fake_dev_data;

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;
    dsnap.sample_rate = 2000000;
    dsnap.has_correction = true;
    dsnap.correction = 80;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(input.state, INPUT_RUNNING);
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("correction"), std::string::npos);
    EXPECT_NE(result.skipped_requires_restart[0].find("no restart needed"), std::string::npos);
}

TEST_F(DiffApplyTest, mixer_count_mismatch_is_requires_restart) {
    ConfigSnapshot snapshot;
    snapshot.mixers.push_back({"mix1", true});
    mixer_count = 0;  // live has none

    DiffResult result = compute_and_apply_diff(snapshot);

    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("mixer count changed"), std::string::npos);
}

TEST_F(DiffApplyTest, mixer_enabled_diff_calls_mixer_enable_disable) {
    mixer_t mixer = {};
    mixer.name = "mix1";
    mixer.enabled = true;
    mixer.pending_enable_request = -1;
    mixer.input_count = 0;
    mixer.channel.enabled = true;
    mixer.channel.output_count = 0;
    mixers = &mixer;
    mixer_count = 1;

    ConfigSnapshot snapshot;
    snapshot.mixers.push_back({"mix1", false});  // flip to disabled

    // Same rationale as the channel_enabled_diff test above - simulate the owning output thread
    // consuming the request so this stays fast and exercises the full round trip.
    std::atomic<bool> stop_consumer{false};
    std::thread consumer([&]() {
        while (!stop_consumer.load()) {
            int pending = mixer.pending_enable_request.exchange(-1, std::memory_order_acq_rel);
            if (pending == 1) {
                mixer_enable(&mixer);
            } else if (pending == 0) {
                mixer_disable(&mixer);
            }
            std::this_thread::yield();
        }
    });

    DiffResult result = compute_and_apply_diff(snapshot);
    stop_consumer.store(true);
    consumer.join();

    EXPECT_FALSE(mixer.enabled);
    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("mix1"), std::string::npos);
}

// remote_inputs has no live-apply primitive - unlike name/enabled, a changed remote_inputs block
// must be positively detected and reported under skipped_requires_restart. Before this, it was
// silently never inspected at all - see mixer_t::remote_inputs_signature's comment (rtl_airband.h).
TEST_F(DiffApplyTest, remote_inputs_changed_is_requires_restart_and_applies_nothing) {
    mixer_t mixer = {};
    mixer.name = "mix1";
    mixer.enabled = true;
    mixer.pending_enable_request = -1;
    mixer.input_count = 0;
    mixer.channel.enabled = true;
    mixer.channel.output_count = 0;
    mixer.remote_inputs_signature = strdup("{stream_id=1;listen_path=\"/tmp/a.sock\";}");
    mixers = &mixer;
    mixer_count = 1;

    ConfigSnapshot snapshot;
    MixerConfigSnapshot snap;
    snap.name = "mix1";
    snap.enabled = true;                                                          // unchanged - isolates the remote_inputs check from the enabled check
    snap.remote_inputs_signature = "{stream_id=2;listen_path=\"/tmp/a.sock\";}";  // stream_id changed
    snapshot.mixers.push_back(snap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_TRUE(result.applied.empty());
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("remote_inputs changed"), std::string::npos);
}

TEST_F(DiffApplyTest, remote_inputs_unchanged_reports_nothing) {
    mixer_t mixer = {};
    mixer.name = "mix1";
    mixer.enabled = true;
    mixer.pending_enable_request = -1;
    mixer.input_count = 0;
    mixer.channel.enabled = true;
    mixer.channel.output_count = 0;
    mixer.remote_inputs_signature = strdup("{stream_id=1;listen_path=\"/tmp/a.sock\";}");
    mixers = &mixer;
    mixer_count = 1;

    ConfigSnapshot snapshot;
    MixerConfigSnapshot snap;
    snap.name = "mix1";
    snap.enabled = true;
    snap.remote_inputs_signature = "{stream_id=1;listen_path=\"/tmp/a.sock\";}";  // identical
    snapshot.mixers.push_back(snap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_TRUE(result.applied.empty());
    EXPECT_TRUE(result.skipped_requires_restart.empty());
}

// A hand-built mixer_t fixture that predates this field (remote_inputs_signature left at its
// zero-initialized NULL, same as every other pre-existing DiffApplyTest mixer case above) must
// not spuriously report a change - only parse_mixers() (config.cpp) ever sets this in a real
// process, so a NULL live signature means "not observed," not "definitely absent."
TEST_F(DiffApplyTest, remote_inputs_signature_null_on_live_mixer_skips_the_check) {
    mixer_t mixer = {};
    mixer.name = "mix1";
    mixer.enabled = true;
    mixer.pending_enable_request = -1;
    mixer.input_count = 0;
    mixer.channel.enabled = true;
    mixer.channel.output_count = 0;
    // mixer.remote_inputs_signature deliberately left NULL
    mixers = &mixer;
    mixer_count = 1;

    ConfigSnapshot snapshot;
    MixerConfigSnapshot snap;
    snap.name = "mix1";
    snap.enabled = true;
    snap.remote_inputs_signature = "{stream_id=1;listen_path=\"/tmp/a.sock\";}";  // differs from "<absent>"
    snapshot.mixers.push_back(snap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_TRUE(result.applied.empty());
    EXPECT_TRUE(result.skipped_requires_restart.empty());
}

// Dynamic channel add: compute_and_apply_diff() detecting/applying a config file's channel count
// growing for a device (a pure tail append, within dev->channel_capacity's pre-reserved
// headroom - see rtl_airband.h's comment on channel_capacity). Unlike the other DiffApplyTest
// cases above, these need a real raw_channels_setting/raw_channel_indices, which only
// parse_config_snapshot() populates from an actual file - hence write_config() here too.
class ChannelAppendTest : public DiffApplyTest {
   protected:
    std::string write_config(const std::string& contents) {
        std::string path = temp_dir + "/test.conf";
        FILE* f = fopen(path.c_str(), "w");
        fwrite(contents.data(), 1, contents.size(), f);
        fclose(f);
        return path;
    }
};

TEST_F(ChannelAppendTest, appends_one_new_channel_within_capacity) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    // index 0 is the already-live channel; index 1 is pre-reserved (zeroed) capacity, exactly as
    // parse_devices() would leave it after sizing for reserve_channels = 1.
    channel_t chans[2] = {};
    chans[0].enabled = true;
    size_t bins[2] = {0, 0};
    size_t base_bins[2] = {0, 0};

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 1;
    dev.channel_capacity = 2;
    dev.channels = chans;
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); },
    { freq = 120050000; label = "new-chan"; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "y"; } ); }
  );
});
)");
    ConfigSnapshot snapshot;
    std::string parse_error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &parse_error)) << parse_error;
    // Channel 0's definition is unchanged between "what's live" and "what the file now says" -
    // give it the signature parse_channel() would have set at its own (real) parse time, so
    // compute_and_apply_diff()'s common-prefix check correctly treats it as untouched rather than
    // (having no signature to compare at all) assuming it diverged too.
    chans[0].config_signature = strdup(snapshot.devices[0].channel_signature[0].c_str());

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.channel_count.load(), 2);
    EXPECT_TRUE(result.skipped_requires_restart.empty());
    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("added 1 channel"), std::string::npos);
    ASSERT_EQ(chans[1].freq_count, 1);
    EXPECT_EQ(chans[1].freqlist[0].frequency, 120050000);
    EXPECT_STREQ(chans[1].freqlist[0].label, "new-chan");
    EXPECT_EQ(chans[1].output_count, 1);
    // The existing (index 0) channel must be untouched.
    EXPECT_EQ(chans[0].output_count, 0);
}

TEST_F(ChannelAppendTest, appends_multiple_new_channels_within_capacity) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    channel_t chans[3] = {};
    size_t bins[3] = {0, 0, 0};
    size_t base_bins[3] = {0, 0, 0};

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 0;
    dev.channel_capacity = 3;
    dev.channels = chans;
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "a"; } ); },
    { freq = 120050000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "b"; } ); },
    { freq = 120100000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "c"; } ); }
  );
});
)");
    ConfigSnapshot snapshot;
    std::string parse_error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &parse_error)) << parse_error;

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.channel_count.load(), 3);
    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("added 3 channel"), std::string::npos);
    EXPECT_EQ(chans[0].freqlist[0].frequency, 120000000);
    EXPECT_EQ(chans[1].freqlist[0].frequency, 120050000);
    EXPECT_EQ(chans[2].freqlist[0].frequency, 120100000);
}

TEST_F(ChannelAppendTest, exceeding_reserved_capacity_is_requires_restart_and_applies_nothing) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    channel_t chans[1] = {};
    chans[0].enabled = true;
    size_t bins[1] = {0};
    size_t base_bins[1] = {0};

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 1;
    dev.channel_capacity = 1;  // no reserve_channels headroom
    dev.channels = chans;
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); },
    { freq = 120050000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "y"; } ); }
  );
});
)");
    ConfigSnapshot snapshot;
    std::string parse_error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &parse_error)) << parse_error;
    chans[0].config_signature = strdup(snapshot.devices[0].channel_signature[0].c_str());

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.channel_count.load(), 1);  // unchanged
    EXPECT_TRUE(result.applied.empty());
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("reserve_channels"), std::string::npos);
}

// A dynamically-appended channel can itself declare a `type = "mixer"` output pointing at an
// already-running mixer - this is what previously reached the unguarded XREALLOC growth in
// mixer_connect_input() (see mixer_t::input_capacity's comment, rtl_airband.h). mixer_finalize_
// capacity() is called explicitly here to simulate the post-parse_devices()/pre-thread-start state
// main() would have left the mixer in, the same way this fixture already hand-builds device_t as
// parse_devices() would have left it rather than calling parse_devices() itself.
TEST_F(ChannelAppendTest, appends_channel_with_mixer_output_within_reserve_inputs) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    channel_t chans[2] = {};
    chans[0].enabled = true;
    size_t bins[2] = {0, 0};
    size_t base_bins[2] = {0, 0};

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 1;
    dev.channel_capacity = 2;
    dev.channels = chans;
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    mixer_t mixer = {};
    mixer.name = "mix1";
    mixer.enabled = true;  // already running - avoid a spurious "enabled -> true" diff entry
    mixer.channel.enabled = true;
    mixer.channel.output_count = 0;
    mixer.pending_enable_request = -1;
    mixer.input_count = 0;
    mixer.input_capacity = 0;
    mixer.reserve_inputs = 1;  // headroom for exactly one live-appended input
    mixers = &mixer;
    mixer_count = 1;
    mixer_finalize_capacity();

    std::string path = write_config(R"(
mixers: {
  mix1: {
    outputs: ( { type = "file"; directory = "/tmp"; filename_template = "mix"; } );
  };
};
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); },
    { freq = 120050000; label = "new-chan"; outputs: ( { type = "mixer"; name = "mix1"; } ); }
  );
});
)");
    ConfigSnapshot snapshot;
    std::string parse_error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &parse_error)) << parse_error;
    chans[0].config_signature = strdup(snapshot.devices[0].channel_signature[0].c_str());

    mixinput_t* inputs_before = mixer.inputs;

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.channel_count.load(), 2);
    EXPECT_TRUE(result.skipped_requires_restart.empty());
    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_EQ(mixer.input_count.load(), 1);
    EXPECT_EQ(mixer.inputs, inputs_before);  // no realloc - proves the reserved slot was reused
    ASSERT_EQ(chans[1].output_count, 1);
    mixer_data* mdata = (mixer_data*)chans[1].outputs[0].data;
    EXPECT_EQ(mdata->mixer, &mixer);
    EXPECT_EQ(mdata->input, 0);
}

// Regression test for a real parse_outputs() (config.cpp) hazard: "mixer_remote" starts with
// the same 5-character prefix ("mixer") that the pre-existing "mixer" branch's strncmp() matches
// on. If the "mixer_remote" branch weren't checked first, this config would be silently
// misparsed as a local `type = "mixer"` output instead - which would then throw trying to read
// a "name" setting this config never provides (a mixer_remote output has no local mixer to name;
// it has dest_path/stream_id instead), rather than being rejected as invalid or a bug that's
// merely silently wrong. No mixer needs to be declared at all here - that's the point.
TEST_F(ChannelAppendTest, appends_channel_with_mixer_remote_output_not_swallowed_by_mixer_branch) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    channel_t chans[2] = {};
    chans[0].enabled = true;
    size_t bins[2] = {0, 0};
    size_t base_bins[2] = {0, 0};

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 1;
    dev.channel_capacity = 2;
    dev.channels = chans;
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); },
    { freq = 120050000; label = "new-chan"; outputs: ( { type = "mixer_remote"; dest_path = "/run/rtl_airband/mix1.sock"; stream_id = 7; } ); }
  );
});
)");
    ConfigSnapshot snapshot;
    std::string parse_error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &parse_error)) << parse_error;
    chans[0].config_signature = strdup(snapshot.devices[0].channel_signature[0].c_str());

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.channel_count.load(), 2);
    EXPECT_TRUE(result.skipped_requires_restart.empty());
    ASSERT_EQ(result.applied.size(), 1u);
    ASSERT_EQ(chans[1].output_count, 1);
    ASSERT_EQ(chans[1].outputs[0].type, O_MIXER_REMOTE);
    mixer_remote_send_data* rdata = (mixer_remote_send_data*)chans[1].outputs[0].data;
    EXPECT_STREQ(rdata->dest_path, "/run/rtl_airband/mix1.sock");
    EXPECT_EQ(rdata->stream_id, 7u);
}

TEST_F(ChannelAppendTest, appends_channel_with_mixer_output_exceeding_reserve_inputs_is_requires_restart) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    channel_t chans[2] = {};
    chans[0].enabled = true;
    size_t bins[2] = {0, 0};
    size_t base_bins[2] = {0, 0};

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 1;
    dev.channel_capacity = 2;
    dev.channels = chans;
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    mixer_t mixer = {};
    mixer.name = "mix1";
    mixer.enabled = true;  // already running - avoid a spurious "enabled -> true" diff entry
    mixer.channel.enabled = true;
    mixer.channel.output_count = 0;
    mixer.pending_enable_request = -1;
    mixer.input_count = 0;
    mixer.input_capacity = 0;
    mixer.reserve_inputs = 0;  // no headroom - the live append below must be rejected, not crash
    mixers = &mixer;
    mixer_count = 1;
    mixer_finalize_capacity();

    std::string path = write_config(R"(
mixers: {
  mix1: {
    outputs: ( { type = "file"; directory = "/tmp"; filename_template = "mix"; } );
  };
};
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); },
    { freq = 120050000; outputs: ( { type = "mixer"; name = "mix1"; } ); }
  );
});
)");
    ConfigSnapshot snapshot;
    std::string parse_error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &parse_error)) << parse_error;
    chans[0].config_signature = strdup(snapshot.devices[0].channel_signature[0].c_str());

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.channel_count.load(), 1);  // unchanged - the whole batch is rejected
    EXPECT_TRUE(result.applied.empty());
    EXPECT_EQ(mixer.input_count.load(), 0);  // mixer itself untouched
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("capacity"), std::string::npos);
}

TEST_F(ChannelAppendTest, malformed_new_channel_is_reported_and_does_not_crash_or_partially_apply) {
    // The critical safety-net test for the recoverable-error mechanism (logging.h/.cpp): a
    // startup-time config error here would call error() -> _Exit(1), taking the whole test
    // binary (and, in production, the whole running instance) down. This new channel has no
    // "outputs" block at all, which parse_channel() rejects the same way it always has.
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    channel_t chans[2] = {};
    chans[0].enabled = true;
    size_t bins[2] = {0, 0};
    size_t base_bins[2] = {0, 0};

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 1;
    dev.channel_capacity = 2;
    dev.channels = chans;
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); },
    { freq = 120050000; }
  );
});
)");
    ConfigSnapshot snapshot;
    std::string parse_error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &parse_error)) << parse_error;
    chans[0].config_signature = strdup(snapshot.devices[0].channel_signature[0].c_str());

    DiffResult result = compute_and_apply_diff(snapshot);

    // Still running, count untouched, failure surfaced instead of a crash.
    EXPECT_EQ(dev.channel_count.load(), 1);
    EXPECT_TRUE(result.applied.empty());
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("failed to parse"), std::string::npos);
    EXPECT_NE(result.skipped_requires_restart[0].find("no restart needed"), std::string::npos);
    // config_error_is_recoverable and cerr must both be restored, not left in the "diverted"
    // state, so a later real error() call (e.g. from a subsequent test) behaves normally.
    EXPECT_FALSE(config_error_is_recoverable);
}

TEST_F(ChannelAppendTest, r_scan_channel_count_change_is_requires_restart_not_append) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;

    channel_t chans[1] = {};
    device_t dev = {};
    dev.input = &input;
    dev.mode = R_SCAN;
    dev.channel_count = 1;
    dev.channel_capacity = 1;
    dev.channels = chans;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_SCAN;
    dsnap.channel_count = 2;  // shouldn't be reachable for a real R_SCAN config, but guarded anyway
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.channel_count.load(), 1);
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("R_SCAN"), std::string::npos);
}

// Fixture for the channel-decrease tests below: builds a 2-channel device with real freqlist
// entries and hand-assigned config_signature strings (needed for compute_and_apply_diff()'s
// common-prefix check - see DeviceConfigSnapshot::channel_signature's comment) and a background
// consumer thread
// standing in for output_thread(), mirroring DiffApplyTest's mixer_enabled_diff_calls_mixer_
// enable_disable test's pattern (a real consumer thread is needed since channel_request_remove()
// blocks polling for a response, and nothing services that in a unit test otherwise).
class ChannelRemoveTest : public ChannelAppendTest {
   protected:
    input_t input = {};
    // freq_t embeds a Squelch, which - unlike this fixture's other plain-struct members - has a
    // real constructor that unconditionally calls debug_print() (Squelch::Squelch() ->
    // set_squelch_snr_threshold(), squelch.cpp). In a Debug build (-DDEBUG) that macro expands to
    // an fprintf(debugf, ...) - and debugf (logging.cpp) is only ever fopen()'d by the real
    // startup path, so it's NULL in the unittests binary, and constructing a freq_t normally here
    // segfaults. Production code sidesteps this the same way mk_freqlist() (config.cpp) does:
    // XCALLOC/calloc, not a real C++ construction - so it never runs Squelch's constructor at
    // all. Mirrored here rather than fixed at the source, since a raw-allocated, never-truly-
    // constructed Squelch is exactly what every existing channel_t in this file already relies on
    // too (channel_t chans[N] = {} never constructs a freq_t - freqlist starts NULL).
    freq_t* freq0 = nullptr;
    freq_t* freq1 = nullptr;
    channel_t chans[2] = {};
    device_t dev = {};

    void SetUp() override {
        ChannelAppendTest::SetUp();
        input.driver_type = "rtlsdr";
        input.sample_rate = 2000000;
        input.centerfreq = 120000000;

        freq0 = (freq_t*)calloc(1, sizeof(freq_t));
        freq1 = (freq_t*)calloc(1, sizeof(freq_t));
        freq0->frequency = 120000000;
        freq1->frequency = 120050000;

        chans[0].enabled = true;
        chans[0].pending_remove_request = -1;
        chans[0].freqlist = freq0;
        chans[0].freq_count = 1;
        chans[0].config_signature = strdup("chan0-signature");
        chans[1].enabled = true;
        chans[1].pending_remove_request = -1;
        chans[1].freqlist = freq1;
        chans[1].freq_count = 1;
        chans[1].config_signature = strdup("chan1-signature");

        dev.input = &input;
        dev.mode = R_MULTICHANNEL;
        dev.channel_count = 2;
        dev.channel_capacity = 2;
        dev.channels = chans;
        dev.pending_centerfreq_request = -1;
        devices = &dev;
        device_count = 1;
    }

    void TearDown() override {
        // Drain whatever channel_teardown_for_removal() (called via start_consumer() in the tests
        // below) queued for deferred free - see reclaim_grace_period_sec's comment
        // (live_reconfig.h). Without this, a later test that shrinks the grace period to reclaim
        // deterministically (ReclaimPendingChannelFreesTest) would double-free memory this
        // fixture's own direct frees below already freed.
        double saved_grace_period = reclaim_grace_period_sec;
        reclaim_grace_period_sec = 0.0;
        reclaim_pending_channel_frees();
        reclaim_grace_period_sec = saved_grace_period;

        free(freq0);
        free(chans[0].config_signature);
        if (!chans[1].removed.load()) {
            // Never torn down (e.g. the timeout test, which runs no consumer thread) - the drain
            // above had nothing queued for it, so this still needs a direct free.
            free(freq1);
            free(chans[1].config_signature);
        }
        ChannelAppendTest::TearDown();
    }

    // Services pending_remove_request for every channel in `chans`, same shape as the real
    // output_thread() consumption added in output.cpp - runs until stop_consumer is set.
    std::thread start_consumer(std::atomic<bool>* stop_consumer) {
        return std::thread([this, stop_consumer]() {
            while (!stop_consumer->load()) {
                for (channel_t& channel : chans) {
                    if (channel.pending_remove_request.load(std::memory_order_acquire) == 1) {
                        channel_teardown_for_removal(&channel);
                        channel.pending_remove_request.store(-1, std::memory_order_release);
                    }
                }
                std::this_thread::yield();
            }
        });
    }
};

TEST_F(ChannelRemoveTest, removes_one_channel_from_the_tail_when_prefix_matches) {
    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 1;
    dsnap.centerfreq = 120000000;  // unchanged - no retune expected
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = false;
    dsnap.channel_enabled = {true};
    dsnap.channel_signature = {"chan0-signature"};  // matches chans[0] - chans[1] is being removed
    snapshot.devices.push_back(dsnap);

    std::atomic<bool> stop_consumer{false};
    std::thread consumer = start_consumer(&stop_consumer);

    DiffResult result = compute_and_apply_diff(snapshot);
    stop_consumer.store(true);
    consumer.join();

    EXPECT_EQ(dev.channel_count.load(), 1);
    EXPECT_TRUE(result.skipped_requires_restart.empty());
    ASSERT_EQ(result.applied.size(), 1u);
    EXPECT_NE(result.applied[0].find("removed 1 channel"), std::string::npos);
    EXPECT_FALSE(chans[1].enabled.load());  // torn down
    EXPECT_TRUE(chans[0].enabled.load());   // untouched
}

// Deleting a channel from the MIDDLE (or start) of the config, not the tail, used to be flatly
// rejected (item 29's original freq-only prefix check) - the more general signature-based common-
// prefix comparison (item 30) instead finds where live and file first disagree and rebuilds
// everything from that point on, tearing down whatever no longer matches at each index and
// re-appending whatever the file now says belongs there. This is a deliberate behavior change,
// not just a refactor: deleting channel A (index 0) here leaves channel B's definition - itself
// completely unchanged in the file - alone at the new index 0. From the diff's perspective,
// "index 0 doesn't match live index 0" is indistinguishable from an in-place edit, so B still
// gets torn down and reconnected as a side effect of correctly removing A - a brief interruption
// for B, not data loss, and a strictly better outcome than the old behavior's flat refusal.
TEST_F(ChannelAppendTest, deleting_a_non_tail_channel_rebuilds_from_the_point_of_divergence) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    // See ChannelRemoveTest's fixture comment (above) for why freq_t is calloc'd rather than
    // genuinely constructed here.
    freq_t* freq_a = (freq_t*)calloc(1, sizeof(freq_t));
    freq_t* freq_b = (freq_t*)calloc(1, sizeof(freq_t));
    freq_a->frequency = 120000000;
    freq_b->frequency = 120050000;

    channel_t chans[2] = {};
    chans[0].enabled = true;
    chans[0].freqlist = freq_a;
    chans[0].freq_count = 1;
    chans[0].pending_remove_request = -1;
    chans[1].enabled = true;
    chans[1].freqlist = freq_b;
    chans[1].freq_count = 1;
    chans[1].pending_remove_request = -1;
    size_t bins[2] = {0, 0};
    size_t base_bins[2] = {0, 0};

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 2;
    dev.channel_capacity = 2;
    dev.channels = chans;
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    // Parse the ORIGINAL (both channels present) config to prime each live channel's
    // config_signature exactly the way startup would.
    std::string original_path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "a"; } ); },
    { freq = 120050000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "b"; } ); }
  );
});
)");
    ConfigSnapshot original_snapshot;
    std::string parse_error;
    ASSERT_TRUE(parse_config_snapshot(original_path, &original_snapshot, &parse_error)) << parse_error;
    chans[0].config_signature = strdup(original_snapshot.devices[0].channel_signature[0].c_str());
    chans[1].config_signature = strdup(original_snapshot.devices[0].channel_signature[1].c_str());

    // Channel A is now deleted from the file - only B's (unchanged) definition remains, at index 0.
    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120050000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "b"; } ); }
  );
});
)");
    ConfigSnapshot snapshot;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &parse_error)) << parse_error;

    // Services pending_remove_request for both channels, standing in for output_thread() - same
    // shape as ChannelRemoveTest::start_consumer().
    std::atomic<bool> stop_consumer{false};
    std::thread consumer([&]() {
        while (!stop_consumer.load()) {
            for (channel_t& channel : chans) {
                if (channel.pending_remove_request.load(std::memory_order_acquire) == 1) {
                    channel_teardown_for_removal(&channel);
                    channel.pending_remove_request.store(-1, std::memory_order_release);
                }
            }
            std::this_thread::yield();
        }
    });

    DiffResult result = compute_and_apply_diff(snapshot);
    stop_consumer.store(true);
    consumer.join();

    ASSERT_EQ(dev.channel_count.load(), 1);
    EXPECT_TRUE(result.skipped_requires_restart.empty());
    bool saw_removed = false, saw_added = false;
    for (const auto& s : result.applied) {
        if (s.find("removed 2 channel") != std::string::npos)
            saw_removed = true;
        if (s.find("added 1 channel") != std::string::npos)
            saw_added = true;
    }
    EXPECT_TRUE(saw_removed);
    EXPECT_TRUE(saw_added);
    // Final state: exactly one channel, matching B's definition, correctly re-established at
    // index 0 - not the stale chans[0] slot's old (channel A) content.
    ASSERT_EQ(chans[0].freq_count, 1);
    EXPECT_EQ(chans[0].freqlist[0].frequency, 120050000);
    EXPECT_EQ(chans[0].output_count, 1);
}

// Regression test for the "silent permanent death" half of the tombstone fix: a channel whose
// removal previously timed out (try_remove_channels() gave up without decrementing
// dev->channel_count - see channel_t::removed's comment, rtl_airband.h) but has since actually
// been torn down by output_thread() is left with config_signature unchanged. If the operator
// reverts their config edit back to that channel's exact original definition, the file's
// signature at that index now MATCHES the live (stale) one - before this fix,
// compute_and_apply_diff()'s common-prefix walk would see "signature matches" and conclude
// nothing needs to happen, permanently skipping re-diffing this index on every future
// reload_diff, even though the live channel is actually dead (enabled=false, LAME freed).
// Marking `removed` channels as always-diverged (regardless of signature) forces this index to
// be retried and correctly revived.
TEST_F(ChannelAppendTest, tombstoned_channel_is_revived_even_when_its_signature_still_matches) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    freq_t* freq_a = (freq_t*)calloc(1, sizeof(freq_t));
    freq_a->frequency = 120000000;

    channel_t chans[1] = {};
    chans[0].freqlist = freq_a;
    chans[0].freq_count = 1;
    chans[0].pending_remove_request = -1;
    size_t bins[1] = {0};
    size_t base_bins[1] = {0};

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 1;
    dev.channel_capacity = 1;
    dev.channels = chans;
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "a"; } ); }
  );
});
)");
    ConfigSnapshot snapshot;
    std::string parse_error;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &parse_error)) << parse_error;

    // Simulates the exact bug scenario: chans[0]'s signature matches the (reverted) config
    // exactly, but it was already torn down by a previous, timed-out-from-the-caller's-
    // perspective removal - channel_count was never decremented for it.
    chans[0].config_signature = strdup(snapshot.devices[0].channel_signature[0].c_str());
    chans[0].enabled = false;  // already torn down
    chans[0].removed = true;   // the tombstone

    std::atomic<bool> stop_consumer{false};
    std::thread consumer([&]() {
        while (!stop_consumer.load()) {
            if (chans[0].pending_remove_request.load(std::memory_order_acquire) == 1) {
                channel_teardown_for_removal(&chans[0]);
                chans[0].pending_remove_request.store(-1, std::memory_order_release);
            }
            std::this_thread::yield();
        }
    });

    DiffResult result = compute_and_apply_diff(snapshot);
    stop_consumer.store(true);
    consumer.join();

    ASSERT_EQ(dev.channel_count.load(), 1);
    EXPECT_TRUE(result.skipped_requires_restart.empty());
    bool saw_removed = false, saw_added = false;
    for (const auto& s : result.applied) {
        if (s.find("removed 1 channel") != std::string::npos)
            saw_removed = true;
        if (s.find("added 1 channel") != std::string::npos)
            saw_added = true;
    }
    EXPECT_TRUE(saw_removed) << "a tombstoned channel must still be retried, not silently skipped";
    EXPECT_TRUE(saw_added) << "and successfully revived";
    EXPECT_TRUE(chans[0].enabled.load()) << "channel should be live again";
    EXPECT_FALSE(chans[0].removed.load()) << "tombstone must be cleared by parse_channel() on revival";
}

TEST_F(ChannelRemoveTest, removal_timeout_leaves_a_valid_partially_reduced_count) {
    // No consumer thread running - channel_request_remove() must time out (this fixture's default
    // 500ms timeout makes this test slow but deterministic) rather than hang forever, and
    // dev->channel_count must be left at whatever was actually confirmed removed (here: nothing).
    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_MULTICHANNEL;
    dsnap.channel_count = 0;
    dsnap.centerfreq = 120000000;  // unchanged - no retune expected
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = false;
    dsnap.channel_enabled = {};
    dsnap.channel_signature = {};
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.channel_count.load(), 2);  // nothing was ever confirmed removed
    EXPECT_TRUE(result.applied.empty());
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("timed out"), std::string::npos);
}

TEST_F(ChannelAppendTest, r_scan_channel_count_decrease_is_requires_restart) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;

    channel_t chans[1] = {};
    device_t dev = {};
    dev.input = &input;
    dev.mode = R_SCAN;
    dev.channel_count = 1;
    dev.channel_capacity = 1;
    dev.channels = chans;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    ConfigSnapshot snapshot;
    DeviceConfigSnapshot dsnap;
    dsnap.type = "rtlsdr";
    dsnap.mode = R_SCAN;
    dsnap.channel_count = 0;  // shouldn't be reachable for a real R_SCAN config, but guarded anyway
    dsnap.sample_rate = 2000000;
    dsnap.has_gain = false;
    snapshot.devices.push_back(dsnap);

    DiffResult result = compute_and_apply_diff(snapshot);

    EXPECT_EQ(dev.channel_count.load(), 1);
    ASSERT_EQ(result.skipped_requires_restart.size(), 1u);
    EXPECT_NE(result.skipped_requires_restart[0].find("R_SCAN"), std::string::npos);
}

// build_channel_identity_signature() (config.cpp) is what backs the common-prefix check above -
// these test it directly, against real parsed config text, independent of the diff machinery.
TEST(ChannelIdentitySignatureTest, identical_configs_produce_identical_signatures) {
    libconfig::Config cfg1, cfg2;
    cfg1.readString(R"(chan: { freq = 120000000; modulation = "nfm"; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); };)");
    cfg2.readString(R"(chan: { freq = 120000000; modulation = "nfm"; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); };)");
    EXPECT_EQ(build_channel_identity_signature(cfg1.lookup("chan")), build_channel_identity_signature(cfg2.lookup("chan")));
}

TEST(ChannelIdentitySignatureTest, differing_scalar_field_produces_different_signature) {
    libconfig::Config cfg1, cfg2;
    cfg1.readString("chan: { freq = 120000000; bandwidth = 5000; };");
    cfg2.readString("chan: { freq = 120000000; bandwidth = 8000; };");
    EXPECT_NE(build_channel_identity_signature(cfg1.lookup("chan")), build_channel_identity_signature(cfg2.lookup("chan")));
}

TEST(ChannelIdentitySignatureTest, differing_nested_output_field_produces_different_signature) {
    // outputs is a list of groups - proves serialize_setting()'s recursion actually reaches into
    // nested structures, not just top-level scalar fields.
    libconfig::Config cfg1, cfg2;
    cfg1.readString(R"(chan: { freq = 120000000; outputs: ( { type = "mixer"; name = "mix1"; balance = 0.0; } ); };)");
    cfg2.readString(R"(chan: { freq = 120000000; outputs: ( { type = "mixer"; name = "mix1"; balance = 0.5; } ); };)");
    EXPECT_NE(build_channel_identity_signature(cfg1.lookup("chan")), build_channel_identity_signature(cfg2.lookup("chan")));
}

TEST(ChannelIdentitySignatureTest, enabled_field_is_excluded_from_signature) {
    // "enabled" is diffed/applied separately via a cheap flag flip - see
    // build_channel_identity_signature()'s comment (config.cpp) for why it's deliberately left
    // out, so toggling it alone never forces a full tear-down/replace.
    libconfig::Config cfg1, cfg2;
    cfg1.readString("chan: { freq = 120000000; enabled = true; };");
    cfg2.readString("chan: { freq = 120000000; enabled = false; };");
    EXPECT_EQ(build_channel_identity_signature(cfg1.lookup("chan")), build_channel_identity_signature(cfg2.lookup("chan")));
}

// build_mixer_remote_inputs_signature() (config.cpp) is what backs the reload_diff check added
// above (DiffApplyTest.remote_inputs_changed_is_requires_restart_and_applies_nothing) - tested
// directly against real parsed config text, same style as ChannelIdentitySignatureTest above.
TEST(MixerRemoteInputsSignatureTest, identical_remote_inputs_produce_identical_signatures) {
    libconfig::Config cfg1, cfg2;
    cfg1.readString(R"(mix: { remote_inputs: ( { listen_path = "/tmp/a.sock"; stream_id = 1; } ); };)");
    cfg2.readString(R"(mix: { remote_inputs: ( { listen_path = "/tmp/a.sock"; stream_id = 1; } ); };)");
    EXPECT_EQ(build_mixer_remote_inputs_signature(cfg1.lookup("mix")), build_mixer_remote_inputs_signature(cfg2.lookup("mix")));
}

TEST(MixerRemoteInputsSignatureTest, differing_stream_id_produces_different_signature) {
    libconfig::Config cfg1, cfg2;
    cfg1.readString(R"(mix: { remote_inputs: ( { listen_path = "/tmp/a.sock"; stream_id = 1; } ); };)");
    cfg2.readString(R"(mix: { remote_inputs: ( { listen_path = "/tmp/a.sock"; stream_id = 2; } ); };)");
    EXPECT_NE(build_mixer_remote_inputs_signature(cfg1.lookup("mix")), build_mixer_remote_inputs_signature(cfg2.lookup("mix")));
}

TEST(MixerRemoteInputsSignatureTest, differing_listen_path_produces_different_signature) {
    libconfig::Config cfg1, cfg2;
    cfg1.readString(R"(mix: { remote_inputs: ( { listen_path = "/tmp/a.sock"; stream_id = 1; } ); };)");
    cfg2.readString(R"(mix: { remote_inputs: ( { listen_path = "/tmp/b.sock"; stream_id = 1; } ); };)");
    EXPECT_NE(build_mixer_remote_inputs_signature(cfg1.lookup("mix")), build_mixer_remote_inputs_signature(cfg2.lookup("mix")));
}

TEST(MixerRemoteInputsSignatureTest, adding_a_second_route_produces_different_signature) {
    libconfig::Config cfg1, cfg2;
    cfg1.readString(R"(mix: { remote_inputs: ( { listen_path = "/tmp/a.sock"; stream_id = 1; } ); };)");
    cfg2.readString(R"(mix: { remote_inputs: (
        { listen_path = "/tmp/a.sock"; stream_id = 1; },
        { listen_path = "/tmp/a.sock"; stream_id = 2; }
    ); };)");
    EXPECT_NE(build_mixer_remote_inputs_signature(cfg1.lookup("mix")), build_mixer_remote_inputs_signature(cfg2.lookup("mix")));
}

TEST(MixerRemoteInputsSignatureTest, absent_remote_inputs_is_distinct_from_an_empty_list) {
    // Not just "different from any real content" - specifically distinct from "{}" (a present but
    // empty remote_inputs list), so a mixer gaining its first remote_inputs entry (or losing its
    // last one) is also detected as a change, not just edits to an already-present list.
    libconfig::Config cfg1, cfg2;
    cfg1.readString("mix: { outputs: (); };");  // no remote_inputs key at all
    cfg2.readString("mix: { remote_inputs: (); outputs: (); };");
    string absent_sig = build_mixer_remote_inputs_signature(cfg1.lookup("mix"));
    string empty_sig = build_mixer_remote_inputs_signature(cfg2.lookup("mix"));
    EXPECT_NE(absent_sig, empty_sig);
    EXPECT_EQ(empty_sig, "{}");
}

TEST(MixerRemoteInputsSignatureTest, identical_absent_remote_inputs_produce_identical_signatures) {
    libconfig::Config cfg1, cfg2;
    cfg1.readString("mix: { outputs: (); };");
    cfg2.readString("mix: { outputs: (); };");
    EXPECT_EQ(build_mixer_remote_inputs_signature(cfg1.lookup("mix")), build_mixer_remote_inputs_signature(cfg2.lookup("mix")));
}

// End-to-end coverage for the tail-replace mechanism these signatures back: editing an already-
// live channel's field (not just adding/removing channels) is picked up by reload_diff.
TEST_F(ChannelAppendTest, replaces_tail_channel_when_its_definition_changes) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    channel_t chans[2] = {};
    chans[0].enabled = true;
    size_t bins[2] = {0, 0};
    size_t base_bins[2] = {0, 0};

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 2;
    dev.channel_capacity = 2;
    dev.channels = chans;
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    std::string original_path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); },
    { freq = 120050000; bandwidth = 5000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "y"; } ); }
  );
});
)");
    ConfigSnapshot original_snapshot;
    std::string parse_error;
    ASSERT_TRUE(parse_config_snapshot(original_path, &original_snapshot, &parse_error)) << parse_error;
    ASSERT_TRUE(parse_channel((*original_snapshot.devices[0].raw_channels_setting)[0], &dev, 0, 0, &chans[0]));
    ASSERT_TRUE(parse_channel((*original_snapshot.devices[0].raw_channels_setting)[1], &dev, 0, 1, &chans[1]));
    std::string old_signature(chans[1].config_signature);

    // Channel 1's bandwidth changes - same freq, same output, different bandwidth. Channel 0 is
    // untouched in the file.
    std::string path = write_config(R"(
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "x"; } ); },
    { freq = 120050000; bandwidth = 8000; outputs: ( { type = "file"; directory = "/tmp"; filename_template = "y"; } ); }
  );
});
)");
    ConfigSnapshot snapshot;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &parse_error)) << parse_error;

    // Services pending_remove_request for both channels, standing in for output_thread() - the
    // replaced tail channel's teardown needs this or channel_request_remove() times out.
    std::atomic<bool> stop_consumer{false};
    std::thread consumer([&]() {
        while (!stop_consumer.load()) {
            for (channel_t& channel : chans) {
                if (channel.pending_remove_request.load(std::memory_order_acquire) == 1) {
                    channel_teardown_for_removal(&channel);
                    channel.pending_remove_request.store(-1, std::memory_order_release);
                }
            }
            std::this_thread::yield();
        }
    });

    DiffResult result = compute_and_apply_diff(snapshot);
    stop_consumer.store(true);
    consumer.join();

    EXPECT_EQ(dev.channel_count.load(), 2);
    EXPECT_TRUE(result.skipped_requires_restart.empty());
    bool saw_removed = false, saw_added = false;
    for (const auto& s : result.applied) {
        if (s.find("removed 1 channel") != std::string::npos)
            saw_removed = true;
        if (s.find("added 1 channel") != std::string::npos)
            saw_added = true;
    }
    EXPECT_TRUE(saw_removed);
    EXPECT_TRUE(saw_added);
    // Channel 0 (unchanged) must be untouched - still the exact same parsed instance, not a
    // freshly re-parsed one.
    EXPECT_EQ(chans[0].output_count, 1);
    // Channel 1 was genuinely replaced - freq is the same (by design, only bandwidth changed),
    // but its signature must reflect the new config, proving it's a fresh parse, not the stale one.
    ASSERT_EQ(chans[1].freq_count, 1);
    EXPECT_EQ(chans[1].freqlist[0].frequency, 120050000);
    ASSERT_NE(chans[1].config_signature, nullptr);
    EXPECT_NE(std::string(chans[1].config_signature), old_signature);
}

// Same mechanism, applied to the case item 30 was specifically asked for: editing an existing
// channel's mixer connection (ampfactor/balance) live, not just adding a brand-new one (item 28).
TEST_F(ChannelAppendTest, replaces_tail_channel_with_edited_mixer_output) {
    input_t input = {};
    input.driver_type = "rtlsdr";
    input.sample_rate = 2000000;
    input.centerfreq = 120000000;

    channel_t chans[1] = {};
    size_t bins[1] = {0};
    size_t base_bins[1] = {0};

    device_t dev = {};
    dev.input = &input;
    dev.mode = R_MULTICHANNEL;
    dev.channel_count = 1;
    dev.channel_capacity = 1;
    dev.channels = chans;
    dev.bins = bins;
    dev.base_bins = base_bins;
    dev.pending_centerfreq_request = -1;
    devices = &dev;
    device_count = 1;

    mixer_t mixer = {};
    mixer.name = "mix1";
    mixer.enabled = true;
    mixer.channel.enabled = true;
    mixer.channel.output_count = 0;
    mixer.pending_enable_request = -1;
    mixer.input_count = 0;
    mixer.input_capacity = 0;
    // No reserve_inputs headroom needed for the edit itself - the replaced channel's old mixer
    // input slot is reclaimed by mixer_disable_input(..., permanent=true) and reused by the
    // replacement's mixer_connect_input() call (see input_removed's comment, rtl_airband.h).
    mixer.reserve_inputs = 0;
    mixers = &mixer;
    mixer_count = 1;

    std::string original_path = write_config(R"(
mixers: {
  mix1: {
    outputs: ( { type = "file"; directory = "/tmp"; filename_template = "mix"; } );
  };
};
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "mixer"; name = "mix1"; balance = 0.0; } ); }
  );
});
)");
    ConfigSnapshot original_snapshot;
    std::string parse_error;
    ASSERT_TRUE(parse_config_snapshot(original_path, &original_snapshot, &parse_error)) << parse_error;
    ASSERT_TRUE(parse_channel((*original_snapshot.devices[0].raw_channels_setting)[0], &dev, 0, 0, &chans[0]));
    mixer_finalize_capacity();
    ASSERT_EQ(mixer.input_count.load(), 1);  // connected once at "startup"

    // The channel's balance changes - same freq, same mixer, different balance.
    std::string path = write_config(R"(
mixers: {
  mix1: {
    outputs: ( { type = "file"; directory = "/tmp"; filename_template = "mix"; } );
  };
};
devices:
({
  type = "rtlsdr";
  index = 0;
  centerfreq = 120000000;
  sample_rate = 2000000;
  channels: (
    { freq = 120000000; outputs: ( { type = "mixer"; name = "mix1"; balance = 0.7; } ); }
  );
});
)");
    ConfigSnapshot snapshot;
    ASSERT_TRUE(parse_config_snapshot(path, &snapshot, &parse_error)) << parse_error;

    // Services pending_remove_request, standing in for output_thread() - the replaced channel's
    // teardown needs this or the request times out. channel_teardown_for_removal() calls
    // disable_channel_outputs() (src/output.cpp), which test_mixer.cpp's file-scope stub
    // reproduces the real O_MIXER branch of (mixer_disable_input() is real mixer.cpp code, not
    // stubbed - only icecast/file/pulse teardown is skipped), so the slot-reclaim path below is
    // genuinely exercised, not simulated.
    std::atomic<bool> stop_consumer{false};
    std::thread consumer([&]() {
        while (!stop_consumer.load()) {
            for (channel_t& channel : chans) {
                if (channel.pending_remove_request.load(std::memory_order_acquire) == 1) {
                    channel_teardown_for_removal(&channel);
                    channel.pending_remove_request.store(-1, std::memory_order_release);
                }
            }
            std::this_thread::yield();
        }
    });

    DiffResult result = compute_and_apply_diff(snapshot);
    stop_consumer.store(true);
    consumer.join();

    EXPECT_EQ(dev.channel_count.load(), 1);
    EXPECT_TRUE(result.skipped_requires_restart.empty());
    // The old slot (index 0) is reclaimed and reused by the replacement channel's
    // mixer_connect_input() call - input_count stays at 1, no reserve_inputs headroom consumed.
    EXPECT_EQ(mixer.input_count.load(), 1);
    ASSERT_EQ(chans[0].output_count, 1);
    mixer_data* mdata = (mixer_data*)chans[0].outputs[0].data;
    EXPECT_EQ(mdata->mixer, &mixer);
    EXPECT_EQ(mdata->input, 0);
    EXPECT_TRUE(mixer.input_mask[0]);
    EXPECT_FALSE(mixer.input_removed[0]);
    EXPECT_FLOAT_EQ(mixer.inputs[mdata->input].ampl, fminf(1.0f, 1.0f - 0.7f));
}

// item 40: channel_teardown_for_removal() used to deliberately leak channel->outputs/freqlist/
// config_signature and each output's own `data` struct, because write_stats_file() (no `enabled`
// gate at all) and, more weakly, the demod thread's and output_check_thread's `enabled`-gated
// loops could still be mid-access when a channel is torn down. reclaim_pending_channel_frees()
// defers the actual free by reclaim_grace_period_sec (production: 30s, comfortably longer than
// write_stats_file()'s 15s throttle) instead of freeing inline, closing that window without
// touching any hot-path reader's code at all.
class ReclaimPendingChannelFreesTest : public TestBaseClass {
   protected:
    void TearDown() override {
        // Whatever a given test left queued, make sure nothing lingers for a LATER test's own
        // drain to trip over - same concern as ChannelRemoveTest::TearDown() (this file, above).
        reclaim_grace_period_sec = 0.0;
        reclaim_pending_channel_frees();
        reclaim_grace_period_sec = 30.0;
        TestBaseClass::TearDown();
    }
};

TEST_F(ReclaimPendingChannelFreesTest, teardown_queues_without_freeing_immediately) {
    channel_t channel = {};
    channel.pending_remove_request = -1;
    channel.freqlist = (freq_t*)calloc(1, sizeof(freq_t));
    channel.freqlist[0].frequency = 120000000;
    channel.freqlist[0].label = strdup("test-freq");
    channel.freq_count = 1;
    channel.config_signature = strdup("test-signature");
    channel.output_count = 0;
    channel.outputs = nullptr;

    ASSERT_EQ(pending_channel_free_backlog(), 0u);
    // Effectively "never", so the immediate reclaim attempt below is guaranteed to find nothing
    // ready, regardless of how much wall-clock time this process has already been running.
    reclaim_grace_period_sec = 3600.0;

    channel_teardown_for_removal(&channel);
    EXPECT_EQ(pending_channel_free_backlog(), 1u);
    EXPECT_TRUE(channel.removed.load());

    reclaim_pending_channel_frees();
    EXPECT_EQ(pending_channel_free_backlog(), 1u) << "grace period hasn't elapsed - nothing should free yet";

    reclaim_grace_period_sec = 0.0;
    reclaim_pending_channel_frees();
    EXPECT_EQ(pending_channel_free_backlog(), 0u);
}

// Real allocations matching parse_outputs()'s (config.cpp) exact shape for every output type
// except O_MIXER (mixer_data's slot-reclaim path is already covered end-to-end by test_mixer.cpp
// and the mixer-edit test above; free_output_data()'s O_MIXER branch is a plain free() with no
// nested pointers, so there's nothing type-specific left to prove here). The actual assertion is
// implicit: a double-free, a free()/delete mismatch, or a heap-buffer-overflow anywhere in
// free_output_data() aborts the whole test binary under ASan before reaching the final EXPECT.
TEST_F(ReclaimPendingChannelFreesTest, frees_every_non_mixer_output_type_without_corrupting_the_heap) {
    channel_t channel = {};
    channel.pending_remove_request = -1;
    channel.freqlist = (freq_t*)calloc(2, sizeof(freq_t));
    channel.freqlist[0].frequency = 120000000;
    channel.freqlist[0].label = strdup("freq-a");
    channel.freqlist[1].frequency = 120050000;
    channel.freqlist[1].label = nullptr;  // labels are optional - must tolerate NULL
    channel.freq_count = 2;
    channel.config_signature = strdup("test-signature");

    const int kOutputCount = 4
#ifdef WITH_PULSEAUDIO
                             + 1
#endif /* WITH_PULSEAUDIO */
        ;
    channel.output_count = kOutputCount;
    channel.outputs = (output_t*)XCALLOC(kOutputCount, sizeof(output_t));
    int idx = 0;

    // O_ICECAST - every string field strdup'd (config.cpp).
    channel.outputs[idx].type = O_ICECAST;
    icecast_data* idata = (icecast_data*)XCALLOC(1, sizeof(icecast_data));
    idata->hostname = strdup("icecast.example.com");
    idata->username = strdup("source");
    idata->password = strdup("hunter2");
    idata->mountpoint = strdup("/test");
    idata->name = strdup("Test Stream");
    idata->genre = strdup("Scanner");
    idata->description = strdup("A test stream");
    channel.outputs[idx].data = idata;
    idx++;

    // O_FILE with a nested rdio_scanner block - both `new`'d (config.cpp).
    channel.outputs[idx].type = O_FILE;
    file_data* fdata = new file_data();
    fdata->type = O_FILE;
    fdata->basedir = "/tmp";
    fdata->basename = "test";
    fdata->suffix = ".mp3";
    fdata->post_write_script = "/usr/bin/true";
#ifdef WITH_RDIO_SCANNER
    rdio_scanner_data* rsdata = new rdio_scanner_data();
    rsdata->server = "rdio.example.com";
    rsdata->api_key = "test-key";
    rsdata->system_label = "Test System";
    rsdata->talkgroup_label = "Test TG";
    rsdata->talkgroup_tag = "Tag";
    rsdata->talkgroup_group = "Group";
    fdata->rdio_scanner = rsdata;
#endif /* WITH_RDIO_SCANNER */
    channel.outputs[idx].data = fdata;
    idx++;

    // O_UDP_STREAM - dest_address/dest_port strdup'd; buffers XCALLOC'd exactly as
    // udp_stream_init() (udp_stream.cpp) would for a resampled, non-float, stereo output.
    channel.outputs[idx].type = O_UDP_STREAM;
    udp_stream_data* sdata = (udp_stream_data*)XCALLOC(1, sizeof(udp_stream_data));
    sdata->dest_address = strdup("127.0.0.1");
    sdata->dest_port = strdup("9001");
    sdata->convert_buffer = XCALLOC(100, sizeof(int16_t));
    sdata->resample_buffer = (float*)XCALLOC(100, sizeof(float));
    sdata->resample_left_buffer = (float*)XCALLOC(100, sizeof(float));
    sdata->resample_right_buffer = (float*)XCALLOC(100, sizeof(float));
    sdata->stereo_buffer = (float*)XCALLOC(200, sizeof(float));
    sdata->send_socket = -1;
    channel.outputs[idx].data = sdata;
    idx++;

    // O_MIXER_REMOTE - dest_path strdup'd (config.cpp); send_buf left NULL, matching a channel
    // whose mixer_remote_send_shutdown() already ran during disable_channel_outputs() (which
    // channel_teardown_for_removal() always calls first) and already freed/NULL'd it.
    channel.outputs[idx].type = O_MIXER_REMOTE;
    mixer_remote_send_data* rdata = (mixer_remote_send_data*)XCALLOC(1, sizeof(mixer_remote_send_data));
    rdata->dest_path = strdup("/run/rtl_airband/mix1_remote.sock");
    rdata->stream_id = 3;
    rdata->send_socket = -1;
    rdata->send_buf = nullptr;
    channel.outputs[idx].data = rdata;
    idx++;

#ifdef WITH_PULSEAUDIO
    // O_PULSE - strdup'd string fields; pa_context/pa_stream stay NULL (never opened by this
    // test), matching a channel whose pulse_shutdown() already ran during disable_channel_
    // outputs().
    channel.outputs[idx].type = O_PULSE;
    pulse_data* pdata = (pulse_data*)XCALLOC(1, sizeof(pulse_data));
    pdata->server = strdup("pulse.example.com");
    pdata->name = strdup("rtl_airband");
    pdata->sink = strdup("test-sink");
    pdata->stream_name = strdup("Test Channel");
    channel.outputs[idx].data = pdata;
    idx++;
#endif /* WITH_PULSEAUDIO */

    ASSERT_EQ(idx, kOutputCount);

    channel_teardown_for_removal(&channel);
    EXPECT_EQ(pending_channel_free_backlog(), 1u);

    reclaim_grace_period_sec = 0.0;
    reclaim_pending_channel_frees();
    EXPECT_EQ(pending_channel_free_backlog(), 0u);
}
