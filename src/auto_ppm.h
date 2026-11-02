/*
 * auto_ppm.h
 * Automatic frequency correction (ppm) from the per-frequency carrier offsets measured by
 * device_stats.cpp's freq_offset_commit().
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

#ifndef _AUTO_PPM_H
#define _AUTO_PPM_H 1

#include <cstdint>
#include <libconfig.h++>
#include <mutex>
#include <string>
#include <vector>

struct device_t;

// No configuration can push a correction past this, in either direction. A real crystal this far
// off is broken hardware, not something to chase.
#define AUTO_PPM_HARD_LIMIT 100

struct AutoPpmParams {
    int interval_sec = 30;
    int max_ppm = AUTO_PPM_HARD_LIMIT;
    int max_step = 5;
    // Not configurable: offset updates (squelch-open batches) a frequency needs since the last
    // tuning change before it may vote.
    uint32_t min_updates = 20;

    bool operator==(const AutoPpmParams& o) const { return interval_sec == o.interval_sec && max_ppm == o.max_ppm && max_step == o.max_step && min_updates == o.min_updates; }
    bool operator!=(const AutoPpmParams& o) const { return !(*this == o); }
};

// Reads auto_ppm_interval / auto_ppm_max_ppm / auto_ppm_max_step from a device block, applying
// defaults for absent keys. Returns false with *error set when a value is out of range.
bool auto_ppm_parse_params(libconfig::Setting& dev, AutoPpmParams* out, std::string* error);

struct AutoPpmEstimate {
    int frequency;    // Hz
    float offset_hz;  // carrier minus configured frequency
    uint32_t updates;
};

enum AutoPpmAction { AUTO_PPM_HOLD, AUTO_PPM_ADJUST, AUTO_PPM_REVERT };

// Decision state carried between evaluations.
struct AutoPpmState {
    int base_correction = 0;  // the configured correction; where a tripped divergence guard reverts to
    bool awaiting_check = false;
    double residual_before_step = 0.0;  // |residual| that prompted the last step
    int growing_steps = 0;
};

struct AutoPpmDecision {
    AutoPpmAction action = AUTO_PPM_HOLD;
    int new_correction = 0;
    bool residual_valid = false;
    // Correction still needed, in ppm: the ideal correction is current + residual_ppm.
    double residual_ppm = 0.0;
    int eligible = 0;      // estimates with enough updates
    int contributing = 0;  // eligible estimates that survived plausibility and outlier rejection
    int rejected = 0;      // eligible - contributing
    bool clamped = false;  // the limit stopped the correction from going where the estimate wanted
    std::string reason;
};

// Pure decision step: no globals, no hardware. A carrier received offset_hz above its configured
// frequency means the tuner's LO (and, sharing the same crystal, the ADC clock) runs slow by
// offset_hz / frequency, so the residual is -offset_hz / frequency * 1e6 ppm.
AutoPpmDecision auto_ppm_decide(int current_correction, const std::vector<AutoPpmEstimate>& estimates, const AutoPpmParams& params, AutoPpmState* state);

// Per-device runtime state, allocated by parse_devices() when the device block has an auto_ppm
// key (device_t::auto_ppm is NULL otherwise). Everything below is guarded by lock. Lock order:
// lock, then input_control_mutex() (input-common.h), never the reverse.
struct auto_ppm_device {
    std::mutex lock;
    AutoPpmParams params;
    bool enabled = false;
    bool tripped = false;  // divergence guard fired; cleared by reload_diff or a restart
    std::string disabled_reason;
    AutoPpmState state;
    double next_eval = 0.0;  // monotonic seconds
    bool clamp_warned = false;
    bool have_decision = false;
    AutoPpmDecision last;
    uint64_t adjustments = 0;
    double last_adjust = -1.0;  // monotonic seconds, -1 = never
};

// Runs one evaluation of dev if its interval has elapsed at monotonic time now. Called by the
// auto_ppm thread; exposed for tests.
void auto_ppm_tick(device_t* dev, int index, double now);

// Estimates from every live channel frequency measured since the device's last tuning change.
std::vector<AutoPpmEstimate> auto_ppm_collect_estimates(device_t* dev);

// True when auto_ppm is configured, enabled and not tripped on dev.
bool auto_ppm_active(device_t* dev);

// Rejects a manual correction outside the device's auto_ppm limit. Returns true when allowed.
bool auto_ppm_correction_allowed(device_t* dev, int correction);

// reload_diff hook. Applies want_enabled (re-arming a tripped guard), records config_correction
// as the new revert target, and reports whether the config's correction should be (re)applied:
// always when auto_ppm ends up off, only on a changed value when it stays on. *changes gets a
// human-readable line per state change.
bool auto_ppm_reload(device_t* dev, bool want_enabled, int config_correction, std::vector<std::string>* changes);

// "auto_ppm" member of the control socket status object for dev ("null" when not configured).
std::string auto_ppm_status_json(device_t* dev, double now);

struct AutoPpmMetrics {
    bool residual_valid;
    double residual_ppm;
    uint64_t adjustments;
};
// Returns false when auto_ppm isn't configured on dev.
bool auto_ppm_metrics(device_t* dev, AutoPpmMetrics* out);

double auto_ppm_now();  // the monotonic clock auto_ppm_tick() and auto_ppm_status_json() expect

// Starts the evaluation thread if any device has auto_ppm configured.
void auto_ppm_start();
void auto_ppm_shutdown();

#endif /* _AUTO_PPM_H */
