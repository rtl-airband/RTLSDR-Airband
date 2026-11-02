/*
 * config.cpp
 * Configuration parsing routines
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

#include <assert.h>
#include <stdint.h>  // uint32_t
#include <syslog.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <libconfig.h++>
#include <sstream>
#include "auto_ppm.h"
#include "helper_functions.h"
#include "input-common.h"   // input_t
#include "live_reconfig.h"  // compute_channel_bin, compute_channel_dm_dphi
#include "mixer_remote.h"
#include "rtl_airband.h"

using namespace std;

// report a fatal config error against one output and exit
static void output_error(int i, int j, int o, bool parsing_mixers, const string& msg) {
    if (parsing_mixers) {
        cerr << "Configuration error: mixers.[" << i << "] outputs.[" << o << "]: ";
    } else {
        cerr << "Configuration error: devices.[" << i << "] channels.[" << j << "] outputs.[" << o << "]: ";
    }
    cerr << msg << "\n";
    error();
}

// resolve per-output split file time and naming settings, falling back to the global values.
// must be called after fdata->split_on_transmission is set
static void parse_split_file_times(libconfig::Setting& out, file_data* fdata, int i, int j, int o, bool parsing_mixers) {
    // the split_* time settings only take effect with split_on_transmission
    if (out.exists("split_min_file_time") || out.exists("split_max_file_time") || out.exists("split_max_idle_time")) {
        // mixers never split per transmission, log error and exit
        if (parsing_mixers) {
            output_error(i, j, o, parsing_mixers, "split_*_time settings are not allowed for mixers");
        }

        // if split_on_transmission is not set, log warnings but continue
        if (!fdata->split_on_transmission) {
            log(LOG_WARNING, "Warning: devices.[%d] channels.[%d] outputs.[%d]: split_*_time settings are ignored without split_on_transmission\n", i, j, o);
        }
    }

    // the split_* name settings only take effect with split_on_transmission
    if (out.exists("split_include_transmission_start")) {
        if (parsing_mixers) {
            output_error(i, j, o, parsing_mixers, "split_include_transmission_start is not allowed for mixers");
        }
        if (!fdata->split_on_transmission) {
            log(LOG_WARNING, "Warning: devices.[%d] channels.[%d] outputs.[%d]: split_include_transmission_start is ignored without split_on_transmission\n", i, j, o);
        }
    }

    // parse out split_* time settings
    if (!setting_as_double_or(out, "split_min_file_time", global_split_min_file_time, &fdata->split_min_file_time)) {
        output_error(i, j, o, parsing_mixers, "split_min_file_time must be a number");
    }
    if (!setting_as_double_or(out, "split_max_file_time", global_split_max_file_time, &fdata->split_max_file_time)) {
        output_error(i, j, o, parsing_mixers, "split_max_file_time must be a number");
    }
    if (!setting_as_double_or(out, "split_max_idle_time", global_split_max_idle_time, &fdata->split_max_idle_time)) {
        output_error(i, j, o, parsing_mixers, "split_max_idle_time must be a number");
    }

    // validate split_* time settings
    if (!valid_split_file_times(fdata->split_min_file_time, fdata->split_max_file_time, fdata->split_max_idle_time)) {
        output_error(i, j, o, parsing_mixers, split_file_times_constraint);
    }

    // parse out split_* name settings
    if (!setting_as_bool_or(out, "split_include_transmission_start", global_split_include_transmission_start, &fdata->split_include_transmission_start)) {
        output_error(i, j, o, parsing_mixers, "split_include_transmission_start must be a boolean");
    }
    // if split_on_transmission is false then split_include_transmission_start gets overridden to false. even a valid
    // config can have global_split_include_transmission_start true but this file may not have split_on_transmission
    fdata->split_include_transmission_start = fdata->split_include_transmission_start && fdata->split_on_transmission;
}

// dev is non-null only when called from parse_channels() (a device's own channel outputs);
// parse_mixers() passes NULL, which is safe because parsing_mixers==true already rejects the
// "mixer" output type below - the only branch that dereferences dev.
static int parse_outputs(libconfig::Setting& outs, channel_t* channel, int i, int j, bool parsing_mixers, rec_modes dev_mode, device_t* dev) {
    int oo = 0;
    for (int o = 0; o < channel->output_count; o++) {
        channel->outputs[oo].has_mp3_output = false;
        channel->outputs[oo].lame = NULL;
        channel->outputs[oo].lamebuf = NULL;

        if (outs[o].exists("disable") && (bool)outs[o]["disable"] == true) {
            continue;
        }
        if (!strncmp(outs[o]["type"], "icecast", 7)) {
            // new(), not XCALLOC: icecast_data now has std::string/non-POD members
            // (icecast_tx_tag_state), which calloc'd memory would never construct
            channel->outputs[oo].data = new icecast_data();
            channel->outputs[oo].type = O_ICECAST;
            icecast_data* idata = (icecast_data*)(channel->outputs[oo].data);
            idata->hostname = strdup(outs[o]["server"]);
            idata->port = outs[o]["port"];
            idata->mountpoint = strdup(outs[o]["mountpoint"]);
            idata->username = strdup(outs[o]["username"]);
            idata->password = strdup(outs[o]["password"]);
            if (outs[o].exists("name"))
                idata->name = strdup(outs[o]["name"]);
            if (outs[o].exists("genre"))
                idata->genre = strdup(outs[o]["genre"]);
            if (outs[o].exists("description"))
                idata->description = strdup(outs[o]["description"]);
            if (outs[o].exists("send_scan_freq_tags"))
                idata->send_scan_freq_tags = (bool)outs[o]["send_scan_freq_tags"];
            else
                idata->send_scan_freq_tags = 0;
            if (outs[o].exists("send_tx_tags"))
                idata->send_tx_tags = (bool)outs[o]["send_tx_tags"];
            else
                idata->send_tx_tags = false;
            if (idata->send_tx_tags && dev_mode == R_SCAN) {
                output_error(i, j, o, parsing_mixers,
                             "send_tx_tags is not supported on R_SCAN (scan mode) channels - the frequency itself changes "
                             "at runtime there; use send_scan_freq_tags instead");
            }
#ifdef LIBSHOUT_HAS_TLS
            idata->tls_mode = SHOUT_TLS_DISABLED;
#endif /* LIBSHOUT_HAS_TLS */
            if (outs[o].exists("tls")) {
#ifdef LIBSHOUT_HAS_TLS
                if (outs[o]["tls"].getType() == libconfig::Setting::TypeString) {
                    if (!strcmp(outs[o]["tls"], "auto")) {
                        idata->tls_mode = SHOUT_TLS_AUTO;
                    } else if (!strcmp(outs[o]["tls"], "auto_no_plain")) {
                        idata->tls_mode = SHOUT_TLS_AUTO_NO_PLAIN;
                    } else if (!strcmp(outs[o]["tls"], "transport")) {
                        idata->tls_mode = SHOUT_TLS_RFC2818;
                    } else if (!strcmp(outs[o]["tls"], "upgrade")) {
                        idata->tls_mode = SHOUT_TLS_RFC2817;
                    } else if (!strcmp(outs[o]["tls"], "disabled")) {
                        idata->tls_mode = SHOUT_TLS_DISABLED;
                    } else {
                        output_error(i, j, o, parsing_mixers, "invalid value for tls; must be one of: auto, auto_no_plain, transport, upgrade, disabled");
                    }
                } else {
                    output_error(i, j, o, parsing_mixers, "tls value must be a string");
                }
#else
                output_error(i, j, o, parsing_mixers, "tls is not supported: rtl_airband was built against a libshout without TLS support");
#endif /* LIBSHOUT_HAS_TLS */
            }

            channel->outputs[oo].has_mp3_output = true;
        } else if (!strncmp(outs[o]["type"], "file", 4)) {
            file_data* fdata = new file_data();
            channel->outputs[oo].data = fdata;
            channel->outputs[oo].type = O_FILE;

            fdata->type = O_FILE;
            if (!outs[o].exists("directory") || !outs[o].exists("filename_template")) {
                output_error(i, j, o, parsing_mixers, "both directory and filename_template required for file");
            }
            fdata->basedir = outs[o]["directory"].c_str();
            fdata->basename = outs[o]["filename_template"].c_str();
            fdata->dated_subdirectories = outs[o].exists("dated_subdirectories") ? (bool)(outs[o]["dated_subdirectories"]) : false;
            fdata->suffix = ".mp3";

            fdata->continuous = outs[o].exists("continuous") ? (bool)(outs[o]["continuous"]) : false;
            fdata->append = (!outs[o].exists("append")) || (bool)(outs[o]["append"]);
            fdata->split_on_transmission = outs[o].exists("split_on_transmission") ? (bool)(outs[o]["split_on_transmission"]) : false;
            fdata->include_freq = outs[o].exists("include_freq") ? (bool)(outs[o]["include_freq"]) : false;
            parse_split_file_times(outs[o], fdata, i, j, o, parsing_mixers);
            if (fdata->split_on_transmission) {
                fdata->min_rx_seconds = outs[o].exists("min_rx_seconds") ? (double)(outs[o]["min_rx_seconds"]) : 0.0;
                if (outs[o].exists("post_write_script")) {
                    fdata->post_write_script = outs[o]["post_write_script"].c_str();
                }
#ifdef WITH_RDIO_SCANNER
                if (outs[o].exists("rdio_scanner")) {
                    libconfig::Setting& rs = outs[o]["rdio_scanner"];
                    if (!rs.exists("server") || !rs.exists("port") || !rs.exists("api_key") || !rs.exists("talkgroup_id")) {
                        output_error(i, j, o, parsing_mixers, "rdio_scanner requires server, port, api_key, and talkgroup_id");
                    }
                    if (dev_mode == R_SCAN) {
                        output_error(i, j, o, parsing_mixers,
                                     "rdio_scanner is not supported on R_SCAN (scan mode) channels - talkgroup_id and the other "
                                     "per-channel fields are fixed at config time and can't track the frequency currently being scanned");
                    }
                    rdio_scanner_data* rsdata = new rdio_scanner_data();
                    rsdata->server = rs["server"].c_str();
                    rsdata->port = (int)rs["port"];
                    rsdata->use_tls = rs.exists("use_tls") ? (bool)rs["use_tls"] : false;
                    rsdata->api_key = rs["api_key"].c_str();
                    rsdata->talkgroup_id = (int)rs["talkgroup_id"];
                    rsdata->system_id = rs.exists("system_id") ? (int)rs["system_id"] : -1;
                    rsdata->system_label = rs.exists("system_label") ? rs["system_label"].c_str() : "";
                    rsdata->talkgroup_label = rs.exists("talkgroup_label") ? rs["talkgroup_label"].c_str() : "";
                    rsdata->talkgroup_tag = rs.exists("talkgroup_tag") ? rs["talkgroup_tag"].c_str() : "";
                    rsdata->talkgroup_group = rs.exists("talkgroup_group") ? rs["talkgroup_group"].c_str() : "";
                    rsdata->source_id = rs.exists("source_id") ? (int)rs["source_id"] : 0;
                    rsdata->delete_after_upload = rs.exists("delete_after_upload") ? (bool)rs["delete_after_upload"] : false;
                    rsdata->timeout_ms = rs.exists("timeout_ms") ? (long)(int)rs["timeout_ms"] : 5000;
                    if (rsdata->timeout_ms <= 0) {
                        output_error(i, j, o, parsing_mixers, "rdio_scanner timeout_ms must be greater than 0 (libcurl treats 0 as no timeout)");
                    }
                    rsdata->max_retries = rs.exists("max_retries") ? (int)rs["max_retries"] : 2;
                    if (rsdata->max_retries < 0) {
                        output_error(i, j, o, parsing_mixers, "rdio_scanner max_retries must not be negative");
                    }
                    fdata->rdio_scanner = rsdata;
                    rdio_scanner_enabled = true;
                }
#else
                if (outs[o].exists("rdio_scanner")) {
                    output_error(i, j, o, parsing_mixers, "rdio_scanner support was not compiled in (build with -DRDIO_SCANNER=ON)");
                }
#endif /* WITH_RDIO_SCANNER */
            } else {
                if (outs[o].exists("min_rx_seconds") || outs[o].exists("post_write_script") || outs[o].exists("rdio_scanner")) {
                    output_error(i, j, o, parsing_mixers, "min_rx_seconds, post_write_script, and rdio_scanner require split_on_transmission");
                }
            }

            channel->outputs[oo].has_mp3_output = true;

            if (fdata->split_on_transmission) {
                if (parsing_mixers) {
                    output_error(i, j, o, parsing_mixers, "split_on_transmission is not allowed for mixers");
                }
                if (fdata->continuous) {
                    output_error(i, j, o, parsing_mixers, "can't have both continuous and split_on_transmission");
                }
            }

        } else if (!strncmp(outs[o]["type"], "rawfile", 7)) {
            if (parsing_mixers) {  // rawfile outputs not allowed for mixers
                output_error(i, j, o, parsing_mixers, "rawfile output is not allowed for mixers");
            }
            file_data* fdata = new file_data();
            channel->outputs[oo].data = fdata;
            channel->outputs[oo].type = O_RAWFILE;

            fdata->type = O_RAWFILE;
            if (!outs[o].exists("directory") || !outs[o].exists("filename_template")) {
                output_error(i, j, o, parsing_mixers, "both directory and filename_template required for file");
            }

            fdata->basedir = outs[o]["directory"].c_str();
            fdata->basename = outs[o]["filename_template"].c_str();
            fdata->dated_subdirectories = outs[o].exists("dated_subdirectories") ? (bool)(outs[o]["dated_subdirectories"]) : false;
            fdata->suffix = ".cf32";

            fdata->continuous = outs[o].exists("continuous") ? (bool)(outs[o]["continuous"]) : false;
            fdata->append = (!outs[o].exists("append")) || (bool)(outs[o]["append"]);
            fdata->split_on_transmission = outs[o].exists("split_on_transmission") ? (bool)(outs[o]["split_on_transmission"]) : false;
            fdata->include_freq = outs[o].exists("include_freq") ? (bool)(outs[o]["include_freq"]) : false;
            parse_split_file_times(outs[o], fdata, i, j, o, parsing_mixers);
            fdata->min_rx_seconds = 0.0;
            fdata->post_write_script.clear();
            channel->needs_raw_iq = channel->has_iq_outputs = 1;

            if (fdata->continuous && fdata->split_on_transmission) {
                output_error(i, j, o, parsing_mixers, "can't have both continuous and split_on_transmission");
            }
        } else if (!strncmp(outs[o]["type"], "mixer_remote", 12)) {
            // Checked before the "mixer" branch below: "mixer_remote" also starts with the
            // 5-char prefix that branch's strncmp() matches on, so it must be checked first or
            // it would silently be swallowed by the "mixer" branch instead.
            channel->outputs[oo].data = XCALLOC(1, sizeof(struct mixer_remote_send_data));
            channel->outputs[oo].type = O_MIXER_REMOTE;
            mixer_remote_send_data* rdata = (mixer_remote_send_data*)(channel->outputs[oo].data);

            if (!outs[o].exists("dest_path")) {
                output_error(i, j, o, parsing_mixers, "missing dest_path");
            }
            rdata->dest_path = strdup(outs[o]["dest_path"]);

            if (!outs[o].exists("stream_id")) {
                output_error(i, j, o, parsing_mixers, "missing stream_id");
            }
            int stream_id = (int)outs[o]["stream_id"];
            if (stream_id < 0) {
                output_error(i, j, o, parsing_mixers, "stream_id must not be negative");
            }
            rdata->stream_id = (uint32_t)stream_id;
        } else if (!strncmp(outs[o]["type"], "mixer", 5)) {
            if (parsing_mixers) {  // mixer outputs not allowed for mixers
                output_error(i, j, o, parsing_mixers, "mixer output is not allowed for mixers");
            }
            channel->outputs[oo].data = XCALLOC(1, sizeof(struct mixer_data));
            channel->outputs[oo].type = O_MIXER;
            mixer_data* mdata = (mixer_data*)(channel->outputs[oo].data);
            const char* name = (const char*)outs[o]["name"];
            if ((mdata->mixer = getmixerbyname(name)) == NULL) {
                output_error(i, j, o, parsing_mixers, "unknown mixer \"" + string(name) + "\"");
            }
            float ampfactor = outs[o].exists("ampfactor") ? (float)outs[o]["ampfactor"] : 1.0f;
            float balance = outs[o].exists("balance") ? (float)outs[o]["balance"] : 0.0f;
            if (balance < -1.0f || balance > 1.0f) {
                output_error(i, j, o, parsing_mixers, "balance out of allowed range <-1.0;1.0>");
            }
            if ((mdata->input = mixer_connect_input(mdata->mixer, ampfactor, balance)) < 0) {
                output_error(i, j, o, parsing_mixers, "could not connect to mixer " + string(name) + ": " + mixer_get_error());
            }
            // record where this input's audio comes from so send_tx_tags on the mixer's own
            // icecast output(s) can look up the source channel's live label/freq_idx later.
            // Store indices, not a channel_t* - dev->channels is still XREALLOC'd (by the
            // caller, after parse_channels() returns) which can move the block, but the
            // compaction index computed here is stable across that realloc; devices itself
            // is never reallocated after its one-time XCALLOC in rtl_airband.cpp.
            assert(dev != NULL);  // guaranteed: parsing_mixers rejects "mixer" outputs above
            mdata->mixer->inputs[mdata->input].source_device_idx = (int)(dev - devices);
            mdata->mixer->inputs[mdata->input].source_channel_idx = (int)(channel - dev->channels);
            debug_print("dev[%d].chan[%d].out[%d] connected to mixer %s as input %d (ampfactor=%.1f balance=%.1f)\n", i, j, o, name, mdata->input, ampfactor, balance);
        } else if (!strncmp(outs[o]["type"], "udp_stream", 6)) {
            channel->outputs[oo].data = XCALLOC(1, sizeof(struct udp_stream_data));
            channel->outputs[oo].type = O_UDP_STREAM;

            udp_stream_data* sdata = (udp_stream_data*)channel->outputs[oo].data;

            sdata->continuous = outs[o].exists("continuous") ? (bool)(outs[o]["continuous"]) : false;

            if (outs[o].exists("dest_address")) {
                sdata->dest_address = strdup(outs[o]["dest_address"]);
            } else {
                output_error(i, j, o, parsing_mixers, "missing dest_address");
            }

            if (outs[o].exists("dest_port")) {
                if (outs[o]["dest_port"].getType() == libconfig::Setting::TypeInt) {
                    char buffer[12];
                    sprintf(buffer, "%d", (int)outs[o]["dest_port"]);
                    sdata->dest_port = strdup(buffer);
                } else {
                    sdata->dest_port = strdup(outs[o]["dest_port"]);
                }
            } else {
                output_error(i, j, o, parsing_mixers, "missing dest_port");
            }

            sdata->format = STREAM_FORMAT_FLOAT32;
            if (outs[o].exists("bit_depth")) {
                int bit_depth = (int)outs[o]["bit_depth"];
                if (bit_depth == 32) {
                    sdata->format = STREAM_FORMAT_FLOAT32;
                } else if (bit_depth == 16) {
                    sdata->format = STREAM_FORMAT_S16LE;
                } else if (bit_depth == 8) {
                    sdata->format = STREAM_FORMAT_S8;
                } else {
                    output_error(i, j, o, parsing_mixers, "invalid value for bit_depth (must be one of: 32, 16, 8)");
                }
            }

            sdata->sample_rate = WAVE_RATE;
            if (outs[o].exists("sample_rate")) {
                sdata->sample_rate = (int)outs[o]["sample_rate"];
                if (sdata->sample_rate <= 0) {
                    output_error(i, j, o, parsing_mixers, "sample_rate must be greater than 0");
                }
            }
#ifdef WITH_PULSEAUDIO
        } else if (!strncmp(outs[o]["type"], "pulse", 5)) {
            channel->outputs[oo].data = XCALLOC(1, sizeof(struct pulse_data));
            channel->outputs[oo].type = O_PULSE;

            pulse_data* pdata = (pulse_data*)(channel->outputs[oo].data);
            pdata->continuous = outs[o].exists("continuous") ? (bool)(outs[o]["continuous"]) : false;
            pdata->server = outs[o].exists("server") ? strdup(outs[o]["server"]) : NULL;
            pdata->name = outs[o].exists("name") ? strdup(outs[o]["name"]) : "rtl_airband";
            pdata->sink = outs[o].exists("sink") ? strdup(outs[o]["sink"]) : NULL;

            if (outs[o].exists("stream_name")) {
                pdata->stream_name = strdup(outs[o]["stream_name"]);
            } else {
                if (parsing_mixers) {
                    output_error(i, j, o, parsing_mixers, "PulseAudio outputs of mixers must have stream_name defined");
                }
                char buf[1024];
                snprintf(buf, sizeof(buf), "%.3f MHz", (float)channel->freqlist[0].frequency / 1000000.0f);
                pdata->stream_name = strdup(buf);
            }
#endif /* WITH_PULSEAUDIO */
        } else {
            output_error(i, j, o, parsing_mixers, "unknown output type");
        }
        channel->outputs[oo].enabled = true;
        channel->outputs[oo].active = false;
        oo++;
    }
    return oo;
}

