/*
 * logging.cpp
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

#include <stdarg.h>  // va_start() / va_end()
#include <cstdio>    // fopen()
#include <cstring>   // strerror()
#include <iostream>  // cerr()

#include "logging.h"

LogDestination log_destination = SYSLOG;
FILE* debugf = NULL;

void error() {
    close_debug();
    _Exit(1);
}

void init_debug(const char* file) {
#ifdef DEBUG
    if (!file)
        return;
    if ((debugf = fopen(file, "a")) == NULL) {
        std::cerr << "Could not open debug file " << file << ": " << strerror(errno) << "\n";
        error();
    }
#else
    UNUSED(file);
#endif /* DEBUG */
}

void close_debug() {
#ifdef DEBUG
    if (!debugf)
        return;
    fclose(debugf);
#endif /* DEBUG */
}

void log(int priority, const char* format, ...) {
    va_list args;
    va_start(args, format);
    switch (log_destination) {
        case SYSLOG:
            vsyslog(priority, format, args);
            break;
        case STDERR:
            vfprintf(stderr, format, args);
            break;
        case NONE:
            break;
    }
    va_end(args);
}
