/*
 * input-common.h
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

#ifndef _INPUT_COMMON_H
#define _INPUT_COMMON_H 1
#include <pthread.h>
#include <libconfig.h++>

#if __GNUC__ >= 4
#define MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#else
#define MODULE_EXPORT extern "C"
#endif /* __GNUC__ */

typedef enum { SFMT_UNDEF = 0, SFMT_U8, SFMT_S8, SFMT_S16, SFMT_F32 } sample_format_t;
#define SAMPLE_FORMAT_CNT 5

typedef enum { INPUT_UNKNOWN = 0, INPUT_INITIALIZED, INPUT_RUNNING, INPUT_FAILED, INPUT_STOPPED, INPUT_DISABLED } input_state_t;
#define INPUT_STATE_CNT 6

typedef struct input_t input_t;

struct input_t {
    unsigned char* buffer;
    void* dev_data;
    size_t buf_size, bufs, bufe;
    size_t overflow_count;
    input_state_t state;
    sample_format_t sfmt;
    float fullscale;
    int bytes_per_sample;
    int sample_rate;
    int centerfreq;
    int (*parse_config)(input_t* const input, libconfig::Setting& cfg);
    int (*init)(input_t* const input);
    void* (*run_rx_thread)(void* input_ptr);  // to be launched via pthread_create()
    int (*set_centerfreq)(input_t* const input, int const centerfreq);
    int (*stop)(input_t* const input);
    pthread_t rx_thread;
    pthread_mutex_t buffer_lock;
};

input_t* input_new(char const* const type);
int input_init(input_t* const input);
int input_parse_config(input_t* const input, libconfig::Setting& cfg);
int input_start(input_t* const input);
int input_set_centerfreq(input_t* const input, int const centerfreq);
int input_stop(input_t* const input);

#endif /* _INPUT_COMMON_H */
