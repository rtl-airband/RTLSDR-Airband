/*
 * rate_match.cpp
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

#include "rate_match.h"

#include <math.h>
#include <string.h>

// out-of-class definitions, needed before C++17 when these are bound to references
const int RateMatcher::WARMUP_BATCHES;
constexpr double RateMatcher::MIN_TARGET_MS;
constexpr double RateMatcher::EMA_ALPHA;
constexpr double RateMatcher::KP_PPM_PER_MS;
constexpr double RateMatcher::KI_PPM_PER_MS_S;
constexpr double RateMatcher::MAX_PPM;

RateMatcher::RateMatcher(double batch_s) : batch_s_(batch_s) {
    reset();
}

void RateMatcher::reset() {
    batches_ = 0;
    ema_ms_ = target_ms_ = acc_ = ppm_ = i_ppm_ = 0.0;
    inserted_ = dropped_ = 0;
}

int RateMatcher::step(double latency_ms, size_t batch_samples) {
    if (latency_ms < 0)
        return 0;
    batches_++;
    if (batches_ <= WARMUP_BATCHES) {
        ema_ms_ += (latency_ms - ema_ms_) / batches_;
        target_ms_ = ema_ms_ > MIN_TARGET_MS ? ema_ms_ : MIN_TARGET_MS;
        return 0;
    }
    ema_ms_ += EMA_ALPHA * (latency_ms - ema_ms_);

    double err_ms = target_ms_ - ema_ms_;
    double i_new = i_ppm_ + KI_PPM_PER_MS_S * err_ms * batch_s_;
    double ppm = KP_PPM_PER_MS * err_ms + i_new;
    // anti-windup: only integrate while the output is not saturated
    if (ppm > MAX_PPM)
        ppm = MAX_PPM;
    else if (ppm < -MAX_PPM)
        ppm = -MAX_PPM;
    else
        i_ppm_ = i_new;
    ppm_ = ppm;

    acc_ += ppm * 1e-6 * (double)batch_samples;
    if (acc_ >= 1.0) {
        acc_ -= 1.0;
        inserted_++;
        return 1;
    }
    if (acc_ <= -1.0) {
        acc_ += 1.0;
        dropped_++;
        return -1;
    }
    return 0;
}

// 32-tap Blackman-windowed sinc. Linear interpolation was only -34 dB clean at
// 1 kHz (16 kHz audio) and cubic -28 dB at 3 kHz; this is better than -75 dB
// across the voice band. Taps past either end of the batch reuse the edge
// sample; near the ends the fractional offset is ~0, so they barely count.
static const long HALF_TAPS = 16;

size_t RateMatcher::stretch(const float* in, size_t n, float* out, int d) {
    if (d == 0 || n < 2) {
        memcpy(out, in, n * sizeof(float));
        return n;
    }
    size_t m = n + d;
    double step = (double)(n - 1) / (double)(m - 1);
    for (size_t i = 0; i < m; i++) {
        double pos = i * step;
        long k = (long)pos;
        double t = pos - (double)k;
        if (t < 1e-9) {
            out[i] = in[k < (long)n ? k : (long)n - 1];
            continue;
        }
        double acc = 0.0, wsum = 0.0;
        for (long j = -HALF_TAPS + 1; j <= HALF_TAPS; j++) {
            double x = (double)j - t;
            double w = 0.42 + 0.5 * cos(M_PI * x / HALF_TAPS) + 0.08 * cos(2.0 * M_PI * x / HALF_TAPS);
            double h = sin(M_PI * x) / (M_PI * x) * w;
            long idx = k + j;
            if (idx < 0)
                idx = 0;
            if (idx >= (long)n)
                idx = (long)n - 1;
            acc += in[idx] * h;
            wsum += h;
        }
        out[i] = (float)(acc / wsum);
    }
    return m;
}
