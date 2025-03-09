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

int input_set_centerfreq(input_t* const input, int const centerfreq) {
    assert(input != NULL);
    assert(input->dev_data != NULL);
    if (input->state != INPUT_RUNNING) {
        return -1;
    }
    int ret = input->set_centerfreq(input, centerfreq);
    if (ret != 0) {
        input->state = INPUT_FAILED;
        return -1;
    }
    input->centerfreq = centerfreq;
    return 0;
}
