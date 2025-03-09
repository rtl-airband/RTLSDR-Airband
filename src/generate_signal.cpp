/*
 * generate_signal.cpp
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

#include <cmath>

#include "generate_signal.h"

using namespace std;

float Tone::WEAK = 0.05;
float Tone::NORMAL = 0.2;
float Tone::STRONG = 0.4;

Tone::Tone(int sample_rate, const float& freq, const float& ampl) : sample_rate_(sample_rate), freq_(freq), ampl_(ampl), sample_count_(0) {}

float Tone::get_sample(void) {
    sample_count_++;
    return ampl_ * sin(2 * M_PI * sample_count_ * freq_ / sample_rate_);
}

float Noise::WEAK = 0.05;
float Noise::NORMAL = 0.2;
float Noise::STRONG = 0.5;

Noise::Noise(const float& ampl) : ampl_(ampl) {
    // create a seeded generator
    std::random_device r;
    std::seed_seq s{r(), r(), r(), r(), r(), r(), r(), r()};
    generator = std::mt19937(s);

    // centered at 0.0, standard deviation of 0.1
    distribution = normal_distribution<float>(0.0, 0.1);
}
float Noise::get_sample(void) {
    return ampl_ * distribution(generator);
}

GenerateSignal::GenerateSignal(int sample_rate) : sample_rate_(sample_rate) {}

void GenerateSignal::add_tone(const float& freq, const float& ampl) {
    tones_.push_back(Tone(sample_rate_, freq, ampl));
}

void GenerateSignal::add_noise(const float& ampl) {
    noises_.push_back(Noise(ampl));
}

float GenerateSignal::get_sample(void) {
    float value = 0.0;

    for (vector<Tone>::iterator tone = tones_.begin(); tone != tones_.end(); ++tone) {
        value += tone->get_sample();
    }

    for (vector<Noise>::iterator noise = noises_.begin(); noise != noises_.end(); ++noise) {
        value += noise->get_sample();
    }

    return value;
}

void GenerateSignal::write_file(const string& filepath, const float& seconds) {
    FILE* fp = fopen(filepath.c_str(), "wb");

    for (int i = 0; i < sample_rate_ * seconds; ++i) {
        float sample = get_sample();
        fwrite(&sample, sizeof(float), 1, fp);
    }
    fclose(fp);
}
