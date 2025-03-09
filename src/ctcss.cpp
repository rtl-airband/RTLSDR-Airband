/*
 * ctcss.h
 *
 * Copyright (C) 2022-2023 charlie-foxtrot
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

#include <math.h>     // M_PI
#include <algorithm>  // sort

#include "logging.h"  // debug_print()

#include "ctcss.h"

using namespace std;

// Implementation of https://www.embedded.com/detecting-ctcss-tones-with-goertzels-algorithm/
// also https://www.embedded.com/the-goertzel-algorithm/
ToneDetector::ToneDetector(float tone_freq, float sample_rate, int window_size) {
    tone_freq_ = tone_freq;
    magnitude_ = 0.0;

    window_size_ = window_size;

    int k = (0.5 + window_size * tone_freq / sample_rate);
    float omega = (2.0 * M_PI * k) / window_size;
    coeff_ = 2.0 * cos(omega);

    reset();
}

void ToneDetector::process_sample(const float& sample) {
    q0_ = coeff_ * q1_ - q2_ + sample;
    q2_ = q1_;
    q1_ = q0_;

    count_++;
    if (count_ == window_size_) {
        magnitude_ = q1_ * q1_ + q2_ * q2_ - q1_ * q2_ * coeff_;
        count_ = 0;
    }
}

void ToneDetector::reset(void) {
    count_ = 0;
    q0_ = q1_ = q2_ = 0.0;
}

bool ToneDetectorSet::add(const float& tone_freq, const float& sample_rate, int window_size) {
    ToneDetector new_tone = ToneDetector(tone_freq, sample_rate, window_size);

    for (const auto tone : tones_) {
        if (new_tone.coefficient() == tone.coefficient()) {
            debug_print("Skipping tone %f, too close to other tones\n", tone_freq);
            return false;
        }
    }

    tones_.push_back(new_tone);
    return true;
}

void ToneDetectorSet::process_sample(const float& sample) {
    for (vector<ToneDetector>::iterator it = tones_.begin(); it != tones_.end(); ++it) {
        it->process_sample(sample);
    }
}

void ToneDetectorSet::reset(void) {
    for (vector<ToneDetector>::iterator it = tones_.begin(); it != tones_.end(); ++it) {
        it->reset();
    }
}

float ToneDetectorSet::sorted_powers(vector<ToneDetectorSet::PowerIndex>& powers) {
    powers.clear();

    float total_power = 0.0;
    for (size_t i = 0; i < tones_.size(); ++i) {
        powers.push_back({tones_[i].relative_power(), tones_[i].freq()});
        total_power += tones_[i].relative_power();
    }

    sort(powers.begin(), powers.end(), [](PowerIndex a, PowerIndex b) { return a.power > b.power; });

    return total_power / tones_.size();
}

vector<float> CTCSS::standard_tones = {67.0,  69.3,  71.9,  74.4,  77.0,  79.7,  82.5,  85.4,  88.5,  91.5,  94.8,  97.4,  100.0, 103.5, 107.2, 110.9, 114.8,
                                       118.8, 123.0, 127.3, 131.8, 136.5, 141.3, 146.2, 150.0, 151.4, 156.7, 159.8, 162.2, 165.5, 167.9, 171.3, 173.8, 177.3,
                                       179.9, 183.5, 186.2, 189.9, 192.8, 196.6, 199.5, 203.5, 206.5, 210.7, 218.1, 225.7, 229.1, 233.6, 241.8, 250.3, 254.1};

CTCSS::CTCSS(const float& ctcss_freq, const float& sample_rate, int window_size) : enabled_(true), ctcss_freq_(ctcss_freq), window_size_(window_size), found_count_(0), not_found_count_(0) {
    debug_print("Adding CTCSS detector for %f Hz with a sample rate of %f and window %d\n", ctcss_freq, sample_rate, window_size_);

    // Add the target CTCSS frequency first followed by the other "standard tones", except those
    // within +/- 5 Hz
    powers_.add(ctcss_freq, sample_rate, window_size_);

    for (const auto tone : standard_tones) {
        if (abs(ctcss_freq - tone) < 5) {
            debug_print("Skipping tone %f, too close to other tones\n", tone);
            continue;
        }
        powers_.add(tone, sample_rate, window_size_);
    }

    // clear all values to start NOTE: has_tone_ will be true until the first window count of samples are processed
    reset();
}

void CTCSS::process_audio_sample(const float& sample) {
    if (!enabled_) {
        return;
    }

    powers_.process_sample(sample);

    sample_count_++;
    if (sample_count_ < window_size_) {
        return;
    }

    enough_samples_ = true;

    // if this is sample fills out the window then check if one of the "strongest"
    // tones is the CTCSS tone we are looking for.  NOTE: there can be multiple "strongest"
    // tones based on floating point math
    vector<ToneDetectorSet::PowerIndex> tone_powers;
    float avg_power = powers_.sorted_powers(tone_powers);
    float ctcss_tone_power = 0.0;
    for (const auto i : tone_powers) {
        if (i.freq == ctcss_freq_) {
            ctcss_tone_power = i.power;
            break;
        }
    }
    if (ctcss_tone_power == tone_powers[0].power && ctcss_tone_power > avg_power) {
        debug_print("CTCSS tone of %f Hz detected\n", ctcss_freq_);
        has_tone_ = true;
        found_count_++;
    } else {
        debug_print("CTCSS tone of %f Hz not detected - highest power was %f Hz at %f vs %f\n", ctcss_freq_, tone_powers[0].freq, tone_powers[0].power, ctcss_tone_power);
        has_tone_ = false;
        not_found_count_++;
    }

    // reset everything for the next window's worth of samples
    powers_.reset();
    sample_count_ = 0;
}

void CTCSS::reset(void) {
    if (enabled_) {
        powers_.reset();
        enough_samples_ = false;
        sample_count_ = 0;
        has_tone_ = false;
    }
}
