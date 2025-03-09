/*
 * squelch.cpp
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

#include "squelch.h"

#ifdef DEBUG_SQUELCH
#include <errno.h>   // errno
#include <string.h>  // strerror()
#endif               /* DEBUG_SQUELCH _*/

#include <stdlib.h>   // calloc()
#include <algorithm>  // min()
#include <cassert>    // assert()
#include <cmath>      // pow()

#include "logging.h"  // debug_print()

using namespace std;

Squelch::Squelch(void) {
    noise_floor_ = 5.0f;
    set_squelch_snr_threshold(9.54f);  // depends on noise_floor_, sets using_manual_level_, normal_signal_ratio_, flappy_signal_ratio_, and moving_avg_cap_
    manual_signal_level_ = -1.0;

    pre_filter_ = {0.001f, 0.001f};
    post_filter_ = {0.001f, 0.001f};

    squelch_level_ = 0.0f;

    using_post_filter_ = false;
    pre_vs_post_factor_ = 0.9f;

    open_delay_ = 197;
    close_delay_ = 197;
    low_signal_abort_ = 88;

    next_state_ = CLOSED;
    current_state_ = CLOSED;

    delay_ = 0;
    open_count_ = 0;
    sample_count_ = -1;
    flappy_count_ = 0;
    low_signal_count_ = 0;

    recent_sample_size_ = 1000;
    flap_opens_threshold_ = 3;
    recent_open_count_ = 0;
    closed_sample_count_ = 0;

    buffer_size_ = 102;  // NOTE: this is specific to the 2nd order lowpass Bessel filter
    buffer_head_ = 0;
    buffer_tail_ = 1;
    buffer_ = (float*)calloc(buffer_size_, sizeof(float));

#ifdef DEBUG_SQUELCH
    debug_file_ = NULL;
    raw_input_ = 0.0;
    filtered_input_ = 0.0;
#endif /* DEBUG_SQUELCH */

    assert(open_delay_ > buffer_size_);

    debug_print("Created Squelch, open_delay_: %d, close_delay_: %d, low_signal_abort: %d, using_manual_level_: %s\n", open_delay_, close_delay_, low_signal_abort_,
                using_manual_level_ ? "true" : "false");
}

void Squelch::set_squelch_level_threshold(const float& level) {
    if (level > 0) {
        using_manual_level_ = true;
        manual_signal_level_ = level;
    } else {
        using_manual_level_ = false;
    }

    // Need to update moving_avg_cap_ - depends on using_manual_level_ and manual_signal_level_
    calculate_moving_avg_cap();

    debug_print("Set level threshold, using_manual_level_: %s, manual_signal_level_: %f, moving_avg_cap_: %f\n", using_manual_level_ ? "true" : "false", manual_signal_level_, moving_avg_cap_);
}

void Squelch::set_squelch_snr_threshold(const float& db) {
    using_manual_level_ = false;
    normal_signal_ratio_ = pow(10.0, db / 20.0);
    flappy_signal_ratio_ = normal_signal_ratio_ * 0.9f;

    // Need to update moving_avg_cap_ - depends on using_manual_level_ and normal_signal_ratio_
    calculate_moving_avg_cap();

    debug_print("SNR threshold updated, using_manual_level_: %s, normal_signal_ratio_: %f, flappy_signal_ratio_: %f, moving_avg_cap_: %f\n", using_manual_level_ ? "true" : "false",
                normal_signal_ratio_, flappy_signal_ratio_, moving_avg_cap_);
}

void Squelch::set_ctcss_freq(const float& ctcss_freq, const float& sample_rate) {
    // create two CTCSS detectors with different window sizes.  0.4 sec is required to tell between all the "standard"
    // tones but 0.05 is enough to tell between tones ~20 Hz appart.  Will use ctcss_fast_ until there are enough samples
    // for ctcss_slow_
    ctcss_fast_ = CTCSS(ctcss_freq, sample_rate, sample_rate * 0.05);
    ctcss_slow_ = CTCSS(ctcss_freq, sample_rate, sample_rate * 0.4);
}

bool Squelch::is_open(void) const {
    // if current state is OPEN or CLOSING then decide based on CTCSS (if enabled)
    if (current_state_ == OPEN || current_state_ == CLOSING) {
        // if CTCSS is enabled then use slow (more accurate) if it has enough samples, otherwise
        // use fast (will return false if also not enough samples)
        if (ctcss_slow_.is_enabled()) {
            if (ctcss_slow_.enough_samples()) {
                return ctcss_slow_.has_tone();
            }
            return ctcss_fast_.has_tone();
        }

        return true;
    }

    return false;
}

