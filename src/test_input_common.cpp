/*
 * test_input_common.cpp
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

#include <gtest/gtest.h>

#include "input-common.h"
#include "rtl_airband.h"

// librtlsdr's total USB buffering before this change (10 x 320000 bytes); the auto count must not
// go below it.
static const size_t LEGACY_TOTAL_USB_BYTES = (size_t)RTLSDR_DEFAULT_LIBUSB_BUFFER_COUNT * RTLSDR_MAX_AUTO_USB_BUF_LEN;

// Bytes of U8 I/Q in one audio batch (WAVE_BATCH output samples) at this sample rate.
static size_t batch_bytes(int sample_rate) {
    return (size_t)sample_rate * 2 * (WAVE_BATCH) / (WAVE_RATE);
}

TEST(RtlsdrUsbBufLen, half_a_batch_at_common_rates) {
    EXPECT_EQ(rtlsdr_auto_usb_buf_len(1024000), 128000u);  // 62.5 ms, was 156 ms
    EXPECT_EQ(rtlsdr_auto_usb_buf_len(1400000), 174592u);  // 175000 rounded down to 512
    EXPECT_EQ(rtlsdr_auto_usb_buf_len(2000000), 249856u);  // 250000 rounded down to 512
    EXPECT_EQ(rtlsdr_auto_usb_buf_len(2048000), 256000u);
    EXPECT_EQ(rtlsdr_auto_usb_buf_len(2560000), 320000u);  // unchanged from the old fixed size
}

TEST(RtlsdrUsbBufLen, clamped_to_old_fixed_size_at_high_rates) {
    EXPECT_EQ(rtlsdr_auto_usb_buf_len(3200000), RTLSDR_MAX_AUTO_USB_BUF_LEN);
}

TEST(RtlsdrUsbBufLen, clamped_to_one_urb_at_low_rates) {
    EXPECT_EQ(rtlsdr_auto_usb_buf_len(100000), 16384u);
    EXPECT_EQ(rtlsdr_auto_usb_buf_len(0), 16384u);
    EXPECT_EQ(rtlsdr_auto_usb_buf_len(-1), 16384u);
}

TEST(RtlsdrUsbBufLen, never_more_than_half_a_batch_and_always_512_aligned) {
    for (int rate = 225001; rate <= 3200000; rate += 12345) {
        size_t len = rtlsdr_auto_usb_buf_len(rate);
        EXPECT_EQ(len % 512, 0u) << rate;
        if (len > 16384) {
            EXPECT_LE(len * 2, batch_bytes(rate)) << rate;
        }
    }
}

TEST(RtlsdrUsbBufCount, keeps_total_buffering_at_least_the_legacy_total) {
    EXPECT_EQ(rtlsdr_auto_usb_buf_count(320000), RTLSDR_DEFAULT_LIBUSB_BUFFER_COUNT);
    EXPECT_EQ(rtlsdr_auto_usb_buf_count(128000), 25);
    EXPECT_EQ(rtlsdr_auto_usb_buf_count(174592), 19);
    EXPECT_EQ(rtlsdr_auto_usb_buf_count(16384), 196);
}

TEST(RtlsdrUsbBufCount, smallest_count_that_covers_the_legacy_total) {
    for (size_t len = 16384; len <= RTLSDR_MAX_AUTO_USB_BUF_LEN; len += 512) {
        size_t count = (size_t)rtlsdr_auto_usb_buf_count(len);
        EXPECT_GE(count * len, LEGACY_TOTAL_USB_BYTES) << len;
        EXPECT_LT((count - 1) * len, LEGACY_TOTAL_USB_BYTES) << len;
    }
}

TEST(RtlsdrUsbBufCount, zero_length_is_safe) {
    EXPECT_EQ(rtlsdr_auto_usb_buf_count(0), RTLSDR_DEFAULT_LIBUSB_BUFFER_COUNT);
}

TEST(RtlsdrValidUsbBufLen, accepts_multiples_of_512) {
    EXPECT_TRUE(rtlsdr_valid_usb_buf_len(512));
    EXPECT_TRUE(rtlsdr_valid_usb_buf_len(128000));
    EXPECT_TRUE(rtlsdr_valid_usb_buf_len(320000));
    EXPECT_TRUE(rtlsdr_valid_usb_buf_len(RTLSDR_MAX_USB_BUF_LEN));
}

TEST(RtlsdrValidUsbBufLen, rejects_unaligned_nonpositive_and_oversized) {
    EXPECT_FALSE(rtlsdr_valid_usb_buf_len(0));
    EXPECT_FALSE(rtlsdr_valid_usb_buf_len(-512));
    EXPECT_FALSE(rtlsdr_valid_usb_buf_len(1000));
    EXPECT_FALSE(rtlsdr_valid_usb_buf_len(RTLSDR_MAX_USB_BUF_LEN + 512));
}
