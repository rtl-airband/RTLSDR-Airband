/*
 * test_stats_update_interval.cpp
 *
 * Copyright (C) 2026 OA6DXV
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <gtest/gtest.h>

#include "stats_update_interval.h"

TEST(StatsUpdateIntervalTest, default_is_fifteen_seconds)
{
	EXPECT_EQ(DEFAULT_STATS_UPDATE_INTERVAL, 15);
}

TEST(StatsUpdateIntervalTest, accepts_positive_values)
{
	EXPECT_TRUE(is_valid_stats_update_interval(1));
	EXPECT_TRUE(is_valid_stats_update_interval(15));
}

TEST(StatsUpdateIntervalTest, rejects_zero_and_negative_values)
{
	EXPECT_FALSE(is_valid_stats_update_interval(0));
	EXPECT_FALSE(is_valid_stats_update_interval(-1));
}