bool Squelch::should_filter_sample(void) {
    return ((has_pre_filter_signal() || current_state_ != CLOSED) && current_state_ != LOW_SIGNAL_ABORT);
}

bool Squelch::should_process_audio(void) {
    return (current_state_ == OPEN || current_state_ == CLOSING);
}

bool Squelch::first_open_sample(void) const {
    return (current_state_ != OPEN && next_state_ == OPEN);
}

bool Squelch::last_open_sample(void) const {
    return (current_state_ == CLOSING && next_state_ == CLOSED) || (current_state_ != LOW_SIGNAL_ABORT && next_state_ == LOW_SIGNAL_ABORT);
}

bool Squelch::signal_outside_filter(void) {
    return (using_post_filter_ && has_pre_filter_signal() && !has_post_filter_signal());
}

const float& Squelch::noise_level(void) const {
    return noise_floor_;
}

const float& Squelch::signal_level(void) const {
    return pre_filter_.full_;
}

const float& Squelch::squelch_level(void) {
    if (using_manual_level_) {
        return manual_signal_level_;
    }

    if (squelch_level_ == 0.0f) {
        if (currently_flapping() && flappy_signal_ratio_ < normal_signal_ratio_) {
            squelch_level_ = flappy_signal_ratio_ * noise_floor_;
        } else {
            squelch_level_ = normal_signal_ratio_ * noise_floor_;
        }
    }
    return squelch_level_;
}

const size_t& Squelch::open_count(void) const {
    return open_count_;
}

const size_t& Squelch::flappy_count(void) const {
    return flappy_count_;
}

const size_t& Squelch::ctcss_count(void) const {
    return ctcss_slow_.found_count();
}

const size_t& Squelch::no_ctcss_count(void) const {
    return ctcss_slow_.not_found_count();
}

void Squelch::process_raw_sample(const float& sample) {
    // Update current state based on previous state from last iteration
    update_current_state();

#ifdef DEBUG_SQUELCH
    raw_input_ = sample;
#endif /* DEBUG_SQUELCH */

    sample_count_++;

    // Auto noise floor
    //  - Doing this every 16 samples instead of every sample allows a gradual signal increase
    //    to cross the squelch threshold (that is a function of the noise floor) sooner.
    //  - Updating even when squelch is open and / or signal is outside filter means the noise
    //    floor (and squelch threshold) will slowly increasing during a long signal.  This can lead
    //    to flapping, but this keeps a sudden and sustained increase of noise from locking squelch
    //    OPEN.
    if (sample_count_ % 16 == 0) {
        calculate_noise_floor();
    }

    update_moving_avg(pre_filter_, sample);

    // Apply the comparison factor before adding to the buffer, will later be used as the threshold
    // for the post_filter_
    buffer_[buffer_head_] = pre_filter_.capped_ * pre_vs_post_factor_;

    // Check signal against thresholds
    if (current_state_ == OPEN && !has_signal()) {
        debug_print("Closing at %zu: no signal after timeout (%f, %f, %f)\n", sample_count_, pre_filter_.capped_, post_filter_.capped_, squelch_level());
        set_state(CLOSING);
    }

    if (current_state_ == CLOSED && has_signal()) {
        debug_print("Opening at %zu: signal (%f, %f, %f)\n", sample_count_, pre_filter_.capped_, post_filter_.capped_, squelch_level());
        set_state(OPENING);
    }

    // Override squelch and close if there are repeated samples under the squelch level
    // NOTE: this can cause squelch to close, but it may immediately be re-opened if the signal level still hasn't fallen after the delays
    if (current_state_ != CLOSED && current_state_ != LOW_SIGNAL_ABORT) {
        if (sample >= squelch_level()) {
            low_signal_count_ = 0;
        } else {
            low_signal_count_++;
            if (low_signal_count_ >= low_signal_abort_) {
                debug_print("Low signal abort at %zu: low signal count %d\n", sample_count_, low_signal_count_);
                set_state(LOW_SIGNAL_ABORT);
            }
        }
    }
}

