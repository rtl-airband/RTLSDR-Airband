/*
 * filters.cpp
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

#include "logging.h"  // debug_print()

#include "filters.h"

using namespace std;

// Default constructor is no filter
NotchFilter::NotchFilter(void) : enabled_(false) {}

// Notch Filter based on https://www.dsprelated.com/showcode/173.php
NotchFilter::NotchFilter(float notch_freq, float sample_freq, float q) : enabled_(true), x{0.0}, y{0.0} {
    if (notch_freq <= 0.0) {
        debug_print("Invalid frequency %f Hz, disabling notch filter\n", notch_freq);
        enabled_ = false;
        return;
    }

    debug_print("Adding notch filter for %f Hz with parameters {%f, %f}\n", notch_freq, sample_freq, q);

    float wo = 2 * M_PI * (notch_freq / sample_freq);

    e = 1 / (1 + tan(wo / (q * 2)));
    p = cos(wo);
    d[0] = e;
    d[1] = 2 * e * p;
    d[2] = (2 * e - 1);

    debug_print("wo:%f e:%f p:%f d:{%f,%f,%f}\n", wo, e, p, d[0], d[1], d[2]);
}

void NotchFilter::apply(float& value) {
    if (!enabled_) {
        return;
    }

    x[0] = x[1];
    x[1] = x[2];
    x[2] = value;

    y[0] = y[1];
    y[1] = y[2];
    y[2] = d[0] * x[2] - d[1] * x[1] + d[0] * x[0] + d[1] * y[1] - d[2] * y[0];

    value = y[2];
}

// Default constructor is no filter
LowpassFilter::LowpassFilter(void) : enabled_(false) {}

// 2nd order lowpass Bessel filter, based entirely on a simplification of https://www-users.cs.york.ac.uk/~fisher/mkfilter/
LowpassFilter::LowpassFilter(float freq, float sample_freq) : enabled_(true) {
    if (freq <= 0.0) {
        debug_print("Invalid frequency %f Hz, disabling lowpass filter\n", freq);
        enabled_ = false;
        return;
    }

    debug_print("Adding lowpass filter at %f Hz with a sample rate of %f\n", freq, sample_freq);

    double raw_alpha = (double)freq / sample_freq;
    double warped_alpha = tan(M_PI * raw_alpha) / M_PI;

    complex<double> zeros[2] = {-1.0, -1.0};
    complex<double> poles[2];
    poles[0] = blt(M_PI * 2 * warped_alpha * complex<double>(-1.10160133059e+00, 6.36009824757e-01));
    poles[1] = blt(M_PI * 2 * warped_alpha * conj(complex<double>(-1.10160133059e+00, 6.36009824757e-01)));

    complex<double> topcoeffs[3];
    complex<double> botcoeffs[3];
    expand(zeros, 2, topcoeffs);
    expand(poles, 2, botcoeffs);
    complex<double> gain_complex = evaluate(topcoeffs, 2, botcoeffs, 2, 1.0);
    gain = hypot(gain_complex.imag(), gain_complex.real());

    for (int i = 0; i <= 2; i++) {
        ycoeffs[i] = -(botcoeffs[i].real() / botcoeffs[2].real());
    }

    debug_print("gain: %f, ycoeffs: {%f, %f}\n", gain, ycoeffs[0], ycoeffs[1]);
}

complex<double> LowpassFilter::blt(complex<double> pz) {
    return (2.0 + pz) / (2.0 - pz);
}

/* evaluate response, substituting for z */
complex<double> LowpassFilter::evaluate(complex<double> topco[], int nz, complex<double> botco[], int np, complex<double> z) {
    return eval(topco, nz, z) / eval(botco, np, z);
}

/* evaluate polynomial in z, substituting for z */
complex<double> LowpassFilter::eval(complex<double> coeffs[], int npz, complex<double> z) {
    complex<double> sum(0.0);
    for (int i = npz; i >= 0; i--) {
        sum = (sum * z) + coeffs[i];
    }
    return sum;
}

/* compute product of poles or zeros as a polynomial of z */
void LowpassFilter::expand(complex<double> pz[], int npz, complex<double> coeffs[]) {
    coeffs[0] = 1.0;
    for (int i = 0; i < npz; i++) {
        coeffs[i + 1] = 0.0;
    }
    for (int i = 0; i < npz; i++) {
        multin(pz[i], npz, coeffs);
    }
    /* check computed coeffs of z^k are all real */
    for (int i = 0; i < npz + 1; i++) {
        if (fabs(coeffs[i].imag()) > 1e-10) {
            log(LOG_ERR, "coeff of z^%d is not real; poles/zeros are not complex conjugates\n", i);
            error();
        }
    }
}

void LowpassFilter::multin(complex<double> w, int npz, complex<double> coeffs[]) {
    /* multiply factor (z-w) into coeffs */
    complex<double> nw = -w;
    for (int i = npz; i >= 1; i--) {
        coeffs[i] = (nw * coeffs[i]) + coeffs[i - 1];
    }
    coeffs[0] = nw * coeffs[0];
}

void LowpassFilter::apply(float& r, float& j) {
    if (!enabled_) {
        return;
    }

    complex<float> input(r, j);

    xv[0] = xv[1];
    xv[1] = xv[2];
    xv[2] = input / gain;

    yv[0] = yv[1];
    yv[1] = yv[2];
    yv[2] = (xv[0] + xv[2]) + (2.0f * xv[1]) + (ycoeffs[0] * yv[0]) + (ycoeffs[1] * yv[1]);

    r = yv[2].real();
    j = yv[2].imag();
}