static struct freq_t* mk_freqlist(int n) {
    if (n < 1) {
        cerr << "mk_freqlist: invalid list length " << n << "\n";
        error();
    }
    struct freq_t* fl = (struct freq_t*)XCALLOC(n, sizeof(struct freq_t));
    for (int i = 0; i < n; i++) {
        fl[i].frequency = 0;
        fl[i].label = NULL;
        fl[i].agcavgfast = 0.5f;
        fl[i].ampfactor = 1.0f;
        fl[i].squelch = Squelch();
        fl[i].active_counter = 0;
        fl[i].modulation = MOD_AM;
    }
    return fl;
}

static void warn_if_freq_not_in_range(int devidx, int chanidx, int freq, int centerfreq, int sample_rate) {
    static const float soft_bw_threshold = 0.9f;
    float bw_limit = (float)sample_rate / 2.f * soft_bw_threshold;
    if ((float)abs(freq - centerfreq) >= bw_limit) {
        log(LOG_WARNING, "Warning: dev[%d].channel[%d]: frequency %.3f MHz is outside of SDR operating bandwidth (%.3f-%.3f MHz)\n", devidx, chanidx, (double)freq / 1e6,
            (double)(centerfreq - bw_limit) / 1e6, (double)(centerfreq + bw_limit) / 1e6);
    }
}

