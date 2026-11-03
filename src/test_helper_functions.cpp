/*
 * test_output.cpp
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

#include "test_base_class.h"

#include <cstring>
#include <libconfig.h++>

#include "helper_functions.h"
#include "rtl_airband.h"

using namespace std;

class HelperFunctionsTest : public TestBaseClass {
   protected:
    void SetUp(void) { TestBaseClass::SetUp(); }

    void create_file(const string& filepath) {
        fclose(fopen(filepath.c_str(), "wb"));
        EXPECT_TRUE(file_exists(filepath));
    }
};

TEST_F(HelperFunctionsTest, dir_exists_true) {
    EXPECT_TRUE(dir_exists(temp_dir));
}

TEST_F(HelperFunctionsTest, dir_exists_false) {
    EXPECT_FALSE(dir_exists("/not/a/real/dir"));
}

TEST_F(HelperFunctionsTest, dir_exists_not_dir) {
    string file_in_dir = temp_dir + "/some_file";
    create_file(file_in_dir);
    EXPECT_FALSE(dir_exists(file_in_dir));
}

TEST_F(HelperFunctionsTest, file_exists_true) {
    string file_in_dir = temp_dir + "/some_file";
    create_file(file_in_dir);
    EXPECT_TRUE(file_exists(file_in_dir));
}

TEST_F(HelperFunctionsTest, file_exists_false) {
    EXPECT_FALSE(file_exists(temp_dir + "/nothing"));
}

TEST_F(HelperFunctionsTest, file_exists_not_file) {
    EXPECT_FALSE(file_exists(temp_dir));
    EXPECT_TRUE(dir_exists(temp_dir));
}

TEST_F(HelperFunctionsTest, make_dir_normal) {
    const string dir_path = temp_dir + "/a";
    EXPECT_FALSE(dir_exists(dir_path));
    EXPECT_TRUE(make_dir(dir_path));
    EXPECT_TRUE(dir_exists(dir_path));
}

TEST_F(HelperFunctionsTest, make_dir_exists) {
    EXPECT_TRUE(dir_exists(temp_dir));
    EXPECT_TRUE(make_dir(temp_dir));
    EXPECT_TRUE(dir_exists(temp_dir));
}

TEST_F(HelperFunctionsTest, make_dir_empty) {
    EXPECT_FALSE(make_dir(""));
}

TEST_F(HelperFunctionsTest, make_dir_fail) {
    EXPECT_FALSE(make_dir("/this/path/does/not/exist"));
}

TEST_F(HelperFunctionsTest, make_dir_file_in_the_way) {
    const string file_path = temp_dir + "/some_file";
    create_file(file_path);
    EXPECT_FALSE(make_dir(file_path));
}

TEST_F(HelperFunctionsTest, make_subdirs_exists) {
    EXPECT_TRUE(dir_exists(temp_dir));
    EXPECT_TRUE(make_subdirs(temp_dir, ""));
    EXPECT_TRUE(dir_exists(temp_dir));
}

TEST_F(HelperFunctionsTest, make_subdirs_one_subdir) {
    const string path = "bob";
    EXPECT_FALSE(dir_exists(temp_dir + "/" + path));
    EXPECT_TRUE(make_subdirs(temp_dir, path));
    EXPECT_TRUE(dir_exists(temp_dir + "/" + path));
}

TEST_F(HelperFunctionsTest, make_subdirs_multiple_subdir) {
    const string path = "bob/joe/sam";
    EXPECT_FALSE(dir_exists(temp_dir + "/" + path));
    EXPECT_TRUE(make_subdirs(temp_dir, path));
    EXPECT_TRUE(dir_exists(temp_dir + "/" + path));
}

TEST_F(HelperFunctionsTest, make_subdirs_file_in_the_way) {
    const string file_in_dir = temp_dir + "/some_file";
    create_file(file_in_dir);
    EXPECT_TRUE(file_exists(file_in_dir));
    EXPECT_FALSE(make_subdirs(temp_dir, "some_file/some_dir"));
    EXPECT_FALSE(dir_exists(file_in_dir));
    EXPECT_TRUE(file_exists(file_in_dir));
}

TEST_F(HelperFunctionsTest, make_icecast_mountpoint_normal) {
    EXPECT_EQ(make_icecast_mountpoint("stream.mp3"), "/stream.mp3");
}

TEST_F(HelperFunctionsTest, make_icecast_mountpoint_empty) {
    EXPECT_EQ(make_icecast_mountpoint(""), "/");
}

TEST_F(HelperFunctionsTest, make_icecast_mountpoint_longer_than_fixed_buffer_used_to_allow) {
    // output.cpp used to build this string into a fixed char[100] stack buffer via
    // sprintf(), which would overflow for a mountpoint this long. Confirm a long value
    // is preserved in full rather than truncated or overflowing.
    const string long_mountpoint(500, 'a');
    EXPECT_EQ(make_icecast_mountpoint(long_mountpoint), "/" + long_mountpoint);
}

TEST_F(HelperFunctionsTest, make_subdirs_create_base) {
    EXPECT_FALSE(dir_exists(temp_dir + "/base_dir/a"));
    EXPECT_TRUE(make_subdirs(temp_dir + "/base_dir", "a"));
    EXPECT_TRUE(dir_exists(temp_dir + "/base_dir/a"));
}

TEST_F(HelperFunctionsTest, make_subdirs_extra_slashes) {
    EXPECT_FALSE(dir_exists(temp_dir + "/a/b/c/d"));
    EXPECT_TRUE(make_subdirs(temp_dir, "///a/b////c///d"));
    EXPECT_TRUE(dir_exists(temp_dir + "/a/b/c/d"));
}

TEST_F(HelperFunctionsTest, make_dated_subdirs_normal) {
    struct tm time_struct;

    strptime("2010-3-7", "%Y-%m-%d", &time_struct);

    const string dir_path = temp_dir + "/2010/03/07";

    EXPECT_FALSE(dir_exists(dir_path));
    EXPECT_EQ(make_dated_subdirs(temp_dir, &time_struct), dir_path);
    EXPECT_TRUE(dir_exists(dir_path));
}

TEST_F(HelperFunctionsTest, make_dated_subdirs_fail) {
    struct tm time_struct;

    strptime("2010-3-7", "%Y-%m-%d", &time_struct);

    EXPECT_EQ(make_dated_subdirs("/invalid/base/dir", &time_struct), "");
}

TEST_F(HelperFunctionsTest, rusage_cpu_seconds_combines_user_and_system_time) {
    struct rusage ru;
    memset(&ru, 0, sizeof(ru));
    ru.ru_utime.tv_sec = 2;
    ru.ru_utime.tv_usec = 500000;
    ru.ru_stime.tv_sec = 1;
    ru.ru_stime.tv_usec = 250000;

    EXPECT_DOUBLE_EQ(rusage_cpu_seconds(ru), 3.75);
}

TEST_F(HelperFunctionsTest, rusage_cpu_seconds_zero) {
    struct rusage ru;
    memset(&ru, 0, sizeof(ru));

    EXPECT_DOUBLE_EQ(rusage_cpu_seconds(ru), 0.0);
}

TEST_F(HelperFunctionsTest, compute_tx_tag_content_no_signal_is_empty) {
    EXPECT_EQ(compute_tx_tag_content(false, "Fire Dispatch", 154265000), "");
}

TEST_F(HelperFunctionsTest, compute_tx_tag_content_uses_label) {
    EXPECT_EQ(compute_tx_tag_content(true, "Fire Dispatch", 154265000), "Fire Dispatch");
}

TEST_F(HelperFunctionsTest, compute_tx_tag_content_falls_back_to_frequency_without_label) {
    EXPECT_EQ(compute_tx_tag_content(true, NULL, 154265000), "154.265 MHz");
}

namespace {
struct timeval make_tv(long sec) {
    struct timeval tv;
    tv.tv_sec = sec;
    tv.tv_usec = 0;
    return tv;
}
}  // namespace

TEST_F(HelperFunctionsTest, icecast_tx_tag_step_noop_when_already_matches_applied) {
    icecast_tx_tag_state state;
    string out;
    EXPECT_FALSE(icecast_tx_tag_step(&state, "", make_tv(100), 3, &out));
    EXPECT_FALSE(state.pending);
    EXPECT_EQ(state.applied, "");
}

TEST_F(HelperFunctionsTest, icecast_tx_tag_step_defers_first_change_until_deadline) {
    icecast_tx_tag_state state;
    string out;

    // transmission starts - change is queued but not applied yet
    EXPECT_FALSE(icecast_tx_tag_step(&state, "Fire Dispatch", make_tv(100), 3, &out));
    EXPECT_TRUE(state.pending);
    EXPECT_EQ(state.applied, "");

    // deadline not reached yet
    EXPECT_FALSE(icecast_tx_tag_step(&state, "Fire Dispatch", make_tv(102), 3, &out));
    EXPECT_EQ(state.applied, "");

    // deadline reached - applies now
    EXPECT_TRUE(icecast_tx_tag_step(&state, "Fire Dispatch", make_tv(103), 3, &out));
    EXPECT_EQ(out, "Fire Dispatch");
    EXPECT_EQ(state.applied, "Fire Dispatch");
    EXPECT_FALSE(state.pending);
}

TEST_F(HelperFunctionsTest, icecast_tx_tag_step_zero_delay_applies_immediately) {
    icecast_tx_tag_state state;
    string out;
    EXPECT_TRUE(icecast_tx_tag_step(&state, "Fire Dispatch", make_tv(100), 0, &out));
    EXPECT_EQ(out, "Fire Dispatch");
    EXPECT_EQ(state.applied, "Fire Dispatch");
}

TEST_F(HelperFunctionsTest, icecast_tx_tag_step_change_while_pending_updates_value_not_deadline) {
    icecast_tx_tag_state state;
    string out;

    // first change starts a 3s deferred window
    EXPECT_FALSE(icecast_tx_tag_step(&state, "A", make_tv(100), 3, &out));

    // a second, different desired value arrives before the deadline - updates the pending
    // value but must not push the deadline back
    EXPECT_FALSE(icecast_tx_tag_step(&state, "B", make_tv(101), 3, &out));
    EXPECT_EQ(state.pending_value, "B");

    // original deadline (100+3=103) still governs, not a new one relative to t=101
    EXPECT_FALSE(icecast_tx_tag_step(&state, "B", make_tv(102), 3, &out));
    EXPECT_TRUE(icecast_tx_tag_step(&state, "B", make_tv(103), 3, &out));
    EXPECT_EQ(out, "B");
    EXPECT_EQ(state.applied, "B");
}

TEST_F(HelperFunctionsTest, icecast_tx_tag_step_revert_to_applied_while_pending_cancels) {
    icecast_tx_tag_state state;
    string out;

    // squelch opens...
    EXPECT_FALSE(icecast_tx_tag_step(&state, "A", make_tv(100), 3, &out));
    EXPECT_TRUE(state.pending);

    // ...and closes again before the deferred window elapses - reverting to the
    // already-applied value ("") must cancel the pending change outright
    EXPECT_FALSE(icecast_tx_tag_step(&state, "", make_tv(101), 3, &out));
    EXPECT_FALSE(state.pending);

    // confirm no delayed firing past the original deadline
    EXPECT_FALSE(icecast_tx_tag_step(&state, "", make_tv(104), 3, &out));
    EXPECT_EQ(state.applied, "");
}

TEST_F(HelperFunctionsTest, icecast_tx_tag_step_steady_state_repeats_are_noops) {
    icecast_tx_tag_state state;
    string out;

    EXPECT_TRUE(icecast_tx_tag_step(&state, "A", make_tv(100), 0, &out));
    EXPECT_EQ(state.applied, "A");

    EXPECT_FALSE(icecast_tx_tag_step(&state, "A", make_tv(105), 0, &out));
    EXPECT_EQ(state.applied, "A");
}

TEST_F(HelperFunctionsTest, make_dated_subdirs_some_exist) {
    struct tm time_struct;

    const string dir_through_month = temp_dir + "/2010/03/";

    strptime("2010-3-7", "%Y-%m-%d", &time_struct);
    EXPECT_EQ(make_dated_subdirs(temp_dir, &time_struct), dir_through_month + "07");

    EXPECT_TRUE(dir_exists(dir_through_month));
    EXPECT_FALSE(dir_exists(dir_through_month + "08"));

    strptime("2010-3-8", "%Y-%m-%d", &time_struct);
    EXPECT_EQ(make_dated_subdirs(temp_dir, &time_struct), dir_through_month + "08");
    EXPECT_TRUE(dir_exists(dir_through_month + "08"));
}

TEST_F(HelperFunctionsTest, should_close_split_file_max_duration) {
    EXPECT_TRUE(should_close_split_file(6001.0, 0.0, 1.0, 6000.0, 0.5));
}

TEST_F(HelperFunctionsTest, should_close_split_file_duration_at_max) {
    EXPECT_FALSE(should_close_split_file(6000.0, 0.0, 1.0, 6000.0, 0.5));
}

TEST_F(HelperFunctionsTest, should_close_split_file_idle_after_min_duration) {
    EXPECT_TRUE(should_close_split_file(10.0, 0.6, 1.0, 6000.0, 0.5));
}

TEST_F(HelperFunctionsTest, should_close_split_file_idle_at_max) {
    EXPECT_FALSE(should_close_split_file(10.0, 0.5, 1.0, 6000.0, 0.5));
}

TEST_F(HelperFunctionsTest, should_close_split_file_idle_before_min_duration) {
    EXPECT_FALSE(should_close_split_file(0.5, 10.0, 1.0, 6000.0, 0.5));
}

TEST_F(HelperFunctionsTest, should_close_split_file_duration_at_min) {
    EXPECT_FALSE(should_close_split_file(1.0, 10.0, 1.0, 6000.0, 0.5));
}

TEST_F(HelperFunctionsTest, should_close_split_file_no_idle) {
    EXPECT_FALSE(should_close_split_file(10.0, 0.1, 1.0, 6000.0, 0.5));
}

TEST_F(HelperFunctionsTest, should_close_split_file_max_time_past_max) {
    EXPECT_TRUE(should_close_split_file_max_time(6001.0, 6000.0));
}

TEST_F(HelperFunctionsTest, should_close_split_file_max_time_at_max) {
    EXPECT_FALSE(should_close_split_file_max_time(6000.0, 6000.0));
}

TEST_F(HelperFunctionsTest, should_close_split_file_max_time_under_max) {
    EXPECT_FALSE(should_close_split_file_max_time(10.0, 6000.0));
}

TEST_F(HelperFunctionsTest, should_close_split_file_idle_past_max_idle) {
    EXPECT_TRUE(should_close_split_file_idle(10.0, 0.6, 1.0, 0.5));
}

TEST_F(HelperFunctionsTest, should_close_split_file_idle_at_max_idle) {
    EXPECT_FALSE(should_close_split_file_idle(10.0, 0.5, 1.0, 0.5));
}

TEST_F(HelperFunctionsTest, should_close_split_file_idle_at_min_duration) {
    EXPECT_FALSE(should_close_split_file_idle(1.0, 10.0, 1.0, 0.5));
}

TEST_F(HelperFunctionsTest, should_close_split_file_idle_under_min_duration) {
    EXPECT_FALSE(should_close_split_file_idle(0.5, 10.0, 1.0, 0.5));
}

TEST_F(HelperFunctionsTest, should_close_split_file_idle_ignores_max_time) {
    EXPECT_FALSE(should_close_split_file_idle(6001.0, 0.1, 1.0, 0.5));
}

TEST_F(HelperFunctionsTest, valid_split_file_times_ok) {
    EXPECT_TRUE(valid_split_file_times(1.0, 60.0, 0.5));
}

TEST_F(HelperFunctionsTest, valid_split_file_times_min_too_small) {
    EXPECT_FALSE(valid_split_file_times(0.5, 60.0, 0.5));
}

TEST_F(HelperFunctionsTest, valid_split_file_times_max_not_greater) {
    EXPECT_FALSE(valid_split_file_times(60.0, 60.0, 0.5));
    EXPECT_FALSE(valid_split_file_times(60.0, 30.0, 0.5));
}

TEST_F(HelperFunctionsTest, valid_split_file_times_idle_not_positive) {
    EXPECT_FALSE(valid_split_file_times(1.0, 60.0, 0.0));
    EXPECT_FALSE(valid_split_file_times(1.0, 60.0, -1.0));
}

TEST_F(HelperFunctionsTest, valid_stats_update_interval_positive) {
    EXPECT_TRUE(valid_stats_update_interval(15.0));
    EXPECT_TRUE(valid_stats_update_interval(0.5));
}

TEST_F(HelperFunctionsTest, valid_stats_update_interval_not_positive) {
    EXPECT_FALSE(valid_stats_update_interval(0.0));
    EXPECT_FALSE(valid_stats_update_interval(-1.0));
}

TEST_F(HelperFunctionsTest, setting_as_double_int) {
    libconfig::Config config;
    libconfig::Setting& s = config.getRoot().add("value", libconfig::Setting::TypeInt);
    s = 5;
    double value = 0.0;
    EXPECT_TRUE(setting_as_double(s, &value));
    EXPECT_DOUBLE_EQ(value, 5.0);
}

TEST_F(HelperFunctionsTest, setting_as_double_float) {
    libconfig::Config config;
    libconfig::Setting& s = config.getRoot().add("value", libconfig::Setting::TypeFloat);
    // 0.3 has no exact float representation, so this fails if the value is narrowed on the way out
    s = 0.3;
    double value = 0.0;
    EXPECT_TRUE(setting_as_double(s, &value));
    EXPECT_DOUBLE_EQ(value, 0.3);
}

TEST_F(HelperFunctionsTest, setting_as_double_bool_rejected) {
    libconfig::Config config;
    libconfig::Setting& s = config.getRoot().add("value", libconfig::Setting::TypeBoolean);
    s = true;
    double value = 0.0;
    EXPECT_FALSE(setting_as_double(s, &value));
}

TEST_F(HelperFunctionsTest, setting_as_double_string_rejected) {
    libconfig::Config config;
    libconfig::Setting& s = config.getRoot().add("value", libconfig::Setting::TypeString);
    s = "5.0";
    double value = 0.0;
    EXPECT_FALSE(setting_as_double(s, &value));
}

TEST_F(HelperFunctionsTest, setting_as_double_or_absent_uses_fallback) {
    libconfig::Config config;
    double value = 0.0;
    EXPECT_TRUE(setting_as_double_or(config.getRoot(), "split_max_idle_time", 0.5, &value));
    EXPECT_DOUBLE_EQ(value, 0.5);
}

TEST_F(HelperFunctionsTest, setting_as_double_or_present_overrides_fallback) {
    libconfig::Config config;
    config.getRoot().add("split_max_file_time", libconfig::Setting::TypeInt) = 1800;
    double value = 0.0;
    EXPECT_TRUE(setting_as_double_or(config.getRoot(), "split_max_file_time", 3600.0, &value));
    EXPECT_DOUBLE_EQ(value, 1800.0);
}

TEST_F(HelperFunctionsTest, setting_as_double_or_rejects_non_number) {
    libconfig::Config config;
    config.getRoot().add("split_max_idle_time", libconfig::Setting::TypeString) = "2.0";
    double value = 0.0;
    EXPECT_FALSE(setting_as_double_or(config.getRoot(), "split_max_idle_time", 0.5, &value));
}

TEST_F(HelperFunctionsTest, setting_as_bool_or_absent_uses_fallback) {
    libconfig::Config config;
    bool value = false;
    EXPECT_TRUE(setting_as_bool_or(config.getRoot(), "split_include_transmission_start", true, &value));
    EXPECT_TRUE(value);
}

TEST_F(HelperFunctionsTest, setting_as_bool_or_present_overrides_fallback) {
    libconfig::Config config;
    config.getRoot().add("split_include_transmission_start", libconfig::Setting::TypeBoolean) = false;
    bool value = true;
    EXPECT_TRUE(setting_as_bool_or(config.getRoot(), "split_include_transmission_start", true, &value));
    EXPECT_FALSE(value);
}

TEST_F(HelperFunctionsTest, setting_as_bool_or_rejects_non_bool) {
    libconfig::Config config;
    config.getRoot().add("int_value", libconfig::Setting::TypeInt) = 1;
    config.getRoot().add("string_value", libconfig::Setting::TypeString) = "true";
    bool value = false;
    EXPECT_FALSE(setting_as_bool_or(config.getRoot(), "int_value", false, &value));
    EXPECT_FALSE(setting_as_bool_or(config.getRoot(), "string_value", false, &value));
}

TEST_F(HelperFunctionsTest, fork_features_names_the_fork_and_its_capabilities) {
    std::string const features = " " + fork_features() + " ";
    EXPECT_EQ(fork_features().rfind("fork ", 0), 0u);
    for (const char* name : {"control_socket", "device_status", "reserve", "mixer_remote", "send_tx_tags", "stats_http", "json_log", "post_write_script", "auto_ppm", "usb_chunk_sizing"}) {
        EXPECT_NE(features.find(std::string(" ") + name + " "), std::string::npos) << name;
    }
#ifdef NFM
    EXPECT_NE(features.find(" nfm "), std::string::npos);
#else
    EXPECT_EQ(features.find(" nfm "), std::string::npos);
#endif
}

TEST_F(HelperFunctionsTest, valid_split_tx_drop_shorter_than_disabled) {
    EXPECT_TRUE(valid_split_tx_drop_shorter_than(0.0, 60.0));
}

TEST_F(HelperFunctionsTest, valid_split_tx_drop_shorter_than_below_max) {
    EXPECT_TRUE(valid_split_tx_drop_shorter_than(59.9, 60.0));
}

TEST_F(HelperFunctionsTest, valid_split_tx_drop_shorter_than_at_max) {
    EXPECT_FALSE(valid_split_tx_drop_shorter_than(60.0, 60.0));
}

TEST_F(HelperFunctionsTest, valid_split_tx_drop_shorter_than_negative) {
    EXPECT_FALSE(valid_split_tx_drop_shorter_than(-0.1, 60.0));
}

TEST_F(HelperFunctionsTest, should_drop_split_tx_short_whole_transmission) {
    EXPECT_TRUE(should_drop_split_tx(true, false, 0.4, 1.0));
}

TEST_F(HelperFunctionsTest, should_drop_split_tx_span_at_limit) {
    EXPECT_FALSE(should_drop_split_tx(true, false, 1.0, 1.0));
}

TEST_F(HelperFunctionsTest, should_drop_split_tx_long_transmission) {
    EXPECT_FALSE(should_drop_split_tx(true, false, 3.0, 1.0));
}

TEST_F(HelperFunctionsTest, should_drop_split_tx_disabled) {
    EXPECT_FALSE(should_drop_split_tx(true, false, 0.0, 0.0));
}

TEST_F(HelperFunctionsTest, should_drop_split_tx_keeps_first_part_that_continues) {
    EXPECT_FALSE(should_drop_split_tx(true, true, 0.4, 1.0));
}

TEST_F(HelperFunctionsTest, should_drop_split_tx_keeps_short_later_part) {
    EXPECT_FALSE(should_drop_split_tx(false, false, 0.4, 1.0));
}
