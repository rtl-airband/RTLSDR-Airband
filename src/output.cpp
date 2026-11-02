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
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
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
#include "auto_ppm.h"
#include "config.h"
#include "helper_functions.h"
#include "input-common.h"
#include "live_reconfig.h"
#include "mixer_remote.h"
#include "rtl_airband.h"

static void file_time(const time_t* t, struct tm* time) {
    if (use_localtime) {
        localtime_r(t, time);
    } else {
        gmtime_r(t, time);
    }
}

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
    std::string mp = make_icecast_mountpoint(icecast->mountpoint);
    if (shout_set_mount(shouttemp, mp.c_str()) != SHOUTERR_SUCCESS) {
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

static std::string build_output_filename(const file_data* fdata, bool include_tx_time) {
    // timestamp format includes minutes and seconds if splitting on transmission
    const char* timestamp_format = fdata->split_on_transmission ? "_%Y%m%d_%H%M%S" : "_%Y%m%d_%H";

    struct tm open_time;
    file_time(&fdata->file_open_time.tv_sec, &open_time);
    struct tm transmission_time;
    file_time(&fdata->transmission_start_time.tv_sec, &transmission_time);

    // build timestamp strings for open time
    char open_timestamp[32];
    if (strftime(open_timestamp, sizeof(open_timestamp), timestamp_format, &open_time) == 0) {
        log(LOG_NOTICE, "strftime returned 0\n");
        return "";
    }

    // make sure the output directory exists
    std::string output_dir;
    if (fdata->dated_subdirectories) {
        // if using tx time then directory is named for transmission_time, otherwise open_time
        const struct tm* directory_time = include_tx_time ? &transmission_time : &open_time;
        output_dir = make_dated_subdirs(fdata->basedir, directory_time);

        if (output_dir.empty()) {
            log(LOG_ERR, "Failed to create dated subdirectory\n");
            return "";
        }
    } else {
        output_dir = fdata->basedir;
        make_dir(output_dir);
    }

    // use a string stream to build the output filepath
    std::stringstream ss;
    ss << output_dir << '/' << fdata->basename;

    // add transmission start first (if asked)
    if (include_tx_time) {
        char transmission_timestamp[32];
        if (strftime(transmission_timestamp, sizeof(transmission_timestamp), timestamp_format, &transmission_time) == 0) {
            log(LOG_NOTICE, "strftime returned 0\n");
            return "";
        }

        ss << transmission_timestamp;
    }

    // then open time (always)
    ss << open_timestamp;

    // then frequency (if valid)
    if (fdata->frequency > 0) {
        ss << '_' << fdata->frequency;
    }

    // finally suffix and return
    ss << fdata->suffix;
    return ss.str();
}

/*
 * Open output file (mp3 or raw IQ) for append or initial write.
 * If appending to an audio file, insert discontinuity indictor tones
 * as well as the appropriate amount of silence when in continuous mode.
 */
static int open_file(file_data* fdata, mix_modes mixmode, int is_audio) {
    int rename_result = rename_if_exists(fdata->file_path.c_str(), fdata->file_path_tmp.c_str());
    if (fdata->append) {
        // Open without O_APPEND so fseek+fwrite works correctly (e.g. for the lametag).
        // Try r+ (existing file) first; fall back to w (new file) if it doesn't exist yet.
        fdata->f = fopen(fdata->file_path_tmp.c_str(), "r+");
        if (fdata->f == NULL) {
            fdata->f = fopen(fdata->file_path_tmp.c_str(), "w");
        } else {
            fseek(fdata->f, 0, SEEK_END);
        }
    } else {
        fdata->f = fopen(fdata->file_path_tmp.c_str(), "w");
    }
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

    double duration_sec = delta_sec(&fdata->file_open_time, &fdata->last_write_time);

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

        // Reset the encoder bitstream so the frame count is correct for the next file
        lame_init_bitstream(output->lame);
    }

    if (fdata->f) {
        fclose(fdata->f);
        fdata->f = NULL;
        bool keep = true;
        if (fdata->split_on_transmission && fdata->min_rx_seconds > 0.0 && duration_sec < fdata->min_rx_seconds) {
            keep = false;
        }

        if (keep) {
            rename_if_exists(fdata->file_path_tmp.c_str(), fdata->file_path.c_str());
            if (fdata->split_on_transmission && !fdata->post_write_script.empty()) {
                pid_t pid = fork();
                if (pid < 0) {
                    log(LOG_ERR, "Cannot fork for post_write_script: %s\n", strerror(errno));
                } else if (pid == 0) {
                    pid_t pid2 = fork();
                    if (pid2 == 0) {
                        execl("/bin/sh", "sh", fdata->post_write_script.c_str(), fdata->file_path.c_str(), (char*)NULL);
                        _exit(1);
                    }
                    _exit(0);
                } else {
                    waitpid(pid, NULL, 0);
                }
            }
#ifdef WITH_RDIO_SCANNER
            if (fdata->split_on_transmission && fdata->rdio_scanner) {
                rdio_scanner_enqueue(fdata->rdio_scanner, fdata->file_path, fdata->file_open_time, fdata->open_frequency);
            }
#endif /* WITH_RDIO_SCANNER */
        } else {
            unlink(fdata->file_path_tmp.c_str());
        }
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
 * reopening: the caller will open a new file right away if this one is closed
 *   (ie there is a signal present).
 * Returns true if the file closed due to should_close_split_file_max_time,
 *   so the next file may continue the same transmission.
 */
static bool close_if_necessary(output_t* output, bool reopening) {
    file_data* fdata = (file_data*)(output->data);

    if (!fdata || !fdata->f) {
        return false;
    }

    timeval current_time;
    gettimeofday(&current_time, NULL);

    if (fdata->split_on_transmission) {
        bool closed_due_to_max_time = false;
        double duration_sec = delta_sec(&fdata->file_open_time, &current_time);
        double idle_sec = delta_sec(&fdata->last_write_time, &current_time);

        // check if the split should be closed
        if (should_close_split_file(duration_sec, idle_sec, fdata->split_min_file_time, fdata->split_max_file_time, fdata->split_max_idle_time)) {
            // check if the reason to close is max time and only max time
            if (should_close_split_file_max_time(duration_sec, fdata->split_max_file_time) &&
                !should_close_split_file_idle(duration_sec, idle_sec, fdata->split_min_file_time, fdata->split_max_idle_time)) {
                closed_due_to_max_time = true;

                // if is the first split (file open time and transmission start time are the same),
                // AND filenames include transmission start times, AND the caller intends to open a
                // new file if this one is closed, then perform a first split rename
                if (timercmp(&fdata->file_open_time, &fdata->transmission_start_time, ==) && fdata->split_include_transmission_start && reopening) {
                    const std::string new_filepath = build_output_filename(fdata, true);
                    if (new_filepath.empty()) {
                        log(LOG_NOTICE, "failed to generate an output filename for a first split rename\n");
                    } else {
                        debug_print("changing output file name from %s to %s\n", fdata->file_path.c_str(), new_filepath.c_str());
                        fdata->file_path = new_filepath;
                    }
                }
            }

            debug_print("closing file %s, duration %f sec, idle %f sec\n", fdata->file_path.c_str(), duration_sec, idle_sec);
            close_file(output);
        }
        return closed_due_to_max_time;
    }

    // Check if the hour boundary was just crossed.  NOTE: Actual hour number doesn't matter but still
    // need to use localtime if enabled (some timezones have partial hour offsets)
    struct tm tmp_tm;
    file_time(&(fdata->file_open_time.tv_sec), &tmp_tm);
    const int start_hour = tmp_tm.tm_hour;
    file_time(&(current_time.tv_sec), &tmp_tm);
    const int current_hour = tmp_tm.tm_hour;

    if (start_hour != current_hour) {
        debug_print("closing file %s after crossing hour boundary\n", fdata->file_path.c_str());
        close_file(output);
    }
    return false;
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

    // check if an open file needs to be closed, and if it is, if it was a split transmission closed due to max time
    const bool closed_due_to_max_time = close_if_necessary(output, true);

    // if the output file is still open, nothing else to do
    if (fdata->f) {
        return true;
    }

    // otherwise, opening a new output file . . .

    // start by getting current time
    timeval current_time;
    gettimeofday(&current_time, NULL);

    // set the transmission_start_time ONLY if not continuing a transmission
    if (!closed_due_to_max_time) {
        fdata->transmission_start_time = current_time;
    }

    // always set open time and last write time to now
    fdata->file_open_time = current_time;
    fdata->last_write_time = current_time;

    // A mixer's own channel_t has no freqlist (parse_mixers() never sets one - a mixed stream
    // has no single source frequency), so channel->freqlist is NULL there; guard it rather
    // than dereferencing it unconditionally.
    int const channel_frequency = (channel->freqlist != NULL) ? channel->freqlist[channel->freq_idx].frequency : 0;

    // if including frequency then set, otherwise use 0 so it's not included
    // in the output filename
    fdata->frequency = fdata->include_freq ? channel_frequency : 0;
#ifdef WITH_RDIO_SCANNER
    fdata->open_frequency = channel_frequency;
#endif /* WITH_RDIO_SCANNER */

    // build the output filename and temp name.  at this point (opening a new file)
    // we can know if this is a 2nd or later split of a transmission but can't know if
    // this is the 1st file of a transmission that will later be split (eg part 1 will be
    // renamed in close_if_necessary when its transmission continues)
    const bool include_tx_time = fdata->split_include_transmission_start && closed_due_to_max_time;

    // build output filename and a tmp version
    const std::string output_filename = build_output_filename(fdata, include_tx_time);
    if (output_filename.empty()) {
        log(LOG_NOTICE, "failed to generate an output filename\n");
        return false;
    }
    fdata->file_path = output_filename;
    fdata->file_path_tmp = fdata->file_path + ".tmp";

    // open the (temp) file
    const int is_audio = output->type == O_RAWFILE ? 0 : 1;
    if (open_file(fdata, channel->mode, is_audio) < 0) {
        log(LOG_WARNING, "Cannot open output file %s (%s)\n", fdata->file_path_tmp.c_str(), strerror(errno));
        return false;
    }

    return true;
}

// Create all the output for a particular channel. tx_tag is the desired send_tx_tags content
// for this tick (the caller has context process_outputs() itself doesn't - device channel vs.
// mixer channel - needed to compute it; see compute_tx_tag_content()/mixer_select_active_tag_input()
// at the call sites in output_thread()).
void process_outputs(channel_t* channel, int cur_scan_freq, const std::string& tx_tag) {
    if (!channel->enabled) {
        return;
    }
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
                channel->outputs[k].lame_encode_failure_count++;
            }

            if (mp3_bytes == 0) {
                continue;
            }

            int ret = shout_send(icecast->shout, channel->outputs[k].lamebuf, mp3_bytes);

            if (ret != SHOUTERR_SUCCESS || shout_queuelen(icecast->shout) > MAX_SHOUT_QUEUELEN) {
                if (shout_queuelen(icecast->shout) > MAX_SHOUT_QUEUELEN) {
                    log(LOG_WARNING, "Exceeded max backlog for %s:%d/%s, disconnecting\n", icecast->hostname, icecast->port, icecast->mountpoint);
                    icecast->backlog_exceeded_count++;
                }
                // reset connection
                log(LOG_WARNING, "Lost connection to %s:%d/%s\n", icecast->hostname, icecast->port, icecast->mountpoint);
                icecast->disconnect_count++;
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
            } else if (icecast->send_tx_tags) {
                struct timeval now;
                gettimeofday(&now, NULL);
                std::string out_value;
                if (icecast_tx_tag_step(&icecast->tx_tag_state, tx_tag, now, shout_metadata_delay, &out_value)) {
                    shout_metadata_t* meta = shout_metadata_new();
                    if (shout_metadata_add(meta, "song", out_value.c_str()) != SHOUTERR_SUCCESS) {
                        log(LOG_WARNING, "Failed to add shout metadata\n");
                    }
                    if (SHOUT_SET_METADATA(icecast->shout, meta) != SHOUTERR_SUCCESS) {
                        log(LOG_WARNING, "Failed to set shout metadata\n");
                    } else {
                        icecast->tx_tag_update_count++;
                    }
                    shout_metadata_free(meta);
                }
            }
        } else if (channel->outputs[k].type == O_FILE || channel->outputs[k].type == O_RAWFILE) {
            file_data* fdata = (file_data*)(channel->outputs[k].data);

            if (fdata->continuous == false && channel->axcindicate == NO_SIGNAL && channel->outputs[k].active == false) {
                close_if_necessary(&channel->outputs[k], false);
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
                    channel->outputs[k].lame_encode_failure_count++;
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
                fdata->write_failure_count++;
                close_file(&channel->outputs[k]);
                channel->outputs[k].enabled = false;
            }
            channel->outputs[k].active = (channel->axcindicate != NO_SIGNAL);
            gettimeofday(&fdata->last_write_time, NULL);
        } else if (channel->outputs[k].type == O_MIXER) {
            mixer_data* mdata = (mixer_data*)(channel->outputs[k].data);
            mixer_put_samples(mdata->mixer, mdata->input, channel->waveout, channel->axcindicate != NO_SIGNAL, WAVE_BATCH);
        } else if (channel->outputs[k].type == O_MIXER_REMOTE) {
            // Called unconditionally every tick, not gated on squelch like O_UDP_STREAM's
            // `continuous` check - mirrors O_MIXER's own unconditional mixer_put_samples() call
            // above. This is load-bearing: the receiver's mixer_thread() silence-fills a slot
            // that misses its ~1/16s window, but only correctly distinguishes "sender alive,
            // channel quiet" from "sender/process gone" if packets keep arriving every tick.
            mixer_remote_send_data* rdata = (mixer_remote_send_data*)(channel->outputs[k].data);
            mixer_remote_send(rdata, channel->waveout, (size_t)WAVE_BATCH, channel->axcindicate != NO_SIGNAL);
        } else if (channel->outputs[k].type == O_UDP_STREAM) {
            udp_stream_data* sdata = (udp_stream_data*)channel->outputs[k].data;

            if (sdata->continuous == false && channel->axcindicate == NO_SIGNAL) {
                continue;
            }

            if (channel->mode == MM_MONO) {
                udp_stream_write(sdata, channel->waveout, (size_t)WAVE_BATCH);
            } else {
                udp_stream_write(sdata, channel->waveout, channel->waveout_r, (size_t)WAVE_BATCH);
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

void disable_channel_outputs(channel_t* channel, bool permanent) {
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
            mixer_disable_input(mdata->mixer, mdata->input, permanent);
        } else if (output->type == O_MIXER_REMOTE) {
            mixer_remote_send_data* rdata = (mixer_remote_send_data*)(output->data);
            mixer_remote_send_shutdown(rdata);
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

static void output_channel_freq_offsets(FILE* f) {
    fprintf(f,
            "# HELP channel_freq_offset_hz Smoothed carrier offset from the configured frequency, measured while "
            "squelch is open. Absent for a frequency that hasn't had a transmission yet.\n"
            "# TYPE channel_freq_offset_hz gauge\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            for (int k = 0; k < channel->freq_count; k++) {
                freq_t& fparms = channel->freqlist[k];
                if (fparms.freq_offset_updates.load(std::memory_order_acquire) == 0) {
                    continue;
                }
                print_channel_metric(f, "channel_freq_offset_hz", fparms.frequency, fparms.label);
                fprintf(f, "\t%.1f\n", fparms.freq_offset_hz.load(std::memory_order_relaxed));
            }
        }
    }
    fprintf(f, "\n");
}

static void output_device_peak_levels(FILE* f) {
    fprintf(f,
            "# HELP device_peak_dbfs Largest I/Q sample magnitude over the last ~1 s, relative to the ADC's full "
            "scale. 0 means the ADC is clipping; its negation is the headroom left before it does.\n"
            "# TYPE device_peak_dbfs gauge\n");

    for (int i = 0; i < device_count; i++) {
        input_t* input = devices[i].input;
        if (!input->peak_valid.load(std::memory_order_acquire)) {
            continue;
        }
        float const peak = input->peak_level.load(std::memory_order_relaxed);
        fprintf(f, "device_peak_dbfs{device=\"%d\"}\t%.2f\n", i, peak > 0.0f ? 20.0f * log10f(peak) : -200.0f);
    }
    fprintf(f, "\n");
}

static void output_device_clipped_samples(FILE* f) {
    fprintf(f,
            "# HELP device_clipped_sample_count Number of I/Q samples where either component hit the ADC's full-scale "
            "limit. A climbing count means the gain is too high for the strongest signal in the passband.\n"
            "# TYPE device_clipped_sample_count counter\n");

    for (int i = 0; i < device_count; i++) {
        fprintf(f, "device_clipped_sample_count{device=\"%d\"}\t%zu\n", i, devices[i].input->clipped_count.load(std::memory_order_relaxed));
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

static void output_device_buffer_underruns(FILE* f) {
    fprintf(f,
            "# HELP buffer_underrun_count Number of times a device's demod loop found insufficient input "
            "data to process a batch. Expected to increment frequently under healthy load, since the demod "
            "thread normally waits on new samples between batches; a low/flat value alongside a climbing "
            "buffer_overflow_count for the same device indicates the demod thread is CPU-saturated rather "
            "than waiting on input.\n"
            "# TYPE buffer_underrun_count counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        fprintf(f, "buffer_underrun_count{device=\"%d\"}\t%zu\n", i, dev->input->underrun_count);
    }
    fprintf(f, "\n");
}

static void output_device_centerfreq_retune_failures(FILE* f) {
    fprintf(f,
            "# HELP centerfreq_retune_failure_count Number of times a live centerfreq retune (dynamic_reload "
            "control socket, or R_SCAN's automatic frequency-hopping) failed at the hardware level. Does not "
            "indicate the device stopped running - only that this specific retune attempt did not take effect.\n"
            "# TYPE centerfreq_retune_failure_count counter\n");

    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        fprintf(f, "centerfreq_retune_failure_count{device=\"%d\"}\t%zu\n", i, dev->input->centerfreq_retune_failure_count);
    }
    fprintf(f, "\n");
}

static void output_device_auto_ppm(FILE* f) {
    bool any = false;
    for (int i = 0; i < device_count; i++) {
        any = any || devices[i].auto_ppm != NULL;
    }
    if (!any) {
        return;
    }
    std::string correction, residual, adjustments;
    char line[128];
    for (int i = 0; i < device_count; i++) {
        AutoPpmMetrics m;
        if (!auto_ppm_metrics(devices + i, &m)) {
            continue;
        }
        input_t* input = devices[i].input;
        input_settings_t settings;
        bool have_settings = false;
        {
            std::lock_guard<std::recursive_mutex> control(input_control_mutex());
            have_settings = input->get_settings(input, &settings) == 0;
        }
        if (have_settings) {
            snprintf(line, sizeof(line), "device_ppm_correction{device=\"%d\"}\t%g\n", i, settings.correction);
            correction += line;
        }
        if (m.residual_valid) {
            snprintf(line, sizeof(line), "device_auto_ppm_residual_ppm{device=\"%d\"}\t%.2f\n", i, m.residual_ppm);
            residual += line;
        }
        snprintf(line, sizeof(line), "device_auto_ppm_adjustment_count{device=\"%d\"}\t%llu\n", i, (unsigned long long)m.adjustments);
        adjustments += line;
    }
    fprintf(f,
            "# HELP device_ppm_correction Frequency correction currently applied to the tuner, in ppm. Only reported for "
            "devices with auto_ppm configured.\n"
            "# TYPE device_ppm_correction gauge\n%s\n",
            correction.c_str());
    fprintf(f,
            "# HELP device_auto_ppm_residual_ppm Correction auto_ppm still estimates is needed on top of the current one, "
            "from the median of the channels' carrier offsets. Absent until enough transmissions have been measured.\n"
            "# TYPE device_auto_ppm_residual_ppm gauge\n%s\n",
            residual.c_str());
    fprintf(f,
            "# HELP device_auto_ppm_adjustment_count Number of times auto_ppm has changed the device's correction.\n"
            "# TYPE device_auto_ppm_adjustment_count counter\n%s\n",
            adjustments.c_str());
}

static void output_process_cpu_seconds(FILE* f) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        return;
    }

    fprintf(f,
            "# HELP process_cpu_seconds_total Total user and system CPU time spent by this process, in seconds.\n"
            "# TYPE process_cpu_seconds_total counter\n"
            "process_cpu_seconds_total\t%.6f\n\n",
            rusage_cpu_seconds(ru));
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

static void output_icecast_health(FILE* f) {
    fprintf(f,
            "# HELP icecast_disconnect_count Number of times an icecast output lost its connection "
            "(network error, exceeded send backlog, or the owning device failing) and had to be "
            "reconnected by output_check_thread.\n"
            "# TYPE icecast_disconnect_count counter\n");
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = dev->channels + j;
            for (int k = 0; k < channel->output_count; k++) {
                if (channel->outputs[k].type != O_ICECAST)
                    continue;
                icecast_data* icecast = (icecast_data*)channel->outputs[k].data;
                fprintf(f, "icecast_disconnect_count{device=\"%d\",channel=\"%d\",output=\"%d\"}\t%zu\n", i, j, k, icecast->disconnect_count);
            }
        }
    }
    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        for (int k = 0; k < mixer->channel.output_count; k++) {
            if (mixer->channel.outputs[k].type != O_ICECAST)
                continue;
            icecast_data* icecast = (icecast_data*)mixer->channel.outputs[k].data;
            fprintf(f, "icecast_disconnect_count{mixer=\"%d\",output=\"%d\"}\t%zu\n", i, k, icecast->disconnect_count);
        }
    }
    fprintf(f, "\n");

    fprintf(f,
            "# HELP icecast_backlog_exceeded_count Number of times an icecast output's send queue exceeded "
            "MAX_SHOUT_QUEUELEN, forcing a disconnect. A subset of icecast_disconnect_count - specifically "
            "the case where the local encode rate outpaced what Icecast could drain, rather than a network-level error.\n"
            "# TYPE icecast_backlog_exceeded_count counter\n");
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = dev->channels + j;
            for (int k = 0; k < channel->output_count; k++) {
                if (channel->outputs[k].type != O_ICECAST)
                    continue;
                icecast_data* icecast = (icecast_data*)channel->outputs[k].data;
                fprintf(f, "icecast_backlog_exceeded_count{device=\"%d\",channel=\"%d\",output=\"%d\"}\t%zu\n", i, j, k, icecast->backlog_exceeded_count);
            }
        }
    }
    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        for (int k = 0; k < mixer->channel.output_count; k++) {
            if (mixer->channel.outputs[k].type != O_ICECAST)
                continue;
            icecast_data* icecast = (icecast_data*)mixer->channel.outputs[k].data;
            fprintf(f, "icecast_backlog_exceeded_count{mixer=\"%d\",output=\"%d\"}\t%zu\n", i, k, icecast->backlog_exceeded_count);
        }
    }
    fprintf(f, "\n");

    fprintf(f,
            "# HELP icecast_tx_tag_update_count Number of times a send_tx_tags metadata update "
            "(on-air label set on transmission start, cleared on transmission end) was successfully applied.\n"
            "# TYPE icecast_tx_tag_update_count counter\n");
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = dev->channels + j;
            for (int k = 0; k < channel->output_count; k++) {
                if (channel->outputs[k].type != O_ICECAST)
                    continue;
                icecast_data* icecast = (icecast_data*)channel->outputs[k].data;
                fprintf(f, "icecast_tx_tag_update_count{device=\"%d\",channel=\"%d\",output=\"%d\"}\t%zu\n", i, j, k, icecast->tx_tag_update_count);
            }
        }
    }
    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        for (int k = 0; k < mixer->channel.output_count; k++) {
            if (mixer->channel.outputs[k].type != O_ICECAST)
                continue;
            icecast_data* icecast = (icecast_data*)mixer->channel.outputs[k].data;
            fprintf(f, "icecast_tx_tag_update_count{mixer=\"%d\",output=\"%d\"}\t%zu\n", i, k, icecast->tx_tag_update_count);
        }
    }
    fprintf(f, "\n");
}

