/*
 * test_rdio_scanner.cpp
 *
 * Copyright (C) 2026 Jared Schmall
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

#include "rtl_airband.h"

#ifdef WITH_RDIO_SCANNER

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>

using namespace std;

// rdio_scanner.cpp references this as extern; normally defined in rtl_airband.cpp,
// which can't be linked here (its main() would conflict with gtest's) - matches the
// xcalloc-stub pattern in test_udp_stream.cpp for the same reason. rdio_scanner_build_fields(),
// the only thing under test here, never touches the upload queue depth.
int rdio_scanner_queue_depth = 64;

namespace {

map<string, string> as_map(const vector<pair<string, string>>& fields) {
    map<string, string> m;
    for (const auto& field : fields) {
        m[field.first] = field.second;
    }
    return m;
}

rdio_scanner_job make_job() {
    rdio_scanner_job job;
    job.config.server = "rdio.example.invalid";
    job.config.port = 3000;
    job.config.use_tls = false;
    job.config.api_key = "00000000-0000-0000-0000-000000000000";
    job.config.system_id = -1;
    job.config.system_label = "";
    job.config.talkgroup_id = 10612;
    job.config.talkgroup_label = "";
    job.config.talkgroup_tag = "";
    job.config.talkgroup_group = "";
    job.config.source_id = 0;
    job.config.delete_after_upload = false;
    job.config.timeout_ms = 5000;
    job.config.max_retries = 2;
    job.file_path = "/mnt/audio/cdf/tac_1/cdf_tac_1_20260723_120000.mp3";
    job.timestamp_sec = 1784894400LL;
    job.frequency = 154265000;
    return job;
}

}  // namespace

class RdioScannerTest : public TestBaseClass {};

TEST_F(RdioScannerTest, required_fields_always_present) {
    rdio_scanner_job job = make_job();
    map<string, string> fields = as_map(rdio_scanner_build_fields(job));

    EXPECT_EQ(fields.at("audioType"), "audio/mpeg");
    EXPECT_EQ(fields.at("dateTime"), "1784894400");
    EXPECT_EQ(fields.at("frequency"), "154265000");
    EXPECT_EQ(fields.at("key"), "00000000-0000-0000-0000-000000000000");
    EXPECT_EQ(fields.at("source"), "0");
    EXPECT_EQ(fields.at("talkgroup"), "10612");
}

TEST_F(RdioScannerTest, optional_fields_omitted_when_unset) {
    rdio_scanner_job job = make_job();
    map<string, string> fields = as_map(rdio_scanner_build_fields(job));

    EXPECT_EQ(fields.count("system"), 0u);
    EXPECT_EQ(fields.count("systemLabel"), 0u);
    EXPECT_EQ(fields.count("talkgroupLabel"), 0u);
    EXPECT_EQ(fields.count("talkgroupTag"), 0u);
    EXPECT_EQ(fields.count("talkgroupGroup"), 0u);
}

TEST_F(RdioScannerTest, optional_fields_included_when_set) {
    rdio_scanner_job job = make_job();
    job.config.system_id = 106;
    job.config.system_label = "CDF";
    job.config.talkgroup_label = "A/G 1";
    job.config.talkgroup_tag = "Fire Tac";
    job.config.talkgroup_group = "Fire";

    map<string, string> fields = as_map(rdio_scanner_build_fields(job));

    EXPECT_EQ(fields.at("system"), "106");
    EXPECT_EQ(fields.at("systemLabel"), "CDF");
    EXPECT_EQ(fields.at("talkgroupLabel"), "A/G 1");
    EXPECT_EQ(fields.at("talkgroupTag"), "Fire Tac");
    EXPECT_EQ(fields.at("talkgroupGroup"), "Fire");
}

TEST_F(RdioScannerTest, system_id_zero_is_a_valid_value_not_unset) {
    rdio_scanner_job job = make_job();
    job.config.system_id = 0;

    map<string, string> fields = as_map(rdio_scanner_build_fields(job));

    ASSERT_EQ(fields.count("system"), 1u);
    EXPECT_EQ(fields.at("system"), "0");
}

class RdioScannerShutdownTest : public TestBaseClass {
   protected:
    void TearDown(void) override {
        rdio_scanner_shutdown();  // no-op if already shut down
        if (listen_fd != -1)
            close(listen_fd);
        TestBaseClass::TearDown();
    }

    // Sets up a listening socket that accepts the TCP connection but never sends a
    // response, so a client blocks waiting for one - simulating a stalled upload target.
    // Returns the port it's bound to.
    int start_stalled_listener() {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_NE(listen_fd, -1);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = 0;  // let the OS pick a free port
        EXPECT_EQ(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)), 0);
        EXPECT_EQ(listen(listen_fd, 1), 0);

        socklen_t addr_len = sizeof(addr);
        EXPECT_EQ(getsockname(listen_fd, (struct sockaddr*)&addr, &addr_len), 0);
        return ntohs(addr.sin_port);
    }

    int listen_fd = -1;
};

TEST_F(RdioScannerShutdownTest, shutdown_interrupts_stalled_upload_promptly) {
    // Before this fix, rdio_scanner_shutdown()'s pthread_join() had to wait out the
    // full connect/transfer timeout (and any retry backoff) on a job stuck talking to
    // an unresponsive server. Use a deliberately long timeout_ms so a non-interruptible
    // shutdown would clearly stall this test; the fix should make shutdown return in
    // roughly the curl progress-callback interval (~1s), not anywhere near 60s.
    int port = start_stalled_listener();

    const string file_path = temp_dir + "/stalled_upload.mp3";
    FILE* f = fopen(file_path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite("fake mp3 data", 1, 13, f);
    fclose(f);

    rdio_scanner_job job = make_job();
    job.config.server = "127.0.0.1";
    job.config.port = port;
    job.config.timeout_ms = 60000;
    job.config.max_retries = 2;
    job.file_path = file_path;

    rdio_scanner_start();
    struct timeval open_time;
    open_time.tv_sec = job.timestamp_sec;
    open_time.tv_usec = 0;
    rdio_scanner_enqueue(&job.config, job.file_path, open_time, job.frequency);

    // give the worker thread time to dequeue the job and get into the stalled upload
    usleep(300000);

    auto shutdown_start = chrono::steady_clock::now();
    rdio_scanner_shutdown();
    auto elapsed_sec = chrono::duration_cast<chrono::duration<double>>(chrono::steady_clock::now() - shutdown_start).count();

    EXPECT_LT(elapsed_sec, 5.0);
}

#endif /* WITH_RDIO_SCANNER */
