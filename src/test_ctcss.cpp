/*
 * test_ctcss.cpp
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

#include "generate_signal.h"
#include "test_base_class.h"

#include "ctcss.h"

using namespace std;

class CTCSSTest : public TestBaseClass {
   protected:
    int sample_rate;
    int fast_window_size;
    int slow_window_size;

    void SetUp(void) {
        TestBaseClass::SetUp();
        sample_rate = 8000;
        fast_window_size = sample_rate * 0.05;
        slow_window_size = sample_rate * 0.4;
    }

    void write_file(const vector<float>& samples, const string& filepath) {
        cerr << "writing file out to " << filepath << endl;

        FILE* fp = fopen(filepath.c_str(), "wb");

        for (auto sample : samples) {
            fwrite(&sample, sizeof(float), 1, fp);
        }
        fclose(fp);
    }

    void load_from_file(CTCSS& ctcss, const string& filepath) {
        FILE* fp = fopen(filepath.c_str(), "rb");

        while (!ctcss.enough_samples()) {
            float sample;
            if (fread(&sample, sizeof(float), 1, fp) != 1) {
                break;
            }
            ctcss.process_audio_sample(sample);
        }
        fclose(fp);

        ASSERT_TRUE(ctcss.enough_samples());
    }

    void test_all_tones(GenerateSignal& signal, const float& tone = 0) {
        for (auto standard_tone : CTCSS::standard_tones) {
            // skipping tones within +/- 5Hz
            if (abs(standard_tone - tone) < 5) {
                continue;
            }

            CTCSS ctcss(standard_tone, sample_rate, slow_window_size);
            vector<float> samples;
            run_signal(ctcss, signal, samples);

            EXPECT_FALSE(ctcss.has_tone()) << "Tone of " << standard_tone << " found, expected " << tone;

            // on failure write out a file for debugging
            if (ctcss.has_tone()) {
                // double the samples to write to the file for later testing
                size_t initial_count = samples.size();
                while (samples.size() < initial_count * 2) {
                    samples.push_back(signal.get_sample());
                }

                string filepath = "/tmp/found_" + to_string(standard_tone) + "_expected_" + to_string(tone);
                write_file(samples, filepath);
            }
        }
        if (tone != 0) {
            CTCSS ctcss(tone, sample_rate, slow_window_size);
            vector<float> samples;
            run_signal(ctcss, signal, samples);

            EXPECT_TRUE(ctcss.has_tone()) << "Expected tone of " << tone << " not found";

            // on failure write out a file for debugging
            if (!ctcss.has_tone()) {
                // double the samples to write to the file for later testing
                size_t initial_count = samples.size();
                while (samples.size() < initial_count * 2) {
                    samples.push_back(signal.get_sample());
                }

                string filepath = "/tmp/didnt_find_" + to_string(tone);
                write_file(samples, filepath);
            }
        }
    }

    void run_signal(CTCSS& ctcss, GenerateSignal& signal, vector<float>& samples) {
        EXPECT_TRUE(ctcss.is_enabled()) << "CTCSS not enabled";
        while (!ctcss.enough_samples()) {
            float sample = signal.get_sample();
            samples.push_back(sample);
            ctcss.process_audio_sample(sample);
        }
    }
};

TEST_F(CTCSSTest, creation) {
    CTCSS ctcss;
    EXPECT_FALSE(ctcss.is_enabled());
}

TEST_F(CTCSSTest, no_signal) {
    GenerateSignal signal(sample_rate);
    test_all_tones(signal);
}

TEST_F(CTCSSTest, has_tone) {
    float tone = CTCSS::standard_tones[0];
    GenerateSignal signal(sample_rate);
    signal.add_tone(tone, Tone::NORMAL);
    signal.add_noise(Noise::NORMAL);
    test_all_tones(signal, tone);
}

TEST_F(CTCSSTest, has_non_standard_tone) {
    float tone = (CTCSS::standard_tones[0] + CTCSS::standard_tones[0]) / 2;
    GenerateSignal signal(sample_rate);
    signal.add_tone(tone, Tone::NORMAL);
    signal.add_noise(Noise::NORMAL);
    test_all_tones(signal, tone);
}

TEST_F(CTCSSTest, has_each_standard_tone) {
    for (auto tone : CTCSS::standard_tones) {
        GenerateSignal signal(sample_rate);
        signal.add_tone(tone, Tone::NORMAL);
        signal.add_noise(Noise::NORMAL);
        test_all_tones(signal, tone);
    }
}