void Squelch::process_filtered_sample(const float& sample) {
#ifdef DEBUG_SQUELCH
    filtered_input_ = sample;
#endif /* DEBUG_SQUELCH */

    if (!should_filter_sample()) {
        return;
    }

    if (current_state_ == OPENING) {
        // While OPENING, need to wait until the pre-filter value gets through the buffer
        if (delay_ < buffer_size_) {
            return;
        }
        // Buffer has been filled, initialize post-filter with the pre-filter value
        if (delay_ == buffer_size_) {
            post_filter_ = {buffer_[buffer_tail_], buffer_[buffer_tail_]};
        }
    }

    using_post_filter_ = true;
    update_moving_avg(post_filter_, sample);

    // Always comparing the post-filter average to the buffered pre-filtered value
    if (post_filter_.capped_ < buffer_[buffer_tail_]) {
        debug_print("Closing at %zu: signal level post filter (%f < %f)\n", sample_count_, post_filter_.capped_, squelch_level());
        set_state(CLOSED);
    }
}

void Squelch::process_audio_sample(const float& sample) {
#ifdef DEBUG_SQUELCH
    audio_input_ = sample;
#endif /* DEBUG_SQUELCH */

    if (!ctcss_slow_.is_enabled()) {
        return;
    }

    // ctcss_ is reset on transition to CLOSED and stays "unused" while CLOSED
    if (current_state_ != CLOSED) {
        // always send the sample to the slow (more accurate) detector, also send to the fast if there havent been enough yet
        ctcss_slow_.process_audio_sample(sample);
        if (!ctcss_slow_.enough_samples()) {
            ctcss_fast_.process_audio_sample(sample);
        }
    }
}

void Squelch::set_state(State update) {
    // Valid transitions (current_state_ -> next_state_) are:

    //  - CLOSED -> CLOSED
    //  - CLOSED -> OPENING
    //    ---------------------------
    //  - OPENING -> CLOSED
    //  - OPENING -> OPENING
    //  - OPENING -> CLOSING
    //  - OPENING -> OPEN
    //    ---------------------------
    //  - CLOSING -> CLOSED
    //  - CLOSING -> OPENING
    //  - CLOSING -> CLOSING
    //  - CLOSING -> LOW_SIGNAL_ABORT
    //  - CLOSING -> OPEN
    //    ---------------------------
    //  - LOW_SIGNAL_ABORT -> CLOSED
    //  - LOW_SIGNAL_ABORT -> LOW_SIGNAL_ABORT
    //    ---------------------------
    //  - OPEN -> CLOSING
    //  - OPEN -> LOW_SIGNAL_ABORT
    //  - OPEN -> OPEN

    // Invalid transistions (current_state_ -> next_state_) are:

    //  CLOSED -> CLOSING (if already CLOSED cant go backwards)
    if (current_state_ == CLOSED && update == CLOSING) {
        update = CLOSED;
    }

    //  CLOSED -> LOW_SIGNAL_ABORT (if already CLOSED cant go backwards)
    else if (current_state_ == CLOSED && update == LOW_SIGNAL_ABORT) {
        update = CLOSED;
    }

    //  CLOSED -> OPEN (must go through OPENING to get to OPEN)
    else if (current_state_ == CLOSED && update == OPEN) {
        update = OPENING;
    }

    //  OPENING -> LOW_SIGNAL_ABORT (just go to CLOSED instead)
    else if (current_state_ == OPENING && update == LOW_SIGNAL_ABORT) {
        update = CLOSED;
    }

    //  LOW_SIGNAL_ABORT -> OPENING (LOW_SIGNAL_ABORT can only go to CLOSED)
    //  LOW_SIGNAL_ABORT -> OPEN (LOW_SIGNAL_ABORT can only go to CLOSED)
    //  LOW_SIGNAL_ABORT -> CLOSING (LOW_SIGNAL_ABORT can only go to CLOSED)
    else if (current_state_ == LOW_SIGNAL_ABORT && update != LOW_SIGNAL_ABORT && update != CLOSED) {
        update = CLOSED;
    }

    //  OPEN -> CLOSED (must go through CLOSING to get to CLOSED)
    else if (current_state_ == OPEN && update == CLOSED) {
        update = CLOSING;
    }

    //  OPEN -> OPENING (if already OPEN cant go backwards)
    else if (current_state_ == OPEN && update == OPENING) {
        update = OPEN;
    }

    next_state_ = update;
}

