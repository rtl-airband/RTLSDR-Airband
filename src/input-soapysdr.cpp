/*
 *  input-soapysdr.cpp
 *  SoapySDR-specific routines
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

#include "input-soapysdr.h"    // soapysdr_dev_data_t
#include <SoapySDR/Device.h>   // SoapySDRDevice, SoapySDRDevice_makeStrArgs
#include <SoapySDR/Formats.h>  // SOAPY_SDR_CS constants
#include <SoapySDR/Version.h>  // SOAPY_SDR_API_VERSION
#include <assert.h>
#include <limits.h>  // SCHAR_MAX, SHRT_MAX
#include <math.h>    // round
#include <stdlib.h>  // calloc
#include <string.h>  // memcpy, strcmp
#include <syslog.h>  // LOG_* macros
#include <iostream>
#include <libconfig.h++>    // Setting
#include "input-common.h"   // input_t, sample_format_t, input_state_t, MODULE_EXPORT
#include "input-helpers.h"  // circbuffer_append
#include "rtl_airband.h"    // do_exit, fft_size, debug_print, XCALLOC, error()

using namespace std;

// Map SoapySDR sample format string to our internal sample format
// and set bytes_per_sample and fullscale values appropriately.
// We prefer U8 and S8 over S16 to minimize CPU load.
// If fullscale is > 0, it means it has been read by
// SoapySDRDevice_getNativeStreamFormat, so we treat this value as valid.
// Otherwise, guess a suitable default value.
static bool soapysdr_match_sfmt(input_t* const input, char const* const fmt, double const fullscale) {
    if (strcmp(fmt, SOAPY_SDR_CU8) == 0) {
        input->sfmt = SFMT_U8;
        input->bytes_per_sample = sizeof(unsigned char);
        input->fullscale = (fullscale > 0 ? fullscale : (float)SCHAR_MAX - 0.5f);
        goto matched;
    } else if (strcmp(fmt, SOAPY_SDR_CS8) == 0) {
        input->sfmt = SFMT_S8;
        input->bytes_per_sample = sizeof(char);
        input->fullscale = (fullscale > 0 ? fullscale : (float)SCHAR_MAX - 0.5f);
        goto matched;
    } else if (strcmp(fmt, SOAPY_SDR_CS16) == 0) {
        input->sfmt = SFMT_S16;
        input->bytes_per_sample = sizeof(short);
        input->fullscale = (fullscale > 0 ? fullscale : (float)SHRT_MAX - 0.5f);
        goto matched;
    } else if (strcmp(fmt, SOAPY_SDR_CF32) == 0) {
        input->sfmt = SFMT_F32;
        input->bytes_per_sample = sizeof(float);
        input->fullscale = (fullscale > 0 ? fullscale : 1.0f);
        goto matched;
    }
    return false;
matched:
    soapysdr_dev_data_t* dev_data = (soapysdr_dev_data_t*)input->dev_data;
    dev_data->sample_format = strdup(fmt);
    return true;
}

// Choose a suitable sample format.
// Bail out if no supported sample format is found.
static bool soapysdr_choose_sample_format(SoapySDRDevice* const sdr, input_t* const input) {
    bool ret = false;
    size_t len = 0;
    char** formats = NULL;
    soapysdr_dev_data_t* dev_data = (soapysdr_dev_data_t*)input->dev_data;
    input->sfmt = SFMT_UNDEF;
    // First try device's native format to avoid extra conversion
    double fullscale = 0.0;
    char* fmt = SoapySDRDevice_getNativeStreamFormat(sdr, SOAPY_SDR_RX, dev_data->channel, &fullscale);

    if (soapysdr_match_sfmt(input, fmt, fullscale) == true) {
        log(LOG_NOTICE, "SoapySDR: device '%s': using native sample format '%s' (fullScale=%.1f)\n", dev_data->device_string, fmt, input->fullscale);
        ret = true;
        goto end;
    }
    // Native format is not supported by rtl_airband; find out if there is anything else.
    formats = SoapySDRDevice_getStreamFormats(sdr, SOAPY_SDR_RX, dev_data->channel, &len);
    if (formats == NULL || len == 0) {
        log(LOG_ERR, "SoapySDR: device '%s': failed to read supported sample formats\n", dev_data->device_string);
        ret = false;
        goto end;
    }
    for (size_t i = 0; i < len; i++) {
        if (soapysdr_match_sfmt(input, formats[i], -1.0) == true) {
            log(LOG_NOTICE, "SoapySDR: device '%s': using non-native sample format '%s' (assuming fullScale=%.1f)\n", dev_data->device_string, formats[i], input->fullscale);
            ret = true;
            goto end;
        }
    }
    // Nothing found; we can't use this device.
    log(LOG_ERR, "SoapySDR: device '%s': no supported sample format found\n", dev_data->device_string);
end:
    return ret;
}

static int sdrplay_get_nearest_sample_rate(SoapySDRDevice* sdr, int channel, int sample_rate) {
    size_t len = 0;
    double sr = (double)sample_rate;
    SoapySDRRange* range = SoapySDRDevice_getSampleRateRange(sdr, SOAPY_SDR_RX, channel, &len);
    if (range == NULL) {
        log(LOG_ERR, "SoapySDR: failed to read supported sampling rate ranges from the device\n");
        return -1;
    }
    debug_print("Got %zu ranges\n", len);
    double nearest_rate = range[0].minimum;
    double offset1, offset2;
    for (size_t i = 0; i < len; i++) {
        debug_print("sr=%.1f min=%.1f max=%.1f step=%.1f\n", sr, range[i].minimum, range[i].maximum, range[i].step);
        if (sr >= range[i].minimum && sr <= range[i].maximum) {
            debug_print("Found suitable range: min=%.0f max=%0.f step=%0.f\n", range[i].minimum, range[i].maximum, range[i].step);
            if (range[i].step == 0.0 || range[i].step >= (range[i].maximum - range[i].minimum)) {
                return (int)(range[i].maximum - sr > sr - range[i].minimum ? range[i].minimum : range[i].maximum);
            }
            sr = (int)(range[i].minimum + range[i].step * round((sr - range[i].minimum) / range[i].step));
            if (sr > range[i].maximum) {
                sr = (int)range[i].maximum;
            }
            return (int)sr;
        } else {
            offset1 = abs(sr - nearest_rate);
            offset2 = abs(sr - range[i].minimum);
            if (offset2 < offset1)
                nearest_rate = range[i].minimum;
            offset1 = abs(sr - nearest_rate);
            offset2 = abs(sr - range[i].maximum);
            if (offset2 < offset1)
                nearest_rate = range[i].maximum;
        }
    }
    return (int)nearest_rate;
}

int soapysdr_parse_config(input_t* const input, libconfig::Setting& cfg) {
    soapysdr_dev_data_t* dev_data = (soapysdr_dev_data_t*)input->dev_data;

    if (cfg.exists("device_string")) {
        dev_data->device_string = strdup(cfg["device_string"]);
    } else {
        cerr << "SoapySDR configuration error: mandatory parameter missing: device_string\n";
        error();
    }
    if (cfg.exists("gain")) {
        if (cfg["gain"].getType() == libconfig::Setting::TypeInt) {
            dev_data->gain = (double)((int)cfg["gain"]);
        } else if (cfg["gain"].getType() == libconfig::Setting::TypeFloat) {
            dev_data->gain = (double)cfg["gain"];
        } else {
            // Either it's a string or an unsupported type which will cause an exception - this is fine
            dev_data->gains = SoapySDRKwargs_fromString((const char*)cfg["gain"]);
            if (dev_data->gains.size < 1) {
                cerr << "SoapySDR configuration error: device '" << dev_data->device_string << "': gain: syntax error (must be a sequence of 'name1=value1,name2=value2,...')\n";
                error();
            }
        }
        dev_data->agc = false;
    } else {
        dev_data->agc = true;
    }
    if (cfg.exists("correction")) {
        if (cfg["correction"].getType() == libconfig::Setting::TypeInt) {
            dev_data->correction = (double)((int)cfg["correction"]);
        } else if (cfg["correction"].getType() == libconfig::Setting::TypeFloat) {
            dev_data->correction = (float)cfg["correction"];
        } else {
            cerr << "SoapySDR configuration error: device '" << dev_data->device_string << "': correction value must be numeric\n";
            error();
        }
    }
    if (cfg.exists("channel")) {
        dev_data->channel = (size_t)(int)cfg["channel"];
    }
    if (cfg.exists("antenna")) {
        dev_data->antenna = strdup(cfg["antenna"]);
    }
    // Find a suitable sample format and sample rate (unless set in the config)
    // based on device capabilities.
    // We have to do this here and not in soapysdr_init, because parse_devices()
    // requires sample_rate and bytes_per_sample to be set correctly in order to
    // calculate the size of the sample buffer, which has to be done before
    // soapysdr_init() is run.
    SoapySDRDevice* sdr = SoapySDRDevice_makeStrArgs(dev_data->device_string);
    if (sdr == NULL) {
        log(LOG_ERR, "Failed to open SoapySDR device '%s': %s\n", dev_data->device_string, SoapySDRDevice_lastError());
        error();
    }
    if (soapysdr_choose_sample_format(sdr, input) == false) {
        cerr << "SoapySDR configuration error: device '" << dev_data->device_string << "': no suitable sample format found\n";
        error();
    }
    if (input->sample_rate < 0) {
        input->sample_rate = sdrplay_get_nearest_sample_rate(sdr, dev_data->channel, SOAPYSDR_DEFAULT_SAMPLE_RATE);
        if (input->sample_rate < 0) {
            log(LOG_ERR, "Failed to find a suitable sample rate for SoapySDR device '%s'\n", dev_data->device_string);
            log(LOG_ERR, "Specify a supported value using \"sample_rate\" option in the device configuration\n");
            error();
        }
    }
    SoapySDRDevice_unmake(sdr);
    return 0;
}

int soapysdr_init(input_t* const input) {
    soapysdr_dev_data_t* dev_data = (soapysdr_dev_data_t*)input->dev_data;

    dev_data->dev = SoapySDRDevice_makeStrArgs(dev_data->device_string);
    if (dev_data->dev == NULL) {
        log(LOG_ERR, "Failed to open SoapySDR device '%s': %s\n", dev_data->device_string, SoapySDRDevice_lastError());
        error();
    }
    SoapySDRDevice* sdr = dev_data->dev;

    if (SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_RX, dev_data->channel, input->sample_rate) != 0) {
        log(LOG_ERR, "Failed to set sample rate for SoapySDR device '%s': %s\n", dev_data->device_string, SoapySDRDevice_lastError());
        error();
    }
    log(LOG_INFO, "SoapySDR: device '%s': sample rate set to %.0f sps\n", dev_data->device_string, SoapySDRDevice_getSampleRate(sdr, SOAPY_SDR_RX, dev_data->channel));
    if (SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_RX, dev_data->channel, input->centerfreq, NULL) != 0) {
        log(LOG_ERR, "Failed to set frequency for SoapySDR device '%s': %s\n", dev_data->device_string, SoapySDRDevice_lastError());
        error();
    }
    if (SoapySDRDevice_setFrequencyCorrection(sdr, SOAPY_SDR_RX, dev_data->channel, dev_data->correction) != 0) {
        log(LOG_ERR, "Failed to set frequency correction for SoapySDR device '%s': %s\n", dev_data->device_string, SoapySDRDevice_lastError());
        error();
    }
    if (dev_data->antenna != NULL) {
        if (SoapySDRDevice_setAntenna(sdr, SOAPY_SDR_RX, dev_data->channel, dev_data->antenna) != 0) {
            log(LOG_ERR, "Failed to set antenna to '%s' for SoapySDR device '%s': %s\n", dev_data->device_string, dev_data->antenna, SoapySDRDevice_lastError());
            error();
        }
        log(LOG_INFO, "SoapySDR: device '%s': antenna set to '%s'\n", dev_data->device_string, SoapySDRDevice_getAntenna(sdr, SOAPY_SDR_RX, dev_data->channel));
    }
    if (SoapySDRDevice_setGainMode(sdr, SOAPY_SDR_RX, dev_data->channel, dev_data->agc) != 0) {
        log(LOG_ERR, "Failed to %s AGC for SoapySDR device '%s': %s\n", dev_data->agc ? "enable" : "disable", dev_data->device_string, SoapySDRDevice_lastError());
        error();
    }
    log(LOG_INFO, "SoapySDR: device '%s': AGC %s (requested: %s)\n", dev_data->device_string, SoapySDRDevice_getGainMode(sdr, SOAPY_SDR_RX, dev_data->channel) ? "on" : "off",
        dev_data->agc ? "on" : "off");
    if (!dev_data->agc) {
        if (dev_data->gains.size > 0) {
            for (size_t i = 0; i < dev_data->gains.size; i++) {
                char* const key = dev_data->gains.keys[i];
                double val = atof(dev_data->gains.vals[i]);
                if (SoapySDRDevice_setGainElement(sdr, SOAPY_SDR_RX, dev_data->channel, key, val) != 0) {
                    log(LOG_ERR, "Failed to set gain element '%s' for SoapySDR device '%s': %s\n", key, dev_data->device_string, SoapySDRDevice_lastError());
                    error();
                }
                log(LOG_INFO, "SoapySDR: device '%s': gain '%s' set to %.1f dB\n", dev_data->device_string, key, SoapySDRDevice_getGainElement(sdr, SOAPY_SDR_RX, dev_data->channel, key));
            }
        } else {
            if (SoapySDRDevice_setGain(sdr, SOAPY_SDR_RX, dev_data->channel, dev_data->gain) != 0) {
                log(LOG_ERR, "Failed to set gain for SoapySDR device '%s': %s\n", dev_data->device_string, SoapySDRDevice_lastError());
                error();
            }
            log(LOG_INFO, "SoapySDR: device '%s': gain set to %.1f dB\n", dev_data->device_string, SoapySDRDevice_getGain(sdr, SOAPY_SDR_RX, dev_data->channel));
        }
    }
    log(LOG_INFO, "SoapySDR: device '%s' initialized\n", dev_data->device_string);
    return 0;
}

void* soapysdr_rx_thread(void* ctx) {
    input_t* input = (input_t*)ctx;
    soapysdr_dev_data_t* dev_data = (soapysdr_dev_data_t*)input->dev_data;
    SoapySDRDevice* sdr = dev_data->dev;
    assert(sdr != NULL);

    unsigned char buf[SOAPYSDR_BUFSIZE];
    // size of the buffer in number of I/Q sample pairs
    size_t num_elems = SOAPYSDR_BUFSIZE / (2 * input->bytes_per_sample);

    SoapySDRStream* rxStream = NULL;
#if SOAPY_SDR_API_VERSION < 0x00080000
    if (SoapySDRDevice_setupStream(sdr, &rxStream, SOAPY_SDR_RX, dev_data->sample_format, &dev_data->channel, 1, NULL) != 0) {
#else
    if ((rxStream = SoapySDRDevice_setupStream(sdr, SOAPY_SDR_RX, dev_data->sample_format, &dev_data->channel, 1, NULL)) == NULL) {
#endif /* SOAPY_SDR_API_VERSION */
        log(LOG_ERR, "Failed to set up stream for SoapySDR device '%s': %s\n", dev_data->device_string, SoapySDRDevice_lastError());
        input->state = INPUT_FAILED;
        goto cleanup;
    }
    if (SoapySDRDevice_activateStream(sdr, rxStream, 0, 0, 0)) {  // start streaming
        log(LOG_ERR, "Failed to activate stream for SoapySDR device '%s': %s\n", dev_data->device_string, SoapySDRDevice_lastError());
        input->state = INPUT_FAILED;
        goto cleanup;
    }
    input->state = INPUT_RUNNING;
    log(LOG_NOTICE, "SoapySDR: device '%s' started\n", dev_data->device_string);

    while (!do_exit) {
        void* bufs[] = {buf};  // array of buffers
        int flags;             // flags set by receive operation
        long long timeNs;      // timestamp for receive buffer
        int samples_read = SoapySDRDevice_readStream(sdr, rxStream, bufs, num_elems, &flags, &timeNs, SOAPYSDR_READSTREAM_TIMEOUT_US);
        if (samples_read < 0) {  // when it's negative, it's the error code
            log(LOG_ERR, "SoapySDR device '%s': readStream failed: %s\n", dev_data->device_string, SoapySDR_errToStr(samples_read));
            continue;
        }
        circbuffer_append(input, buf, (size_t)(samples_read * 2 * input->bytes_per_sample));
    }