static void output_lame_encode_failures(FILE* f) {
    fprintf(f,
            "# HELP lame_encode_failure_count Number of times lame_encode_buffer_ieee_float() returned an "
            "error for this output. Only emitted for outputs that actually encode mp3 (icecast, file).\n"
            "# TYPE lame_encode_failure_count counter\n");
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = dev->channels + j;
            for (int k = 0; k < channel->output_count; k++) {
                if (!channel->outputs[k].has_mp3_output)
                    continue;
                fprintf(f, "lame_encode_failure_count{device=\"%d\",channel=\"%d\",output=\"%d\"}\t%zu\n", i, j, k, channel->outputs[k].lame_encode_failure_count);
            }
        }
    }
    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        for (int k = 0; k < mixer->channel.output_count; k++) {
            if (!mixer->channel.outputs[k].has_mp3_output)
                continue;
            fprintf(f, "lame_encode_failure_count{mixer=\"%d\",output=\"%d\"}\t%zu\n", i, k, mixer->channel.outputs[k].lame_encode_failure_count);
        }
    }
    fprintf(f, "\n");
}

static void output_file_write_failures(FILE* f) {
    fprintf(f,
            "# HELP file_write_failure_count Number of times fwrite() on a file/rawfile output came up "
            "short (disk full, I/O error). The output is disabled when this happens, so a healthy instance "
            "should see this stay at 0.\n"
            "# TYPE file_write_failure_count counter\n");
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = dev->channels + j;
            for (int k = 0; k < channel->output_count; k++) {
                if (channel->outputs[k].type != O_FILE && channel->outputs[k].type != O_RAWFILE)
                    continue;
                file_data* fdata = (file_data*)channel->outputs[k].data;
                fprintf(f, "file_write_failure_count{device=\"%d\",channel=\"%d\",output=\"%d\"}\t%zu\n", i, j, k, fdata->write_failure_count);
            }
        }
    }
    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        for (int k = 0; k < mixer->channel.output_count; k++) {
            if (mixer->channel.outputs[k].type != O_FILE && mixer->channel.outputs[k].type != O_RAWFILE)
                continue;
            file_data* fdata = (file_data*)mixer->channel.outputs[k].data;
            fprintf(f, "file_write_failure_count{mixer=\"%d\",output=\"%d\"}\t%zu\n", i, k, fdata->write_failure_count);
        }
    }
    fprintf(f, "\n");
}

