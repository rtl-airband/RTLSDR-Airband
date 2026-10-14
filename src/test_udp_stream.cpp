/*
 * test_udp_stream.cpp
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

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "rtl_airband.h"

// util.cpp also contains level conversion helpers; their FFT size normally
// comes from rtl_airband.cpp, which contains the application's main().
size_t fft_size = 512;

class UdpStreamTest : public ::testing::Test {
   protected:
    void SetUp() override {
        stream.send_socket = -1;
        receiver = socket(AF_INET, SOCK_DGRAM, 0);
        ASSERT_NE(receiver, -1);
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(bind(receiver, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        socklen_t address_len = sizeof(address);
        ASSERT_EQ(getsockname(receiver, reinterpret_cast<sockaddr*>(&address), &address_len), 0);
        port = std::to_string(ntohs(address.sin_port));
        const timeval timeout = {1, 0};
        ASSERT_EQ(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
        stream.dest_address = "127.0.0.1";
        stream.dest_port = port.c_str();
    }

    void TearDown() override {
        udp_stream_shutdown(&stream);
        // Production retains this allocation for the output's lifetime.
        free(stream.stereo_buffer);
        if (receiver != -1) {
            close(receiver);
        }
    }

    static void FillSamples(float* samples, size_t count, bool right = false) {
        for (size_t i = 0; i < count; ++i) {
            samples[i] = static_cast<float>(i + 1) / 4096.0f * (right ? -1.0f : 1.0f);
        }
        // Verify bit preservation, including signed zero, subnormal values,
        // infinities and NaN payloads, rather than comparing float values.
        const uint32_t patterns[] = {0x00000000, 0x80000000, 0x00000001, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc01234};
        const size_t pattern_count = sizeof(patterns) / sizeof(patterns[0]);
        for (size_t i = 0; i < count && i < pattern_count; ++i) {
            const uint32_t bits = patterns[right ? pattern_count - 1 - i : i];
            memcpy(samples + i, &bits, sizeof(bits));
        }
    }

    static std::vector<unsigned char> InterleavedBytes(const float* left, const float* right, size_t count) {
        std::vector<unsigned char> expected(count * 2 * sizeof(float));
        for (size_t i = 0; i < count; ++i) {
            memcpy(expected.data() + 2 * i * sizeof(float), left + i, sizeof(float));
            memcpy(expected.data() + (2 * i + 1) * sizeof(float), right + i, sizeof(float));
        }
        return expected;
    }

    void ExpectPacket(const unsigned char* expected, size_t bytes) {
        // Large enough to observe the entire datagram even if its length is wrong.
        std::vector<unsigned char> packet(65536);
        const ssize_t received = recv(receiver, packet.data(), packet.size(), 0);
        ASSERT_EQ(received, static_cast<ssize_t>(bytes));
        if (bytes != 0) {
            EXPECT_EQ(memcmp(packet.data(), expected, bytes), 0);
        }
        // Each write must emit exactly one datagram.
        EXPECT_EQ(recv(receiver, packet.data(), packet.size(), MSG_DONTWAIT), -1);
        EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    }

    udp_stream_data stream = {};
    int receiver = -1;
    std::string port;
};

TEST_F(UdpStreamTest, MonoDoesNotAllocateStereoBuffer) {
    ASSERT_TRUE(udp_stream_init(&stream, MM_MONO, WAVE_BATCH * sizeof(float)));
    EXPECT_EQ(stream.stereo_buffer, nullptr);
    EXPECT_EQ(stream.stereo_buffer_len, 0U);
}

TEST_F(UdpStreamTest, StereoAllocatesOneInterleavedBlock) {
    ASSERT_TRUE(udp_stream_init(&stream, MM_STEREO, WAVE_BATCH * sizeof(float)));
    ASSERT_NE(stream.stereo_buffer, nullptr);
    EXPECT_EQ(stream.stereo_buffer_len, 2U * WAVE_BATCH);
}

TEST_F(UdpStreamTest, MonoSendBufferFitsDatagram) {
    ASSERT_TRUE(udp_stream_init(&stream, MM_MONO, WAVE_BATCH * sizeof(float)));
    int sndbuf = 0;
    socklen_t optlen = sizeof(sndbuf);
    ASSERT_EQ(getsockopt(stream.send_socket, SOL_SOCKET, SO_SNDBUF, &sndbuf, &optlen), 0);
    EXPECT_GE(sndbuf, WAVE_BATCH * static_cast<int>(sizeof(float)));
}

TEST_F(UdpStreamTest, StereoSendBufferFitsDatagram) {
    ASSERT_TRUE(udp_stream_init(&stream, MM_STEREO, WAVE_BATCH * sizeof(float)));
    int sndbuf = 0;
    socklen_t optlen = sizeof(sndbuf);
    ASSERT_EQ(getsockopt(stream.send_socket, SOL_SOCKET, SO_SNDBUF, &sndbuf, &optlen), 0);
    EXPECT_GE(sndbuf, 2 * WAVE_BATCH * static_cast<int>(sizeof(float)));
}

TEST_F(UdpStreamTest, MonoBlockPreservesEveryByte) {
    const size_t count = WAVE_BATCH;
    std::unique_ptr<float[]> samples(new float[count]);
    FillSamples(samples.get(), count);
    const size_t bytes = count * sizeof(float);
    std::vector<unsigned char> expected(bytes);
    memcpy(expected.data(), samples.get(), bytes);
    ASSERT_TRUE(udp_stream_init(&stream, MM_MONO, bytes));
    udp_stream_write(&stream, samples.get(), bytes);
    ExpectPacket(expected.data(), bytes);
    EXPECT_EQ(memcmp(samples.get(), expected.data(), bytes), 0);
}

TEST_F(UdpStreamTest, StereoBlockPreservesEveryByte) {
    const size_t count = WAVE_BATCH;
    // Exact-sized source allocations let ASan detect any read past the block.
    std::unique_ptr<float[]> left(new float[count]);
    std::unique_ptr<float[]> right(new float[count]);
    FillSamples(left.get(), count);
    FillSamples(right.get(), count, true);
    const auto expected = InterleavedBytes(left.get(), right.get(), count);
    ASSERT_TRUE(udp_stream_init(&stream, MM_STEREO, count * sizeof(float)));
    udp_stream_write(&stream, left.get(), right.get(), count * sizeof(float));
    ExpectPacket(expected.data(), expected.size());
    // Interleaving must not modify either source channel.
    EXPECT_EQ(InterleavedBytes(left.get(), right.get(), count), expected);
}

TEST_F(UdpStreamTest, StereoWaveLengthSources) {
    // Match the actual waveout/waveout_r array lengths, with separate
    // allocations so ASan can detect crossing either array's boundary.
    std::unique_ptr<float[]> left(new float[WAVE_LEN]);
    std::unique_ptr<float[]> right(new float[WAVE_LEN]);
    FillSamples(left.get(), WAVE_LEN);
    FillSamples(right.get(), WAVE_LEN, true);
    const auto expected = InterleavedBytes(left.get(), right.get(), WAVE_BATCH);
    ASSERT_TRUE(udp_stream_init(&stream, MM_STEREO, WAVE_BATCH * sizeof(float)));
    udp_stream_write(&stream, left.get(), right.get(), WAVE_BATCH * sizeof(float));
    ExpectPacket(expected.data(), expected.size());
}

TEST_F(UdpStreamTest, StereoPaddedSourcesPreservePacketBytes) {
    // Oversized sources keep the old implementation's extra reads in bounds.
    // This test can therefore verify the unchanged wire payload before and
    // after the fix, independently of the memory safety regression tests.
    const size_t count = WAVE_BATCH;
    std::vector<float> left(count * sizeof(float));
    std::vector<float> right(count * sizeof(float));
    FillSamples(left.data(), left.size());
    FillSamples(right.data(), right.size(), true);
    const auto expected = InterleavedBytes(left.data(), right.data(), count);
    ASSERT_TRUE(udp_stream_init(&stream, MM_STEREO, count * sizeof(float)));
    udp_stream_write(&stream, left.data(), right.data(), count * sizeof(float));
    ExpectPacket(expected.data(), expected.size());
}

TEST_F(UdpStreamTest, StereoShortBlocksDoNotReadOrSendATail) {
    ASSERT_TRUE(udp_stream_init(&stream, MM_STEREO, WAVE_BATCH * sizeof(float)));
    const size_t counts[] = {WAVE_BATCH, 17, 1};
    for (size_t count : counts) {
        std::unique_ptr<float[]> left(new float[count]);
        std::unique_ptr<float[]> right(new float[count]);
        FillSamples(left.get(), count);
        FillSamples(right.get(), count, true);
        const auto expected = InterleavedBytes(left.get(), right.get(), count);
        udp_stream_write(&stream, left.get(), right.get(), count * sizeof(float));
        ExpectPacket(expected.data(), expected.size());
    }
}

TEST_F(UdpStreamTest, MonoZeroLengthDatagram) {
    ASSERT_TRUE(udp_stream_init(&stream, MM_MONO, WAVE_BATCH * sizeof(float)));
    udp_stream_write(&stream, nullptr, 0);
    ExpectPacket(nullptr, 0);
}

TEST_F(UdpStreamTest, StereoZeroLengthDatagram) {
    ASSERT_TRUE(udp_stream_init(&stream, MM_STEREO, WAVE_BATCH * sizeof(float)));
    udp_stream_write(&stream, nullptr, nullptr, 0);
    ExpectPacket(nullptr, 0);
}

TEST_F(UdpStreamTest, InactiveStreamDoesNotReadSources) {
    stream.send_socket = -1;
    udp_stream_write(&stream, nullptr, WAVE_BATCH * sizeof(float));
    udp_stream_write(&stream, nullptr, nullptr, WAVE_BATCH * sizeof(float));
}

#ifndef NDEBUG
TEST_F(UdpStreamTest, StereoRejectsPartialSamples) {
    ASSERT_TRUE(udp_stream_init(&stream, MM_STEREO, WAVE_BATCH * sizeof(float)));
    EXPECT_DEATH(udp_stream_write(&stream, nullptr, nullptr, sizeof(float) - 1), "len_bytes % sizeof\\(float\\)");
}

TEST_F(UdpStreamTest, StereoRejectsBlocksLargerThanBuffer) {
    ASSERT_TRUE(udp_stream_init(&stream, MM_STEREO, WAVE_BATCH * sizeof(float)));
    EXPECT_DEATH(udp_stream_write(&stream, nullptr, nullptr, (WAVE_BATCH + 1) * sizeof(float)), "sample_count <=");
}

TEST_F(UdpStreamTest, StereoInitRejectsPartialSamples) {
    EXPECT_DEATH(udp_stream_init(&stream, MM_STEREO, sizeof(float) - 1), "len_bytes % sizeof\\(float\\)");
}
#endif
