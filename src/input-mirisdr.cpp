/*
 *  input-mirisdr.cpp
 *  MiriSDR-specific routines
 *
 *  Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
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

#include "input-mirisdr.h"  // mirisdr_dev_data_t
#include <assert.h>
#include <limits.h>  // SCHAR_MAX
#include <mirisdr.h>
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

static void mirisdr_callback(unsigned char* buf, uint32_t len, void* ctx) {
    if (do_exit)
        return;
    input_t* input = (input_t*)ctx;
    circbuffer_append(input, buf, (size_t)len);
}

/* based on librtlsdr-keenerd, (c) Kyle Keen */
static bool mirisdr_nearest_gain(mirisdr_dev_t* dev, int target_gain, int* nearest) {
    assert(nearest != NULL);
    int i, r, err1, err2, count;
    int* gains;
    r = mirisdr_set_tuner_gain_mode(dev, 1);
    if (r < 0) {
        return false;
    }
    count = mirisdr_get_tuner_gains(dev, NULL);
    if (count <= 0) {
        return false;
    }
    gains = (int*)XCALLOC(count, sizeof(int));
    count = mirisdr_get_tuner_gains(dev, gains);
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

static int mirisdr_find_device_by_serial(char const* const s) {
    char vendor[256] = {0}, product[256] = {0}, serial[256] = {0};
    int count = mirisdr_get_device_count();
    if (count < 1) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        mirisdr_get_device_usb_strings(i, vendor, product, serial);
        if (strcmp(s, serial) != 0) {
            continue;
        }
        return i;
    }
    return -1;
}

int mirisdr_init(input_t* const input) {
    mirisdr_dev_data_t* dev_data = (mirisdr_dev_data_t*)input->dev_data;
    if (dev_data->serial != NULL) {
        dev_data->index = mirisdr_find_device_by_serial(dev_data->serial);
        if (dev_data->index < 0) {
            cerr << "MiriSDR device with serial number " << dev_data->serial << " not found\n";
            error();
        }
    }

    dev_data->dev = NULL;
    mirisdr_open(&dev_data->dev, dev_data->index);
    if (NULL == dev_data->dev) {
        log(LOG_ERR, "Failed to open mirisdr device #%d.\n", dev_data->index);
        error();
    }

    char transfer_str[] = "BULK";
    char sample_format_str[] = "504_S8";

    mirisdr_dev_t* miri = dev_data->dev;
    int r = mirisdr_set_transfer(miri, transfer_str);
    if (r < 0) {
        log(LOG_ERR, "Failed to set bulk transfer mode for MiriSDR device #%d: error %d\n", dev_data->index, r);
        error();
    }
    r = mirisdr_set_sample_rate(miri, input->sample_rate);
    if (r < 0) {
        log(LOG_ERR, "Failed to set sample rate for device #%d. Error %d.\n", dev_data->index, r);
    }

    r = mirisdr_set_center_freq(miri, input->centerfreq - dev_data->correction);
    if (r < 0) {
        log(LOG_ERR, "Failed to set center freq for device #%d. Error %d.\n", dev_data->index, r);
    }

    int ngain = 0;
    if (mirisdr_nearest_gain(miri, dev_data->gain, &ngain) != true) {
        log(LOG_ERR, "Failed to read supported gain list for device #%d\n", dev_data->index);
        error();
    }
    r = mirisdr_set_tuner_gain_mode(miri, 1);
    r |= mirisdr_set_tuner_gain(miri, ngain);
    if (r < 0) {
        log(LOG_ERR, "Failed to set gain to %d for device #%d: error %d\n", ngain, dev_data->index, r);
    } else {
        log(LOG_INFO, "Device #%d: gain set to %d dB\n", dev_data->index, mirisdr_get_tuner_gain(miri));
    }
    r = mirisdr_set_sample_format(miri, sample_format_str);
    if (r < 0) {
        log(LOG_ERR, "Failed to set sample format for device #%d: error %d\n", dev_data->index, r);
        error();
    }
    mirisdr_reset_buffer(miri);
    log(LOG_INFO, "MiriSDR device %d initialized\n", dev_data->index);
    return 0;
}