void Squelch::update_current_state(void) {
    if (next_state_ == OPENING) {
        if (current_state_ != OPENING) {
            debug_print("%zu: transitioning to OPENING\n", sample_count_);
            delay_ = 0;
            low_signal_count_ = 0;
            using_post_filter_ = false;
            current_state_ = next_state_;
        } else {
            // in OPENING delay
            delay_++;
            if (delay_ >= open_delay_) {
                // After getting through OPENING delay, count this as an "open" for flap
                // detection even if signal has gone.  NOTE - if process_filtered_sample() would
                // have already sent state to CLOSED before the delay if post_filter_.capped_ was
                // too low, so that wont count towards flapping
                if (closed_sample_count_ < recent_sample_size_) {
                    recent_open_count_++;
                    if (currently_flapping()) {
                        flappy_count_++;
                    }

                    // Force squelch_level_ recalculation at next call to squelch_level()
                    squelch_level_ = 0.0f;
                }

                // Check signal level after delay to either go to OPEN or CLOSED
                if (has_signal()) {
                    next_state_ = OPEN;
                } else {
                    debug_print("%zu: no signal after OPENING delay, going to CLOSED\n", sample_count_);
                    next_state_ = CLOSED;
                }
            }
        }
    } else if (next_state_ == CLOSING) {
        if (current_state_ != CLOSING) {
            debug_print("%zu: transitioning to CLOSING\n", sample_count_);
            delay_ = 0;
            current_state_ = next_state_;
        } else {
            // in CLOSING delay
            delay_++;
            if (delay_ >= close_delay_) {
                if (!has_signal()) {
                    next_state_ = CLOSED;
                } else {
                    debug_print("%zu: signal after CLOSING delay, reverting to OPEN\n", sample_count_);
                    current_state_ = OPEN;  // set current_state_ to avoid incrementing open_count_
                    next_state_ = OPEN;
                }
            }
        }
    } else if (next_state_ == LOW_SIGNAL_ABORT) {
        if (current_state_ != LOW_SIGNAL_ABORT) {
            debug_print("%zu: transitioning to LOW_SIGNAL_ABORT\n", sample_count_);
            // If coming from CLOSING then keep the delay counter that has already started
            if (current_state_ != CLOSING) {
                delay_ = 0;
            }
            current_state_ = next_state_;
        } else {
            // in LOW_SIGNAL_ABORT delay
            delay_++;
            if (delay_ >= close_delay_) {
                next_state_ = CLOSED;
            }
        }
    } else if (next_state_ == OPEN && current_state_ != OPEN) {
        debug_print("%zu: transitioning to OPEN\n", sample_count_);
        open_count_++;
        current_state_ = next_state_;
    } else if (next_state_ == CLOSED && current_state_ != CLOSED) {
        debug_print("%zu: transitioning to CLOSED\n", sample_count_);
        using_post_filter_ = false;
        closed_sample_count_ = 0;
        current_state_ = next_state_;
        ctcss_fast_.reset();
        ctcss_slow_.reset();
    } else if (next_state_ == CLOSED && current_state_ == CLOSED) {
        // Count this as a closed sample towards flap detection (can stop counting at recent_sample_size_)
        if (closed_sample_count_ < recent_sample_size_) {
            closed_sample_count_++;
        } else if (closed_sample_count_ == recent_sample_size_) {
            recent_open_count_ = 0;
            squelch_level_ = 0.0f;  // Force squelch_level_ recalculation
        }
    } else {
        current_state_ = next_state_;
    }

    buffer_tail_ = (buffer_tail_ + 1) % buffer_size_;
    buffer_head_ = (buffer_head_ + 1) % buffer_size_;

#ifdef DEBUG_SQUELCH
    debug_state();
#endif /* DEBUG_SQUELCH */
}

bool Squelch::has_pre_filter_signal(void) {
    return pre_filter_.capped_ >= squelch_level();
}

bool Squelch::has_post_filter_signal(void) {
    return using_post_filter_ && post_filter_.capped_ >= buffer_[buffer_tail_];
}

bool Squelch::has_signal(void) {
    if (using_post_filter_) {
        return has_pre_filter_signal() && has_post_filter_signal();
    }
    return has_pre_filter_signal();
}

void Squelch::calculate_noise_floor(void) {
    static const float decay_factor = 0.97f;
    static const float new_factor = 1.0 - decay_factor;

    noise_floor_ = noise_floor_ * decay_factor + std::min(pre_filter_.capped_, noise_floor_) * new_factor + 1e-6f;

    debug_print("%zu: noise floor is now %f\n", sample_count_, noise_floor_);

    // Need to update moving_avg_cap_ - depends on noise_floor_
    calculate_moving_avg_cap();

    // Force squelch_level_ recalculation at next call to squelch_level() - depends on noise_floor_
    squelch_level_ = 0.0f;
}

