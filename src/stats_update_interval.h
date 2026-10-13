/*
 * stats_update_interval.h
 * Statistics file update interval configuration
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

#ifndef RTL_AIRBAND_STATS_UPDATE_INTERVAL_H
#define RTL_AIRBAND_STATS_UPDATE_INTERVAL_H

constexpr int DEFAULT_STATS_UPDATE_INTERVAL = 15;

inline bool is_valid_stats_update_interval(int interval)
{
	return interval > 0;
}

#endif /* RTL_AIRBAND_STATS_UPDATE_INTERVAL_H */
