/*
 * control_socket.cpp
 * dynamic_reload Unix domain control socket - listener thread + JSON command protocol
 *
 * Same-host-only trust model: the socket file is created with restrictive permissions (0600),
 * matching how this fork's ~12 instances are already deployed (systemd-managed, no network
 * exposure - see CLAUDE.md's Deployment Context). SO_PEERCRED is also checked as cheap
 * defense-in-depth against a misconfigured umask.
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

#include "control_socket.h"
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>  // struct timeval, for SO_RCVTIMEO
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <csignal>  // sig_atomic_t
#include <cstdio>   // snprintf
#include <cstdlib>
#include <cstring>
#include "auto_ppm.h"
#include "device_stats.h"
#include "input-common.h"
#include "live_reconfig.h"
#include "rtl_airband.h"

using namespace std;

namespace {

pthread_t control_thread;
int listen_fd = -1;
volatile sig_atomic_t control_shutdown_requested = 0;
bool control_started = false;
string socket_path;

string json_escape(const string& s) {
    string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                // Not just JSON-invalid without this - the wire protocol is one JSON object per
                // line (control_socket_read_thread(), below), so a raw, unescaped newline in an
                // error message (nearly all of them are built from a captured cerr message
                // ending in "\n", e.g. config.cpp's parse-error printouts - see
                // config_error_is_recoverable, logging.h) truncates the response line before its
                // closing brace, breaking every client's parser.
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

string ok_response() {
    return "{\"ok\":true}";
}

string error_response(const string& msg) {
    return "{\"ok\":false,\"error\":\"" + json_escape(msg) + "\"}";
}

bool get_field(const map<string, string>& fields, const string& key, string* value) {
    auto it = fields.find(key);
    if (it == fields.end()) {
        return false;
    }
    *value = it->second;
    return true;
}

bool get_int_field(const map<string, string>& fields, const string& key, int* value) {
    string raw;
    if (!get_field(fields, key, &raw) || raw.empty()) {
        return false;
    }
    char* end = nullptr;
    long parsed = strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end != '\0') {
        return false;
    }
    *value = (int)parsed;
    return true;
}

bool get_float_field(const map<string, string>& fields, const string& key, float* value) {
    string raw;
    if (!get_field(fields, key, &raw) || raw.empty()) {
        return false;
    }
    char* end = nullptr;
    double parsed = strtod(raw.c_str(), &end);
    if (end == raw.c_str() || *end != '\0') {
        return false;
    }
    *value = (float)parsed;
    return true;
}

bool get_device(const map<string, string>& fields, device_t** dev, string* err) {
    int idx;
    if (!get_int_field(fields, "device", &idx)) {
        *err = "missing or invalid 'device'";
        return false;
    }
    if (idx < 0 || idx >= device_count) {
        *err = "device index out of range";
        return false;
    }
    *dev = devices + idx;
    return true;
}

bool get_device_and_channel(const map<string, string>& fields, channel_t** channel, string* err) {
    device_t* dev;
    if (!get_device(fields, &dev, err)) {
        return false;
    }
    int idx;
    if (!get_int_field(fields, "channel", &idx)) {
        *err = "missing or invalid 'channel'";
        return false;
    }
    if (idx < 0 || idx >= dev->channel_count) {
        *err = "channel index out of range";
        return false;
    }
    channel_t* candidate = dev->channels + idx;
    // A removal request against this index may still be in flight (try_remove_channels(),
    // live_reconfig.cpp, gave up waiting on a slow output thread without decrementing
    // dev->channel_count) or may have already completed (channel_teardown_for_removal() already
    // freed this channel's LAME encoder) - dev->channel_count alone can't distinguish either case
    // from "still a normal, live channel". Either way this index must be rejected: applying a
    // channel_enable here would reconnect outputs without ever reallocating LAME, and the next
    // process_outputs() pass would call into it with a null encoder. See channel_t::removed's
    // comment (rtl_airband.h) for the full rationale.
    if (candidate->pending_remove_request.load(std::memory_order_acquire) != -1 || candidate->removed.load(std::memory_order_acquire)) {
        *err = "channel is being removed or has already been removed";
        return false;
    }
    *channel = candidate;
    return true;
}

bool get_mixer(const map<string, string>& fields, mixer_t** mixer, string* err) {
    string name;
    if (!get_field(fields, "mixer", &name) || name.empty()) {
        *err = "missing 'mixer'";
        return false;
    }
    mixer_t* m = getmixerbyname(name.c_str());
    if (m == NULL) {
        *err = "unknown mixer '" + name + "'";
        return false;
    }
    *mixer = m;
    return true;
}

string handle_retune(const map<string, string>& fields) {
    string err;
    device_t* dev;
    if (!get_device(fields, &dev, &err)) {
        return error_response(err);
    }
    int freq;
    if (!get_int_field(fields, "freq", &freq)) {
        return error_response("missing or invalid 'freq'");
    }
    if (!device_request_retune(dev, freq)) {
        return error_response("device is not R_MULTICHANNEL - R_SCAN devices retune automatically via their own controller thread");
    }
    // Wait for the owning demod thread to consume the request, so the response reflects actual
    // hardware-retune success/failure rather than just "request accepted". The demod loop visits
    // every device on its slice at least every WAVE_BATCH-ish interval, well under this budget.
    bool timed_out = false;
    bool ok = device_confirm_retune(dev, /*timeout_us=*/500000, &timed_out);
    if (timed_out) {
        return error_response("retune request timed out waiting for the demod thread");
    }
    if (!ok) {
        return error_response("hardware retune failed, see logs");
    }
    return ok_response();
}