static void output_udp_stream_drops(FILE* f) {
    fprintf(f,
            "# HELP udp_stream_dropped_packet_count Number of packets dropped by udp_stream's bounds "
            "checks (a mismatch between the computed length and the pre-sized send buffer).\n"
            "# TYPE udp_stream_dropped_packet_count counter\n");
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = dev->channels + j;
            for (int k = 0; k < channel->output_count; k++) {
                if (channel->outputs[k].type != O_UDP_STREAM)
                    continue;
                udp_stream_data* sdata = (udp_stream_data*)channel->outputs[k].data;
                fprintf(f, "udp_stream_dropped_packet_count{device=\"%d\",channel=\"%d\",output=\"%d\"}\t%zu\n", i, j, k, sdata->dropped_packet_count);
            }
        }
    }
    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        for (int k = 0; k < mixer->channel.output_count; k++) {
            if (mixer->channel.outputs[k].type != O_UDP_STREAM)
                continue;
            udp_stream_data* sdata = (udp_stream_data*)mixer->channel.outputs[k].data;
            fprintf(f, "udp_stream_dropped_packet_count{mixer=\"%d\",output=\"%d\"}\t%zu\n", i, k, sdata->dropped_packet_count);
        }
    }
    fprintf(f, "\n");
}

static void output_mixer_remote_send_drops(FILE* f) {
    fprintf(f,
            "# HELP mixer_remote_dropped_packet_count Number of packets dropped by a mixer_remote "
            "output's bounds check or the underlying sendto() call (e.g. because the receiving "
            "instance isn't listening yet).\n"
            "# TYPE mixer_remote_dropped_packet_count counter\n");
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = dev->channels + j;
            for (int k = 0; k < channel->output_count; k++) {
                if (channel->outputs[k].type != O_MIXER_REMOTE)
                    continue;
                mixer_remote_send_data* rdata = (mixer_remote_send_data*)channel->outputs[k].data;
                fprintf(f, "mixer_remote_dropped_packet_count{device=\"%d\",channel=\"%d\",output=\"%d\"}\t%zu\n", i, j, k, rdata->dropped_packet_count);
            }
        }
    }
    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        for (int k = 0; k < mixer->channel.output_count; k++) {
            if (mixer->channel.outputs[k].type != O_MIXER_REMOTE)
                continue;
            mixer_remote_send_data* rdata = (mixer_remote_send_data*)mixer->channel.outputs[k].data;
            fprintf(f, "mixer_remote_dropped_packet_count{mixer=\"%d\",output=\"%d\"}\t%zu\n", i, k, rdata->dropped_packet_count);
        }
    }
    fprintf(f, "\n");
}

