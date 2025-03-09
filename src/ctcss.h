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

#ifndef _CTCSS_H
#define _CTCSS_H 1

#include <cstddef>  // size_t
#include <vector>

class ToneDetector {
   public:
    ToneDetector(float tone_freq, float sample_freq, int window_size);
    void process_sample(const float& sample);
    void reset(void);

    const float& relative_power(void) const { return magnitude_; }
    const float& freq(void) const { return tone_freq_; }
    const float& coefficient(void) const { return coeff_; }

   private:
    float tone_freq_;
    float magnitude_;

    int window_size_;
    float coeff_;

    int count_;
    float q0_;
    float q1_;
    float q2_;
};

class ToneDetectorSet {
   public:
    struct PowerIndex {
        float power;
        float freq;
    };

    ToneDetectorSet() {}

    bool add(const float& tone_freq, const float& sample_freq, int window_size);
    void process_sample(const float& sample);
    void reset(void);

    float sorted_powers(std::vector<PowerIndex>& powers);

   private:
    std::vector<ToneDetector> tones_;
};

class CTCSS {
   public:
    CTCSS(void) : enabled_(false), found_count_(0), not_found_count_(0) {}
    CTCSS(const float& ctcss_freq, const float& sample_rate, int window_size);
    void process_audio_sample(const float& sample);
    void reset(void);

    const size_t& found_count(void) const { return found_count_; }
    const size_t& not_found_count(void) const { return not_found_count_; }

    bool is_enabled(void) const { return enabled_; }
    bool enough_samples(void) const { return enough_samples_; }
    bool has_tone(void) const { return !enabled_ || has_tone_; }

    static std::vector<float> standard_tones;

   private:
    bool enabled_;
    float ctcss_freq_;
    int window_size_;
    size_t found_count_;
    size_t not_found_count_;

    ToneDetectorSet powers_;

    bool enough_samples_;
    int sample_count_;
    bool has_tone_;
};

#endif /* _CTCSS_H */
