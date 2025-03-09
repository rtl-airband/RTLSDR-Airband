/*
 * input-rtlsdr.cpp
 * RTLSDR-specific routines
 *
 * Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
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

#include "input-rtlsdr.h"  // rtlsdr_dev_data_t
#include <assert.h>
#include <limits.h>  // SCHAR_MAX
#include <rtl-sdr.h>
#include <stdint.h>  // uint32_t
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>  // FIXME: get rid of this
#include <iostream>
#include <libconfig.h++>    // Setting
#include "input-common.h"   // input_t, sample_format_t, input_state_t, MODULE_EXPORT
#include "input-helpers.h"  // circbuffer_append
#include "rtl_airband.h"    // do_exit, fft_size, debug_print, XCALLOC, error()

using namespace std;

static void rtlsdr_callback(unsigned char* buf, uint32_t len, void* ctx) {
    if (do_exit)
        return;
    input_t* input = (input_t*)ctx;
    circbuffer_append(input, buf, (size_t)len);
}

/* based on librtlsdr-keenerd, (c) Kyle Keen */
static bool rtlsdr_nearest_gain(rtlsdr_dev_t* dev, int target_gain, int* nearest) {
    assert(nearest != NULL);
    int i, r, err1, err2, count;
    int* gains;
    r = rtlsdr_set_tuner_gain_mode(dev, 1);
    if (r < 0) {
        return false;
    }
    count = rtlsdr_get_tuner_gains(dev, NULL);
    if (count <= 0) {
        return false;
    }
    gains = (int*)XCALLOC(count, sizeof(int));
    count = rtlsdr_get_tuner_gains(dev, gains);
    *nearest = gains[0];
    for (i = 0; i < count; i++) {
        err1 = abs(target_gain - *nearest);
        err2 = abs(target_gain - gains[i]);
        if (err2 < err1) {
            *nearest = gains[i];
        }
    }
    free(gains);
    return true;
}

static int rtlsdr_find_device_by_serial(char const* const s) {
    char vendor[256] = {0}, product[256] = {0}, serial[256] = {0};
    int count = rtlsdr_get_device_count();
    if (count < 1) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        rtlsdr_get_device_usb_strings(i, vendor, product, serial);
        if (strcmp(s, serial) != 0) {
            continue;
        }
        return i;
    }
    return -1;
}

int rtlsdr_init(input_t* const input) {
    rtlsdr_dev_data_t* dev_data = (rtlsdr_dev_data_t*)input->dev_data;
    if (dev_data->serial != NULL) {
        dev_data->index = rtlsdr_find_device_by_serial(dev_data->serial);
        if (dev_data->index < 0) {
            cerr << "RTLSDR device with serial number " << dev_data->serial << " not found\n";
            error();
        }
    }

    dev_data->dev = NULL;
    rtlsdr_open(&dev_data->dev, dev_data->index);
    if (NULL == dev_data->dev) {
        log(LOG_ERR, "Failed to open rtlsdr device #%d.\n", dev_data->index);
        error();
    }

    rtlsdr_dev_t* rtl = dev_data->dev;
    int r = rtlsdr_set_sample_rate(rtl, input->sample_rate);
    if (r < 0) {
        log(LOG_ERR, "Failed to set sample rate for device #%d. Error %d.\n", dev_data->index, r);
    }

    r = rtlsdr_set_center_freq(rtl, input->centerfreq);
    if (r < 0) {
        log(LOG_ERR, "Failed to set center freq for device #%d. Error %d.\n", dev_data->index, r);
    }

    r = rtlsdr_set_freq_correction(rtl, dev_data->correction);
    if (r < 0 && r != -2) {
        log(LOG_ERR, "Failed to set freq correction for device #%d. Error %d.\n", dev_data->index, r);
    }

    // Fitipower FC0012 gain needs to be initialized to its lowest value before setting it to the desired value
    if (rtlsdr_get_tuner_type(rtl) == RTLSDR_TUNER_FC0012) {
        int initialGain = 0;
        if (rtlsdr_nearest_gain(rtl, -99, &initialGain) != true) {
            log(LOG_ERR, "Failed to read supported gain list for device #%d\n", dev_data->index);
            error();
        }

        r |= rtlsdr_set_tuner_gain(rtl, initialGain);
        if (r < 0) {
            log(LOG_ERR, "Failed to initialize gain for device #%d: error %d\n", (float)initialGain / 10.f, dev_data->index, r);
        }
    }

    int ngain = 0;
    if (rtlsdr_nearest_gain(rtl, dev_data->gain, &ngain) != true) {
        log(LOG_ERR, "Failed to read supported gain list for device #%d\n", dev_data->index);
        error();
    }
    r = rtlsdr_set_tuner_gain_mode(rtl, 1);
    r |= rtlsdr_set_tuner_gain(rtl, ngain);
    if (r < 0) {
        log(LOG_ERR, "Failed to set gain to %0.2f for device #%d: error %d\n", (float)ngain / 10.f, dev_data->index, r);
    } else {
        log(LOG_INFO, "Device #%d: gain set to %0.2f dB\n", dev_data->index, (float)rtlsdr_get_tuner_gain(rtl) / 10.f);
    }

    r = rtlsdr_set_agc_mode(rtl, 0);
    if (r < 0) {
        log(LOG_ERR, "Failed to disable AGC for device #%d. Error %d.\n", dev_data->index, r);
    }
    rtlsdr_reset_buffer(rtl);
    log(LOG_INFO, "RTLSDR device %d initialized\n", dev_data->index);
    return 0;
}