string handle_set_sample_rate(const map<string, string>& fields) {
    string err;
    device_t* dev;
    if (!get_device(fields, &dev, &err)) {
        return error_response(err);
    }
    int sample_rate;
    if (!get_int_field(fields, "sample_rate", &sample_rate)) {
        return error_response("missing or invalid 'sample_rate'");
    }
    if (!device_request_sample_rate(dev, sample_rate)) {
        return error_response("device is not R_MULTICHANNEL, or driver does not support live sample_rate changes");
    }
    // Budget a longer timeout than retune's - the apply side includes a full RX-thread join and
    // hardware device reopen, not just one API call. See device_confirm_sample_rate()'s
    // declaration comment (live_reconfig.h).
    bool timed_out = false;
    bool ok = device_confirm_sample_rate(dev, /*timeout_us=*/3000000, &timed_out);
    if (timed_out) {
        return error_response("sample_rate change timed out waiting for the demod thread");
    }
    if (!ok) {
        return error_response("sample_rate change failed and was rolled back to the previous rate (or the device is now down), see logs");
    }
    return ok_response();
}

string handle_set_gain(const map<string, string>& fields) {
    string err;
    device_t* dev;
    if (!get_device(fields, &dev, &err)) {
        return error_response(err);
    }
    float gain;
    if (!get_float_field(fields, "gain", &gain)) {
        return error_response("missing or invalid 'gain'");
    }
    errno = 0;
    if (input_set_gain(dev->input, gain) != 0) {
        if (errno == ENOTSUP) {
            return error_response("gain not supported by this driver");
        }
        return error_response("failed to set gain, see logs");
    }
    return ok_response();
}

string handle_set_bandwidth(const map<string, string>& fields) {
    string err;
    device_t* dev;
    if (!get_device(fields, &dev, &err)) {
        return error_response(err);
    }
    int bandwidth;
    if (!get_int_field(fields, "bandwidth", &bandwidth)) {
        return error_response("missing or invalid 'bandwidth'");
    }
    errno = 0;
    if (input_set_bandwidth(dev->input, bandwidth) != 0) {
        if (errno == ENOTSUP) {
            return error_response("bandwidth not supported by this driver");
        }
        return error_response("failed to set bandwidth, see logs");
    }
    return ok_response();
}

string handle_set_correction(const map<string, string>& fields) {
    string err;
    device_t* dev;
    if (!get_device(fields, &dev, &err)) {
        return error_response(err);
    }
    int correction;
    if (!get_int_field(fields, "correction", &correction)) {
        return error_response("missing or invalid 'correction'");
    }
    errno = 0;
    if (!auto_ppm_correction_allowed(dev, correction)) {
        return error_response("correction is outside this device's auto_ppm_max_ppm limit");
    }
    if (input_set_correction(dev->input, correction) != 0) {
        if (errno == ENOTSUP) {
            return error_response("correction not supported by this driver");
        }
        return error_response("failed to set correction, see logs");
    }
    return ok_response();
}

string handle_channel_enable(const map<string, string>& fields) {
    string err;
    channel_t* channel;
    if (!get_device_and_channel(fields, &channel, &err)) {
        return error_response(err);
    }
    if (!channel_request_enable(channel)) {
        return error_response("request posted but not yet confirmed applied - the owning output thread may be busy, try again shortly");
    }
    return ok_response();
}