static int parse_anynum2int(libconfig::Setting& f) {
    int ret = 0;
    if (f.getType() == libconfig::Setting::TypeInt) {
        ret = (int)f;
    } else if (f.getType() == libconfig::Setting::TypeFloat) {
        ret = (int)((double)f * 1e6);
    } else if (f.getType() == libconfig::Setting::TypeString) {
        char* s = strdup((char const*)f);
        ret = (int)atofs(s);
        free(s);
    }
    return ret;
}

// Parses one channel definition from chan_setting into *channel (dev->channels[chan_idx]),
// including its outputs, and writes dev->bins[chan_idx]/dev->base_bins[chan_idx]. Shared by the
// startup parse_channels() loop below and by the dynamic_reload live channel-append path
// (compute_and_apply_diff(), live_reconfig.cpp) - reusing this exact function is what keeps the
// two paths from drifting apart. chan_idx is used both to target the array slot and for error
// message / debug filename text, so appended-channel error messages report the channel's
// position in dev->channels rather than its raw position in the config file's channel list.
// Returns false in the same two pre-existing legacy-value-quirk cases the old inline
// parse_channels() body silently `continue`d past its own channel entirely (single-value
// squelch_snr_threshold == -1, single-value bandwidth == 0 - see the two "disable" comments
// below): the caller must NOT count this channel (channel_count/jj isn't advanced) if this
// returns false, matching that pre-existing behavior byte-for-byte. Not something introduced or
// fixed by this refactor - flagged separately, since it looks like a genuine latent bug.
// Recursively renders an arbitrary libconfig::Setting subtree (scalar, group, list, or array)
// into a canonical, order-preserving string. Used only by build_channel_identity_signature()
// below - kept file-local since nothing else needs a generic config-subtree serializer.
static string serialize_setting(const libconfig::Setting& s) {
    switch (s.getType()) {
        case libconfig::Setting::TypeInt:
            return to_string((int)s);
        case libconfig::Setting::TypeInt64:
            return to_string((long long)s);
        case libconfig::Setting::TypeFloat: {
            ostringstream o;
            o << (double)s;
            return o.str();
        }
        case libconfig::Setting::TypeString:
            return string("\"") + (const char*)s + "\"";
        case libconfig::Setting::TypeBoolean:
            return (bool)s ? "true" : "false";
        case libconfig::Setting::TypeGroup:
        case libconfig::Setting::TypeArray:
        case libconfig::Setting::TypeList: {
            string out = "{";
            for (int i = 0; i < s.getLength(); i++) {
                const libconfig::Setting& child = s[i];
                const char* name = child.getName();
                if (name != nullptr) {
                    out += name;
                    out += "=";
                }
                out += serialize_setting(child);
                out += ";";
            }
            out += "}";
            return out;
        }
        default:
            return "?";
    }
}

