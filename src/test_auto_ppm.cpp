/*
 * test_auto_ppm.cpp
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

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "auto_ppm.h"
#include "input-common.h"
#include "rtl_airband.h"

using namespace std;

namespace {

// Offset a carrier at freq_hz shows when the correction is residual_ppm short of right.
AutoPpmEstimate est(int freq_hz, double residual_ppm, uint32_t updates = 50) {
    return {freq_hz, (float)(-residual_ppm * freq_hz / 1e6), updates};
}

}  // namespace

class AutoPpmDecideTest : public TestBaseClass {
   protected:
    AutoPpmParams params;
    AutoPpmState state;
};

TEST_F(AutoPpmDecideTest, no_estimates_holds) {
    AutoPpmDecision d = auto_ppm_decide(0, {}, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_FALSE(d.residual_valid);
    EXPECT_EQ(d.eligible, 0);
}

TEST_F(AutoPpmDecideTest, estimates_with_too_few_updates_are_ignored) {
    AutoPpmDecision d = auto_ppm_decide(0, {est(120000000, 3.0, params.min_updates - 1)}, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_EQ(d.eligible, 0);
}

TEST_F(AutoPpmDecideTest, carrier_above_frequency_means_correction_must_drop) {
    // +360 Hz at 120 MHz: the LO runs 3 ppm slow, so the correction has to come down 3 ppm.
    AutoPpmDecision d = auto_ppm_decide(0, {{120000000, 360.0f, 50}}, params, &state);
    ASSERT_TRUE(d.residual_valid);
    EXPECT_NEAR(d.residual_ppm, -3.0, 1e-3);
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    EXPECT_EQ(d.new_correction, -3);
}

TEST_F(AutoPpmDecideTest, residual_scales_with_channel_frequency_not_centerfreq) {
    AutoPpmDecision d = auto_ppm_decide(10, {est(118000000, 2.0), est(136000000, 2.0)}, params, &state);
    ASSERT_TRUE(d.residual_valid);
    EXPECT_NEAR(d.residual_ppm, 2.0, 1e-3);
    EXPECT_EQ(d.new_correction, 12);
}

TEST_F(AutoPpmDecideTest, small_residual_inside_deadband_holds) {
    AutoPpmDecision d = auto_ppm_decide(4, {est(120000000, 0.6), est(125000000, 0.7)}, params, &state);
    EXPECT_TRUE(d.residual_valid);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_EQ(d.new_correction, 4);
}

TEST_F(AutoPpmDecideTest, offset_carrier_channel_is_rejected_as_an_outlier) {
    // Three channels agree on +2 ppm; a climax site sits 5 kHz off (~40 ppm at 125 MHz).
    vector<AutoPpmEstimate> e = {est(120000000, 2.0), est(121500000, 2.2), est(123000000, 1.9), {125000000, -5000.0f, 50}};
    AutoPpmDecision d = auto_ppm_decide(0, e, params, &state);
    EXPECT_EQ(d.eligible, 4);
    EXPECT_EQ(d.contributing, 3);
    EXPECT_EQ(d.rejected, 1);
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    EXPECT_EQ(d.new_correction, 2);
}

TEST_F(AutoPpmDecideTest, two_disagreeing_channels_hold) {
    AutoPpmDecision d = auto_ppm_decide(0, {est(120000000, 1.0), est(125000000, 6.0)}, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_FALSE(d.residual_valid);
}

TEST_F(AutoPpmDecideTest, no_majority_among_many_channels_holds) {
    vector<AutoPpmEstimate> e = {est(120000000, -10.0), est(121000000, 0.0), est(122000000, 10.0)};
    AutoPpmDecision d = auto_ppm_decide(0, e, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_EQ(d.contributing, 1);
}

TEST_F(AutoPpmDecideTest, wide_spread_inside_the_outlier_window_holds) {
    // Every value is within 1.5 ppm of the median (1.25) but the extremes are 2.5 apart.
    vector<AutoPpmEstimate> e = {est(120000000, 0.0), est(121000000, 1.0), est(122000000, 1.5), est(123000000, 2.5)};
    AutoPpmDecision d = auto_ppm_decide(0, e, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
}

TEST_F(AutoPpmDecideTest, step_is_limited_to_max_step) {
    AutoPpmDecision d = auto_ppm_decide(0, {est(120000000, 23.0)}, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    EXPECT_EQ(d.new_correction, 5);
    EXPECT_FALSE(d.clamped);

    d = auto_ppm_decide(0, {est(120000000, -23.0)}, params, &state);
    EXPECT_EQ(d.new_correction, -5);
}

TEST_F(AutoPpmDecideTest, never_goes_past_plus_minus_100_ppm) {
    // Plausible estimates (|implied| <= 100) that would still push past the limit.
    AutoPpmDecision d = auto_ppm_decide(98, {est(120000000, 1.9)}, params, &state);
    EXPECT_EQ(d.new_correction, 100);
    EXPECT_FALSE(d.clamped);

    state = AutoPpmState();
    params.max_step = 100;
    // At the hard limit, anything asking for more implies >100 ppm and is rejected outright.
    d = auto_ppm_decide(100, {est(120000000, 0.99)}, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_HOLD);
    EXPECT_EQ(d.new_correction, 100);
    EXPECT_EQ(d.rejected, 1);

    state = AutoPpmState();
    d = auto_ppm_decide(-99, {est(120000000, -0.99)}, params, &state);
    EXPECT_EQ(d.new_correction, -100);
}

TEST_F(AutoPpmDecideTest, estimates_implying_more_than_100_ppm_are_rejected) {
    params.max_step = 100;
    for (double residual : {150.0, -300.0}) {
        state = AutoPpmState();
        AutoPpmDecision d = auto_ppm_decide(0, {est(120000000, residual), est(122000000, residual)}, params, &state);
        EXPECT_EQ(d.action, AUTO_PPM_HOLD) << residual;
        EXPECT_EQ(d.new_correction, 0) << residual;
        EXPECT_EQ(d.rejected, 2) << residual;
        EXPECT_FALSE(d.residual_valid) << residual;
    }
}

TEST_F(AutoPpmDecideTest, one_implausible_channel_does_not_block_the_rest) {
    AutoPpmDecision d = auto_ppm_decide(0, {est(120000000, 2.0), est(121000000, 2.1), est(122000000, 400.0)}, params, &state);
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    EXPECT_EQ(d.new_correction, 2);
    EXPECT_EQ(d.rejected, 1);
}

TEST_F(AutoPpmDecideTest, smaller_max_ppm_clamps_tighter) {
    params.max_ppm = 20;
    params.max_step = 100;
    AutoPpmDecision d = auto_ppm_decide(0, {est(120000000, 40.0)}, params, &state);
    EXPECT_EQ(d.new_correction, 20);
    EXPECT_TRUE(d.clamped);
}

TEST_F(AutoPpmDecideTest, converging_steps_never_trip_the_divergence_guard) {
    int correction = 0;
    double const truth = 23.0;
    for (int i = 0; i < 10; i++) {
        AutoPpmDecision d = auto_ppm_decide(correction, {est(120000000, truth - correction)}, params, &state);
        ASSERT_NE(d.action, AUTO_PPM_REVERT) << i;
        correction = d.new_correction;
    }
    EXPECT_EQ(correction, 23);
}

TEST_F(AutoPpmDecideTest, diverging_steps_revert_to_the_base_correction) {
    // A sign error in the loop: each step makes the measured error worse.
    state.base_correction = 7;
    int correction = 7;
    double residual = 10.0;
    AutoPpmDecision d = auto_ppm_decide(correction, {est(120000000, residual)}, params, &state);
    ASSERT_EQ(d.action, AUTO_PPM_ADJUST);
    for (int i = 0; i < 2; i++) {
        correction = d.new_correction;
        residual += 5.0;
        d = auto_ppm_decide(correction, {est(120000000, residual)}, params, &state);
    }
    EXPECT_EQ(d.action, AUTO_PPM_REVERT);
    EXPECT_EQ(d.new_correction, 7);
}

TEST_F(AutoPpmDecideTest, one_noisy_step_does_not_trip_the_guard) {
    AutoPpmDecision d = auto_ppm_decide(0, {est(120000000, 10.0)}, params, &state);
    d = auto_ppm_decide(d.new_correction, {est(120000000, 10.5)}, params, &state);  // grew once
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    d = auto_ppm_decide(d.new_correction, {est(120000000, 4.0)}, params, &state);  // then shrank
    EXPECT_EQ(d.action, AUTO_PPM_ADJUST);
    d = auto_ppm_decide(d.new_correction, {est(120000000, 5.0)}, params, &state);  // grew once more
    EXPECT_NE(d.action, AUTO_PPM_REVERT);
}

class AutoPpmParamsTest : public TestBaseClass {
   protected:
    bool parse(const char* text, AutoPpmParams* out, string* error) {
        libconfig::Config cfg;
        cfg.readString(text);
        return auto_ppm_parse_params(cfg.lookup("dev"), out, error);
    }
};

TEST_F(AutoPpmParamsTest, defaults) {
    AutoPpmParams p;
    string error;
    ASSERT_TRUE(parse("dev: { auto_ppm = true; };", &p, &error)) << error;
    EXPECT_EQ(p.interval_sec, 30);
    EXPECT_EQ(p.max_ppm, 100);
    EXPECT_EQ(p.max_step, 5);
}

TEST_F(AutoPpmParamsTest, explicit_values) {
    AutoPpmParams p;
    string error;
    ASSERT_TRUE(parse("dev: { auto_ppm_interval = 10; auto_ppm_max_ppm = 40; auto_ppm_max_step = 2; };", &p, &error)) << error;
    EXPECT_EQ(p.interval_sec, 10);
    EXPECT_EQ(p.max_ppm, 40);
    EXPECT_EQ(p.max_step, 2);
}

TEST_F(AutoPpmParamsTest, out_of_range_values_are_rejected) {
    for (const char* text : {"dev: { auto_ppm_max_ppm = 101; };", "dev: { auto_ppm_max_ppm = 0; };", "dev: { auto_ppm_max_step = 0; };", "dev: { auto_ppm_interval = 4; };",
                             "dev: { auto_ppm_interval = -30; };", "dev: { auto_ppm_max_ppm = 50.5; };"}) {
        AutoPpmParams p;
        string error;
        EXPECT_FALSE(parse(text, &p, &error)) << text;
        EXPECT_FALSE(error.empty()) << text;
    }
}

// Driver fakes for auto_ppm_tick(): correction lives in a global the fake get_settings reports.
namespace {
int fake_correction = 0;
int fake_set_count = 0;
bool fake_set_fails = false;

int fake_set_correction(input_t* const, int const correction) {
    if (fake_set_fails) {
        return -1;
    }
    fake_correction = correction;
    fake_set_count++;
    return 0;
}

int fake_get_settings(input_t* const, input_settings_t* out) {
    out->gain = 0.0f;
    out->gain_known = false;
    out->correction = (float)fake_correction;
    out->bandwidth = 0;
    return 0;
}
}  // namespace

class AutoPpmTickTest : public TestBaseClass {
   protected:
    void SetUp() override {
        TestBaseClass::SetUp();
        fake_correction = 0;
        fake_set_count = 0;
        fake_set_fails = false;
        input.driver_type = "rtlsdr";
        input.state = INPUT_RUNNING;
        input.dev_data = &fake_dev_data;
        input.set_correction = &fake_set_correction;
        input.get_settings = &fake_get_settings;

        freqs = (freq_t*)calloc(kChannels, sizeof(freq_t));
        channels.reset(new channel_t[kChannels]());
        for (int j = 0; j < kChannels; j++) {
            freqs[j].frequency = 120000000 + j * 1000000;
            channels[j].freqlist = &freqs[j];
            channels[j].freq_count = 1;
            channels[j].enabled = true;
            channels[j].pending_remove_request = -1;
        }
        dev.input = &input;
        dev.channels = channels.get();
        dev.channel_count = kChannels;
        dev.channel_capacity = kChannels;
        ap = new auto_ppm_device();
        ap->enabled = true;
        dev.auto_ppm = ap;
    }
    void TearDown() override {
        delete ap;
        free(freqs);
        TestBaseClass::TearDown();
    }
    // What the demod thread would publish for every channel, under the current generation, if
    // the tuner's true error were truth_ppm.
    void measure(double truth_ppm, uint32_t updates = 50) {
        uint32_t const gen = input.tuning_generation.load();
        for (int j = 0; j < kChannels; j++) {
            freqs[j].ppm_offset_hz = (float)(-(truth_ppm - fake_correction) * freqs[j].frequency / 1e6);
            freqs[j].ppm_offset_updates = updates;
            freqs[j].ppm_offset_generation = gen;
        }
    }
    // Runs one tick past the interval; set_correction is faked, so bump the generation the way
    // input_set_correction() does.
    void tick() {
        uint32_t const before = (uint32_t)fake_set_count;
        now += 1000.0;
        auto_ppm_tick(&dev, 0, now);
        if ((uint32_t)fake_set_count != before) {
            EXPECT_GT(input.tuning_generation.load(), 0u);
        }
    }

    static const int kChannels = 3;
    int fake_dev_data = 0;
    input_t input{};
    freq_t* freqs = nullptr;
    unique_ptr<channel_t[]> channels;
    device_t dev{};
    auto_ppm_device* ap = nullptr;
    double now = 0.0;
};

TEST_F(AutoPpmTickTest, converges_in_bounded_steps) {
    for (int i = 0; i < 10; i++) {
        measure(17.0);
        tick();
        EXPECT_LE(abs(fake_correction), 17);
    }
    EXPECT_EQ(fake_correction, 17);
    EXPECT_EQ(ap->adjustments, 4u);  // 5 + 5 + 5 + 2
}

TEST_F(AutoPpmTickTest, stale_generation_estimates_are_not_used) {
    measure(10.0);
    tick();
    ASSERT_EQ(fake_correction, 5);
    // No fresh measurements since the change: the old ones are from the previous generation.
    tick();
    EXPECT_EQ(fake_correction, 5);
    EXPECT_EQ(ap->last.eligible, 0);
}

TEST_F(AutoPpmTickTest, waits_for_the_interval) {
    ap->params.interval_sec = 30;
    measure(10.0);
    auto_ppm_tick(&dev, 0, 100.0);
    EXPECT_EQ(fake_correction, 5);
    measure(10.0);
    auto_ppm_tick(&dev, 0, 110.0);
    EXPECT_EQ(fake_correction, 5);
    auto_ppm_tick(&dev, 0, 130.0);
    EXPECT_EQ(fake_correction, 10);
}

TEST_F(AutoPpmTickTest, never_exceeds_the_limit_even_when_the_dongle_is_out_of_spec) {
    ap->params.max_ppm = 100;
    ap->params.max_step = 100;
    fake_correction = 95;
    for (int i = 0; i < 5; i++) {
        measure(140.0);  // implies 140 ppm: rejected as implausible
        tick();
        EXPECT_LE(fake_correction, 100);
    }
    EXPECT_EQ(fake_correction, 95);

    for (int i = 0; i < 5; i++) {
        measure(99.6);  // plausible, but rounds past the limit
        tick();
        EXPECT_LE(fake_correction, 100);
    }
    EXPECT_EQ(fake_correction, 100);
}

TEST_F(AutoPpmTickTest, divergence_reverts_and_disables) {
    ap->state.base_correction = 3;
    fake_correction = 3;
    double truth = 20.0;
    for (int i = 0; i < 3; i++) {
        measure(truth);
        tick();
        truth += 10.0;  // the error runs away faster than each step closes it
    }
    EXPECT_TRUE(ap->tripped);
    EXPECT_EQ(fake_correction, 3);
    EXPECT_FALSE(auto_ppm_active(&dev));

    int const count = fake_set_count;
    measure(5.0);
    tick();
    EXPECT_EQ(fake_set_count, count) << "a tripped device is left alone";
}

TEST_F(AutoPpmTickTest, disabled_device_is_left_alone) {
    ap->enabled = false;
    measure(10.0);
    tick();
    EXPECT_EQ(fake_set_count, 0);
}

TEST_F(AutoPpmTickTest, removed_and_disabled_channels_do_not_vote) {
    channels[0].removed = true;
    channels[1].enabled = false;
    measure(10.0);
    vector<AutoPpmEstimate> e = auto_ppm_collect_estimates(&dev);
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].frequency, freqs[2].frequency);
}

TEST_F(AutoPpmTickTest, failed_set_correction_is_not_counted) {
    fake_set_fails = true;
    measure(10.0);
    tick();
    EXPECT_EQ(ap->adjustments, 0u);
    EXPECT_FALSE(ap->state.awaiting_check);
}

TEST_F(AutoPpmTickTest, reload_rearms_and_reports_whether_to_apply_config_correction) {
    ap->tripped = true;
    ap->state.base_correction = 3;
    vector<string> changes;

    EXPECT_FALSE(auto_ppm_reload(&dev, true, 3, &changes)) << "unchanged correction stays with auto_ppm";
    EXPECT_FALSE(ap->tripped);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], "auto_ppm re-armed");

    changes.clear();
    EXPECT_TRUE(auto_ppm_reload(&dev, true, 8, &changes)) << "a changed correction is the new base";
    EXPECT_EQ(ap->state.base_correction, 8);
    EXPECT_TRUE(changes.empty());

    EXPECT_TRUE(auto_ppm_reload(&dev, false, 8, &changes)) << "with auto_ppm off the config value applies";
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0], "auto_ppm -> false");
}

TEST_F(AutoPpmTickTest, manual_correction_outside_the_limit_is_refused) {
    ap->params.max_ppm = 50;
    EXPECT_TRUE(auto_ppm_correction_allowed(&dev, 50));
    EXPECT_FALSE(auto_ppm_correction_allowed(&dev, -51));
    ap->enabled = false;
    EXPECT_TRUE(auto_ppm_correction_allowed(&dev, -51));
    dev.auto_ppm = nullptr;
    EXPECT_TRUE(auto_ppm_correction_allowed(&dev, 500));
    dev.auto_ppm = ap;
}

TEST_F(AutoPpmTickTest, status_json) {
    EXPECT_NE(auto_ppm_status_json(&dev, now).find("\"residual_ppm\":null"), string::npos);
    measure(2.0);
    tick();
    string const json = auto_ppm_status_json(&dev, now + 12.0);
    EXPECT_NE(json.find("\"enabled\":true"), string::npos) << json;
    EXPECT_NE(json.find("\"residual_ppm\":2.00"), string::npos) << json;
    EXPECT_NE(json.find("\"contributing\":3"), string::npos) << json;
    EXPECT_NE(json.find("\"adjustments\":1"), string::npos) << json;
    EXPECT_NE(json.find("\"last_adjust_age_s\":12"), string::npos) << json;
    EXPECT_NE(json.find("\"max_ppm\":100"), string::npos) << json;
    EXPECT_NE(json.find("\"disabled_reason\":null"), string::npos) << json;

    dev.auto_ppm = nullptr;
    EXPECT_EQ(auto_ppm_status_json(&dev, now), "null");
    dev.auto_ppm = ap;
}