string handle_channel_disable(const map<string, string>& fields) {
    string err;
    channel_t* channel;
    if (!get_device_and_channel(fields, &channel, &err)) {
        return error_response(err);
    }
    if (!channel_request_disable(channel)) {
        return error_response("request posted but not yet confirmed applied - the owning output thread may be busy, try again shortly");
    }
    return ok_response();
}

string handle_mixer_enable(const map<string, string>& fields) {
    string err;
    mixer_t* mixer;
    if (!get_mixer(fields, &mixer, &err)) {
        return error_response(err);
    }
    if (!mixer_request_enable(mixer)) {
        return error_response("request posted but not yet confirmed applied - the owning output thread may be busy, try again shortly");
    }
    return ok_response();
}

string handle_mixer_disable(const map<string, string>& fields) {
    string err;
    mixer_t* mixer;
    if (!get_mixer(fields, &mixer, &err)) {
        return error_response(err);
    }
    if (!mixer_request_disable(mixer)) {
        return error_response("request posted but not yet confirmed applied - the owning output thread may be busy, try again shortly");
    }
    return ok_response();
}

string json_string_array(const vector<string>& items) {
    string out = "[";
    for (size_t i = 0; i < items.size(); i++) {
        if (i > 0) {
            out += ",";
        }
        out += "\"" + json_escape(items[i]) + "\"";
    }
    out += "]";
    return out;
}

string handle_reload_diff(const map<string, string>&) {
    if (cfgfile == NULL) {
        return error_response("no config file path known");
    }
    ConfigSnapshot snapshot;
    string parse_error;
    if (!parse_config_snapshot(cfgfile, &snapshot, &parse_error)) {
        return error_response("failed to re-read config: " + parse_error);
    }
    DiffResult diff = compute_and_apply_diff(snapshot);
    return "{\"ok\":true,\"applied\":" + json_string_array(diff.applied) + ",\"skipped_requires_restart\":" + json_string_array(diff.skipped_requires_restart) + "}";
}

bool peer_is_same_uid(int conn_fd) {
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(conn_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        return false;
    }
    return cred.uid == getuid();
}

void* control_main(void*) {
    while (!control_shutdown_requested) {
        struct pollfd pfd;
        pfd.fd = listen_fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 500);
        if (ret <= 0) {
            continue;
        }
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            continue;
        }
        if (!peer_is_same_uid(conn_fd)) {
            log(LOG_WARNING, "control_socket: rejected connection from a different uid\n");
            close(conn_fd);
            continue;
        }
        handle_connection(conn_fd);
    }
    return NULL;
}

}  // namespace

// Bounds on a single client connection so one stalled or misbehaving client can't wedge every
// other control-socket operation on this instance: timeout_sec makes a stuck recv() return
// instead of blocking control_main()'s single accept loop indefinitely (and, since this runs
// entirely inside handle_connection() with no other thread able to service new connections in
// the meantime, also bounds how long a stuck client can delay a clean process shutdown -
// control_shutdown_requested is only re-checked between recv() calls). max_buffered_bytes bounds
// how much a client that never sends a newline can make this process buffer - the same-uid trust
// model already limits who can open this socket at all, but there's no reason to trust a
// connected client to behave, either. Exposed (declared in control_socket.h) with real socket I/O
// - unlike the parse/dispatch functions above, there's no meaningful way to unit test a recv()
// timeout without a real socket pair - so tests pass a short timeout/small cap rather than the
// production defaults to keep this fast.
void handle_connection(int conn_fd, int timeout_sec, size_t max_buffered_bytes) {
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    string buffer;
    char chunk[4096];
    while (!control_shutdown_requested) {
        ssize_t n = recv(conn_fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            // n == 0: peer closed. n < 0: either SO_RCVTIMEO expired (EAGAIN/EWOULDBLOCK) or a
            // real socket error - either way, don't wait on this connection any longer.
            break;
        }
        buffer.append(chunk, (size_t)n);
        if (buffer.size() > max_buffered_bytes && buffer.find('\n') == string::npos) {
            log(LOG_WARNING, "control_socket: client sent %zu bytes with no newline, closing connection\n", buffer.size());
            break;
        }
        size_t newline;
        while ((newline = buffer.find('\n')) != string::npos) {
            string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            string response = control_socket_dispatch_command_line(line) + "\n";
            send(conn_fd, response.data(), response.size(), MSG_NOSIGNAL);
        }
    }
    close(conn_fd);
}

string control_socket_dispatch_command_line(const string& line) {
    map<string, string> fields;
    string err;
    if (!control_socket_parse_command_line(line, &fields, &err)) {
        return error_response("parse error: " + err);
    }
    return control_socket_dispatch_command(fields);
}