string build_channel_identity_signature(const libconfig::Setting& chan_setting) {
    string out = "{";
    for (int i = 0; i < chan_setting.getLength(); i++) {
        const libconfig::Setting& child = chan_setting[i];
        const char* name = child.getName();
        // Diffed/applied separately via a cheap flag flip (channel_request_enable/disable,
        // live_reconfig.cpp) with no teardown - excluded here so toggling just "enabled" stays on
        // that fast path instead of spuriously triggering a full tear-down-and-replace.
        if (name != nullptr && !strcmp(name, "enabled")) {
            continue;
        }
        if (name != nullptr) {
            out += name;
            out += "=";
        }
        out += serialize_setting(child);
        out += ";";
    }
    out += "}";
    return out;
}

string build_mixer_remote_inputs_signature(const libconfig::Setting& mixer_setting) {
    // Distinct from "{}" (an empty-but-present remote_inputs list, a legal if pointless config)
    // so a mixer gaining or losing its remote_inputs block entirely is also detected as changed.
    if (!mixer_setting.exists("remote_inputs")) {
        return "<absent>";
    }
    return serialize_setting(mixer_setting["remote_inputs"]);
}

bool parse_channel(libconfig::Setting& chan_setting, device_t* dev, int dev_idx, int chan_idx, channel_t* channel) {
    for (int k = 0; k < AGC_EXTRA; k++) {
        channel->wavein[k] = 20;
        channel->waveout[k] = 0.5;
    }
    channel->axcindicate = NO_SIGNAL;
    channel->mode = MM_MONO;
    channel->freq_count = 1;
    channel->freq_idx = 0;
    // "enabled" is distinct from "disable" above: "disable" skips this config entry entirely
    // at parse time (no array slot allocated). "enabled" still allocates everything below
    // (bins, dm_dphi, outputs) but starts the channel skipped by the hot loops, so the
    // dynamic_reload control socket can flip it on live without any array resize.
    channel->enabled = chan_setting.exists("enabled") ? (bool)chan_setting["enabled"] : true;
    channel->pending_enable_request = -1;
    channel->pending_remove_request = -1;
    channel->removed = false;
    channel->highpass = chan_setting.exists("highpass") ? (int)chan_setting["highpass"] : 100;
    channel->lowpass = chan_setting.exists("lowpass") ? (int)chan_setting["lowpass"] : 2500;
#ifdef NFM
    channel->pr = 0;
    channel->pj = 0;
    channel->prev_waveout = 0.5;
    channel->alpha = dev->alpha;
#endif /* NFM */

    // Make sure lowpass / highpass aren't flipped.
    // If lowpass is enabled (greater than zero) it must be larger than highpass
    if (channel->lowpass > 0 && channel->lowpass < channel->highpass) {
        cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: lowpass (" << channel->lowpass << ") must be greater than or equal to highpass (" << channel->highpass
             << ")\n";
        error();
    }

    modulations channel_modulation = MOD_AM;
    if (chan_setting.exists("modulation")) {
#ifdef NFM
        if (strncmp(chan_setting["modulation"], "nfm", 3) == 0) {
            channel_modulation = MOD_NFM;
        } else
#endif /* NFM */
            if (strncmp(chan_setting["modulation"], "am", 2) != 0) {
                cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: unknown modulation\n";
                error();
            }
    }
    channel->afc = chan_setting.exists("afc") ? (unsigned char)(unsigned int)chan_setting["afc"] : 0;
    if (dev->mode == R_MULTICHANNEL) {
        channel->freqlist = mk_freqlist(1);
        channel->freqlist[0].frequency = parse_anynum2int(chan_setting["freq"]);
        warn_if_freq_not_in_range(dev_idx, chan_idx, channel->freqlist[0].frequency, dev->input->centerfreq, dev->input->sample_rate);
        if (chan_setting.exists("label")) {
            channel->freqlist[0].label = strdup(chan_setting["label"]);
        }
        channel->freqlist[0].modulation = channel_modulation;
    } else { /* R_SCAN */
        channel->freq_count = chan_setting["freqs"].getLength();
        if (channel->freq_count < 1) {
            cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: freqs should be a list with at least one element\n";
            error();
        }
        channel->freqlist = mk_freqlist(channel->freq_count);
        if (chan_setting.exists("labels") && chan_setting["labels"].getLength() < channel->freq_count) {
            cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: labels should be a list with at least " << channel->freq_count << " elements\n";
            error();
        }
        if (chan_setting.exists("squelch_threshold") && libconfig::Setting::TypeList == chan_setting["squelch_threshold"].getType() &&
            chan_setting["squelch_threshold"].getLength() < channel->freq_count) {
            cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: squelch_threshold should be an int or a list of ints with at least " << channel->freq_count
                 << " elements\n";
            error();
        }
        if (chan_setting.exists("squelch_snr_threshold") && libconfig::Setting::TypeList == chan_setting["squelch_snr_threshold"].getType() &&
            chan_setting["squelch_snr_threshold"].getLength() < channel->freq_count) {
            cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx
                 << "]: squelch_snr_threshold should be an int, a float or a list of "
                    "ints or floats with at least "
                 << channel->freq_count << " elements\n";
            error();
        }
        if (chan_setting.exists("notch") && libconfig::Setting::TypeList == chan_setting["notch"].getType() && chan_setting["notch"].getLength() < channel->freq_count) {
            cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: notch should be an float or a list of floats with at least " << channel->freq_count
                 << " elements\n";
            error();
        }
        if (chan_setting.exists("notch_q") && libconfig::Setting::TypeList == chan_setting["notch_q"].getType() && chan_setting["notch_q"].getLength() < channel->freq_count) {
            cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: notch_q should be a float or a list of floats with at least " << channel->freq_count
                 << " elements\n";
            error();
        }
        if (chan_setting.exists("ctcss") && libconfig::Setting::TypeList == chan_setting["ctcss"].getType() && chan_setting["ctcss"].getLength() < channel->freq_count) {
            cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: ctcss should be an float or a list of floats with at least " << channel->freq_count
                 << " elements\n";
            error();
        }
        if (chan_setting.exists("modulation") && chan_setting.exists("modulations")) {
            cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: can't set both modulation and modulations\n";
            error();
        }
        if (chan_setting.exists("modulations") && chan_setting["modulations"].getLength() < channel->freq_count) {
            cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: modulations should be a list with at least " << channel->freq_count << " elements\n";
            error();
        }

        for (int f = 0; f < channel->freq_count; f++) {
            channel->freqlist[f].frequency = parse_anynum2int((chan_setting["freqs"][f]));
            if (chan_setting.exists("labels")) {
                channel->freqlist[f].label = strdup(chan_setting["labels"][f]);
            }
            if (chan_setting.exists("modulations")) {
#ifdef NFM
                if (strncmp(chan_setting["modulations"][f], "nfm", 3) == 0) {
                    channel->freqlist[f].modulation = MOD_NFM;
                } else
#endif /* NFM */
                    if (strncmp(chan_setting["modulations"][f], "am", 2) == 0) {
                        channel->freqlist[f].modulation = MOD_AM;
                    } else {
                        cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "] modulations.[" << f << "]: unknown modulation\n";
                        error();
                    }
            } else {
                channel->freqlist[f].modulation = channel_modulation;
            }
        }
        // Set initial frequency for scanning
        // We tune 20 FFT bins higher to avoid DC spike
        dev->input->centerfreq = channel->freqlist[0].frequency + 20 * (double)(dev->input->sample_rate / fft_size);
    }
    if (chan_setting.exists("squelch")) {
        cerr << "Warning: 'squelch' no longer supported and will be ignored, use 'squelch_threshold' or 'squelch_snr_threshold' instead\n";
    }
    if (chan_setting.exists("squelch_threshold") && chan_setting.exists("squelch_snr_threshold")) {
        cerr << "Warning: Both 'squelch_threshold' and 'squelch_snr_threshold' are set and may conflict\n";
    }
    if (chan_setting.exists("squelch_threshold")) {
        // Value is dBFS, zero disables manual threshold (ie use auto squelch), negative is valid, positive is invalid
        if (libconfig::Setting::TypeList == chan_setting["squelch_threshold"].getType()) {
            // New-style array of per-frequency squelch settings
            for (int f = 0; f < channel->freq_count; f++) {
                int threshold_dBFS = (int)chan_setting["squelch_threshold"][f];
                if (threshold_dBFS > 0) {
                    cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: squelch_threshold must be less than or equal to 0\n";
                    error();
                } else if (threshold_dBFS == 0) {
                    channel->freqlist[f].squelch.set_squelch_level_threshold(0);
                } else {
                    channel->freqlist[f].squelch.set_squelch_level_threshold(dBFS_to_level(threshold_dBFS));
                }
            }
        } else if (libconfig::Setting::TypeInt == chan_setting["squelch_threshold"].getType()) {
            // Legacy (single squelch for all frequencies)
            int threshold_dBFS = (int)chan_setting["squelch_threshold"];
            float level;
            if (threshold_dBFS > 0) {
                cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: squelch_threshold must be less than or equal to 0\n";
                error();
            } else if (threshold_dBFS == 0) {
                level = 0;
            } else {
                level = dBFS_to_level(threshold_dBFS);
            }

            for (int f = 0; f < channel->freq_count; f++) {
                channel->freqlist[f].squelch.set_squelch_level_threshold(level);
            }
        } else {
            cerr << "Invalid value for squelch_threshold (should be int or list - use parentheses)\n";
            error();
        }
    }
    if (chan_setting.exists("squelch_snr_threshold")) {
        // Value is SNR in dB, zero disables squelch (ie always open), -1 uses default value, positive is valid, other negative values are invalid
        if (libconfig::Setting::TypeList == chan_setting["squelch_snr_threshold"].getType()) {
            // New-style array of per-frequency squelch settings
            for (int f = 0; f < channel->freq_count; f++) {
                float snr = 0.f;
                if (libconfig::Setting::TypeFloat == chan_setting["squelch_snr_threshold"][f].getType()) {
                    snr = (float)chan_setting["squelch_snr_threshold"][f];
                } else if (libconfig::Setting::TypeInt == chan_setting["squelch_snr_threshold"][f].getType()) {
                    snr = (int)chan_setting["squelch_snr_threshold"][f];
                } else {
                    cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: squelch_snr_threshold list must be of int or float\n";
                    error();
                }

                if (snr == -1.0) {
                    continue;  // "disable" for this channel in list
                } else if (snr < 0) {
                    cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: squelch_snr_threshold must be greater than or equal to 0\n";
                    error();
                } else {
                    channel->freqlist[f].squelch.set_squelch_snr_threshold(snr);
                }
            }
        } else if (libconfig::Setting::TypeFloat == chan_setting["squelch_snr_threshold"].getType() || libconfig::Setting::TypeInt == chan_setting["squelch_snr_threshold"].getType()) {
            // Legacy (single squelch for all frequencies)
            float snr = (libconfig::Setting::TypeFloat == chan_setting["squelch_snr_threshold"].getType()) ? (float)chan_setting["squelch_snr_threshold"] : (int)chan_setting["squelch_snr_threshold"];

            if (snr == -1.0) {
                return false;  // "disable" so use the default without error message
            } else if (snr < 0) {
                cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: squelch_snr_threshold must be greater than or equal to 0\n";
                error();
            }

            for (int f = 0; f < channel->freq_count; f++) {
                channel->freqlist[f].squelch.set_squelch_snr_threshold(snr);
            }
        } else {
            cerr << "Invalid value for squelch_snr_threshold (should be float, int, or list of int/float - use parentheses)\n";
            error();
        }
    }
    if (chan_setting.exists("notch")) {
        static const float default_q = 10.0;

        if (chan_setting.exists("notch_q") && chan_setting["notch"].getType() != chan_setting["notch_q"].getType()) {
            cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: notch_q (if set) must be the same type as notch - "
                 << "float or a list of floats with at least " << channel->freq_count << " elements\n";
            error();
        }
        if (libconfig::Setting::TypeList == chan_setting["notch"].getType()) {
            for (int f = 0; f < channel->freq_count; f++) {
                float freq = (float)chan_setting["notch"][f];
                float q = chan_setting.exists("notch_q") ? (float)chan_setting["notch_q"][f] : default_q;

                if (q == 0.0) {
                    q = default_q;
                } else if (q <= 0.0) {
                    cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "] freq.[" << f << "]: invalid value for notch_q: " << q << " (must be greater than 0.0)\n";
                    error();
                }

                if (freq == 0) {
                    continue;  // "disable" for this channel in list
                } else if (freq < 0) {
                    cerr << "devices.[" << dev_idx << "] channels.[" << chan_idx << "] freq.[" << f << "]: invalid value for notch: " << freq << ", ignoring\n";
                } else {
                    channel->freqlist[f].notch_filter = NotchFilter(freq, WAVE_RATE, q);
                }
            }
        } else if (libconfig::Setting::TypeFloat == chan_setting["notch"].getType()) {
            float freq = (float)chan_setting["notch"];
            float q = chan_setting.exists("notch_q") ? (float)chan_setting["notch_q"] : default_q;
            if (q <= 0.0) {
                cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: invalid value for notch_q: " << q << " (must be greater than 0.0)\n";
                error();
            }
            for (int f = 0; f < channel->freq_count; f++) {
                if (freq == 0) {
                    continue;  // "disable" is default so ignore without error message
                } else if (freq < 0) {
                    cerr << "devices.[" << dev_idx << "] channels.[" << chan_idx << "]: notch value '" << freq << "' invalid, ignoring\n";
                } else {
                    channel->freqlist[f].notch_filter = NotchFilter(freq, WAVE_RATE, q);
                }
            }
        } else {
            cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: notch should be an float or a list of floats with at least " << channel->freq_count
                 << " elements\n";
            error();
        }
    }
    if (chan_setting.exists("ctcss")) {
        if (libconfig::Setting::TypeList == chan_setting["ctcss"].getType()) {
            for (int f = 0; f < channel->freq_count; f++) {
                float freq = (float)chan_setting["ctcss"][f];

                if (freq == 0) {
                    continue;  // "disable" for this channel in list
                } else if (freq < 0) {
                    cerr << "devices.[" << dev_idx << "] channels.[" << chan_idx << "] freq.[" << f << "]: invalid value for ctcss: " << freq << ", ignoring\n";
                } else {
                    channel->freqlist[f].squelch.set_ctcss_freq(freq, WAVE_RATE);
                }
            }
        } else if (libconfig::Setting::TypeFloat == chan_setting["ctcss"].getType()) {
            float freq = (float)chan_setting["ctcss"];
            for (int f = 0; f < channel->freq_count; f++) {
                if (freq <= 0) {
                    cerr << "devices.[" << dev_idx << "] channels.[" << chan_idx << "]: ctcss value '" << freq << "' invalid, ignoring\n";
                } else {
                    channel->freqlist[f].squelch.set_ctcss_freq(freq, WAVE_RATE);
                }
            }
        } else {
            cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: ctcss should be an float or a list of floats with at least " << channel->freq_count
                 << " elements\n";
            error();
        }
    }
    if (chan_setting.exists("bandwidth")) {
        channel->needs_raw_iq = 1;

        if (libconfig::Setting::TypeList == chan_setting["bandwidth"].getType()) {
            for (int f = 0; f < channel->freq_count; f++) {
                int bandwidth = parse_anynum2int(chan_setting["bandwidth"][f]);

                if (bandwidth == 0) {
                    continue;  // "disable" for this channel in list
                } else if (bandwidth < 0) {
                    cerr << "devices.[" << dev_idx << "] channels.[" << chan_idx << "] freq.[" << f << "]: bandwidth value '" << bandwidth << "' invalid, ignoring\n";
                } else {
                    channel->freqlist[f].lowpass_filter = LowpassFilter((float)bandwidth / 2, WAVE_RATE);
                }
            }
        } else {
            int bandwidth = parse_anynum2int(chan_setting["bandwidth"]);
            if (bandwidth == 0) {
                return false;  // "disable" is default so ignore without error message
            } else if (bandwidth < 0) {
                cerr << "devices.[" << dev_idx << "] channels.[" << chan_idx << "]: bandwidth value '" << bandwidth << "' invalid, ignoring\n";
            } else {
                for (int f = 0; f < channel->freq_count; f++) {
                    channel->freqlist[f].lowpass_filter = LowpassFilter((float)bandwidth / 2, WAVE_RATE);
                }
            }
        }
    }
    if (chan_setting.exists("ampfactor")) {
        if (libconfig::Setting::TypeList == chan_setting["ampfactor"].getType()) {
            for (int f = 0; f < channel->freq_count; f++) {
                float ampfactor = (float)chan_setting["ampfactor"][f];

                if (ampfactor < 0) {
                    cerr << "devices.[" << dev_idx << "] channels.[" << chan_idx << "] freq.[" << f << "]: ampfactor '" << ampfactor << "' must not be negative\n";
                    error();
                }

                channel->freqlist[f].ampfactor = ampfactor;
            }
        } else {
            float ampfactor = (float)chan_setting["ampfactor"];

            if (ampfactor < 0) {
                cerr << "devices.[" << dev_idx << "] channels.[" << chan_idx << "]: ampfactor '" << ampfactor << "' must not be negative\n";
                error();
            }

            for (int f = 0; f < channel->freq_count; f++) {
                channel->freqlist[f].ampfactor = ampfactor;
            }
        }
    }

#ifdef NFM
    if (chan_setting.exists("tau")) {
        channel->alpha = ((int)chan_setting["tau"] == 0 ? 0.0f : exp(-1.0f / (WAVE_RATE * 1e-6 * (int)chan_setting["tau"])));
    }
#endif /* NFM */
    libconfig::Setting& outputs = chan_setting["outputs"];
    channel->output_count = outputs.getLength();
    if (channel->output_count < 1) {
        cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: no outputs defined\n";
        error();
    }
    channel->outputs = (output_t*)XCALLOC(channel->output_count, sizeof(struct output_t));
    int outputs_enabled = parse_outputs(outputs, channel, dev_idx, chan_idx, false, dev->mode, dev);
    if (outputs_enabled < 1) {
        cerr << "Configuration error: devices.[" << dev_idx << "] channels.[" << chan_idx << "]: no outputs defined\n";
        error();
    }
    channel->outputs = (output_t*)XREALLOC(channel->outputs, outputs_enabled * sizeof(struct output_t));
    channel->output_count = outputs_enabled;

    dev->base_bins[chan_idx] = dev->bins[chan_idx] = compute_channel_bin(channel->freqlist[0].frequency, dev->input->centerfreq, dev->input->sample_rate, fft_size);
    debug_print("bins[%d]: %zu\n", chan_idx, dev->bins[chan_idx]);

#ifdef NFM
    for (int f = 0; f < channel->freq_count; f++) {
        if (channel->freqlist[f].modulation == MOD_NFM) {
            channel->needs_raw_iq = 1;
            break;
        }
    }
#endif /* NFM */

    if (channel->needs_raw_iq) {
        // Downmixing is done only for NFM and raw IQ outputs. It's not critical to have some residual
        // freq offset in AM, as it doesn't affect sound quality significantly.
        // See compute_channel_dm_dphi() (live_reconfig.cpp) for the derivation - shared with
        // the live-retune path so both agree by construction.
        channel->dm_dphi = compute_channel_dm_dphi(channel->freqlist[0].frequency, dev->input->centerfreq, dev->input->sample_rate);
        debug_print("dev[%d].chan[%d]: dm_dphi=0x%x\n", dev_idx, chan_idx, channel->dm_dphi);
        channel->dm_phi = 0.f;
    }

#ifdef DEBUG_SQUELCH
    // Setup squelch debug file, if enabled
    char tmp_filepath[1024];
    for (int f = 0; f < channel->freq_count; f++) {
        snprintf(tmp_filepath, sizeof(tmp_filepath), "./squelch_debug-%d-%d.dat", chan_idx, f);
        channel->freqlist[f].squelch.set_debug_file(tmp_filepath);
    }
#endif /* DEBUG_SQUELCH */
    channel->config_signature = strdup(build_channel_identity_signature(chan_setting).c_str());
    return true;
}

static int parse_channels(libconfig::Setting& chans, device_t* dev, int i) {
    int jj = 0;
    for (int j = 0; j < chans.getLength(); j++) {
        if (chans[j].exists("disable") && (bool)chans[j]["disable"] == true) {
            continue;
        }
        channel_t* channel = dev->channels + jj;
        if (parse_channel(chans[j], dev, i, jj, channel)) {
            jj++;
        }
    }
    return jj;
}

int parse_devices(libconfig::Setting& devs) {
    int devcnt = 0;
    for (int i = 0; i < devs.getLength(); i++) {
        if (devs[i].exists("disable") && (bool)devs[i]["disable"] == true)
            continue;
        device_t* dev = devices + devcnt;
        if (devs[i].exists("type")) {
            dev->input = input_new(devs[i]["type"]);
            if (dev->input == NULL) {
                cerr << "Configuration error: devices.[" << i << "]: unsupported device type\n";
                error();
            }
        } else {
#ifdef WITH_RTLSDR
            cerr << "Warning: devices.[" << i << "]: assuming device type \"rtlsdr\", please set \"type\" in the device section.\n";
            dev->input = input_new("rtlsdr");
#else
            cerr << "Configuration error: devices.[" << i << "]: mandatory parameter missing: type\n";
            error();
#endif /* WITH_RTLSDR */
        }
        assert(dev->input != NULL);
        if (devs[i].exists("sample_rate")) {
            int sample_rate = parse_anynum2int(devs[i]["sample_rate"]);
            if (sample_rate < WAVE_RATE) {
                cerr << "Configuration error: devices.[" << i << "]: sample_rate must be greater than " << WAVE_RATE << "\n";
                error();
            }
            dev->input->sample_rate = sample_rate;
        }
        if (devs[i].exists("mode")) {
            if (!strncmp(devs[i]["mode"], "multichannel", 12)) {
                dev->mode = R_MULTICHANNEL;
            } else if (!strncmp(devs[i]["mode"], "scan", 4)) {
                dev->mode = R_SCAN;
            } else {
                cerr << "Configuration error: devices.[" << i << "]: invalid mode (must be one of: \"scan\", \"multichannel\")\n";
                error();
            }
        } else {
            dev->mode = R_MULTICHANNEL;
        }
        // Extra channel array capacity reserved at startup so a channel can be appended live
        // later (dynamic_reload's reload_diff) without ever reallocating dev->channels/bins/
        // base_bins - see device_t::channel_capacity's comment (rtl_airband.h) for why that
        // matters. Only meaningful for R_MULTICHANNEL: an R_SCAN device always has exactly one
        // channel (its freqlist holds the scanned frequencies instead), so "add a channel" has
        // no meaning there.
        int reserve_channels = devs[i].exists("reserve_channels") ? (int)devs[i]["reserve_channels"] : 0;
        if (reserve_channels < 0) {
            cerr << "Configuration error: devices.[" << i << "]: reserve_channels must not be negative\n";
            error();
        }
        if (dev->mode == R_SCAN && reserve_channels != 0) {
            cerr << "Configuration error: devices.[" << i << "]: reserve_channels is not supported in scan mode\n";
            error();
        }
        if (dev->mode == R_MULTICHANNEL) {
            dev->input->centerfreq = parse_anynum2int(devs[i]["centerfreq"]);
        }  // centerfreq for R_SCAN will be set by parse_channels() after frequency list has been read
#ifdef NFM
        if (devs[i].exists("tau")) {
            dev->alpha = ((int)devs[i]["tau"] == 0 ? 0.0f : exp(-1.0f / (WAVE_RATE * 1e-6 * (int)devs[i]["tau"])));
        } else {
            dev->alpha = alpha;
        }
#endif /* NFM */

        // Parse hardware-dependent configuration parameters
        if (input_parse_config(dev->input, devs[i]) < 0) {
            // FIXME: get and display error string from input_parse_config
            // Right now it exits the program on failure.
        }
        // Some basic sanity checks for crucial parameters which have to be set
        // (or can be modified) by the input driver
        assert(dev->input->sfmt != SFMT_UNDEF);
        assert(dev->input->fullscale > 0);
        assert(dev->input->bytes_per_sample > 0);
        assert(dev->input->sample_rate > WAVE_RATE);

        dev->auto_ppm = NULL;
        if (devs[i].exists("auto_ppm")) {
            input_t* input = dev->input;
            if (input->set_correction == NULL || input->get_settings == NULL) {
                cerr << "Configuration error: devices.[" << i << "]: auto_ppm is not supported by the " << (input->driver_type ? input->driver_type : "?") << " driver\n";
                error();
            }
            AutoPpmParams params;
            string param_error;
            if (!auto_ppm_parse_params(devs[i], &params, &param_error)) {
                cerr << "Configuration error: devices.[" << i << "]: " << param_error << "\n";
                error();
            }
            input_settings_t settings;
            int start_correction = 0;
            if (input->get_settings(input, &settings) == 0) {
                start_correction = (int)lround(settings.correction);
            }
            if (abs(start_correction) > params.max_ppm) {
                cerr << "Configuration error: devices.[" << i << "]: correction " << start_correction << " is outside auto_ppm_max_ppm (+/-" << params.max_ppm << ")\n";
                error();
            }
            auto_ppm_device* ap = new auto_ppm_device();
            ap->params = params;
            ap->enabled = (bool)devs[i]["auto_ppm"];
            ap->state.base_correction = start_correction;
            dev->auto_ppm = ap;
        }

        // Shared with live_reconfig.cpp's device_apply_sample_rate() so both paths agree by
        // construction - see compute_input_buf_size()'s declaration comment (live_reconfig.h).
        dev->input->buf_size = compute_input_buf_size(dev->input->sample_rate, dev->input->bytes_per_sample);
        debug_print("dev->input->buf_size: %zu\n", dev->input->buf_size);
        dev->input->buffer = (unsigned char*)XCALLOC(sizeof(unsigned char), dev->input->buf_size + 2 * dev->input->bytes_per_sample * fft_size);
        dev->input->bufs = dev->input->bufe = 0;
        dev->input->overflow_count = 0;
        dev->input->underrun_count = 0;
        dev->input->centerfreq_retune_failure_count = 0;
        dev->output_overrun_count = 0;
        dev->waveend = dev->waveavail = dev->row = dev->tq_head = dev->tq_tail = 0;
        dev->last_frequency = -1;
        dev->pending_centerfreq_request = -1;
        dev->centerfreq_apply_failed = false;
        dev->pending_sample_rate_request = -1;
        dev->sample_rate_apply_failed = false;

        libconfig::Setting& chans = devs[i]["channels"];
        if (chans.getLength() < 1) {
            cerr << "Configuration error: devices.[" << i << "]: no channels configured\n";
            error();
        }
        size_t initial_capacity = (size_t)chans.getLength() + (size_t)reserve_channels;
        dev->channels = (channel_t*)XCALLOC(initial_capacity, sizeof(channel_t));
        dev->bins = (size_t*)XCALLOC(initial_capacity, sizeof(size_t));
        dev->base_bins = (size_t*)XCALLOC(initial_capacity, sizeof(size_t));
        dev->channel_count = 0;
        int channel_count = parse_channels(chans, dev, i);
        if (channel_count < 1) {
            cerr << "Configuration error: devices.[" << i << "]: no channels enabled\n";
            error();
        }
        if (dev->mode == R_SCAN && channel_count > 1) {
            cerr << "Configuration error: devices.[" << i << "]: only one channel is allowed in scan mode\n";
            error();
        }
        // The reserved tail (channel_count..channel_capacity-1) stays zero-initialized from the
        // XCALLOC above - this XREALLOC only trims off the gap left by any disable=true entries
        // between parsed channels and the reserved headroom, it never touches the reserved slots
        // themselves.
        int channel_capacity = channel_count + reserve_channels;
        dev->channels = (channel_t*)XREALLOC(dev->channels, channel_capacity * sizeof(channel_t));
        dev->bins = (size_t*)XREALLOC(dev->bins, channel_capacity * sizeof(size_t));
        dev->base_bins = (size_t*)XREALLOC(dev->base_bins, channel_capacity * sizeof(size_t));
        dev->channel_capacity = channel_capacity;
        dev->channel_count = channel_count;
        devcnt++;
    }
    return devcnt;
}

int parse_mixers(libconfig::Setting& mx) {
    const char* name;
    int mm = 0;
    for (int i = 0; i < mx.getLength(); i++) {
        if (mx[i].exists("disable") && (bool)mx[i]["disable"] == true)
            continue;
        if ((name = mx[i].getName()) == NULL) {
            cerr << "Configuration error: mixers.[" << i << "]: undefined mixer name\n";
            error();
        }
        debug_print("mm=%d name=%s\n", mm, name);
        mixer_t* mixer = &mixers[mm];
        mixer->name = strdup(name);
        mixer->enabled = false;
        mixer->config_wants_disabled = mx[i].exists("enabled") ? !(bool)mx[i]["enabled"] : false;
        mixer->pending_enable_request = -1;
        mixer->interval = MIX_DIVISOR;
        mixer->output_overrun_count = 0;
        mixer->input_count = 0;
        mixer->input_capacity = 0;
        mixer->inputs = NULL;
        mixer->inputs_todo = NULL;
        mixer->input_mask = NULL;
        // Extra input array capacity reserved (by mixer_finalize_capacity(), mixer.cpp, called
        // from main() after parse_devices() returns) so an input can be connected live later
        // (dynamic_reload's reload_diff, when an appended channel's output is `type = "mixer"`)
        // without ever reallocating inputs/inputs_todo/input_mask - see mixer_t::input_capacity's
        // comment (rtl_airband.h) for why that matters.
        mixer->reserve_inputs = mx[i].exists("reserve_inputs") ? (int)mx[i]["reserve_inputs"] : 0;
        if (mixer->reserve_inputs < 0) {
            cerr << "Configuration error: mixers.[" << i << "]: reserve_inputs must not be negative\n";
            error();
        }
        // Captured before remote_inputs is parsed below so reload_diff (compute_and_apply_diff(),
        // live_reconfig.cpp) can detect a live-running mixer's remote_inputs block changing -
        // see mixer_t::remote_inputs_signature's comment (rtl_airband.h) for why this exists.
        mixer->remote_inputs_signature = strdup(build_mixer_remote_inputs_signature(mx[i]).c_str());

        // Cross-instance mixer inputs: each entry reserves a slot fed by a `mixer_remote` output
        // in a DIFFERENT rtl_airband process's config (see mixer_remote.h/mixer_remote_wire.h),
        // connected via the same mixer_connect_input() every local `type = "mixer"` output uses -
        // same single-threaded startup window mixer_finalize_capacity() (mixer.cpp) relies on.
        if (mx[i].exists("remote_inputs")) {
            libconfig::Setting& remote_inputs = mx[i]["remote_inputs"];
            for (int r = 0; r < remote_inputs.getLength(); r++) {
                if (!remote_inputs[r].exists("listen_path")) {
                    cerr << "Configuration error: mixers.[" << i << "] remote_inputs.[" << r << "]: missing listen_path\n";
                    error();
                }
                string listen_path = (const char*)remote_inputs[r]["listen_path"];
                if (listen_path.empty()) {
                    cerr << "Configuration error: mixers.[" << i << "] remote_inputs.[" << r << "]: listen_path must not be empty\n";
                    error();
                }
                if (!remote_inputs[r].exists("stream_id")) {
                    cerr << "Configuration error: mixers.[" << i << "] remote_inputs.[" << r << "]: missing stream_id\n";
                    error();
                }
                int stream_id = (int)remote_inputs[r]["stream_id"];
                if (stream_id < 0) {
                    cerr << "Configuration error: mixers.[" << i << "] remote_inputs.[" << r << "]: stream_id must not be negative\n";
                    error();
                }
                float ampfactor = remote_inputs[r].exists("ampfactor") ? (float)remote_inputs[r]["ampfactor"] : 1.0f;
                float balance = remote_inputs[r].exists("balance") ? (float)remote_inputs[r]["balance"] : 0.0f;
                if (balance < -1.0f || balance > 1.0f) {
                    cerr << "Configuration error: mixers.[" << i << "] remote_inputs.[" << r << "]: balance out of allowed range <-1.0;1.0>\n";
                    error();
                }

                mixer_remote_listener_t* listener = mixer_remote_get_or_create_listener(listen_path);
                for (const mixer_remote_route_t& existing : listener->routes) {
                    if (existing.stream_id == (uint32_t)stream_id) {
                        cerr << "Configuration error: mixers.[" << i << "] remote_inputs.[" << r << "]: duplicate stream_id " << stream_id << " for listen_path " << listen_path << "\n";
                        error();
                    }
                }

                int slot = mixer_connect_input(mixer, ampfactor, balance);
                if (slot < 0) {
                    cerr << "Configuration error: mixers.[" << i << "] remote_inputs.[" << r << "]: could not connect remote input: " << mixer_get_error() << "\n";
                    error();
                }
                if (remote_inputs[r].exists("label")) {
                    mixer->inputs[slot].remote_label = strdup((const char*)remote_inputs[r]["label"]);
                }
                mixer_remote_register_route(listener, (uint32_t)stream_id, mixer, slot);
                debug_print("mixer %s remote_inputs.[%d]: listen_path=%s stream_id=%d slot=%d (ampfactor=%.1f balance=%.1f)\n", name, r, listen_path.c_str(), stream_id, slot, ampfactor, balance);
            }
        }

        channel_t* channel = &mixer->channel;
        // mixer->channel.enabled gates process_outputs() same as any other channel_t (see
        // output.cpp), but unlike device channels it has no separate config keyword - a mixer's
        // own output stage is governed entirely by mixer->enabled (already checked before
        // process_outputs() is called for a mixer, output.cpp:1264), so this just needs to
        // default true and never gets touched again.
        channel->enabled = true;
        channel->pending_enable_request = -1;
        // A mixer's own embedded channel is never itself removable (only device channels are, via
        // reload_diff's tail-decrease detection) - initialized only for the same "-1 sentinel,
        // never a stray value" hygiene as pending_enable_request above.
        channel->pending_remove_request = -1;
        channel->highpass = mx[i].exists("highpass") ? (int)mx[i]["highpass"] : 100;
        channel->lowpass = mx[i].exists("lowpass") ? (int)mx[i]["lowpass"] : 2500;
        channel->mode = MM_MONO;

        // Make sure lowpass / highpass aren't flipped.
        // If lowpass is enabled (greater than zero) it must be larger than highpass
        if (channel->lowpass > 0 && channel->lowpass < channel->highpass) {
            cerr << "Configuration error: mixers.[" << i << "]: lowpass (" << channel->lowpass << ") must be greater than or equal to highpass (" << channel->highpass << ")\n";
            error();
        }

        libconfig::Setting& outputs = mx[i]["outputs"];
        channel->output_count = outputs.getLength();
        if (channel->output_count < 1) {
            cerr << "Configuration error: mixers.[" << i << "]: no outputs defined\n";
            error();
        }
        channel->outputs = (output_t*)XCALLOC(channel->output_count, sizeof(struct output_t));
        int outputs_enabled = parse_outputs(outputs, channel, i, 0, true, R_MULTICHANNEL, nullptr);
        if (outputs_enabled < 1) {
            cerr << "Configuration error: mixers.[" << i << "]: no outputs defined\n";
            error();
        }
        channel->outputs = (output_t*)XREALLOC(channel->outputs, outputs_enabled * sizeof(struct output_t));
        channel->output_count = outputs_enabled;
        mm++;
    }
    return mm;
}

// vim: ts=4