static void output_mixer_remote_recv_health(FILE* f) {
    fprintf(f,
            "# HELP mixer_remote_last_packet_time_seconds Unix timestamp of the last packet "
            "received for a remote_inputs route (0 = never). Lets an operator tell a quiet-but-"
            "alive sender apart from one whose process is gone entirely.\n"
            "# TYPE mixer_remote_last_packet_time_seconds gauge\n");
    for (mixer_remote_listener_t* listener : mixer_remote_listeners) {
        for (const mixer_remote_route_t& route : listener->routes) {
            fprintf(f, "mixer_remote_last_packet_time_seconds{listen_path=\"%s\",stream_id=\"%u\"}\t%ld\n", listener->listen_path.c_str(), route.stream_id, (long)route.last_packet_time);
        }
    }
    fprintf(f, "\n");

    fprintf(f,
            "# HELP mixer_remote_route_failure_count Per-remote_inputs-route packet failures: "
            "rate/sample_count mismatches, malformed payloads, and out-of-order/duplicate "
            "sequence numbers.\n"
            "# TYPE mixer_remote_route_failure_count counter\n");
    for (mixer_remote_listener_t* listener : mixer_remote_listeners) {
        for (const mixer_remote_route_t& route : listener->routes) {
            fprintf(f, "mixer_remote_route_failure_count{listen_path=\"%s\",stream_id=\"%u\",reason=\"rate_mismatch\"}\t%zu\n", listener->listen_path.c_str(), route.stream_id,
                    route.rate_mismatch_count);
            fprintf(f, "mixer_remote_route_failure_count{listen_path=\"%s\",stream_id=\"%u\",reason=\"sample_count_mismatch\"}\t%zu\n", listener->listen_path.c_str(), route.stream_id,
                    route.sample_count_mismatch_count);
            fprintf(f, "mixer_remote_route_failure_count{listen_path=\"%s\",stream_id=\"%u\",reason=\"malformed_payload\"}\t%zu\n", listener->listen_path.c_str(), route.stream_id,
                    route.malformed_payload_count);
            fprintf(f, "mixer_remote_route_failure_count{listen_path=\"%s\",stream_id=\"%u\",reason=\"seq_gap\"}\t%zu\n", listener->listen_path.c_str(), route.stream_id, route.seq_gap_count);
            fprintf(f, "mixer_remote_route_failure_count{listen_path=\"%s\",stream_id=\"%u\",reason=\"seq_reorder_or_duplicate\"}\t%zu\n", listener->listen_path.c_str(), route.stream_id,
                    route.seq_reorder_or_duplicate_count);
        }
    }
    fprintf(f, "\n");

    fprintf(f,
            "# HELP mixer_remote_listener_failure_count Per-listener packet failures that "
            "couldn't be attributed to a specific route: rejected sender UID, or a header that "
            "couldn't be trusted enough to even identify a stream_id.\n"
            "# TYPE mixer_remote_listener_failure_count counter\n");
    for (mixer_remote_listener_t* listener : mixer_remote_listeners) {
        fprintf(f, "mixer_remote_listener_failure_count{listen_path=\"%s\",reason=\"rejected_uid\"}\t%zu\n", listener->listen_path.c_str(), listener->rejected_uid_count);
        fprintf(f, "mixer_remote_listener_failure_count{listen_path=\"%s\",reason=\"malformed_header\"}\t%zu\n", listener->listen_path.c_str(), listener->malformed_header_count);
        fprintf(f, "mixer_remote_listener_failure_count{listen_path=\"%s\",reason=\"unknown_stream\"}\t%zu\n", listener->listen_path.c_str(), listener->unknown_stream_count);
    }
    fprintf(f, "\n");
}

