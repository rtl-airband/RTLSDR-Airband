/*
 * test_filters.cpp
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

#include "filters.h"

using namespace std;

class FiltersTest : public TestBaseClass {
   protected:
    void SetUp(void) { TestBaseClass::SetUp(); }

    void TearDown(void) { TestBaseClass::TearDown(); }
};

TEST_F(FiltersTest, default_notch) {
    NotchFilter notch;
    EXPECT_FALSE(notch.enabled());
}

TEST_F(FiltersTest, default_lowpass) {
    LowpassFilter lowpass;
    EXPECT_FALSE(lowpass.enabled());
}