void Squelch::calculate_moving_avg_cap(void) {
    // set max value for MovingAverage's capped_ to 1.5 x the normal / manual squelch level.
    if (using_manual_level_) {
        moving_avg_cap_ = 1.5f * manual_signal_level_;
    } else {
        moving_avg_cap_ = 1.5f * normal_signal_ratio_ * noise_floor_;
    }
}

void Squelch::update_moving_avg(MovingAverage& avg, const float& sample) {
    static const float decay_factor = 0.99f;
    static const float new_factor = 1.0 - decay_factor;

    avg.full_ = avg.full_ * decay_factor + sample * new_factor;

    // Cap average level, this lets the average drop after the signal goes away more quickly
    // (if current value and update are both at/above the max then can avoid the float multiplications)
    if (avg.capped_ >= moving_avg_cap_ && sample >= moving_avg_cap_) {
        avg.capped_ = moving_avg_cap_;
    } else {
        avg.capped_ = min(moving_avg_cap_, avg.capped_ * decay_factor + sample * new_factor);
    }
}

bool Squelch::currently_flapping(void) const {
    return recent_open_count_ >= flap_opens_threshold_;
}

#ifdef DEBUG_SQUELCH
/*
 Debug file methods
 ==================

 Values written to file are:
         - (int16_t) process_raw_sample input
         - (int16_t) process_filtered_sample input
         - (int16_t) process_audio_sample input
         - (int16_t) noise_floor_
         - (int16_t) pre_filter_.capped_
         - (int16_t) post_filter_.capped_
         - (int) current_state_
         - (int) delay_
         - (int) low_signalcount_
         - (int) ctcss_fast_.has_tone()
         - (int) ctcss_slow_.has_tone()

  The output file can be read / plotted in python as follows:

        import matplotlib.pyplot as plt
        import numpy as np

        def plot_squelch_debug(filepath):

                dt = np.dtype([('raw_input', np.single),
                                           ('filtered_input', np.single),
                                           ('audio_input', np.single),
                                           ('noise_floor', np.single),
                                           ('pre_filter_capped', np.single),
                                           ('post_filter_capped', np.single),
                                           ('current_state', np.intc),
                                           ('delay', np.intc),
                                           ('low_signalcount', np.intc),
                                           ('ctcss_fast_has_tone', np.intc),
                                           ('ctcss_slow_has_tone', np.intc)
                                          ])

                dat = np.fromfile(filepath, dtype=dt)

                plt.figure()
                plt.plot(dat['raw_input'], 'b')
                plt.plot(dat['pre_filter_capped'], 'g')
                plt.plot(dat['noise_floor'], 'r')
                plt.show(block=False)

                plt.figure()
                plt.plot(dat['post_filter_capped'], 'k')
                plt.show(block=False)

                plt.figure()
                axis = plt.subplot2grid((3, 1), (0, 0))
                axis.plot(dat['current_state'], 'c')
                axis = plt.subplot2grid((3, 1), (1, 0))
                axis.plot(dat['delay'], 'm')
                axis = plt.subplot2grid((3, 1), (2, 0))
                axis.plot(dat['low_signalcount'], 'y')
                plt.show(block=False)

                return

  */

Squelch::~Squelch(void) {
    if (debug_file_) {
        fclose(debug_file_);
    }
}

void Squelch::set_debug_file(const char* filepath) {
    debug_file_ = fopen(filepath, "wb");
}

void Squelch::debug_value(const float& value) {
    if (!debug_file_) {
        return;
    }

    if (fwrite(&value, sizeof(value), 1, debug_file_) != 1) {
        debug_print("Error writing to squelch debug file: %s\n", strerror(errno));
    }
}

void Squelch::debug_value(const int& value) {
    if (!debug_file_) {
        return;
    }

    if (fwrite(&value, sizeof(value), 1, debug_file_) != 1) {
        debug_print("Error writing to squelch debug file: %s\n", strerror(errno));
    }
}

void Squelch::debug_state(void) {
    if (!debug_file_) {
        return;
    }
    debug_value(raw_input_);
    debug_value(filtered_input_);
    debug_value(audio_input_);

    raw_input_ = 0.0;
    filtered_input_ = 0.0;
    audio_input_ = 0.0;

    debug_value(noise_floor_);
    debug_value(pre_filter_.capped_);
    debug_value(post_filter_.capped_);
    debug_value((int)current_state_);
    debug_value(delay_);
    debug_value(low_signal_count_);
    debug_value((int)ctcss_fast_.has_tone());
    debug_value((int)ctcss_slow_.has_tone());
}

#endif /* DEBUG_SQUELCH */
