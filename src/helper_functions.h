/*
 * helper_functions.h
 *
 * Copyright (C) 2023 charlie-foxtrot
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

#ifndef _HELPER_FUNCTIONS_H
#define _HELPER_FUNCTIONS_H

#include <sys/resource.h>  // struct rusage
#include <sys/time.h>      // struct timeval
#include <ctime>           // struct tm
#include <string>

// defined in rtl_airband.h; forward-declared here so this header doesn't need to pull it in
struct icecast_tx_tag_state;

namespace libconfig {
class Setting;
}

bool dir_exists(const std::string& dir_path);
bool file_exists(const std::string& file_path);
bool make_dir(const std::string& dir_path);
bool make_subdirs(const std::string& basedir, const std::string& subdirs);
std::string make_dated_subdirs(const std::string& basedir, const struct tm* time);

bool should_close_split_file(double duration_sec, double idle_sec, double split_min_file_time, double split_max_file_time, double split_max_idle_time);
bool should_close_split_file_max_time(double duration_sec, double split_max_file_time);
bool should_close_split_file_idle(double duration_sec, double idle_sec, double split_min_file_time, double split_max_idle_time);

bool valid_split_file_times(double split_min_file_time, double split_max_file_time, double split_max_idle_time);
extern const char* const split_file_times_constraint;
bool valid_stats_update_interval(double stats_update_interval);
bool valid_split_tx_drop_shorter_than(double split_tx_drop_shorter_than, double split_max_file_time);
extern const char* const split_tx_drop_shorter_than_constraint;
bool should_drop_split_tx(bool first_file, bool continues, double tx_span_sec, double split_tx_drop_shorter_than);
bool setting_as_double(const libconfig::Setting& setting, double* value);
bool setting_as_double_or(const libconfig::Setting& parent, const char* key, double fallback, double* value);
bool setting_as_bool_or(const libconfig::Setting& parent, const char* key, bool fallback, bool* value);
std::string make_icecast_mountpoint(const std::string& mountpoint);
double rusage_cpu_seconds(const struct rusage& ru);

// Space-separated list of what this build supports beyond upstream RTLSDR-Airband, printed by
// `rtl_airband -v` as a "Features: ..." line so tools (rtl-airband-panel) can tell this fork
// from a stock build and gate fork-only settings. Always starts with "fork"; the rest are
// fork capabilities plus the optional build components compiled in. Names are an interface:
// add new ones, never rename or drop one.
std::string fork_features();

// Builds the Icecast "song" tag content for a currently-active transmission: the configured
// label if set, else a "%.3f MHz" formatted frequency (matching the existing
// send_scan_freq_tags fallback in process_outputs()). Returns "" when has_signal is false,
// so callers can send that value straight through to clear the Now Playing tag.
std::string compute_tx_tag_content(bool has_signal, const char* label, int frequency_hz);

// Advances an icecast_tx_tag_state for one process_outputs() tick given the currently-desired
// tag content. Returns true (and fills out_value) exactly when out_value should be sent to
// Icecast now; false if nothing should be sent this tick. See icecast_tx_tag_state's comment
// in rtl_airband.h for the flap-handling rule this implements.
bool icecast_tx_tag_step(icecast_tx_tag_state* state, const std::string& desired_tag, const struct timeval& now, int delay_sec, std::string* out_value);

#endif /* _HELPER_FUNCTIONS_H */
