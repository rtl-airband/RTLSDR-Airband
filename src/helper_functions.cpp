/*
 * helper_functions.cpp
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

#include <sys/stat.h>  // struct stat, S_ISDIR
#include <cstddef>     // size_t
#include <cstring>     // strerror

#include "helper_functions.h"
#include "logging.h"

using namespace std;

bool dir_exists(const string& dir_path) {
    struct stat st;
    return (stat(dir_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
}

bool file_exists(const string& file_path) {
    struct stat st;
    return (stat(file_path.c_str(), &st) == 0 && S_ISREG(st.st_mode));
}

bool make_dir(const string& dir_path) {
    if (dir_exists(dir_path)) {
        return true;
    }

    if (mkdir(dir_path.c_str(), 0755) != 0) {
        log(LOG_ERR, "Could not create directory %s: %s\n", dir_path.c_str(), strerror(errno));
        return false;
    }
    return true;
}

bool make_subdirs(const string& basedir, const string& subdirs) {
    // if final directory exists then nothing to do
    const string delim = "/";
    const string final_path = basedir + delim + subdirs;
    if (dir_exists(final_path)) {
        return true;
    }

    // otherwise scan through subdirs for each slash and make each directory.  start with index of 0
    // to create basedir incase that doesn't exist
    size_t index = 0;
    while (index != string::npos) {
        if (!make_dir(basedir + delim + subdirs.substr(0, index))) {
            return false;
        }
        index = subdirs.find_first_of(delim, index + 1);
    }

    make_dir(final_path);
    return dir_exists(final_path);
}

string make_dated_subdirs(const string& basedir, const struct tm* time) {
    // use the time to build the date subdirectories
    char date_path[11];
    strftime(date_path, sizeof(date_path), "%Y/%m/%d", time);
    const string date_path_str = string(date_path);

    // make all the subdirectories, and return the full path if successful
    if (make_subdirs(basedir, date_path_str)) {
        return basedir + "/" + date_path_str;
    }

    // on any error return empty string
    return "";
}
