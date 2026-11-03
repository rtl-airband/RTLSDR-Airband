/*
 *  input-rtlsdr.h
 *  RTLSDR-specific declarations
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
#include <rtl-sdr.h>  // rtlsdr_dev_t
#define RTLSDR_DEFAULT_SAMPLE_RATE 2560000

typedef struct {
    rtlsdr_dev_t* dev;  // pointer to librtlsdr device struct
    char* serial;       // dongle serial number
    int index;          // dongle index
    int correction;     // PPM correction
    int gain;           // gain in tenths of dB
    int bandwidth;      // tuner bandwidth in Hz, 0 = automatic (librtlsdr default)
    int bufcnt;         // libusb buffer count, 0 = rtlsdr_auto_usb_buf_count()
    int buf_len;        // libusb buffer length in bytes, 0 = rtlsdr_auto_usb_buf_len()
} rtlsdr_dev_data_t;