#ifdef WITH_PULSEAUDIO
static void output_pulse_health(FILE* f) {
    fprintf(f,
            "# HELP pulse_underflow_count Number of times a PulseAudio output stream underflowed "
            "(server drained the buffer faster than we fed it - normal on squelch closing).\n"
            "# TYPE pulse_underflow_count counter\n");
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = dev->channels + j;
            for (int k = 0; k < channel->output_count; k++) {
                if (channel->outputs[k].type != O_PULSE)
                    continue;
                pulse_data* pdata = (pulse_data*)channel->outputs[k].data;
                fprintf(f, "pulse_underflow_count{device=\"%d\",channel=\"%d\",output=\"%d\"}\t%zu\n", i, j, k, pdata->underflow_count);
            }
        }
    }
    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        for (int k = 0; k < mixer->channel.output_count; k++) {
            if (mixer->channel.outputs[k].type != O_PULSE)
                continue;
            pulse_data* pdata = (pulse_data*)mixer->channel.outputs[k].data;
            fprintf(f, "pulse_underflow_count{mixer=\"%d\",output=\"%d\"}\t%zu\n", i, k, pdata->underflow_count);
        }
    }
    fprintf(f, "\n");

    fprintf(f,
            "# HELP pulse_overflow_count Number of times a PulseAudio output stream overflowed "
            "(we fed data faster than the server drained it).\n"
            "# TYPE pulse_overflow_count counter\n");
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = dev->channels + j;
            for (int k = 0; k < channel->output_count; k++) {
                if (channel->outputs[k].type != O_PULSE)
                    continue;
                pulse_data* pdata = (pulse_data*)channel->outputs[k].data;
                fprintf(f, "pulse_overflow_count{device=\"%d\",channel=\"%d\",output=\"%d\"}\t%zu\n", i, j, k, pdata->overflow_count);
            }
        }
    }
    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        for (int k = 0; k < mixer->channel.output_count; k++) {
            if (mixer->channel.outputs[k].type != O_PULSE)
                continue;
            pulse_data* pdata = (pulse_data*)mixer->channel.outputs[k].data;
            fprintf(f, "pulse_overflow_count{mixer=\"%d\",output=\"%d\"}\t%zu\n", i, k, pdata->overflow_count);
        }
    }
    fprintf(f, "\n");

    fprintf(f,
            "# HELP pulse_disconnect_count Number of times a PulseAudio output stream's write path "
            "(latency check or pa_stream_write()) forced a disconnect.\n"
            "# TYPE pulse_disconnect_count counter\n");
    for (int i = 0; i < device_count; i++) {
        device_t* dev = devices + i;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = dev->channels + j;
            for (int k = 0; k < channel->output_count; k++) {
                if (channel->outputs[k].type != O_PULSE)
                    continue;
                pulse_data* pdata = (pulse_data*)channel->outputs[k].data;
                fprintf(f, "pulse_disconnect_count{device=\"%d\",channel=\"%d\",output=\"%d\"}\t%zu\n", i, j, k, pdata->disconnect_count);
            }
        }
    }
    for (int i = 0; i < mixer_count; i++) {
        mixer_t* mixer = mixers + i;
        for (int k = 0; k < mixer->channel.output_count; k++) {
            if (mixer->channel.outputs[k].type != O_PULSE)
                continue;
            pulse_data* pdata = (pulse_data*)mixer->channel.outputs[k].data;
            fprintf(f, "pulse_disconnect_count{mixer=\"%d\",output=\"%d\"}\t%zu\n", i, k, pdata->disconnect_count);
        }
    }
    fprintf(f, "\n");
}
#endif /* WITH_PULSEAUDIO */

