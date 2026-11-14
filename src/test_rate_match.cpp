/*
 * test_rate_match.cpp
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

#include <gtest/gtest.h>
#include <math.h>
#include <vector>

#include "rate_match.h"

using namespace std;

static const size_t BATCH = 2000;  // WAVE_BATCH at 16 kHz
static const double BATCH_S = 0.125;
static const double RATE = 16000.0;

static vector<float> sine(double freq, size_t n) {
    vector<float> v(n);
    for (size_t i = 0; i < n; i++)
        v[i] = (float)sin(2 * M_PI * freq * i / RATE);
    return v;
}

// worst error of a stretched sine against the ideal sine at the stretched
// positions, in dB re full scale
static double stretch_error_db(double freq, int d) {
    vector<float> in = sine(freq, BATCH), out(BATCH + 1);
    size_t m = RateMatcher::stretch(in.data(), BATCH, out.data(), d);
    double step = (double)(BATCH - 1) / (double)(m - 1), worst = 0;
    for (size_t i = 2; i + 2 < m; i++) {
        double e = fabs(out[i] - sin(2 * M_PI * freq * (i * step) / RATE));
        worst = e > worst ? e : worst;
    }
    return 20 * log10(worst + 1e-12);
}

TEST(RateMatchStretch, NoChangeIsACopy) {
    vector<float> in = sine(1000, BATCH), out(BATCH + 1);
    EXPECT_EQ(RateMatcher::stretch(in.data(), BATCH, out.data(), 0), BATCH);
    for (size_t i = 0; i < BATCH; i++)
        EXPECT_EQ(out[i], in[i]);
}

TEST(RateMatchStretch, LengthAndEndpoints) {
    vector<float> in = sine(700, BATCH), out(BATCH + 1);
    for (int d = -1; d <= 1; d += 2) {
        size_t m = RateMatcher::stretch(in.data(), BATCH, out.data(), d);
        EXPECT_EQ(m, BATCH + d);
        EXPECT_EQ(out[0], in[0]);
        EXPECT_EQ(out[m - 1], in[BATCH - 1]);
    }
}

TEST(RateMatchStretch, CleanAcrossTheVoiceBand) {
    for (double f : {300.0, 1000.0, 3000.0}) {
        EXPECT_LT(stretch_error_db(f, +1), -70.0) << f << " Hz";
        EXPECT_LT(stretch_error_db(f, -1), -70.0) << f << " Hz";
    }
}

TEST(RateMatchStretch, KeepsDC) {
    vector<float> in(BATCH, 0.25f), out(BATCH + 1);
    size_t m = RateMatcher::stretch(in.data(), BATCH, out.data(), 1);
    for (size_t i = 0; i < m; i++)
        EXPECT_NEAR(out[i], 0.25f, 1e-6);
}

TEST(RateMatchStep, UnknownLatencyDoesNothing) {
    RateMatcher rm(BATCH_S);
    for (int i = 0; i < 500; i++)
        EXPECT_EQ(rm.step(-1.0, BATCH), 0);
    EXPECT_EQ(rm.target_ms(), 0.0);
}

TEST(RateMatchStep, WarmupLearnsTheMeanAndChangesNothing) {
    RateMatcher rm(BATCH_S);
    for (int i = 0; i < RateMatcher::WARMUP_BATCHES; i++)
        EXPECT_EQ(rm.step(i % 2 ? 110.0 : 130.0, BATCH), 0);
    EXPECT_NEAR(rm.target_ms(), 120.0, 1e-9);
}

TEST(RateMatchStep, TargetHasAFloor) {
    RateMatcher rm(BATCH_S);
    for (int i = 0; i < RateMatcher::WARMUP_BATCHES; i++)
        rm.step(30.0, BATCH);
    EXPECT_EQ(rm.target_ms(), RateMatcher::MIN_TARGET_MS);
}

// Stream whose producer runs `offset_ppm` off the consumer, read with
// +/-40 ms of measurement jitter (as seen on PipeWire). Returns the mean
// latency error over the last 10 minutes and the lowest latency reached.
struct SimResult {
    double mean_err_ms, min_latency_ms, integral_ppm;
};

static SimResult simulate(double offset_ppm, double start_ms, double seconds) {
    RateMatcher rm(BATCH_S);
    double latency = start_ms, sum = 0, lowest = 1e9;
    unsigned seed = 12345;
    long batches = (long)(seconds / BATCH_S), tail = (long)(600 / BATCH_S), n = 0;
    for (long b = 0; b < batches; b++) {
        seed = seed * 1103515245u + 12345u;
        double jitter = ((seed >> 8) % 8001) / 100.0 - 40.0;
        int d = rm.step(latency + jitter, BATCH);
        latency += offset_ppm * 1e-6 * BATCH_S * 1000.0 + d * 1000.0 / RATE;
        lowest = latency < lowest ? latency : lowest;
        if (b >= batches - tail) {
            sum += latency - rm.target_ms();
            n++;
        }
    }
    return {sum / n, lowest, rm.integral_ppm()};
}

TEST(RateMatchStep, HoldsTheTargetWhenTheProducerIsSlow) {
    SimResult r = simulate(-160.0, 90.0, 3600);
    EXPECT_LT(fabs(r.mean_err_ms), 2.0);
    EXPECT_GT(r.min_latency_ms, 40.0);
    EXPECT_NEAR(r.integral_ppm, 160.0, 25.0);
}

TEST(RateMatchStep, HoldsTheTargetWhenTheProducerIsFast) {
    SimResult r = simulate(+160.0, 90.0, 3600);
    EXPECT_LT(fabs(r.mean_err_ms), 2.0);
    EXPECT_NEAR(r.integral_ppm, -160.0, 25.0);
}

TEST(RateMatchStep, WithoutCorrectionTheSameStreamWouldUnderflow) {
    // sanity check of the simulation itself: -160 ppm drains 90 ms in ~9 min
    double latency = 90.0;
    for (long b = 0; b < (long)(600 / BATCH_S); b++)
        latency += -160.0 * 1e-6 * BATCH_S * 1000.0;
    EXPECT_LT(latency, 0.0);
}

TEST(RateMatchStep, OutputIsClampedAndDoesNotWindUp) {
    RateMatcher rm(BATCH_S);
    for (int i = 0; i < RateMatcher::WARMUP_BATCHES; i++)
        rm.step(500.0, BATCH);
    for (int i = 0; i < 2000; i++)
        rm.step(0.0, BATCH);  // 500 ms low: far past saturation
    EXPECT_EQ(rm.ppm(), RateMatcher::MAX_PPM);
    EXPECT_LT(rm.integral_ppm(), RateMatcher::MAX_PPM);
}

TEST(RateMatchStep, ResetForgetsEverything) {
    RateMatcher rm(BATCH_S);
    for (int i = 0; i < 500; i++)
        rm.step(i < RateMatcher::WARMUP_BATCHES ? 120.0 : 60.0, BATCH);
    ASSERT_GT(rm.inserted(), 0u);
    rm.reset();
    EXPECT_EQ(rm.inserted(), 0u);
    EXPECT_EQ(rm.target_ms(), 0.0);
    EXPECT_EQ(rm.ppm(), 0.0);
    EXPECT_EQ(rm.integral_ppm(), 0.0);
}
