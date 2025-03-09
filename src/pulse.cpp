/*
 * pulse.cpp
 * PulseAudio output routines
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

#include <pulse/pulseaudio.h>
#include <syslog.h>
#include <iostream>
#include "rtl_airband.h"

#define SERVER_IFNOTNULL(x) ((x) ? (x) : "<default_server>")
#define PA_LOOP_LOCK(x)                       \
    if (!pa_threaded_mainloop_in_thread(x)) { \
        pa_threaded_mainloop_lock(x);         \
    }
#define PA_LOOP_UNLOCK(x)                     \
    if (!pa_threaded_mainloop_in_thread(x)) { \
        pa_threaded_mainloop_unlock(x);       \
    }

using namespace std;

pa_threaded_mainloop* mainloop = NULL;

void pulse_shutdown(pulse_data* pdata) {
    if (!pdata)
        return;
    PA_LOOP_LOCK(mainloop);
    if (pdata->left) {
        pa_stream_disconnect(pdata->left);
        pa_stream_unref(pdata->left);
        pdata->left = NULL;
    }
    if (pdata->right) {
        pa_stream_disconnect(pdata->right);
        pa_stream_unref(pdata->right);
        pdata->right = NULL;
    }
    if (pdata->context) {
        pa_context_disconnect(pdata->context);
        pa_context_unref(pdata->context);
        pdata->context = NULL;
    }
    PA_LOOP_UNLOCK(mainloop);
}

static void pulse_stream_underflow_cb(pa_stream*, void* userdata) {
    pulse_data* pdata = (pulse_data*)userdata;
    if (pdata->continuous)  // do not flood the logs on every squelch closing
        log(LOG_INFO, "pulse: %s: stream \"%s\": underflow\n", SERVER_IFNOTNULL(pdata->server), pdata->stream_name);
}

static void pulse_stream_overflow_cb(pa_stream*, void* userdata) {
    pulse_data* pdata = (pulse_data*)userdata;
    log(LOG_INFO, "pulse: %s: stream \"%s\": overflow\n", SERVER_IFNOTNULL(pdata->server), pdata->stream_name);
}

static void stream_state_cb(pa_stream* stream, void* userdata) {
    pulse_data* pdata = (pulse_data*)userdata;

    switch (pa_stream_get_state(stream)) {
        case PA_STREAM_READY:
            if (pdata->mode == MM_MONO || (pa_stream_get_state(pdata->left) == PA_STREAM_READY && pa_stream_get_state(pdata->right) == PA_STREAM_READY))
                pa_stream_cork(pdata->left, 0, NULL, NULL);
            break;
        case PA_STREAM_UNCONNECTED:
        case PA_STREAM_CREATING:
            break;
        case PA_STREAM_FAILED:
            log(LOG_WARNING, "pulse: %s: stream \"%s\" failed: %s\n", SERVER_IFNOTNULL(pdata->server), pdata->stream_name, pa_strerror(pa_context_errno(pdata->context)));
            break;
        case PA_STREAM_TERMINATED:
            log(LOG_WARNING, "pulse: %s: stream \"%s\" terminated\n", SERVER_IFNOTNULL(pdata->server), pdata->stream_name);
            break;
            break;
    }
}

static pa_stream* pulse_setup_stream(pulse_data* pdata, const pa_sample_spec* ss, pa_channel_map* cmap, pa_stream* sync_stream) {
    pa_stream* stream = NULL;
    PA_LOOP_LOCK(mainloop);
    if (!(stream = pa_stream_new(pdata->context, pdata->stream_name, ss, cmap))) {
        log(LOG_ERR, "pulse: %s: failed to create stream \"%s\": %s\n", SERVER_IFNOTNULL(pdata->server), pdata->stream_name, pa_strerror(pa_context_errno(pdata->context)));
        goto fail;
    }
    pa_stream_set_state_callback(stream, stream_state_cb, pdata);
    pa_stream_set_underflow_callback(stream, pulse_stream_underflow_cb, pdata);
    pa_stream_set_overflow_callback(stream, pulse_stream_overflow_cb, pdata);
    // Initially streams are corked (paused). For mono streams this is irrelevant,
    // but for stereo mixers it's required to keep left and right channels in sync.
    // Starting the left channel stream before the other stream from the sync pair is
    // set up causes the left channel stream to fail.
    if (pa_stream_connect_playback(stream, pdata->sink, NULL, (pa_stream_flags_t)(PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_ADJUST_LATENCY | PA_STREAM_START_CORKED | PA_STREAM_AUTO_TIMING_UPDATE),
                                   NULL, sync_stream) < 0) {
        log(LOG_ERR, "pulse: %s: failed to connect stream \"%s\": %s\n", SERVER_IFNOTNULL(pdata->server), pdata->stream_name, pa_strerror(pa_context_errno(pdata->context)));
        goto fail;
    }
    log(LOG_INFO, "pulse: %s: stream \"%s\" connected\n", SERVER_IFNOTNULL(pdata->server), pdata->stream_name);
    PA_LOOP_UNLOCK(mainloop);
    return stream;
fail:
    PA_LOOP_UNLOCK(mainloop);
    return NULL;
}

static void pulse_setup_streams(pulse_data* pdata) {
    const pa_sample_spec ss = {
#if __cplusplus >= 199711L
        .format = PA_SAMPLE_FLOAT32LE,
        .rate = WAVE_RATE,
        .channels = 1
#else  // for g++ 4.6 (eg. Raspbian Wheezy)
        PA_SAMPLE_FLOAT32LE,
        WAVE_RATE,
        1
#endif /* __cplusplus */
    };
    pa_channel_map_init_mono(&pdata->lmap);
    pdata->lmap.map[0] = (pdata->mode == MM_STEREO ? PA_CHANNEL_POSITION_LEFT : PA_CHANNEL_POSITION_MONO);
    if (!(pdata->left = pulse_setup_stream(pdata, &ss, &pdata->lmap, NULL)))
        goto fail;
    if (pdata->mode == MM_STEREO) {
        pa_channel_map_init_mono(&pdata->rmap);
        pdata->rmap.map[0] = PA_CHANNEL_POSITION_RIGHT;
        if (!(pdata->right = pulse_setup_stream(pdata, &ss, &pdata->rmap, pdata->left)))
            goto fail;
    }
    return;
