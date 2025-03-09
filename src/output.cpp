/*
 * output.cpp
 * Output related routines
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
#include <math.h>
#include <ogg/ogg.h>
#include <shout/shout.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <vorbis/vorbisenc.h>

// SHOUTERR_RETRY is available since libshout 2.4.0.
// Set it to an impossible value if it's not there.
#ifndef SHOUTERR_RETRY
#define SHOUTERR_RETRY (-255)
#endif /* SHOUTERR_RETRY */

#include <lame/lame.h>

#ifdef WITH_PULSEAUDIO
#include <pulse/pulseaudio.h>
#endif /* WITH_PULSEAUDIO */

#include <syslog.h>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include "config.h"
#include "helper_functions.h"
#include "input-common.h"
#include "rtl_airband.h"

void shout_setup(icecast_data* icecast, mix_modes mixmode) {
    int ret;
    shout_t* shouttemp = shout_new();
    if (shouttemp == NULL) {
        printf("cannot allocate\n");
    }
    if (shout_set_host(shouttemp, icecast->hostname) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (shout_set_protocol(shouttemp, SHOUT_PROTOCOL_HTTP) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (shout_set_port(shouttemp, icecast->port) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
#ifdef LIBSHOUT_HAS_TLS
    if (shout_set_tls(shouttemp, icecast->tls_mode) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
#endif /* LIBSHOUT_HAS_TLS */
    char mp[100];
    sprintf(mp, "/%s", icecast->mountpoint);
    if (shout_set_mount(shouttemp, mp) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (shout_set_user(shouttemp, icecast->username) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (shout_set_password(shouttemp, icecast->password) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
#ifdef LIBSHOUT_HAS_CONTENT_FORMAT
    if (shout_set_content_format(shouttemp, SHOUT_FORMAT_MP3, SHOUT_USAGE_AUDIO, NULL) != SHOUTERR_SUCCESS) {
#else
    if (shout_set_format(shouttemp, SHOUT_FORMAT_MP3) != SHOUTERR_SUCCESS) {
#endif /* LIBSHOUT_HAS_CONTENT_FORMAT */
        shout_free(shouttemp);
        return;
    }
    if (icecast->name && shout_set_meta(shouttemp, SHOUT_META_NAME, icecast->name) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (icecast->genre && shout_set_meta(shouttemp, SHOUT_META_GENRE, icecast->genre) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    if (icecast->description && shout_set_meta(shouttemp, SHOUT_META_DESCRIPTION, icecast->description) != SHOUTERR_SUCCESS) {
        shout_free(shouttemp);
        return;
    }
    char samplerates[20];
    sprintf(samplerates, "%d", MP3_RATE);
    shout_set_audio_info(shouttemp, SHOUT_AI_SAMPLERATE, samplerates);
    shout_set_audio_info(shouttemp, SHOUT_AI_CHANNELS, (mixmode == MM_STEREO ? "2" : "1"));

    if (shout_set_nonblocking(shouttemp, 1) != SHOUTERR_SUCCESS) {
        log(LOG_ERR, "Error setting non-blocking mode: %s\n", shout_get_error(shouttemp));
        return;
    }
    ret = shout_open(shouttemp);
    if (ret == SHOUTERR_SUCCESS)
        ret = SHOUTERR_CONNECTED;

    if (ret == SHOUTERR_BUSY || ret == SHOUTERR_RETRY)
        log(LOG_NOTICE, "Connecting to %s:%d/%s...\n", icecast->hostname, icecast->port, icecast->mountpoint);

    int shout_timeout = 30 * 5;  // 30 * 5 * 200ms = 30s
    while ((ret == SHOUTERR_BUSY || ret == SHOUTERR_RETRY) && shout_timeout-- > 0) {
        SLEEP(200);
        ret = shout_get_connected(shouttemp);
    }

    if (ret == SHOUTERR_CONNECTED) {
        log(LOG_NOTICE, "Connected to %s:%d/%s\n", icecast->hostname, icecast->port, icecast->mountpoint);
        SLEEP(100);
        icecast->shout = shouttemp;
    } else {
        log(LOG_WARNING, "Could not connect to %s:%d/%s: %s\n", icecast->hostname, icecast->port, icecast->mountpoint, shout_get_error(shouttemp));
        shout_close(shouttemp);
        shout_free(shouttemp);
        return;
    }
}

lame_t airlame_init(mix_modes mixmode, int highpass, int lowpass) {
    lame_t lame = lame_init();
    if (!lame) {
        log(LOG_WARNING, "lame_init failed\n");
        return NULL;
    }

    lame_set_in_samplerate(lame, WAVE_RATE);
    lame_set_VBR(lame, vbr_mtrh);
    lame_set_brate(lame, 16);
    lame_set_quality(lame, 7);
    lame_set_lowpassfreq(lame, lowpass);
    lame_set_highpassfreq(lame, highpass);
    lame_set_out_samplerate(lame, MP3_RATE);
    if (mixmode == MM_STEREO) {
        lame_set_num_channels(lame, 2);
        lame_set_mode(lame, JOINT_STEREO);
    } else {
        lame_set_num_channels(lame, 1);
        lame_set_mode(lame, MONO);
    }
    debug_print("lame init with mixmode=%s\n", mixmode == MM_STEREO ? "MM_STEREO" : "MM_MONO");
    lame_init_params(lame);
    return lame;
}

class LameTone {
    unsigned char* _data;
    int _bytes;

   public:
    LameTone(mix_modes mixmode, int msec, unsigned int hz = 0) : _data(NULL), _bytes(0) {
        _data = (unsigned char*)XCALLOC(1, LAMEBUF_SIZE);

        int samples = (msec * WAVE_RATE) / 1000;
        float* buf = (float*)XCALLOC(samples, sizeof(float));

        debug_print("LameTone with mixmode=%s msec=%d hz=%u\n", mixmode == MM_STEREO ? "MM_STEREO" : "MM_MONO", msec, hz);
        if (hz > 0) {
            const float period = 1.0 / (float)hz;
            const float sample_time = 1.0 / (float)WAVE_RATE;
            float t = 0;
            for (int i = 0; i < samples; ++i, t += sample_time) {
                buf[i] = 0.9 * sinf(t * 2.0 * M_PI / period);
            }
        } else
            memset(buf, 0, samples * sizeof(float));
        lame_t lame = airlame_init(mixmode, 0, 0);
        if (lame) {
            _bytes = lame_encode_buffer_ieee_float(lame, buf, (mixmode == MM_STEREO ? buf : NULL), samples, _data, LAMEBUF_SIZE);
            if (_bytes > 0) {
                int flush_ofs = _bytes;
                if (flush_ofs & 0x1f)
                    flush_ofs += 0x20 - (flush_ofs & 0x1f);
                if (flush_ofs < LAMEBUF_SIZE) {
                    int flush_bytes = lame_encode_flush(lame, _data + flush_ofs, LAMEBUF_SIZE - flush_ofs);
                    if (flush_bytes > 0) {
                        memmove(_data + _bytes, _data + flush_ofs, flush_bytes);
                        _bytes += flush_bytes;
                    }
                }
            } else
                log(LOG_WARNING, "lame_encode_buffer_ieee_float: %d\n", _bytes);
            lame_close(lame);
        }
        free(buf);
    }

    ~LameTone() {
        if (_data)
            free(_data);
    }

    int write(FILE* f) {
        if (!_data || _bytes <= 0)
            return 1;

        if (fwrite(_data, 1, _bytes, f) != (unsigned int)_bytes) {
            log(LOG_WARNING, "LameTone: failed to write %d bytes\n", _bytes);
            return -1;
        }

        return 0;
    }
};

int rename_if_exists(char const* oldpath, char const* newpath) {
    int ret = rename(oldpath, newpath);
    if (ret < 0) {
        if (errno == ENOENT) {
            return 0;
        } else {
            log(LOG_ERR, "Could not rename %s to %s: %s\n", oldpath, newpath, strerror(errno));
        }
    }
    return ret;
}

/*
 * Open output file (mp3 or raw IQ) for append or initial write.
 * If appending to an audio file, insert discontinuity indictor tones
 * as well as the appropriate amount of silence when in continuous mode.
 */
static int open_file(file_data* fdata, mix_modes mixmode, int is_audio) {
    int rename_result = rename_if_exists(fdata->file_path.c_str(), fdata->file_path_tmp.c_str());
    fdata->f = fopen(fdata->file_path_tmp.c_str(), fdata->append ? "a+" : "w");
    if (fdata->f == NULL) {
        return -1;
    }

    struct stat st = {};
    if (!fdata->append || fstat(fileno(fdata->f), &st) != 0 || st.st_size == 0) {
        if (!fdata->split_on_transmission) {
            log(LOG_INFO, "Writing to %s\n", fdata->file_path.c_str());
        } else {
            debug_print("Writing to %s\n", fdata->file_path_tmp.c_str());
        }
        return 0;
    }
    if (rename_result < 0) {
        log(LOG_INFO, "Writing to %s\n", fdata->file_path.c_str());
        debug_print("Writing to %s\n", fdata->file_path_tmp.c_str());
    } else {
        log(LOG_INFO, "Appending from pos %llu to %s\n", (unsigned long long)st.st_size, fdata->file_path.c_str());
        debug_print("Appending from pos %llu to %s\n", (unsigned long long)st.st_size, fdata->file_path_tmp.c_str());
    }

    if (is_audio) {
        // fill missing space with marker tones
        LameTone lt_a(mixmode, 120, 2222);
        LameTone lt_b(mixmode, 120, 1111);
        LameTone lt_c(mixmode, 120, 555);

        int r = lt_a.write(fdata->f);
        if (r == 0)
            r = lt_b.write(fdata->f);
        if (r == 0)
            r = lt_c.write(fdata->f);

        // fill in time delta with silence if continuous output mode
        if (fdata->continuous) {
            time_t now = time(NULL);
            if (now > st.st_mtime) {
                time_t delta = now - st.st_mtime;
                if (delta > 3600) {
                    log(LOG_WARNING, "Too big time difference: %llu sec, limiting to one hour\n", (unsigned long long)delta);
                    delta = 3600;
                }
                LameTone lt_silence(mixmode, 1000);
                for (; (r == 0 && delta > 1); --delta)
                    r = lt_silence.write(fdata->f);
            }
        }

        if (r == 0)
            r = lt_c.write(fdata->f);
        if (r == 0)
            r = lt_b.write(fdata->f);
        if (r == 0)
            r = lt_a.write(fdata->f);

        if (r < 0)
            fseek(fdata->f, st.st_size, SEEK_SET);
    }
    return 0;
}

static void close_file(output_t* output) {
    file_data* fdata = (file_data*)(output->data);
    if (!fdata) {
        return;
    }

    // close all mp3 files for every output that has a lame context
    if (fdata->type == O_FILE && fdata->f && output->lame) {
        int encoded = lame_encode_flush_nogap(output->lame, output->lamebuf, LAMEBUF_SIZE);
        debug_print("closing file %s flushed %d\n", fdata->file_path.c_str(), encoded);

        if (encoded > 0) {
            size_t written = fwrite((void*)output->lamebuf, 1, (size_t)encoded, fdata->f);
            if (written == 0 || written < (size_t)encoded)
                log(LOG_WARNING, "Problem writing %s (%s)\n", fdata->file_path.c_str(), strerror(errno));
        }

        // write the lametag to the beginning of the file
        const int lametag_size = lame_get_lametag_frame(output->lame, output->lamebuf, LAMEBUF_SIZE);
        fseek(fdata->f, 0, SEEK_SET);
        fwrite(output->lamebuf, 1, lametag_size, fdata->f);
    }

    if (fdata->f) {
        fclose(fdata->f);
        fdata->f = NULL;
        rename_if_exists(fdata->file_path_tmp.c_str(), fdata->file_path.c_str());
    }
    fdata->file_path.clear();
    fdata->file_path_tmp.clear();
}

/*
 * Close current output file based on certain conditions:
 * If "split_on_transmission" mode is true check:
 *   If current duration too long, or we've been idle too long
 * else (append or continuous) check:
 *   if hour is different.
 */
static void close_if_necessary(output_t* output) {
    file_data* fdata = (file_data*)(output->data);

    static const double MIN_TRANSMISSION_TIME_SEC = 1.0;
    static const double MAX_TRANSMISSION_TIME_SEC = 60.0 * 60.0;
    static const double MAX_TRANSMISSION_IDLE_SEC = 0.5;

    if (!fdata || !fdata->f) {
        return;
    }

    timeval current_time;
    gettimeofday(&current_time, NULL);

    if (fdata->split_on_transmission) {
        double duration_sec = delta_sec(&fdata->open_time, &current_time);
        double idle_sec = delta_sec(&fdata->last_write_time, &current_time);

        if (duration_sec > MAX_TRANSMISSION_TIME_SEC || (duration_sec > MIN_TRANSMISSION_TIME_SEC && idle_sec > MAX_TRANSMISSION_IDLE_SEC)) {
            debug_print("closing file %s, duration %f sec, idle %f sec\n", fdata->file_path.c_str(), duration_sec, idle_sec);
            close_file(output);
        }
        return;
    }

    // Check if the hour boundary was just crossed.  NOTE: Actual hour number doesn't matter but still
    // need to use localtime if enabled (some timezones have partial hour offsets)
    int start_hour;
    int current_hour;
    if (use_localtime) {
        start_hour = localtime(&(fdata->open_time.tv_sec))->tm_hour;
        current_hour = localtime(&current_time.tv_sec)->tm_hour;
    } else {
        start_hour = gmtime(&(fdata->open_time.tv_sec))->tm_hour;
        current_hour = gmtime(&current_time.tv_sec)->tm_hour;
    }

    if (start_hour != current_hour) {
        debug_print("closing file %s after crossing hour boundary\n", fdata->file_path.c_str());
        close_file(output);
    }
}

/*
 * For a particular channel file output, check if there is a file currently open.
 * If so, that file may need to be flushed and closed.
 *
 * If the existing open file is good for continued use, return true.
 * Otherwise, create a file name based on the current timestamp and
 * open that new file.  If that file open succeeded, return true.
 */
static bool output_file_ready(channel_t* channel, output_t* output) {
    file_data* fdata = (file_data*)(output->data);
    if (!fdata) {
        return false;
    }

    close_if_necessary(output);

    if (fdata->f) {  // still open
        return true;
    }

    timeval current_time;
    gettimeofday(&current_time, NULL);
    struct tm* time;
    if (use_localtime) {
        time = localtime(&current_time.tv_sec);
    } else {
        time = gmtime(&current_time.tv_sec);
    }

    char timestamp[32];
    if (strftime(timestamp, sizeof(timestamp), fdata->split_on_transmission ? "_%Y%m%d_%H%M%S" : "_%Y%m%d_%H", time) == 0) {
        log(LOG_NOTICE, "strftime returned 0\n");
        return false;
    }

    std::string output_dir;
    if (fdata->dated_subdirectories) {
        output_dir = make_dated_subdirs(fdata->basedir, time);
        if (output_dir.empty()) {
            log(LOG_ERR, "Failed to create dated subdirectory\n");
            return false;
        }
    } else {
        output_dir = fdata->basedir;
        make_dir(output_dir);
    }

    // use a string stream to build the output filepath
    std::stringstream ss;
    ss << output_dir << '/' << fdata->basename << timestamp;
    if (fdata->include_freq) {
        ss << '_' << channel->freqlist[channel->freq_idx].frequency;
    }
    ss << fdata->suffix;
    fdata->file_path = ss.str();

    fdata->file_path_tmp = fdata->file_path + ".tmp";

    fdata->open_time = fdata->last_write_time = current_time;

    const int is_audio = output->type == O_RAWFILE ? 0 : 1;
    if (open_file(fdata, channel->mode, is_audio) < 0) {
        log(LOG_WARNING, "Cannot open output file %s (%s)\n", fdata->file_path_tmp.c_str(), strerror(errno));
        return false;
    }

    return true;
}

// Create all the output for a particular channel.
void process_outputs(channel_t* channel, int cur_scan_freq) {
    for (int k = 0; k < channel->output_count; k++) {
        if (channel->outputs[k].enabled == false)
            continue;
        if (channel->outputs[k].type == O_ICECAST) {
            icecast_data* icecast = (icecast_data*)(channel->outputs[k].data);
            if (icecast->shout == NULL)
                continue;

            // encode and send mp3 to shoutcast output
            const auto& lame = channel->outputs[k].lame;
            const auto& lamebuf = channel->outputs[k].lamebuf;
            int mp3_bytes = lame_encode_buffer_ieee_float(lame, channel->waveout, (channel->mode == MM_STEREO ? channel->waveout_r : NULL), WAVE_BATCH, lamebuf, LAMEBUF_SIZE);
            if (mp3_bytes < 0) {
                log(LOG_WARNING, "lame_encode_buffer_ieee_float: %d\n", mp3_bytes);
            }

            if (mp3_bytes == 0) {
                continue;
            }

            int ret = shout_send(icecast->shout, channel->outputs[k].lamebuf, mp3_bytes);

            if (ret != SHOUTERR_SUCCESS || shout_queuelen(icecast->shout) > MAX_SHOUT_QUEUELEN) {
                if (shout_queuelen(icecast->shout) > MAX_SHOUT_QUEUELEN)
                    log(LOG_WARNING, "Exceeded max backlog for %s:%d/%s, disconnecting\n", icecast->hostname, icecast->port, icecast->mountpoint);
                // reset connection
                log(LOG_WARNING, "Lost connection to %s:%d/%s\n", icecast->hostname, icecast->port, icecast->mountpoint);
                shout_close(icecast->shout);
                shout_free(icecast->shout);
                icecast->shout = NULL;
            } else if (icecast->send_scan_freq_tags && cur_scan_freq >= 0) {
                shout_metadata_t* meta = shout_metadata_new();
                char description[32];
                if (channel->freqlist[channel->freq_idx].label != NULL) {
                    if (shout_metadata_add(meta, "song", channel->freqlist[channel->freq_idx].label) != SHOUTERR_SUCCESS) {
                        log(LOG_WARNING, "Failed to add shout metadata\n");
                    }
                } else {
                    snprintf(description, sizeof(description), "%.3f MHz", channel->freqlist[channel->freq_idx].frequency / 1000000.0);
                    if (shout_metadata_add(meta, "song", description) != SHOUTERR_SUCCESS) {
                        log(LOG_WARNING, "Failed to add shout metadata\n");
                    }
                }
                if (SHOUT_SET_METADATA(icecast->shout, meta) != SHOUTERR_SUCCESS) {
                    log(LOG_WARNING, "Failed to add shout metadata\n");
                }
                shout_metadata_free(meta);
            }
        } else if (channel->outputs[k].type == O_FILE || channel->outputs[k].type == O_RAWFILE) {
            file_data* fdata = (file_data*)(channel->outputs[k].data);

            if (fdata->continuous == false && channel->axcindicate == NO_SIGNAL && channel->outputs[k].active == false) {
                close_if_necessary(&channel->outputs[k]);
                continue;
            }

            if (!output_file_ready(channel, &channel->outputs[k])) {
                log(LOG_WARNING, "Output disabled\n");
                channel->outputs[k].enabled = false;
                continue;
            };

            // encode mp3 bytes if O_FILE
            const auto& lame = channel->outputs[k].lame;
            const auto& lamebuf = channel->outputs[k].lamebuf;
            int mp3_bytes = 0;
            if (channel->outputs[k].type == O_FILE) {
                mp3_bytes = lame_encode_buffer_ieee_float(lame, channel->waveout, (channel->mode == MM_STEREO ? channel->waveout_r : NULL), WAVE_BATCH, lamebuf, LAMEBUF_SIZE);
                if (mp3_bytes < 0) {
                    log(LOG_WARNING, "lame_encode_buffer_ieee_float: %d\n", mp3_bytes);
                }

                if (mp3_bytes <= 0) {
                    continue;
                }
            }

            size_t buflen = 0, written = 0;
            if (channel->outputs[k].type == O_FILE) {
                buflen = (size_t)mp3_bytes;
                written = fwrite(lamebuf, 1, buflen, fdata->f);
            } else if (channel->outputs[k].type == O_RAWFILE) {
                buflen = 2 * sizeof(float) * WAVE_BATCH;
                written = fwrite(channel->iq_out, 1, buflen, fdata->f);
            }
            if (written < buflen) {
                if (ferror(fdata->f))
                    log(LOG_WARNING, "Cannot write to %s (%s), output disabled\n", fdata->file_path.c_str(), strerror(errno));
                else
                    log(LOG_WARNING, "Short write on %s, output disabled\n", fdata->file_path.c_str());
                close_file(&channel->outputs[k]);
                channel->outputs[k].enabled = false;
            }
            channel->outputs[k].active = (channel->axcindicate != NO_SIGNAL);
            gettimeofday(&fdata->last_write_time, NULL);
        } else if (channel->outputs[k].type == O_MIXER) {
            mixer_data* mdata = (mixer_data*)(channel->outputs[k].data);
            mixer_put_samples(mdata->mixer, mdata->input, channel->waveout, channel->axcindicate != NO_SIGNAL, WAVE_BATCH);
        } else if (channel->outputs[k].type == O_UDP_STREAM) {
            udp_stream_data* sdata = (udp_stream_data*)channel->outputs[k].data;

            if (sdata->continuous == false && channel->axcindicate == NO_SIGNAL) {
                continue;
            }

            if (channel->mode == MM_MONO) {
                udp_stream_write(sdata, channel->waveout, (size_t)WAVE_BATCH * sizeof(float));
            } else {
                udp_stream_write(sdata, channel->waveout, channel->waveout_r, (size_t)WAVE_BATCH * sizeof(float));
            }

#ifdef WITH_PULSEAUDIO
        } else if (channel->outputs[k].type == O_PULSE) {
            pulse_data* pdata = (pulse_data*)(channel->outputs[k].data);
            if (pdata->continuous == false && channel->axcindicate == NO_SIGNAL)
                continue;

            pulse_write_stream(pdata, channel->mode, channel->waveout, channel->waveout_r, (size_t)WAVE_BATCH * sizeof(float));
#endif /* WITH_PULSEAUDIO */
        }
    }
}

void disable_channel_outputs(channel_t* channel) {
    for (int k = 0; k < channel->output_count; k++) {
        output_t* output = channel->outputs + k;
        output->enabled = false;
        if (output->type == O_ICECAST) {
            icecast_data* icecast = (icecast_data*)(channel->outputs[k].data);
            if (icecast->shout == NULL)
                continue;
            log(LOG_WARNING, "Closing connection to %s:%d/%s\n", icecast->hostname, icecast->port, icecast->mountpoint);
            shout_close(icecast->shout);
            shout_free(icecast->shout);
            icecast->shout = NULL;
        } else if (output->type == O_FILE || output->type == O_RAWFILE) {
            close_file(&channel->outputs[k]);
        } else if (output->type == O_MIXER) {
            mixer_data* mdata = (mixer_data*)(output->data);
            mixer_disable_input(mdata->mixer, mdata->input);
        } else if (output->type == O_UDP_STREAM) {
            udp_stream_data* sdata = (udp_stream_data*)output->data;
            udp_stream_shutdown(sdata);
#ifdef WITH_PULSEAUDIO
        } else if (output->type == O_PULSE) {
            pulse_data* pdata = (pulse_data*)(output->data);
            pulse_shutdown(pdata);
#endif /* WITH_PULSEAUDIO */
        }
    }
}

void disable_device_outputs(device_t* dev) {
    log(LOG_INFO, "Disabling device outputs\n");
    for (int j = 0; j < dev->channel_count; j++) {
        disable_channel_outputs(dev->channels + j);
    }
}

static void print_channel_metric(FILE* f, char const* name, float freq, char* label) {
    fprintf(f, "%s{freq=\"%.3f\"", name, freq / 1000000.0);
    if (label != NULL) {
        fprintf(f, ",label=\"%s\"", label);
    }
    fprintf(f, "}");
}

static void output_channel_noise_levels(FILE* f) {
    fprintf(f,
            "# HELP channel_noise_level Raw squelch noise_level.\n"
            "# TYPE channel_noise_level gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_noise_level", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%.3f\n", channel->freqlist[k].squelch.noise_level());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_dbfs_noise_levels(FILE* f) {
    fprintf(f,
            "# HELP channel_dbfs_noise_level Squelch noise_level as dBFS.\n"
            "# TYPE channel_dbfs_noise_level gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_dbfs_noise_level", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%.3f\n", level_to_dBFS(channel->freqlist[k].squelch.noise_level()));
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_signal_levels(FILE* f) {
    fprintf(f,
            "# HELP channel_signal_level Raw squelch signal_level.\n"
            "# TYPE channel_signal_level gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_signal_level", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%.3f\n", channel->freqlist[k].squelch.signal_level());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_dbfs_signal_levels(FILE* f) {
    fprintf(f,
            "# HELP channel_dbfs_signal_level Squelch signal_level as dBFS.\n"
            "# TYPE channel_dbfs_signal_level gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_dbfs_signal_level", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%.3f\n", level_to_dBFS(channel->freqlist[k].squelch.signal_level()));
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_squelch_levels(FILE* f) {
    fprintf(f,
            "# HELP channel_squelch_level Squelch squelch_level.\n"
            "# TYPE channel_squelch_level gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_squelch_level", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%.3f\n", channel->freqlist[k].squelch.squelch_level());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_squelch_counter(FILE* f) {
    fprintf(f,
            "# HELP channel_squelch_counter Squelch open_count.\n"
            "# TYPE channel_squelch_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_squelch_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].squelch.open_count());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_flappy_counter(FILE* f) {
    fprintf(f,
            "# HELP channel_flappy_counter Squelch flappy_count.\n"
            "# TYPE channel_flappy_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_flappy_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].squelch.flappy_count());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_ctcss_counter(FILE* f) {
    fprintf(f,
            "# HELP channel_ctcss_counter count of windows with CTCSS detected.\n"
            "# TYPE channel_ctcss_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_ctcss_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].squelch.ctcss_count());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_no_ctcss_counter(FILE* f) {
    fprintf(f,
            "# HELP channel_no_ctcss_counter count of windows without CTCSS detected.\n"
            "# TYPE channel_no_ctcss_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_no_ctcss_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].squelch.no_ctcss_count());
            }
        }
    }
    fprintf(f, "\n");
}

static void output_channel_activity_counters(FILE* f) {
    fprintf(f,
            "# HELP channel_activity_counter Loops of output_thread with frequency active.\n"
            "# TYPE channel_activity_counter counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                print_channel_metric(f, "channel_activity_counter", channel->freqlist[k].frequency, channel->freqlist[k].label);
                fprintf(f, "\t%zu\n", channel->freqlist[k].active_counter);
            }
        }
    }
    fprintf(f, "\n");
}

static void output_device_buffer_overflows(FILE* f) {
    fprintf(f,
            "# HELP buffer_overflow_count Number of times a device's buffer has overflowed.\n"
            "# TYPE buffer_overflow_count counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        fprintf(f, "buffer_overflow_count{device=\"%d\"}\t%zu\n", i, dev->input->overflow_count);
    }
    fprintf(f, "\n");
}

static void output_output_overruns(FILE* f) {
    fprintf(f,
            "# HELP output_overrun_count Number of times a device or mixer output has overrun.\n"
            "# TYPE output_overrun_count counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        fprintf(f, "output_overrun_count{device=\"%d\"}\t%zu\n", i, dev->output_overrun_count);
    }
    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        fprintf(f, "output_overrun_count{mixer=\"%d\"}\t%zu\n", i, mixer->output_overrun_count);
    }
    fprintf(f, "\n");
}

static void output_input_overruns(FILE* f) {
    if (mixer_count == 0) {
        return;
    }

    fprintf(f,
            "# HELP input_overrun_count Number of times mixer input has overrun.\n"
            "# TYPE input_overrun_count counter\n");

    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        for (int j = 0; j < mixer->input_count; j++) {
            mixinput_t* input = mixer->inputs + j;
            fprintf(f, "input_overrun_count{mixer=\"%d\",input=\"%d\"}\t%zu\n", i, j, input->input_overrun_count);
        }
    }
    fprintf(f, "\n");
}

void write_stats_file(timeval* last_stats_write) {
    if (!stats_filepath) {
        return;
    }

    timeval current_time;
    gettimeofday(&current_time, NULL);

    static const double STATS_FILE_TIMING = 15.0;
    if (!do_exit && delta_sec(last_stats_write, &current_time) < STATS_FILE_TIMING) {
        return;
    }

    *last_stats_write = current_time;

    FILE* file = fopen(stats_filepath, "w");
    if (!file) {
        log(LOG_WARNING, "Cannot open output file %s (%s)\n", stats_filepath, strerror(errno));
        return;
    }

    output_channel_activity_counters(file);
    output_channel_noise_levels(file);
    output_channel_dbfs_noise_levels(file);
    output_channel_signal_levels(file);
    output_channel_dbfs_signal_levels(file);
    output_channel_squelch_counter(file);
    output_channel_squelch_levels(file);
    output_channel_flappy_counter(file);
    output_channel_ctcss_counter(file);
    output_channel_no_ctcss_counter(file);
    output_device_buffer_overflows(file);
    output_output_overruns(file);
    output_input_overruns(file);

    fclose(file);
}

void* output_thread(void* param) {
    assert(param != NULL);
    output_params_t* output_param = (output_params_t*)param;
    struct freq_tag tag;
    struct timeval tv;
    int new_freq = -1;
    timeval last_stats_write = {0, 0};

    debug_print("Starting output thread, devices %d:%d, mixers %d:%d, signal %p\n", output_param->device_start, output_param->device_end, output_param->mixer_start, output_param->mixer_end,
                output_param->mp3_signal);

#ifdef DEBUG
    timeval ts, te;
    gettimeofday(&ts, NULL);
#endif /* DEBUG */
    while (!do_exit) {
        output_param->mp3_signal->wait();
        for (int i = output_param->mixer_start; i < output_param->mixer_end; i++) {
            if (mixers[i].enabled == false)
                continue;
            channel_t* channel = &mixers[i].channel;
            if (channel->state == CH_READY) {
                process_outputs(channel, -1);
                channel->state = CH_DIRTY;
            }
        }
#ifdef DEBUG
        gettimeofday(&te, NULL);
        debug_bulk_print("mixeroutput: %lu.%lu %lu\n", te.tv_sec, (unsigned long)te.tv_usec, (te.tv_sec - ts.tv_sec) * 1000000UL + te.tv_usec - ts.tv_usec);
        ts.tv_sec = te.tv_sec;
        ts.tv_usec = te.tv_usec;
#endif /* DEBUG */
        for (int i = output_param->device_start; i < output_param->device_end; i++) {
            device_t* dev = devices + i;
            if (dev->input->state == INPUT_RUNNING && dev->waveavail) {
                if (dev->mode == R_SCAN) {
                    tag_queue_get(dev, &tag);
                    if (tag.freq >= 0) {
                        tag.tv.tv_sec += shout_metadata_delay;
                        gettimeofday(&tv, NULL);
                        if (tag.tv.tv_sec < tv.tv_sec || (tag.tv.tv_sec == tv.tv_sec && tag.tv.tv_usec <= tv.tv_usec)) {
                            new_freq = tag.freq;
                            tag_queue_advance(dev);
                        }
                    }
                }
                for (int j = 0; j < dev->channel_count; j++) {
                    channel_t* channel = devices[i].channels + j;
                    process_outputs(channel, new_freq);
                    memcpy(channel->waveout, channel->waveout + WAVE_BATCH, AGC_EXTRA * 4);
                }
                dev->waveavail = 0;
            }
            // make sure we don't carry new_freq value to the next receiver which might be working
            // in multichannel mode
            new_freq = -1;
        }
        if (output_param->device_start == 0) {
            write_stats_file(&last_stats_write);
        }
    }
    return 0;
}

// reconnect as required
void* output_check_thread(void*) {
    while (!do_exit) {
        SLEEP(10000);
        for (int i = 0; i < device_count; i++) {
            device_t* dev = devices + i;
            for (int j = 0; j < dev->channel_count; j++) {
                for (int k = 0; k < dev->channels[j].output_count; k++) {
                    if (dev->channels[j].outputs[k].type == O_ICECAST) {
                        icecast_data* icecast = (icecast_data*)(dev->channels[j].outputs[k].data);
                        if (dev->input->state == INPUT_FAILED) {
                            if (icecast->shout) {
                                log(LOG_WARNING, "Device #%d failed, disconnecting stream %s:%d/%s\n", i, icecast->hostname, icecast->port, icecast->mountpoint);
                                shout_close(icecast->shout);
                                shout_free(icecast->shout);
                                icecast->shout = NULL;
                            }
                        } else if (dev->input->state == INPUT_RUNNING) {
                            if (icecast->shout == NULL) {
                                log(LOG_NOTICE, "Trying to reconnect to %s:%d/%s...\n", icecast->hostname, icecast->port, icecast->mountpoint);
                                shout_setup(icecast, dev->channels[j].mode);
                            }
                        }
                    } else if (dev->channels[j].outputs[k].type == O_UDP_STREAM) {
                        udp_stream_data* sdata = (udp_stream_data*)dev->channels[j].outputs[k].data;

                        if (dev->input->state == INPUT_FAILED) {
                            udp_stream_shutdown(sdata);
                        }
#ifdef WITH_PULSEAUDIO
                    } else if (dev->channels[j].outputs[k].type == O_PULSE) {
                        pulse_data* pdata = (pulse_data*)(dev->channels[j].outputs[k].data);
                        if (dev->input->state == INPUT_FAILED) {
                            if (pdata->context) {
                                pulse_shutdown(pdata);
                            }
                        } else if (dev->input->state == INPUT_RUNNING) {
                            if (pdata->context == NULL) {
                                pulse_setup(pdata, dev->channels[j].mode);
                            }
                        }
#endif /* WITH_PULSEAUDIO */
                    }
                }
            }
        }
        for (int i = 0; i < mixer_count; i++) {
            if (mixers[i].enabled == false)
                continue;
            for (int k = 0; k < mixers[i].channel.output_count; k++) {
                if (mixers[i].channel.outputs[k].enabled == false)
                    continue;
                if (mixers[i].channel.outputs[k].type == O_ICECAST) {
                    icecast_data* icecast = (icecast_data*)(mixers[i].channel.outputs[k].data);
                    if (icecast->shout == NULL) {
                        log(LOG_NOTICE, "Trying to reconnect to %s:%d/%s...\n", icecast->hostname, icecast->port, icecast->mountpoint);
                        shout_setup(icecast, mixers[i].channel.mode);
                    }
#ifdef WITH_PULSEAUDIO
                } else if (mixers[i].channel.outputs[k].type == O_PULSE) {
                    pulse_data* pdata = (pulse_data*)(mixers[i].channel.outputs[k].data);
                    if (pdata->context == NULL) {
                        pulse_setup(pdata, mixers[i].channel.mode);
                    }
#endif /* WITH_PULSEAUDIO */
                }
            }
        }
    }
    return 0;
}