#ifdef WITH_RDIO_SCANNER
static void output_rdio_scanner_health(FILE* f) {
    fprintf(f,
            "# HELP rdio_scanner_queue_drop_count Number of completed transmissions dropped because the "
            "shared upload queue (rdio_scanner_queue_depth) was full. Process-wide: every rdio_scanner-"
            "configured output shares one upload queue and worker thread.\n"
            "# TYPE rdio_scanner_queue_drop_count counter\n"
            "rdio_scanner_queue_drop_count\t%zu\n\n",
            rdio_scanner_queue_drop_count.load());

    fprintf(f,
            "# HELP rdio_scanner_upload_failure_count Number of uploads that failed after exhausting "
            "max_retries. Process-wide, same reason as rdio_scanner_queue_drop_count.\n"
            "# TYPE rdio_scanner_upload_failure_count counter\n"
            "rdio_scanner_upload_failure_count\t%zu\n\n",
            rdio_scanner_upload_failure_count.load());
}
#endif /* WITH_RDIO_SCANNER */

void write_stats_file(timeval* last_stats_write) {
    if (!stats_filepath) {
        return;
    }

    timeval current_time;
    gettimeofday(&current_time, NULL);

    if (!do_exit && delta_sec(last_stats_write, &current_time) < stats_update_interval) {
        return;
    }

    *last_stats_write = current_time;

    FILE* file = fopen(stats_filepath, "w");
    if (!file) {
        log(LOG_WARNING, "Cannot open output file %s (%s)\n", stats_filepath, strerror(errno));
        return;
    }

    output_channel_activity_counters(file);
    output_channel_freq_offsets(file);
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
    output_device_buffer_underruns(file);
    output_device_centerfreq_retune_failures(file);
    output_device_auto_ppm(file);
    output_device_peak_levels(file);
    output_device_clipped_samples(file);
    output_output_overruns(file);
    output_input_overruns(file);
    output_process_cpu_seconds(file);
    output_icecast_health(file);
    output_lame_encode_failures(file);
    output_file_write_failures(file);
    output_udp_stream_drops(file);
    output_mixer_remote_send_drops(file);
    output_mixer_remote_recv_health(file);
#ifdef WITH_PULSEAUDIO
    output_pulse_health(file);
#endif /* WITH_PULSEAUDIO */
#ifdef WITH_RDIO_SCANNER
    output_rdio_scanner_health(file);
#endif /* WITH_RDIO_SCANNER */

    fclose(file);
}

