/*
 * rate_match.h
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

#ifndef _RATE_MATCH_H
#define _RATE_MATCH_H

#include <cstddef>

// Keeps a continuous output stream's buffer level steady when the SDR's
// sample clock and the sound server's clock disagree.
//
// The audio rate rtl_airband produces follows the device's real sample rate,
// which can be off nominal by tens to hundreds of ppm (and differs between
// starts on some devices). A sound server playing at its own clock then slowly
// drains (or fills) the stream until it underflows. RateMatcher watches the
// stream latency measured before each write and, through a PI loop, decides
// when to add or drop one sample; stretch() applies that to a batch.
class RateMatcher {
   public:
    // batch_s: duration of one write, used to integrate the error over time.
    explicit RateMatcher(double batch_s);

    // Forget everything, e.g. after the stream reconnects.
    void reset();

    // Feed the latency measured before the last write (ms, negative =
    // unknown) and the batch length in samples. Returns the change in length
    // for this batch: +1 (add a sample), -1 (drop one) or 0.
    int step(double latency_ms, size_t batch_samples);

    // Copy `in` (n samples) to `out`, stretched or shrunk by d (-1, 0, +1)
    // samples with a windowed-sinc interpolator. The first and last samples
    // are kept, so consecutive batches join without a step. `out` must hold
    // n + 1 samples. Returns the output length.
    static size_t stretch(const float* in, size_t n, float* out, int d);

    double ppm() const { return ppm_; }
    double integral_ppm() const { return i_ppm_; }
    double target_ms() const { return target_ms_; }
    double smoothed_ms() const { return ema_ms_; }
    size_t inserted() const { return inserted_; }
    size_t dropped() const { return dropped_; }

    // The first WARMUP_BATCHES measurements are averaged to learn the
    // latency to hold; it is never set below MIN_TARGET_MS, since the server
    // pulls from the stream a whole graph cycle at a time.
    static const int WARMUP_BATCHES = 80;
    static constexpr double MIN_TARGET_MS = 80.0;
    // ~20 s smoothing at 8 batches/s: single readings jitter by tens of ms.
    static constexpr double EMA_ALPHA = 1.0 / 160;
    // 1 ppm of mismatch moves the latency by 0.001 ms/s, so Kp = 10 ppm/ms
    // gives a ~100 s loop, well below the smoothing; the integral zero sits a
    // factor of 4 lower, keeping the phase margin.
    static constexpr double KP_PPM_PER_MS = 10.0;
    static constexpr double KI_PPM_PER_MS_S = KP_PPM_PER_MS / 400.0;
    static constexpr double MAX_PPM = 1000.0;

   private:
    double batch_s_;
    int batches_;
    double ema_ms_;
    double target_ms_;
    double acc_;
    double ppm_;
    double i_ppm_;
    size_t inserted_;
    size_t dropped_;
};

#endif /* _RATE_MATCH_H */