bool control_socket_parse_command_line(const string& line, map<string, string>* fields, string* error) {
    size_t i = 0;
    size_t const len = line.size();
    auto skip_ws = [&]() {
        while (i < len && isspace((unsigned char)line[i])) {
            i++;
        }
    };

    skip_ws();
    if (i >= len || line[i] != '{') {
        *error = "expected '{'";
        return false;
    }
    i++;
    skip_ws();
    if (i < len && line[i] == '}') {
        return true;  // empty object
    }

    while (true) {
        skip_ws();
        if (i >= len || line[i] != '"') {
            *error = "expected key string";
            return false;
        }
        i++;
        string key;
        while (i < len && line[i] != '"') {
            key += line[i++];
        }
        if (i >= len) {
            *error = "unterminated key string";
            return false;
        }
        i++;  // closing quote

        skip_ws();
        if (i >= len || line[i] != ':') {
            *error = "expected ':'";
            return false;
        }
        i++;
        skip_ws();

        string value;
        if (i < len && line[i] == '"') {
            i++;
            while (i < len && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < len) {
                    i++;
                }
                value += line[i++];
            }
            if (i >= len) {
                *error = "unterminated value string";
                return false;
            }
            i++;  // closing quote
        } else {
            while (i < len && line[i] != ',' && line[i] != '}' && !isspace((unsigned char)line[i])) {
                value += line[i++];
            }
            if (value.empty()) {
                *error = "expected value";
                return false;
            }
        }
        (*fields)[key] = value;

        skip_ws();
        if (i < len && line[i] == ',') {
            i++;
            continue;
        }
        if (i < len && line[i] == '}') {
            break;
        }
        *error = "expected ',' or '}'";
        return false;
    }
    return true;
}

string control_socket_dispatch_command(const map<string, string>& fields) {
    string cmd;
    if (!get_field(fields, "cmd", &cmd) || cmd.empty()) {
        return error_response("missing 'cmd'");
    }
    if (cmd == "retune") {
        return handle_retune(fields);
    } else if (cmd == "set_sample_rate") {
        return handle_set_sample_rate(fields);
    } else if (cmd == "set_gain") {
        return handle_set_gain(fields);
    } else if (cmd == "set_bandwidth") {
        return handle_set_bandwidth(fields);
    } else if (cmd == "set_correction") {
        return handle_set_correction(fields);
    } else if (cmd == "channel_enable") {
        return handle_channel_enable(fields);
    } else if (cmd == "channel_disable") {
        return handle_channel_disable(fields);
    } else if (cmd == "mixer_enable") {
        return handle_mixer_enable(fields);
    } else if (cmd == "mixer_disable") {
        return handle_mixer_disable(fields);
    } else if (cmd == "reload_diff") {
        return handle_reload_diff(fields);
    } else if (cmd == "status") {
        return build_status_json();
    }
    return error_response("unknown cmd '" + cmd + "'");
}

bool control_socket_start(const string& path) {
    if (control_started) {
        return true;
    }

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log(LOG_ERR, "control_socket: socket() failed: %s\n", strerror(errno));
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        log(LOG_ERR, "control_socket: path '%s' too long\n", path.c_str());
        close(listen_fd);
        listen_fd = -1;
        return false;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    // Remove a stale socket file left behind by a previous run (e.g. unclean shutdown) - safe
    // because bind() below will fail on its own if another live process still owns this path.
    unlink(path.c_str());

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log(LOG_ERR, "control_socket: bind(%s) failed: %s\n", path.c_str(), strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return false;
    }
    if (chmod(path.c_str(), S_IRUSR | S_IWUSR) < 0) {
        log(LOG_WARNING, "control_socket: chmod(%s) failed: %s\n", path.c_str(), strerror(errno));
    }
    if (listen(listen_fd, 8) < 0) {
        log(LOG_ERR, "control_socket: listen() failed: %s\n", strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        unlink(path.c_str());
        return false;
    }

    log(LOG_INFO, "control_socket: listening on %s\n", path.c_str());
    socket_path = path;
    control_shutdown_requested = 0;
    pthread_create(&control_thread, NULL, &control_main, NULL);
    control_started = true;
    return true;
}

void control_socket_shutdown() {
    if (!control_started) {
        return;
    }
    control_shutdown_requested = 1;
    pthread_join(control_thread, NULL);
    close(listen_fd);
    listen_fd = -1;
    unlink(socket_path.c_str());
    control_started = false;
}