// send_tx_tags content for a device channel: on-air label (or frequency fallback) while
// squelch is open, "" otherwise.
static std::string channel_tx_tag(channel_t* channel) {
    freq_t& fparms = channel->freqlist[channel->freq_idx];
    return compute_tx_tag_content(channel->axcindicate != NO_SIGNAL, fparms.label, fparms.frequency);
}

// send_tx_tags content for a mixer channel: the label of whichever configured input is
// currently talking (mixer_select_active_tag_input()'s lowest-index-wins tie-break), or ""
// if none are.
static std::string mixer_tx_tag(mixer_t* mixer) {
    int idx = mixer_select_active_tag_input(mixer->inputs, mixer->input_count);
    if (idx < 0) {
        return compute_tx_tag_content(false, NULL, 0);
    }
    mixinput_t& input = mixer->inputs[idx];
    if (input.source_device_idx < 0 || input.source_channel_idx < 0) {
        // Always true for a remote_inputs-connected slot (there is no local channel to look
        // up) - fall back to its configured remote_label, if any. compute_tx_tag_content()
        // ignores frequency_hz whenever label is non-NULL, so passing 0 here is harmless; a
        // remote source's frequency isn't known on this side anyway.
        if (input.remote_label != NULL) {
            return compute_tx_tag_content(true, input.remote_label, 0);
        }
        return compute_tx_tag_content(false, NULL, 0);
    }
    channel_t& src = devices[input.source_device_idx].channels[input.source_channel_idx];
    freq_t& fparms = src.freqlist[src.freq_idx];
    return compute_tx_tag_content(true, fparms.label, fparms.frequency);
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
            // Consume any pending dynamic_reload enable/disable request before the enabled
            // check below - this thread owns mixers[i]'s outputs, so applying the change here
            // (rather than directly on the control socket thread) is what keeps it from racing
            // this same thread's concurrent process_outputs() calls on the same output_t structs.
            int pending = mixers[i].pending_enable_request.exchange(-1, std::memory_order_acq_rel);
            if (pending == 1) {
                mixer_enable(&mixers[i]);
            } else if (pending == 0) {
                mixer_disable(&mixers[i]);
            }
            if (mixers[i].enabled == false)
                continue;
            channel_t* channel = &mixers[i].channel;
            if (channel->state == CH_READY) {
                process_outputs(channel, -1, mixer_tx_tag(&mixers[i]));
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
            if (dev->waveavail) {
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
                    // Checked before the enable/disable request below, and - unlike that one -
                    // not consumed via exchange(): channel_teardown_for_removal() frees this
                    // channel's LAME encoder, and the caller waiting on this request
                    // (try_remove_channels(), live_reconfig.cpp) decrements dev->channel_count as
                    // soon as it observes completion, so the field must not signal "done" until
                    // the teardown genuinely is - see channel_t::pending_remove_request's comment
                    // (rtl_airband.h). Skips straight to the next channel: this one's outputs are
                    // being torn down, not processed, for the rest of this pass.
                    if (channel->pending_remove_request.load(std::memory_order_acquire) == 1) {
                        channel_teardown_for_removal(channel);
                        channel->pending_remove_request.store(-1, std::memory_order_release);
                        continue;
                    }
                    // Consume any pending dynamic_reload enable/disable request before
                    // process_outputs() - this thread owns this channel's outputs, so applying
                    // the change here (rather than on the control socket thread) is what keeps
                    // it from racing this same thread's concurrent process_outputs() calls.
                    int pending = channel->pending_enable_request.exchange(-1, std::memory_order_acq_rel);
                    if (pending == 1) {
                        channel_apply_enable(channel);
                    } else if (pending == 0) {
                        channel_apply_disable(channel);
                    }
                    process_outputs(channel, new_freq, channel_tx_tag(channel));
                    memcpy(channel->waveout, channel->waveout + WAVE_BATCH, AGC_EXTRA * sizeof(float));
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
        // Frees whatever channel_teardown_for_removal() (live_reconfig.cpp) queued at least
        // RECLAIM_GRACE_PERIOD_SEC ago - see that function's comment for why the actual free is
        // deferred this long instead of running inline during teardown. Cheap no-op on every pass
        // where nothing is queued, which is the common case.
        reclaim_pending_channel_frees();
    }

    // waveavail=1 set by the demod just before do_exit may have been missed if
    // the final pthread_cond_signal fired while the output thread was busy (signal
    // lost) or if do_exit was checked before the signal was consumed.
    for (int i = output_param->device_start; i < output_param->device_end; i++) {
        device_t* dev = devices + i;
        if (!dev->waveavail)
            continue;
        for (int j = 0; j < dev->channel_count; j++) {
            channel_t* channel = devices[i].channels + j;
            process_outputs(channel, -1, channel_tx_tag(channel));
            memcpy(channel->waveout, channel->waveout + WAVE_BATCH, AGC_EXTRA * sizeof(float));
        }
        dev->waveavail = 0;
    }

    // Final stats flush: do_exit=1 bypasses the stats_update_interval throttle, so this always
    // writes regardless of run duration. Called after the post-loop flush above
    // so counters reflect all processed output.
    if (output_param->device_start == 0) {
        write_stats_file(&last_stats_write);
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
                if (!dev->channels[j].enabled) {
                    // Live-disabled via the control socket - disable_channel_outputs() already
                    // tore down its connections; don't reconnect them behind that command's back.
                    continue;
                }
                for (int k = 0; k < dev->channels[j].output_count; k++) {
                    if (dev->channels[j].outputs[k].type == O_ICECAST) {
                        icecast_data* icecast = (icecast_data*)(dev->channels[j].outputs[k].data);
                        if (dev->input->state == INPUT_FAILED) {
                            if (icecast->shout) {
                                log(LOG_WARNING, "Device #%d failed, disconnecting stream %s:%d/%s\n", i, icecast->hostname, icecast->port, icecast->mountpoint);
                                icecast->disconnect_count++;
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
