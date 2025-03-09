/*
 * squelch.h
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

#ifndef _SQUELCH_H
#define _SQUELCH_H

#include <cstddef>  // size_t

#ifdef DEBUG_SQUELCH
#include <stdio.h>  // needed for debug file output
#endif              /* DEBUG_SQUELCH */

#include "ctcss.h"

/*
 Theory of operation:

 Squelch has 5 states, OPEN (has audio), CLOSED (no audio), OPENING (transitioning from CLOSED to OPEN),
 CLOSING (transitioning from OPEN to CLOSED), and LOW_SIGNAL_ABORT (same as CLOSING but because of a constant
 signal drop).

 Squelch is considered "open" when the state is OPEN or CLOSING and squelch is considered "closed" when the
 state is OPENING, LOW_SIGNAL_ABORT, or CLOSED.

 Noise floor is computed using a low pass filter and updated with the current sample or prior value, whatever
 is lower.  Noise floor is updated every 16 stamples, except when squelch is open.

 Low pass filters are also used to track the current signal levels.  One level is for the sample before
 filtering, the second for post signal filtering (if any).  The pre-filter signal level is updated for every
 sample.  The post-filter level is optional.  When used, the post-filter signal level is compared to a
 delayed pre-filter value.  The post-filter is set to a fraction of the pre-filtered value each time state
 transitions to OPENING, and is not updated while state is CLOSED.

 Squelch level can be set manually or is computed as a function of the noise floor.

 When the signal level exceeds the squelch level, the state transitions to OPENING and a delay counter starts,
 then once the counter is over the state moves to OPEN if there is signal, otherwise back to CLOSED. The same
 (but opposite) happens when the signal level drops below the squelch level.

 While the squelch is OPEN, a count of continuous samples that are below the squelch level is maintained.  If
 this count exceeds a threshold then the state moves to LOW_SIGNAL_ABORT.  This allows the squelch to close
 after a sharp drop off in signal before the signal level has caught up.

 A count of "recent opens" is maintained as a way to detect squelch flapping (ie rapidly opening and closing).
 When flapping is detected the squelch level is decreased in an attempt to keep squelch open longer.

 CTCSS tone detection can be enabled.  If used, two tone detectors are created at different window lengths.
 The “fast” detector has less resolution but needs fewer samples while the “slow” detector is more accurate.
 When CTCSS is enabled, squelch remains CLOSED for an additional 0.05 sec until a tone is detected by the “fast”
 detector.
 */

class Squelch {
   public:
    Squelch();

    void set_squelch_level_threshold(const float& level);
    void set_squelch_snr_threshold(const float& db);
    void set_ctcss_freq(const float& ctcss_freq, const float& sample_rate);

    void process_raw_sample(const float& sample);
    void process_filtered_sample(const float& sample);
    void process_audio_sample(const float& sample);

    bool is_open(void) const;
    bool should_filter_sample(void);
    bool should_process_audio(void);

    bool first_open_sample(void) const;
    bool last_open_sample(void) const;
    bool signal_outside_filter(void);

    const float& noise_level(void) const;
    const float& signal_level(void) const;
    const float& squelch_level(void);

    const size_t& open_count(void) const;
    const size_t& flappy_count(void) const;
    const size_t& ctcss_count(void) const;
    const size_t& no_ctcss_count(void) const;

#ifdef DEBUG_SQUELCH
    ~Squelch(void);
    void set_debug_file(const char* filepath);
#endif /* DEBUG_SQUELCH */

   private:
    enum State {
        CLOSED,            // Audio is suppressed
        OPENING,           // Transitioning closed -> open
        CLOSING,           // Transitioning open -> closed
        LOW_SIGNAL_ABORT,  // Like CLOSING but is_open() is false
        OPEN               // Audio not suppressed
    };

    struct MovingAverage {
        float full_;
        float capped_;
    };

    float noise_floor_;          // noise level
    bool using_manual_level_;    // if using a manually set signal level threshold
    float manual_signal_level_;  // manually configured squelch level, < 0 for disabled
    float normal_signal_ratio_;  // signal-to-noise ratio for normal squelch - ratio, not in dB
    float flappy_signal_ratio_;  // signal-to-noise ratio for flappy squelch - ratio, not in dB

    float moving_avg_cap_;       // the max value for capped moving average
    MovingAverage pre_filter_;   // average signal level for reference sample
    MovingAverage post_filter_;  // average signal level for post-filter sample

    float squelch_level_;  // cached calculation of the squelch_level() value

    bool using_post_filter_;    // if the caller is providing filtered samples
    float pre_vs_post_factor_;  // multiplier when doing pre vs post filter compaison

    int open_delay_;        // how long to wait after signal level crosses squelch to open
    int close_delay_;       // how long to wait after signal level crosses squelch to close
    int low_signal_abort_;  // number of repeated samples below squelch to cause a close

    State next_state_;
    State current_state_;

    int delay_;             // samples to wait before making next squelch decision
    size_t open_count_;     // number of times squelch is opened
    size_t sample_count_;   // number of samples processed (for logging)
    size_t flappy_count_;   // number of times squelch was detected as flapping OPEN/CLOSED
    int low_signal_count_;  // number of repeated samples below squelch

    // Flap detection parameters
    size_t recent_sample_size_;    // number of samples defined as "recent"
    size_t flap_opens_threshold_;  // number of opens to count as flapping
    size_t recent_open_count_;     // number of times squelch recently opened
    size_t closed_sample_count_;   // number of continuous samples where squelch has been CLOSED

    // Buffered pre-filtered values
    int buffer_size_;  // size of buffer
    int buffer_head_;  // index to add new values
    int buffer_tail_;  // index to read buffered values
    float* buffer_;    // buffer

    CTCSS ctcss_fast_;  // ctcss tone detection
    CTCSS ctcss_slow_;  // ctcss tone detection

    void set_state(State update);
    void update_current_state(void);
    bool has_pre_filter_signal(void);
    bool has_post_filter_signal(void);
    bool has_signal(void);
    void calculate_noise_floor(void);
    void calculate_moving_avg_cap(void);
    void update_moving_avg(MovingAverage& avg, const float& sample);
    bool currently_flapping(void) const;

#ifdef DEBUG_SQUELCH
    FILE* debug_file_;
    float raw_input_;
    float filtered_input_;
    float audio_input_;
    void debug_value(const float& value);
    void debug_value(const int& value);
    void debug_state(void);
#endif /* DEBUG_SQUELCH */
};

#endif /* _SQUELCH_H */