void* mirisdr_rx_thread(void* ctx) {
    input_t* input = (input_t*)ctx;
    mirisdr_dev_data_t* dev_data = (mirisdr_dev_data_t*)input->dev_data;
    assert(dev_data->dev != NULL);

    input->state = INPUT_RUNNING;
    if (mirisdr_read_async(dev_data->dev, mirisdr_callback, ctx, dev_data->bufcnt, MIRISDR_BUFSIZE) < 0) {
        log(LOG_ERR, "MiriSDR device #%d: async read failed, disabling\n", dev_data->index);
        input->state = INPUT_FAILED;
    }
    return 0;
}

int mirisdr_stop(input_t* const input) {
    mirisdr_dev_data_t* dev_data = (mirisdr_dev_data_t*)input->dev_data;
    assert(dev_data->dev != NULL);

    if (mirisdr_cancel_async(dev_data->dev) < 0) {
        return -1;
    }
    return 0;
}

int mirisdr_set_centerfreq(input_t* const input, int const centerfreq) {
    mirisdr_dev_data_t* dev_data = (mirisdr_dev_data_t*)input->dev_data;
    assert(dev_data->dev != NULL);

    int r = mirisdr_set_center_freq(dev_data->dev, centerfreq - dev_data->correction);
    if (r < 0) {
        log(LOG_ERR, "Failed to set centerfreq for MiriSDR device #%d: error %d\n", dev_data->index, r);
        return -1;
    }
    return 0;
}

int mirisdr_parse_config(input_t* const input, libconfig::Setting& cfg) {
    mirisdr_dev_data_t* dev_data = (mirisdr_dev_data_t*)input->dev_data;
    if (cfg.exists("serial")) {
        dev_data->serial = strdup(cfg["serial"]);
    } else if (cfg.exists("index")) {
        dev_data->index = (int)cfg["index"];
    } else {
        cerr << "MiriSDR configuration error: no index and no serial number given\n";
        error();
    }
    if (cfg.exists("gain")) {
        dev_data->gain = (int)cfg["gain"];
    } else {
        cerr << "MiriSDR configuration error: gain is not configured\n";
        error();
    }
    if (cfg.exists("correction")) {
        dev_data->correction = (int)cfg["correction"];
    }
    if (cfg.exists("num_buffers")) {
        dev_data->bufcnt = (int)(cfg["num_buffers"]);
        if (dev_data->bufcnt < 1) {
            cerr << "MiriSDR configuration error: num_buffers must be greater than 0\n";
            error();
        }
    }
    return 0;
}

MODULE_EXPORT input_t* mirisdr_input_new() {
    mirisdr_dev_data_t* dev_data = (mirisdr_dev_data_t*)XCALLOC(1, sizeof(mirisdr_dev_data_t));
    dev_data->index = -1;  // invalid default receiver index
    dev_data->gain = -1;   // invalid default gain value
    dev_data->bufcnt = MIRISDR_DEFAULT_LIBUSB_BUFFER_COUNT;
    /*	return &( input_t ){
                    .dev_data = dev_data,
                    .state = INPUT_UNKNOWN,
                    .sfmt = SFMT_U8,
                    .sample_rate = MIRISDR_DEFAULT_SAMPLE_RATE,
                    .parse_config = &mirisdr_parse_config,
                    .init = &mirisdr_init,
                    .run_rx_thread = &mirisdr_rx_thread,
                    .set_centerfreq = &mirisdr_set_centerfreq,
                    .stop = &mirisdr_stop
            }; */
    input_t* input = (input_t*)XCALLOC(1, sizeof(input_t));
    input->dev_data = dev_data;
    input->state = INPUT_UNKNOWN;
    input->sfmt = SFMT_S8;
    input->fullscale = (float)SCHAR_MAX - 0.5f;
    input->bytes_per_sample = sizeof(char);
    input->sample_rate = MIRISDR_DEFAULT_SAMPLE_RATE;
    input->parse_config = &mirisdr_parse_config;
    input->init = &mirisdr_init;
    input->run_rx_thread = &mirisdr_rx_thread;
    input->set_centerfreq = &mirisdr_set_centerfreq;
    input->stop = &mirisdr_stop;
    return input;
}
