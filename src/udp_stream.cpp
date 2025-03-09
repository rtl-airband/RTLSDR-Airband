/*
 * udp_stream.cpp
 *
 * Copyright (C) 2024 charlie-foxtrot
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
#include <string.h>  // strerror()
#include <syslog.h>  // LOG_INFO / LOG_ERR
#include <unistd.h>  // close()
#include <cassert>   // assert()

#include <arpa/inet.h>  // inet_aton()
#include <netdb.h>      // getaddrinfo()

#include "rtl_airband.h"

bool udp_stream_init(udp_stream_data* sdata, mix_modes mode, size_t len) {
    // pre-allocate the stereo buffer
    if (mode == MM_STEREO) {
        sdata->stereo_buffer_len = len * 2;
        sdata->stereo_buffer = (float*)XCALLOC(sdata->stereo_buffer_len, sizeof(float));
    } else {
        sdata->stereo_buffer_len = 0;
        sdata->stereo_buffer = NULL;
    }

    sdata->send_socket = -1;
    sdata->dest_sockaddr_len = 0;

    // lookup address / port
    struct addrinfo hints, *result, *rptr;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = 0;
    hints.ai_protocol = 0;
    int error = getaddrinfo(sdata->dest_address, sdata->dest_port, &hints, &result);
    if (error) {
        log(LOG_ERR, "udp_stream: could not resolve %s:%s - %s\n", sdata->dest_address, sdata->dest_port, gai_strerror(error));
        return false;
    }

    // check each result and try to create a connection
    for (rptr = result; rptr != NULL; rptr = rptr->ai_next) {
        sdata->send_socket = socket(rptr->ai_family, rptr->ai_socktype, rptr->ai_protocol);
        if (sdata->send_socket == -1) {
            log(LOG_ERR, "udp_stream: socket failed: %s\n", strerror(errno));
            continue;
        }

        if (connect(sdata->send_socket, rptr->ai_addr, rptr->ai_addrlen) == -1) {
            log(LOG_INFO, "udp_stream: connect to %s:%s failed: %s\n", sdata->dest_address, sdata->dest_port, strerror(errno));
            close(sdata->send_socket);
            sdata->send_socket = -1;
            continue;
        }

        sdata->dest_sockaddr = *rptr->ai_addr;
        sdata->dest_sockaddr_len = rptr->ai_addrlen;
        break;
    }
    freeaddrinfo(result);

    // error if no valid socket
    if (sdata->send_socket == -1) {
        log(LOG_ERR, "udp_stream: could not set up UDP socket to %s:%s - all addresses failed\n", sdata->dest_address, sdata->dest_port);
        return false;
    }

    log(LOG_INFO, "udp_stream: sending %s 32-bit float at %d Hz to %s:%s\n", mode == MM_MONO ? "Mono" : "Stereo", WAVE_RATE, sdata->dest_address, sdata->dest_port);
    return true;
}

void udp_stream_write(udp_stream_data* sdata, const float* data, size_t len) {
    if (sdata->send_socket != -1) {
        // Send without blocking or checking for success
        sendto(sdata->send_socket, data, len, MSG_DONTWAIT | MSG_NOSIGNAL, &sdata->dest_sockaddr, sdata->dest_sockaddr_len);
    }
}

void udp_stream_write(udp_stream_data* sdata, const float* data_left, const float* data_right, size_t len) {
    if (sdata->send_socket != -1) {
        assert(len * 2 <= sdata->stereo_buffer_len);
        for (size_t i = 0; i < len; ++i) {
            sdata->stereo_buffer[2 * i] = data_left[i];
            sdata->stereo_buffer[2 * i + 1] = data_right[i];
        }
        udp_stream_write(sdata, sdata->stereo_buffer, len * 2);
    }
}

void udp_stream_shutdown(udp_stream_data* sdata) {
    if (sdata->send_socket != -1) {
        close(sdata->send_socket);
    }
}