fail:
    pulse_shutdown(pdata);
}

static void pulse_ctx_state_cb(pa_context* c, void* userdata) {
    pulse_data* pdata = (pulse_data*)userdata;
    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
            pulse_setup_streams(pdata);
            break;
        case PA_CONTEXT_TERMINATED:
            break;
        case PA_CONTEXT_FAILED:
            log(LOG_ERR, "pulse: %s: connection failed: %s\n", SERVER_IFNOTNULL(pdata->server), pa_strerror(pa_context_errno(pdata->context)));
            pulse_shutdown(pdata);
            break;
        case PA_CONTEXT_CONNECTING:
            log(LOG_INFO, "pulse: %s: connecting...\n", SERVER_IFNOTNULL(pdata->server));
            break;
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;
    }
}

void pulse_init() {
    if (!mainloop && !(mainloop = pa_threaded_mainloop_new())) {
        cerr << "Failed to initialize PulseAudio main loop - aborting\n";
        error();
    }
}

int pulse_setup(pulse_data* pdata, mix_modes mixmode) {
    if (!(pdata->context = pa_context_new(pa_threaded_mainloop_get_api(mainloop), pdata->name))) {
        log(LOG_ERR, "%s", "pulse: failed to create context\n");
        return -1;
    }
    pdata->mode = mixmode;
    PA_LOOP_LOCK(mainloop);
    int ret = 0;
    pa_context_set_state_callback(pdata->context, &pulse_ctx_state_cb, pdata);
    if (pa_context_connect(pdata->context, pdata->server, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        log(LOG_WARNING, "pulse: %s: failed to connect: %s\n", SERVER_IFNOTNULL(pdata->server), pa_strerror(pa_context_errno(pdata->context)));
        // Don't clean up things here, context state is now set to PA_CONTEXT_FAILED,
        // so pulse_ctx_state_cb will take care of that.
        ret = -1;
    }
    PA_LOOP_UNLOCK(mainloop);
    return ret;
}

void pulse_start() {
    if (!mainloop)
        return;
    PA_LOOP_LOCK(mainloop);
    pa_threaded_mainloop_start(mainloop);
    PA_LOOP_UNLOCK(mainloop);
}

static int pulse_write_single_stream(pa_stream* stream, pulse_data* pdata, const float* data, size_t len, bool is_master) {
    pa_usec_t latency;
    int ret = -1;
    int lret;

    PA_LOOP_LOCK(mainloop);
    if (!stream || pa_stream_get_state(stream) != PA_STREAM_READY)
        goto end;

    if (is_master) { /* latency info is only meaningful for master stream) */
        lret = pa_stream_get_latency(stream, &latency, NULL);
        if (lret < 0) {
            log(LOG_WARNING, "pulse: %s: failed to get latency info for stream \"%s\" (error is: %s), disconnecting\n", SERVER_IFNOTNULL(pdata->server), pdata->stream_name, pa_strerror(lret));
            goto end;
        }
        if (latency > PULSE_STREAM_LATENCY_LIMIT) {
            log(LOG_INFO, "pulse: %s: exceeded max backlog for stream \"%s\", disconnecting\n", SERVER_IFNOTNULL(pdata->server), pdata->stream_name);
            goto end;
        }
        debug_bulk_print("pulse: %s: stream=\"%s\" lret=%d latency=%f ms\n", SERVER_IFNOTNULL(pdata->server), pdata->stream_name, lret, (float)latency / 1000.0f);
    }
    if (pa_stream_write(stream, data, len, NULL, 0LL, PA_SEEK_RELATIVE) < 0) {
        log(LOG_WARNING, "pulse: %s: could not write to stream \"%s\", disconnecting\n", SERVER_IFNOTNULL(pdata->server), pdata->stream_name);
        goto end;
    }
    ret = 0;
end:
    PA_LOOP_UNLOCK(mainloop);
    return ret;
}

void pulse_write_stream(pulse_data* pdata, mix_modes mode, const float* data_left, const float* data_right, size_t len) {
    PA_LOOP_LOCK(mainloop);
    if (!pdata->context || pa_context_get_state(pdata->context) != PA_CONTEXT_READY)
        goto end;
    if (pulse_write_single_stream(pdata->left, pdata, data_left, len, true) < 0)
        goto fail;
    if (mode == MM_STEREO && pulse_write_single_stream(pdata->right, pdata, data_right, len, false) < 0)
        goto fail;
    goto end;
fail:
    pulse_shutdown(pdata);
end:
    PA_LOOP_UNLOCK(mainloop);
    return;
}
