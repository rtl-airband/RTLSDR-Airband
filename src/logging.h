/*
 * logging.h
 *
 * Copyright (C) 2022-2023 charlie-foxtrot
 * Copyright (c) 2015-2022 Tomasz Lemiech <szpajder@gmail.com>
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

#ifndef _LOGGING_H
#define _LOGGING_H 1

#include <syslog.h>  // LOG_ERR
#include <cstdio>    // FILE

#define nop() \
    do {      \
    } while (0)
#define UNUSED(x) (void)(x)

#ifdef DEBUG
#define DEBUG_PATH "rtl_airband_debug.log"
#define debug_print(fmt, ...)                                 \
    do {                                                      \
        fprintf(debugf, "%s(): " fmt, __func__, __VA_ARGS__); \
        fflush(debugf);                                       \
    } while (0)
#define debug_bulk_print(fmt, ...)                            \
    do {                                                      \
        fprintf(debugf, "%s(): " fmt, __func__, __VA_ARGS__); \
    } while (0)
#else
#define debug_print(fmt, ...) nop()
#define debug_bulk_print(fmt, ...) nop()
#endif /* DEBUG */

enum LogDestination { SYSLOG, STDERR, NONE };
extern LogDestination log_destination;
extern FILE* debugf;

void error();
void init_debug(const char* file);
void close_debug();
void log(int priority, const char* format, ...);

#endif /* _LOGGING_H */
