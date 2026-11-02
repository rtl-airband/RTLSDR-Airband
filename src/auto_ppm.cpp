/*
 * auto_ppm.cpp
 * Automatic frequency correction (ppm) from per-frequency carrier offsets.
 *
 * Copyright (C) 2026 Jared Schmall
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

#include "auto_ppm.h"
#include <time.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <thread>
#include "input-common.h"
#include "logging.h"
#include "rtl_airband.h"

using namespace std;

namespace {

// Estimates further than this from the median are treated as off-frequency transmitters (e.g.
// offset-carrier ATC sites at +/-2.5..7.5 kHz) rather than tuner error.
const double OUTLIER_WINDOW_PPM = 1.5;
const double MAX_SPREAD_PPM = 2.0;
// rtlsdr takes whole ppm; acting on less than this would flap around +/-0.5.
const double DEADBAND_PPM = 0.75;
// Slack before a post-step residual counts as having grown, so measurement noise around a
// near-zero residual doesn't trip the divergence guard.
const double GROWTH_TOLERANCE_PPM = 0.25;
const int DIVERGENCE_STEPS = 2;

double median_of(vector<double> v) {
    sort(v.begin(), v.end());
    size_t const n = v.size();
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

int clamp_int(long v, int limit) {
    return (int)max<long>(-limit, min<long>(limit, v));
}

bool read_int_param(libconfig::Setting& dev, const char* key, int def, int lo, int hi, int* out, string* error) {
    if (!dev.exists(key)) {
        *out = def;
        return true;
    }
    if (dev[key].getType() != libconfig::Setting::TypeInt) {
        *error = string(key) + " must be an integer";
        return false;
    }
    int const v = (int)dev[key];
    if (v < lo || v > hi) {
        *error = string(key) + " must be between " + to_string(lo) + " and " + to_string(hi);
        return false;
    }
    *out = v;
    return true;
}

double monotonic_now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

string json_text(const string& s) {
    string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if ((unsigned char)c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
            out += buf;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

string json_fixed(double v, const char* fmt) {
    char buf[48];
    snprintf(buf, sizeof(buf), fmt, v);
    return buf;
}

void reset_tracking(auto_ppm_device* ap) {
    ap->tripped = false;
    ap->disabled_reason.clear();
    ap->state.awaiting_check = false;
    ap->state.growing_steps = 0;
    ap->clamp_warned = false;
    ap->next_eval = 0.0;
}

}  // namespace

bool auto_ppm_parse_params(libconfig::Setting& dev, AutoPpmParams* out, string* error) {
    AutoPpmParams p;
    if (!read_int_param(dev, "auto_ppm_interval", p.interval_sec, 5, 86400, &p.interval_sec, error) || !read_int_param(dev, "auto_ppm_max_ppm", p.max_ppm, 1, AUTO_PPM_HARD_LIMIT, &p.max_ppm, error) ||
        !read_int_param(dev, "auto_ppm_max_step", p.max_step, 1, AUTO_PPM_HARD_LIMIT, &p.max_step, error)) {
        return false;
    }
    *out = p;
    return true;
}

AutoPpmDecision auto_ppm_decide(int current_correction, const vector<AutoPpmEstimate>& estimates, const AutoPpmParams& params, AutoPpmState* state) {
    AutoPpmDecision d;
    d.new_correction = current_correction;

    vector<double> plausible;
    for (const AutoPpmEstimate& e : estimates) {
        if (e.updates < params.min_updates || e.frequency <= 0) {
            continue;
        }
        d.eligible++;
        double const r = -(double)e.offset_hz / (double)e.frequency * 1e6;
        if (!isfinite(r) || fabs((double)current_correction + r) > AUTO_PPM_HARD_LIMIT) {
            continue;
        }
        plausible.push_back(r);
    }
    if (d.eligible == 0) {
        d.reason = "not enough measurements yet";
        return d;
    }
    if (plausible.empty()) {
        d.rejected = d.eligible;
        d.reason = "every estimate implies a correction beyond +/-" + to_string(AUTO_PPM_HARD_LIMIT) + " ppm";
        return d;
    }

    double const med = median_of(plausible);
    vector<double> kept;
    for (double r : plausible) {
        if (fabs(r - med) <= OUTLIER_WINDOW_PPM) {
            kept.push_back(r);
        }
    }
    d.contributing = (int)kept.size();
    d.rejected = d.eligible - d.contributing;
    if (kept.empty() || (plausible.size() > 2 && kept.size() < 2)) {
        d.reason = "channel estimates disagree";
        return d;
    }
    auto const mm = minmax_element(kept.begin(), kept.end());
    if (*mm.second - *mm.first > MAX_SPREAD_PPM) {
        d.reason = "channel estimates disagree";
        return d;
    }

    double const residual = median_of(kept);
    d.residual_valid = true;
    d.residual_ppm = residual;

    if (state->awaiting_check) {
        state->awaiting_check = false;
        if (fabs(residual) > state->residual_before_step + GROWTH_TOLERANCE_PPM) {
            state->growing_steps++;
        } else {
            state->growing_steps = 0;
        }
        if (state->growing_steps >= DIVERGENCE_STEPS) {
            state->growing_steps = 0;
            d.action = AUTO_PPM_REVERT;
            d.new_correction = clamp_int(state->base_correction, params.max_ppm);
            d.reason = "error grew after " + to_string(DIVERGENCE_STEPS) + " consecutive adjustments";
            return d;
        }
    }

    if (fabs(residual) < DEADBAND_PPM) {
        d.reason = "within deadband";
        return d;
    }
    long const target = lround((double)current_correction + residual);
    d.clamped = target > params.max_ppm || target < -params.max_ppm;

    long const delta = max<long>(-params.max_step, min<long>(params.max_step, target - current_correction));
    int const next = clamp_int(current_correction + delta, params.max_ppm);
    if (next == current_correction) {
        d.reason = "held at the auto_ppm_max_ppm limit";
        return d;
    }
    d.action = AUTO_PPM_ADJUST;
    d.new_correction = next;
    d.reason = "adjusting";
    state->awaiting_check = true;
    state->residual_before_step = fabs(residual);
    return d;
}

vector<AutoPpmEstimate> auto_ppm_collect_estimates(device_t* dev) {
    vector<AutoPpmEstimate> out;
    uint32_t const gen = dev->input->tuning_generation.load(memory_order_acquire);
    int const channel_count = dev->channel_count;
    for (int j = 0; j < channel_count; j++) {
        channel_t* channel = dev->channels + j;
        if (!channel->enabled || channel->removed || channel->pending_remove_request != -1 || channel->freqlist == NULL) {
            continue;
        }
        for (int k = 0; k < channel->freq_count; k++) {
            freq_t& f = channel->freqlist[k];
            if (f.ppm_offset_generation.load(memory_order_acquire) != gen) {
                continue;
            }
            uint32_t const updates = f.ppm_offset_updates.load(memory_order_acquire);
            if (updates == 0) {
                continue;
            }
            out.push_back({f.frequency, f.ppm_offset_hz.load(memory_order_relaxed), updates});
        }
    }
    return out;
}

void auto_ppm_tick(device_t* dev, int index, double now) {
    auto_ppm_device* ap = dev->auto_ppm;
    if (ap == NULL) {
        return;
    }
    lock_guard<mutex> guard(ap->lock);
    if (!ap->enabled || ap->tripped || now < ap->next_eval) {
        return;
    }
    ap->next_eval = now + ap->params.interval_sec;

    input_t* input = dev->input;
    if (input->state != INPUT_RUNNING || input->get_settings == NULL) {
        return;
    }
    input_settings_t settings;
    {
        lock_guard<recursive_mutex> control(input_control_mutex());
        if (input->get_settings(input, &settings) != 0) {
            return;
        }
    }
    int const current = (int)lround(settings.correction);

    AutoPpmDecision d = auto_ppm_decide(current, auto_ppm_collect_estimates(dev), ap->params, &ap->state);
    ap->last = d;
    ap->have_decision = true;

    if (d.clamped && !ap->clamp_warned) {
        log(LOG_WARNING, "Device #%d: auto_ppm wants %+.1f ppm beyond the current %d ppm, past the +/-%d ppm limit - check the dongle\n", index, d.residual_ppm, current, ap->params.max_ppm);
        ap->clamp_warned = true;
    } else if (d.residual_valid && !d.clamped) {
        ap->clamp_warned = false;
    }

    if (d.action == AUTO_PPM_ADJUST) {
        if (input_set_correction(input, d.new_correction) == 0) {
            ap->adjustments++;
            ap->last_adjust = now;
            log(LOG_INFO, "Device #%d: auto_ppm correction %d -> %d ppm (residual %+.2f, %d/%d channels)\n", index, current, d.new_correction, d.residual_ppm, d.contributing, d.eligible);
        } else {
            ap->state.awaiting_check = false;
            log(LOG_ERR, "Device #%d: auto_ppm failed to set correction %d ppm\n", index, d.new_correction);
        }
    } else if (d.action == AUTO_PPM_REVERT) {
        ap->tripped = true;
        ap->disabled_reason = d.reason;
        int const ret = input_set_correction(input, d.new_correction);
        log(LOG_ERR, "Device #%d: auto_ppm disabled: %s; %s correction to %d ppm. Fix the cause, then reload_diff or restart to re-enable\n", index, d.reason.c_str(),
            ret == 0 ? "reverted" : "FAILED to revert", d.new_correction);
    }
}

bool auto_ppm_active(device_t* dev) {
    auto_ppm_device* ap = dev->auto_ppm;
    if (ap == NULL) {
        return false;
    }
    lock_guard<mutex> guard(ap->lock);
    return ap->enabled && !ap->tripped;
}

bool auto_ppm_correction_allowed(device_t* dev, int correction) {
    auto_ppm_device* ap = dev->auto_ppm;
    if (ap == NULL) {
        return true;
    }
    lock_guard<mutex> guard(ap->lock);
    return !ap->enabled || abs(correction) <= ap->params.max_ppm;
}

bool auto_ppm_reload(device_t* dev, bool want_enabled, int config_correction, vector<string>* changes) {
    auto_ppm_device* ap = dev->auto_ppm;
    if (ap == NULL) {
        return true;
    }
    lock_guard<mutex> guard(ap->lock);
    bool const base_changed = config_correction != ap->state.base_correction;
    ap->state.base_correction = config_correction;
    if (want_enabled != ap->enabled) {
        ap->enabled = want_enabled;
        reset_tracking(ap);
        changes->push_back(string("auto_ppm -> ") + (want_enabled ? "true" : "false"));
    } else if (want_enabled && ap->tripped) {
        reset_tracking(ap);
        changes->push_back("auto_ppm re-armed");
    }
    return !ap->enabled || base_changed;
}

bool auto_ppm_metrics(device_t* dev, AutoPpmMetrics* out) {
    auto_ppm_device* ap = dev->auto_ppm;
    if (ap == NULL) {
        return false;
    }
    lock_guard<mutex> guard(ap->lock);
    out->residual_valid = ap->have_decision && ap->last.residual_valid;
    out->residual_ppm = ap->last.residual_ppm;
    out->adjustments = ap->adjustments;
    return true;
}

string auto_ppm_status_json(device_t* dev, double now) {
    auto_ppm_device* ap = dev->auto_ppm;
    if (ap == NULL) {
        return "null";
    }
    lock_guard<mutex> guard(ap->lock);
    string out = string("{\"enabled\":") + (ap->enabled ? "true" : "false");
    out += string(",\"active\":") + (ap->enabled && !ap->tripped ? "true" : "false");
    out += ",\"disabled_reason\":" + (ap->tripped ? json_text(ap->disabled_reason) : string("null"));
    bool const have = ap->have_decision;
    out += ",\"residual_ppm\":" + (have && ap->last.residual_valid ? json_fixed(ap->last.residual_ppm, "%.2f") : string("null"));
    out += ",\"eligible\":" + to_string(have ? ap->last.eligible : 0);
    out += ",\"contributing\":" + to_string(have ? ap->last.contributing : 0);
    out += ",\"rejected\":" + to_string(have ? ap->last.rejected : 0);
    out += string(",\"clamped\":") + (have && ap->last.clamped ? "true" : "false");
    out += ",\"reason\":" + (have ? json_text(ap->last.reason) : string("null"));
    out += ",\"adjustments\":" + to_string(ap->adjustments);
    out += ",\"last_adjust_age_s\":" + (ap->last_adjust >= 0.0 ? json_fixed(now - ap->last_adjust, "%.0f") : string("null"));
    out += ",\"base_correction\":" + to_string(ap->state.base_correction);
    out += ",\"max_ppm\":" + to_string(ap->params.max_ppm);
    out += ",\"max_step\":" + to_string(ap->params.max_step);
    out += ",\"interval\":" + to_string(ap->params.interval_sec);
    return out + "}";
}

namespace {

thread worker;
mutex worker_lock;
condition_variable worker_cv;
bool worker_stop = false;

void worker_main() {
    unique_lock<mutex> lk(worker_lock);
    while (!worker_stop) {
        worker_cv.wait_for(lk, chrono::seconds(1));
        if (worker_stop) {
            break;
        }
        lk.unlock();
        double const now = monotonic_now();
        for (int i = 0; i < device_count; i++) {
            auto_ppm_tick(devices + i, i, now);
        }
        lk.lock();
    }
}

}  // namespace

void auto_ppm_start() {
    bool any = false;
    for (int i = 0; i < device_count; i++) {
        any = any || devices[i].auto_ppm != NULL;
    }
    if (!any || worker.joinable()) {
        return;
    }
    worker_stop = false;
    worker = thread(worker_main);
    log(LOG_INFO, "auto_ppm: started\n");
}

void auto_ppm_shutdown() {
    if (!worker.joinable()) {
        return;
    }
    {
        lock_guard<mutex> lk(worker_lock);
        worker_stop = true;
    }
    worker_cv.notify_all();
    worker.join();
}

double auto_ppm_now() {
    return monotonic_now();
}
