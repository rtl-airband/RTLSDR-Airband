/*
 * test_base_class.h
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

#ifndef _TEST_BASE_CLASS_H
#define _TEST_BASE_CLASS_H

#include <gtest/gtest.h>

#include <string>

class TestBaseClass : public ::testing::Test {
   protected:
    void SetUp(void);
    void TearDown(void);

    std::string temp_dir;
};

#endif /* _TEST_BASE_CLASS_H */
