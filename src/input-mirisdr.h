/*
 *  input-mirisdr.h
 *  MiriSDR-specific declarations
 *
 *  Copyright (c) 2015-2021 Tomasz Lemiech <szpajder@gmail.com>
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
#include <mirisdr.h>  // mirisdr_dev_t
#define MIRISDR_BUFSIZE 320000
#define MIRISDR_DEFAULT_LIBUSB_BUFFER_COUNT 10
#define MIRISDR_DEFAULT_SAMPLE_RATE 2560000

typedef struct {
    mirisdr_dev_t* dev;  // pointer to libmirisdr device struct
    char* serial;        // dongle serial number
    int index;           // dongle index
    int correction;      // correction in Hertz (PPM correction is not supported by libmirisdr)
    int gain;            // gain in dB
    int bufcnt;          // libusb buffer count
} mirisdr_dev_data_t;
