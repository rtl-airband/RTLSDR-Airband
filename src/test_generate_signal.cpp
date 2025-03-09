/*
 * test_generate_signal.cpp
 *
 * Copyright (C) 2023 charlie-foxtrot
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

#include <algorithm>

#include "test_base_class.h"

#include "generate_signal.h"

using namespace std;

class ToneTest : public TestBaseClass {};

TEST_F(ToneTest, simple_object) {
    // simple case the sample rate is a multiple of the frequency so specific points can be measured
    float tone_freq = 100;  // tone at 100 Hz
    // set sample rate to 1000 times the tone so there will be 250 samples per quarter
    float sample_rate = 1000 * tone_freq;
    float amplitude = Tone::STRONG;

    Tone tone(sample_rate, tone_freq, amplitude);

    float last_sample = 0.0;
    float this_sample = 0.0;

    // loop through some number of cycles
    for (int j = 0; j < 10; ++j) {
        // first 249 samples will be positive and increasing
        for (int i = 0; i < 249; ++i) {
            this_sample = tone.get_sample();
            ASSERT_GT(this_sample, 0.0);
            ASSERT_GT(this_sample, last_sample);
            last_sample = this_sample;
        }

        // sample 250 will be the amp
        this_sample = tone.get_sample();
        ASSERT_EQ(this_sample, amplitude);
        ASSERT_GT(this_sample, last_sample);
        last_sample = this_sample;

        // next 249 samples will be positive and decreasing
        for (int i = 0; i < 249; ++i) {
            this_sample = tone.get_sample();
            ASSERT_GT(this_sample, 0.0);
            ASSERT_LT(this_sample, last_sample);
            last_sample = this_sample;
        }

        // sample 500 will be zero-ish
        this_sample = tone.get_sample();
        ASSERT_LT(this_sample, 0.000001);
        ASSERT_LT(this_sample, last_sample);
        last_sample = this_sample;

        // next 249 samples will be negative and decreasing
        for (int i = 0; i < 249; ++i) {
            this_sample = tone.get_sample();
            ASSERT_LT(this_sample, 0.0);
            ASSERT_LT(this_sample, last_sample);
            last_sample = this_sample;
        }

        // sample 750 will be negative amp
        this_sample = tone.get_sample();
        ASSERT_EQ(this_sample, -1.0 * amplitude);
        ASSERT_LT(this_sample, last_sample);
        last_sample = this_sample;

        // next 249 samples will be negative and increasing
        for (int i = 0; i < 249; ++i) {
            this_sample = tone.get_sample();
            ASSERT_LT(this_sample, 0.0);
            ASSERT_GT(this_sample, last_sample);
            last_sample = this_sample;
        }

        // sample 1000 will be zero-ish
        this_sample = tone.get_sample();
        ASSERT_LT(this_sample, 0.000001);
        ASSERT_GT(this_sample, last_sample);
        last_sample = this_sample;
    }
}

TEST_F(ToneTest, strengths) {
    float tone_freq = 100;
    float sample_rate = 8000;

    Tone tone_weak(sample_rate, tone_freq, Tone::WEAK);
    Tone tone_normal(sample_rate, tone_freq, Tone::NORMAL);
    Tone tone_strong(sample_rate, tone_freq, Tone::STRONG);

    for (int i = 0; i < 100 * sample_rate; ++i) {
        float weak_sample = tone_weak.get_sample();
        float normal_sample = tone_normal.get_sample();
        float strong_sample = tone_strong.get_sample();

        if (weak_sample > 0.0) {
            ASSERT_LT(weak_sample, normal_sample);
            ASSERT_LT(normal_sample, strong_sample);
        } else if (weak_sample == 0.0) {
            ASSERT_EQ(weak_sample, 0.0);
            ASSERT_EQ(normal_sample, 0.0);
            ASSERT_EQ(strong_sample, 0.0);
        } else {
            ASSERT_GT(weak_sample, normal_sample);
            ASSERT_GT(normal_sample, strong_sample);
        }
    }
}

class NoiseTest : public TestBaseClass {};

TEST_F(NoiseTest, simple_object) {
    Noise noise(Noise::STRONG);

    int sample_count = 10000;
    float sample_max = 0.0;
    float sample_min = 0.0;
    float sample_sum = 0.0;
    for (int i = 0; i < sample_count; ++i) {
        float sample = noise.get_sample();
        sample_max = max(sample, sample_max);
        sample_min = min(sample, sample_min);
        sample_sum += sample;
    }
    float sample_avg = sample_sum / sample_count;

    // average is near zero
    EXPECT_LE(abs(sample_avg), 0.01);
    // max and min are off of zero
    EXPECT_LE(sample_min, Noise::STRONG * -0.3);
    EXPECT_GT(sample_max, Noise::STRONG * 0.3);
}

TEST_F(NoiseTest, strengths) {
    Noise noise_weak(Noise::WEAK);
    Noise noise_normal(Noise::NORMAL);
    Noise noise_strong(Noise::STRONG);

    float weak_max = 0.0;
    float normal_max = 0.0;
    float strong_max = 0.0;
    for (int i = 0; i < 10000; ++i) {
        weak_max = max(weak_max, abs(noise_weak.get_sample()));
        normal_max = max(normal_max, abs(noise_normal.get_sample()));
        strong_max = max(strong_max, abs(noise_strong.get_sample()));
    }

    EXPECT_NE(weak_max, 0.0);
    EXPECT_GT(normal_max, weak_max);
    EXPECT_GT(strong_max, normal_max);
}

class GenerateSignalTest : public TestBaseClass {
   protected:
    int sample_rate;
    void SetUp(void) {
        TestBaseClass::SetUp();
        sample_rate = 8000;
    }
};

TEST_F(GenerateSignalTest, default_object) {
    GenerateSignal signal(8000);
    EXPECT_EQ(signal.get_sample(), 0.0);
}

TEST_F(GenerateSignalTest, generate_file) {
    float file_seconds = 10.5;
    GenerateSignal signal(sample_rate);

    string test_filepath = temp_dir + "/10_sec_file.dat";
    signal.write_file(test_filepath, file_seconds);

    // make sure the file exists and is the right size
    struct stat info;
    ASSERT_EQ(stat(test_filepath.c_str(), &info), 0);
    EXPECT_TRUE(S_ISREG(info.st_mode));
    EXPECT_EQ(info.st_size, sample_rate * file_seconds * sizeof(float));
}

TEST_F(GenerateSignalTest, get_sample_no_signals) {
    GenerateSignal signal(sample_rate);
    for (int i = 0; i < 60 * sample_rate; ++i) {
        ASSERT_EQ(signal.get_sample(), 0.0);
    }
}

TEST_F(GenerateSignalTest, get_sample_single_tone_only) {
    float tone_freq = 123.34;
    float tone_ampl = 0.32;

    GenerateSignal signal(sample_rate);
    signal.add_tone(tone_freq, tone_ampl);
    Tone tone(sample_rate, tone_freq, tone_ampl);
    for (int i = 0; i < 60 * sample_rate; ++i) {
        ASSERT_FLOAT_EQ(signal.get_sample(), tone.get_sample());
    }
}

TEST_F(GenerateSignalTest, get_sample_two_tones) {
    float tone1_freq = 123.34;
    float tone2_freq = 231.43;
    float tone1_ampl = Tone::NORMAL;
    float tone2_ampl = Tone::STRONG;

    GenerateSignal signal(sample_rate);
    signal.add_tone(tone1_freq, tone1_ampl);
    signal.add_tone(tone2_freq, tone2_ampl);
    Tone tone1(sample_rate, tone1_freq, tone1_ampl);
    Tone tone2(sample_rate, tone2_freq, tone2_ampl);
    for (int i = 0; i < 60 * sample_rate; ++i) {
        ASSERT_NEAR(signal.get_sample(), tone1.get_sample() + tone2.get_sample(), 0.000001);
    }
}

TEST_F(GenerateSignalTest, get_sample_only_noise) {
    GenerateSignal signal(sample_rate);
    signal.add_noise(Noise::NORMAL);

    float max_value = 0;
    float min_value = 0;
    for (int i = 0; i < 600 * sample_rate; ++i) {
        float sample = signal.get_sample();
        min_value = min(sample, min_value);
        max_value = max(sample, max_value);
    }

    EXPECT_GT(max_value, 0);
    EXPECT_LT(max_value, Noise::NORMAL);

    EXPECT_LT(min_value, 0);
    EXPECT_GT(min_value, -1.0 * Noise::NORMAL);
}

TEST_F(GenerateSignalTest, get_sample_two_tones_and_noise) {
    float tone1_freq = 123.34;
    float tone2_freq = 231.43;
    float tone1_ampl = Tone::NORMAL;
    float tone2_ampl = Tone::WEAK;

    GenerateSignal signal(sample_rate);
    signal.add_tone(tone1_freq, tone1_ampl);
    signal.add_tone(tone2_freq, tone2_ampl);
    signal.add_noise(Noise::NORMAL);

    Tone tone1(sample_rate, tone1_freq, tone1_ampl);
    Tone tone2(sample_rate, tone2_freq, tone2_ampl);
    float max_value = 0;
    float min_value = 0;
    for (int i = 0; i < 60 * sample_rate; ++i) {
        float sample_noise = signal.get_sample() - tone1.get_sample() - tone2.get_sample();
        min_value = min(sample_noise, min_value);
        max_value = max(sample_noise, max_value);
    }

    EXPECT_GT(max_value, 0);
    EXPECT_LT(max_value, Noise::NORMAL);

    EXPECT_LT(min_value, 0);
    EXPECT_GT(min_value, -1.0 * Noise::NORMAL);
}
