/*
 * test_base_class.cpp
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

#include <dirent.h>

#include "logging.h"

#include "test_base_class.h"

using namespace std;

void delete_directory(const string& root) {
    DIR* dp = NULL;

    dp = opendir(root.c_str());
    if (dp == NULL) {
        cerr << "Error opening directory " << root << endl;
        return;
    }

    string current_dir = ".";
    string parent_dir = "..";

    struct dirent* entry = NULL;
    while ((entry = readdir(dp))) {
        if (current_dir.compare(entry->d_name) == 0 || parent_dir.compare(entry->d_name) == 0) {
            continue;
        }

        struct stat info;
        string filepath = root + "/" + string(entry->d_name);

        if (stat(filepath.c_str(), &info) != 0) {
            cerr << "Error getting info on " << filepath.c_str() << ": " << strerror(errno) << endl;
            continue;
        }

        if (S_ISDIR(info.st_mode)) {
            delete_directory(filepath);
        } else {
            unlink(filepath.c_str());
        }
    }

    closedir(dp);
    rmdir(root.c_str());
}

string make_temp_dir(void) {
    char temp_path_template[] = "/tmp/temp_unittest_dir_XXXXXX";
    if (mkdtemp(temp_path_template) == NULL) {
        cerr << "Error making temp dir for test files: " << strerror(errno) << endl;
        return "";
    }
    return string(temp_path_template);
}

void TestBaseClass::SetUp(void) {
    ::testing::Test::SetUp();

    // setup debug log file for each test
    temp_dir = make_temp_dir();
    ASSERT_FALSE(temp_dir.empty());
    string debug_filepath = temp_dir + "/debug_file.log";
    init_debug(debug_filepath.c_str());

    // point logging to stderr
    log_destination = STDERR;
}

void TestBaseClass::TearDown(void) {
    ::testing::Test::TearDown();
    close_debug();
    delete_directory(temp_dir);
}

TEST(TestHelpers, make_temp_dir) {
    // make a temp dir
    string temp_dir = make_temp_dir();

    // path should not be empty string
    ASSERT_FALSE(temp_dir.empty());

    // a directory should exist at the path
    struct stat info;
    ASSERT_EQ(stat(temp_dir.c_str(), &info), 0);
    EXPECT_TRUE(S_ISDIR(info.st_mode));

    delete_directory(temp_dir);
}

TEST(TestHelpers, delete_directory) {
    // make a temp dir
    string temp_dir = make_temp_dir();
    ASSERT_FALSE(temp_dir.empty());

    // build a bunch of nested sub-dirs and files
    string path = temp_dir;
    for (int i = 0; i < 5; ++i) {
        path = path + "/sub_dir";
        mkdir(path.c_str(), 0777);

        string filename = path + "/some_file";
        fclose(fopen(filename.c_str(), "w"));
    }

    // last sub-dir should exist and be a directory
    struct stat info;
    ASSERT_EQ(stat(path.c_str(), &info), 0);
    EXPECT_TRUE(S_ISDIR(info.st_mode));

    // last sub-dir should have a file in it
    string filename = path + "/some_file";
    ASSERT_EQ(stat(filename.c_str(), &info), 0);
    EXPECT_TRUE(S_ISREG(info.st_mode));

    // delete the root temp dir
    delete_directory(temp_dir);

    // root temp dir should no longer exist
    ASSERT_NE(stat(temp_dir.c_str(), &info), 0);
}
