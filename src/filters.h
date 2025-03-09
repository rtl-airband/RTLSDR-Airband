/*
 * filters.h
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

#ifndef _FILTERS_H
#define _FILTERS_H 1

#include <complex>

class NotchFilter {
   public:
    NotchFilter(void);
    NotchFilter(float notch_freq, float sample_freq, float q);
    void apply(float& value);
    bool enabled(void) { return enabled_; }

   private:
    bool enabled_;
    float e;
    float p;
    float d[3];
    float x[3];
    float y[3];
};

class LowpassFilter {
   public:
    LowpassFilter(void);
    LowpassFilter(float freq, float sample_freq);
    void apply(float& r, float& j);
    bool enabled(void) const { return enabled_; }

   private:
    static std::complex<double> blt(std::complex<double> pz);
    static void expand(std::complex<double> pz[], int npz, std::complex<double> coeffs[]);
    static void multin(std::complex<double> w, int npz, std::complex<double> coeffs[]);
    static std::complex<double> evaluate(std::complex<double> topco[], int nz, std::complex<double> botco[], int np, std::complex<double> z);
    static std::complex<double> eval(std::complex<double> coeffs[], int npz, std::complex<double> z);

    bool enabled_;
    float ycoeffs[3];
    float gain;

    std::complex<float> xv[3];
    std::complex<float> yv[3];
};

#endif /* _FILTERS_H */