cleanup:
    SoapySDRDevice_deactivateStream(sdr, rxStream, 0, 0);
    SoapySDRDevice_closeStream(sdr, rxStream);
    SoapySDRDevice_unmake(sdr);
    return 0;
}

int soapysdr_set_centerfreq(input_t* const input, int const centerfreq) {
    soapysdr_dev_data_t* dev_data = (soapysdr_dev_data_t*)input->dev_data;
    assert(dev_data->dev != NULL);

    if (SoapySDRDevice_setFrequency(dev_data->dev, SOAPY_SDR_RX, dev_data->channel, centerfreq, NULL) != 0) {
        log(LOG_ERR, "Failed to set frequency for SoapySDR device '%s': %s\n", dev_data->device_string, SoapySDRDevice_lastError());
        return -1;
    }
    return 0;
}

MODULE_EXPORT input_t* soapysdr_input_new() {
    soapysdr_dev_data_t* dev_data = (soapysdr_dev_data_t*)XCALLOC(1, sizeof(soapysdr_dev_data_t));
    dev_data->gain = -1.0;  // invalid default gain value
    dev_data->agc = false;
    memset(&dev_data->gains, 0, sizeof(dev_data->gains));
    dev_data->channel = 0;
    dev_data->antenna = NULL;
    /*	return &( input_t ){
                    .dev_data = dev_data,
                    .state = INPUT_UNKNOWN,
                    .sfmt = SFMT_U8,
                    .sample_rate = -1,
                    .parse_config = &soapysdr_parse_config,
                    .init = &soapysdr_init,
                    .run_rx_thread = &soapysdr_rx_thread,
                    .set_centerfreq = &soapysdr_set_centerfreq,
                    .stop = &soapysdr_stop
            }; */
    input_t* input = (input_t*)XCALLOC(1, sizeof(input_t));
    input->dev_data = dev_data;
    // invalid values as defaults
    input->state = INPUT_UNKNOWN;
    input->sfmt = SFMT_UNDEF;
    input->fullscale = 0.0f;
    input->bytes_per_sample = 0;
    input->sample_rate = -1;

    input->parse_config = &soapysdr_parse_config;
    input->init = &soapysdr_init;
    input->run_rx_thread = &soapysdr_rx_thread;
    input->set_centerfreq = &soapysdr_set_centerfreq;
    input->stop = NULL;
    return input;
}
