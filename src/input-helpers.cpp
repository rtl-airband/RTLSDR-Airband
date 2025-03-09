/*
 * input-helpers.cpp
 * Convenience functions to be called by input drivers
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

#include <pthread.h>       // pthread_mutex_lock, unlock
#include <string.h>        // memcpy
#include <iostream>        // cerr
#include "input-common.h"  // input_t
#include "rtl_airband.h"   // debug_print

/* Write input data into circular buffer input->buffer.
 * In general, input->buffer_size is not an exact multiple of len,
 * so we have to take care about proper wrapping.
 * input->buffer_size is an exact multiple of FFT_BATCH * bps
 * (input bytes per output audio sample) and input->buffer's real length
 * is input->buf_size + 2 * bytes_per_input-sample * fft_size. On each
 * wrap we copy 2 * fft_size bytes from the start of input->buffer to its end,
 * so that the signal windowing function could handle the whole FFT batch
 * without wrapping.
 */
void circbuffer_append(input_t* const input, unsigned char* buf, size_t len) {
    if (len == 0)
        return;
    pthread_mutex_lock(&input->buffer_lock);
    size_t space_left = input->buf_size - input->bufe;
    if (space_left >= len) {
        memcpy(input->buffer + input->bufe, buf, len);
        if (input->bufe == 0) {
            memcpy(input->buffer + input->buf_size, input->buffer, std::min(len, 2 * input->bytes_per_sample * fft_size));
            debug_print("tail_len=%zu bytes\n", std::min(len, 2 * input->bytes_per_sample * fft_size));
        }
    } else {
        memcpy(input->buffer + input->bufe, buf, space_left);
        memcpy(input->buffer, buf + space_left, len - space_left);
        memcpy(input->buffer + input->buf_size, input->buffer, std::min(len - space_left, 2 * input->bytes_per_sample * fft_size));
        debug_print("buf wrap: space_left=%zu len=%zu bufe=%zu wrap_len=%zu tail_len=%zu\n", space_left, len, input->bufe, len - space_left,
                    std::min(len - space_left, 2 * input->bytes_per_sample * fft_size));
    }

    size_t old_end = input->bufe;
    input->bufe = (input->bufe + len) % input->buf_size;
    if (old_end < input->bufs && input->bufe >= input->bufs) {
        std::cerr << "Warning: buffer overflow\n";
        input->overflow_count++;
    }
    pthread_mutex_unlock(&input->buffer_lock);
}