void* rtlsdr_rx_thread(void* ctx) {
    input_t* input = (input_t*)ctx;
    rtlsdr_dev_data_t* dev_data = (rtlsdr_dev_data_t*)input->dev_data;
    assert(dev_data->dev != NULL);

    input->state = INPUT_RUNNING;
    if (rtlsdr_read_async(dev_data->dev, rtlsdr_callback, ctx, dev_data->bufcnt, RTLSDR_BUFSIZE) < 0) {
        log(LOG_ERR, "RTLSDR device #%d: async read failed, disabling\n", dev_data->index);
        input->state = INPUT_FAILED;
    }
    return 0;
}

int rtlsdr_stop(input_t* const input) {
    rtlsdr_dev_data_t* dev_data = (rtlsdr_dev_data_t*)input->dev_data;
    assert(dev_data->dev != NULL);

    if (rtlsdr_cancel_async(dev_data->dev) < 0) {
        return -1;
    }
    return rtlsdr_close(dev_data->dev);
}

int rtlsdr_set_centerfreq(input_t* const input, int const centerfreq) {
    rtlsdr_dev_data_t* dev_data = (rtlsdr_dev_data_t*)input->dev_data;
    assert(dev_data->dev != NULL);

    int r = rtlsdr_set_center_freq(dev_data->dev, centerfreq);
    if (r < 0) {
        log(LOG_ERR, "Failed to set centerfreq for RTLSDR device #%d: error %d\n", dev_data->index, r);
        return -1;
    }
    return 0;
}

int rtlsdr_parse_config(input_t* const input, libconfig::Setting& cfg) {
    rtlsdr_dev_data_t* dev_data = (rtlsdr_dev_data_t*)input->dev_data;
    if (cfg.exists("serial")) {
        dev_data->serial = strdup(cfg["serial"]);
    } else if (cfg.exists("index")) {
        dev_data->index = (int)cfg["index"];
    } else {
        cerr << "RTLSDR configuration error: no index and no serial number given\n";
        error();
    }
    if (cfg.exists("gain")) {
        if (cfg["gain"].getType() == libconfig::Setting::TypeInt) {  // backward compatibility
            dev_data->gain = (int)cfg["gain"] * 10;
        } else if (cfg["gain"].getType() == libconfig::Setting::TypeFloat) {
            dev_data->gain = (int)((float)cfg["gain"] * 10.0f);
        }
    } else {
        cerr << "RTLSDR configuration error: gain is not configured\n";
        error();
    }
    if (cfg.exists("correction")) {
        dev_data->correction = (int)cfg["correction"];
    }
    if (cfg.exists("buffers")) {
        dev_data->bufcnt = (int)(cfg["buffers"]);
        if (dev_data->bufcnt < 1) {
            cerr << "RTLSDR configuration error: buffers must be greater than 0\n";
            error();
        }
    }
    return 0;
}

MODULE_EXPORT input_t* rtlsdr_input_new() {
    rtlsdr_dev_data_t* dev_data = (rtlsdr_dev_data_t*)XCALLOC(1, sizeof(rtlsdr_dev_data_t));
    dev_data->index = -1;  // invalid default receiver index
    dev_data->gain = -1;   // invalid default gain value
    dev_data->bufcnt = RTLSDR_DEFAULT_LIBUSB_BUFFER_COUNT;
    /*	return &( input_t ){
                    .dev_data = dev_data,
                    .state = INPUT_UNKNOWN,
                    .sfmt = SFMT_U8,
                    .sample_rate = RTLSDR_DEFAULT_SAMPLE_RATE,
                    .parse_config = &rtlsdr_parse_config,
                    .init = &rtlsdr_init,
                    .run_rx_thread = &rtlsdr_rx_thread,
                    .set_centerfreq = &rtlsdr_set_centerfreq,
                    .stop = &rtlsdr_stop
            }; */
    input_t* input = (input_t*)XCALLOC(1, sizeof(input_t));
    input->dev_data = dev_data;
    input->state = INPUT_UNKNOWN;
    input->sfmt = SFMT_U8;
    input->fullscale = (float)SCHAR_MAX - 0.5f;
    input->bytes_per_sample = sizeof(unsigned char);
    input->sample_rate = RTLSDR_DEFAULT_SAMPLE_RATE;
    input->parse_config = &rtlsdr_parse_config;
    input->init = &rtlsdr_init;
    input->run_rx_thread = &rtlsdr_rx_thread;
    input->set_centerfreq = &rtlsdr_set_centerfreq;
    input->stop = &rtlsdr_stop;
    return input;
}
