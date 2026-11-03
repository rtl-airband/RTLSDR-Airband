/*
 * input-common.cpp
 * common input handling routines
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

#include "input-common.h"
#include <assert.h>
#include <dlfcn.h>  // dlopen, dlsym
#include <errno.h>
#include <pthread.h>
#include <stdio.h>   // asprintf
#include <stdlib.h>  // free
#include <string.h>
#include <iostream>
#include "rtl_airband.h"  // WAVE_BATCH, WAVE_RATE

using namespace std;

typedef input_t* (*input_new_func_t)(void);

input_t* input_new(char const* const type) {
    assert(type != NULL);
    void* dlhandle = dlopen(NULL, RTLD_NOW);
    assert(dlhandle != NULL);
    char* fname = NULL;
    int chars_written = asprintf(&fname, "%s_input_new", type);
    if (chars_written <= 0) {
        return NULL;
    }
    input_new_func_t fptr = (input_new_func_t)dlsym(dlhandle, fname);
    free(fname);
    if (fptr == NULL) {
        return NULL;
    }
    input_t* input = (*fptr)();
    assert(input->init != NULL);
    assert(input->run_rx_thread != NULL);
    assert(input->set_centerfreq != NULL);
    return input;
}

int input_init(input_t* const input) {
    assert(input != NULL);
    input_state_t new_state = INPUT_FAILED;  // fail-safe default
    errno = 0;
    int ret = input->init(input);
    if (ret < 0) {
        ret = -1;
    } else if ((ret = pthread_mutex_init(&input->buffer_lock, NULL)) != 0) {
        errno = ret;
        ret = -1;
    } else {
        new_state = INPUT_INITIALIZED;
        ret = 0;
    }
    input->state = new_state;
    return ret;
}

int input_start(input_t* const input) {
    assert(input != NULL);
    assert(input->dev_data != NULL);
    assert(input->state == INPUT_INITIALIZED);
    int err = pthread_create(&input->rx_thread, NULL, input->run_rx_thread, (void*)input);
    if (err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}

int input_parse_config(input_t* const input, libconfig::Setting& cfg) {
    assert(input != NULL);
    if (input->parse_config != NULL) {
        return input->parse_config(input, cfg);
    } else {
        // Very simple inputs (like stdin) might not necessarily have any configuration
        // variables, so it's legal not to have parse_config defined.
        return 0;
    }
}

int input_stop(input_t* const input) {
    assert(input != NULL);
    assert(input->dev_data != NULL);
    int err = 0;
    errno = 0;
    if (input->state == INPUT_RUNNING && input->stop != NULL) {
        err = input->stop(input);
        if (err != 0) {
            input->state = INPUT_FAILED;
            return -1;
        }
    }
    input->state = INPUT_STOPPED;
    err = pthread_join(input->rx_thread, NULL);
    if (err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}

std::recursive_mutex& input_control_mutex() {
    static std::recursive_mutex m;
    return m;
}

int input_set_centerfreq(input_t* const input, int const centerfreq) {
    std::lock_guard<std::recursive_mutex> control(input_control_mutex());
    assert(input != NULL);
    assert(input->dev_data != NULL);
    if (input->state != INPUT_RUNNING) {
        return -1;
    }
    int ret = input->set_centerfreq(input, centerfreq);
    if (ret != 0) {
        // A single failed retune does not mean the RX stream died - the device may still be
        // running fine at its previous centerfreq. INPUT_FAILED is reserved for the stream
        // itself dying (rx_thread's async-read failure, input_stop()'s failure path); setting it
        // here would make demodulate() treat a transient hardware error identically to "device
        // never came up" and, on the last running device, exit the whole process.
        input->centerfreq_retune_failure_count++;
        return -1;
    }
    input->centerfreq = centerfreq;
    input->tuning_generation.fetch_add(1, std::memory_order_release);
    return 0;
}

int input_set_gain(input_t* const input, float const gain) {
    std::lock_guard<std::recursive_mutex> control(input_control_mutex());
    assert(input != NULL);
    assert(input->dev_data != NULL);
    if (input->set_gain == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    if (input->state != INPUT_RUNNING) {
        return -1;
    }
    int ret = input->set_gain(input, gain);
    if (ret != 0) {
        // See input_set_centerfreq()'s comment - a failed gain change doesn't mean the RX stream
        // died, so it must not be treated as fatal here either.
        return -1;
    }
    return 0;
}

int input_set_bandwidth(input_t* const input, int const bandwidth) {
    std::lock_guard<std::recursive_mutex> control(input_control_mutex());
    assert(input != NULL);
    assert(input->dev_data != NULL);
    if (input->set_bandwidth == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    if (input->state != INPUT_RUNNING) {
        return -1;
    }
    int ret = input->set_bandwidth(input, bandwidth);
    if (ret != 0) {
        // See input_set_centerfreq()'s comment - a failed bandwidth change doesn't mean the RX
        // stream died, so it must not be treated as fatal here either.
        return -1;
    }
    return 0;
}

int input_set_correction(input_t* const input, int const correction) {
    std::lock_guard<std::recursive_mutex> control(input_control_mutex());
    assert(input != NULL);
    assert(input->dev_data != NULL);
    if (input->set_correction == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    if (input->state != INPUT_RUNNING) {
        return -1;
    }
    int ret = input->set_correction(input, correction);
    if (ret != 0) {
        // See input_set_centerfreq()'s comment - a failed correction change doesn't mean the RX
        // stream died, so it must not be treated as fatal here either.
        return -1;
    }
    input->tuning_generation.fetch_add(1, std::memory_order_release);
    return 0;
}

size_t rtlsdr_auto_usb_buf_len(int sample_rate) {
    const size_t urb_len = 16384;
    if (sample_rate <= 0) {
        return urb_len;
    }
    // U8 I/Q is 2 bytes per sample, and half a batch is WAVE_BATCH / 2 output samples.
    size_t len = (size_t)sample_rate * (WAVE_BATCH) / (WAVE_RATE);
    len -= len % 512;
    if (len < urb_len) {
        return urb_len;
    }
    if (len > RTLSDR_MAX_AUTO_USB_BUF_LEN) {
        return RTLSDR_MAX_AUTO_USB_BUF_LEN;
    }
    return len;
}

int rtlsdr_auto_usb_buf_count(size_t buf_len) {
    const size_t total = (size_t)RTLSDR_DEFAULT_LIBUSB_BUFFER_COUNT * RTLSDR_MAX_AUTO_USB_BUF_LEN;
    if (buf_len == 0) {
        return RTLSDR_DEFAULT_LIBUSB_BUFFER_COUNT;
    }
    return (int)((total + buf_len - 1) / buf_len);
}

bool rtlsdr_valid_usb_buf_len(long long buf_len) {
    return buf_len > 0 && buf_len % 512 == 0 && buf_len <= RTLSDR_MAX_USB_BUF_LEN;
}
