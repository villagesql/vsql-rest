/* Copyright (c) 2026 VillageSQL Contributors
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

#include "http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "third_party/picohttpparser.h"

namespace vsql_rest {

// Maximum accepted request-body size. Caps the allocation driven by a
// client-supplied Content-Length so an oversized header can't exhaust memory.
static constexpr long long kMaxRequestBody = 8LL * 1024 * 1024;  // 8 MiB

// How long a client may take to deliver its whole request. Nothing else bounds
// these reads: read_request_raw() and the body loop in handle_connection()
// block until data arrives, and each connection owns a detached thread, so a
// client that drags out a request holds a thread inside the server process for
// as long as it likes.
//
// Enforced twice, because a socket timeout alone is not enough: SO_RCVTIMEO
// bounds the gap between two reads, so a client dribbling one byte just inside
// that gap resets it forever. request_expired() bounds the request as a whole.
static constexpr int kReadTimeoutSeconds = 20;

using SteadyClock = std::chrono::steady_clock;

static bool request_expired(SteadyClock::time_point started) {
  return SteadyClock::now() - started >
         std::chrono::seconds(kReadTimeoutSeconds);
}

// Ceiling on live connection threads. The read deadline bounds how long any
// one connection can hold a thread; this bounds how many can do so at once.
static constexpr int kMaxConnections = 128;

static std::atomic<int> g_active_connections{0};

// Releases the connection slot when the connection thread exits, by whichever
// path — including an exception escaping handle_connection().
namespace {
struct ConnectionSlot {
  ~ConnectionSlot() { g_active_connections.fetch_sub(1, std::memory_order_relaxed); }
};
}  // namespace

int create_listen_socket(int port, int* bound_port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<uint16_t>(port));

  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 128) < 0) {
    close(fd);
    return -1;
  }
  if (bound_port) {
    struct sockaddr_in actual{};
    socklen_t len = sizeof(actual);
    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&actual), &len) == 0) {
      *bound_port = ntohs(actual.sin_port);
    } else {
      *bound_port = port;
    }
  }
  return fd;
}

std::string format_http_response(const HttpResponse& resp) {
  std::string out;
  out.reserve(256 + resp.body.size());

  // Status line. No default label: -Werror=switch turns a new Status without a
  // reason phrase into a build failure.
  const char* reason = "Unknown";
  switch (resp.status) {
    case Status::kOk:                  reason = "OK"; break;
    case Status::kCreated:             reason = "Created"; break;
    case Status::kNoContent:           reason = "No Content"; break;
    case Status::kBadRequest:          reason = "Bad Request"; break;
    case Status::kUnauthorized:        reason = "Unauthorized"; break;
    case Status::kNotFound:            reason = "Not Found"; break;
    case Status::kMethodNotAllowed:    reason = "Method Not Allowed"; break;
    case Status::kConflict:            reason = "Conflict"; break;
    case Status::kContentTooLarge:     reason = "Content Too Large"; break;
    case Status::kInternalServerError: reason = "Internal Server Error"; break;
    case Status::kNotImplemented:      reason = "Not Implemented"; break;
    case Status::kServiceUnavailable:  reason = "Service Unavailable"; break;
  }

  char status_line[64];
  snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d %s\r\n",
           static_cast<int>(resp.status), reason);
  out += status_line;

  // Required headers.
  out += "Connection: close\r\n";

  char clen[32];
  snprintf(clen, sizeof(clen), "Content-Length: %zu\r\n", resp.body.size());
  out += clen;

  // Caller-provided headers.
  bool has_content_type = false;
  for (const auto& [k, v] : resp.headers) {
    std::string klower = k;
    std::transform(klower.begin(), klower.end(), klower.begin(), ::tolower);
    if (klower == "content-type") has_content_type = true;
    out += k + ": " + v + "\r\n";
  }

  if (!has_content_type && !resp.body.empty()) {
    out += "Content-Type: application/json\r\n";
  }

  out += "\r\n";
  out += resp.body;
  return out;
}

// Read from a connection (TLS or plain) into a buffer until headers are complete.
// Returns the raw bytes read.
template <typename Conn>
static std::string read_request_raw(Conn& conn, SteadyClock::time_point started) {
  std::string buf;
  buf.reserve(4096);
  char tmp[4096];
  while (buf.size() < 65536) {
    if (request_expired(started)) return {};
    int n = conn.read(tmp, static_cast<int>(std::min(sizeof(tmp),
                      static_cast<size_t>(65536 - buf.size()))));
    if (n <= 0) break;
    buf.append(tmp, n);
    // Check if headers are complete.
    if (buf.find("\r\n\r\n") != std::string::npos) break;
  }
  return buf;
}

// Parse an HTTP/1.1 request from raw bytes. Returns false on parse error.
static bool parse_http_request(const std::string& raw, HttpRequest& req,
                                size_t& header_len) {
  const char* method = nullptr;
  size_t method_len = 0;
  const char* path = nullptr;
  size_t path_len = 0;
  int minor_version = 0;
  struct phr_header headers[64];
  size_t num_headers = 64;

  int r = phr_parse_request(
      raw.data(), raw.size(),
      &method, &method_len,
      &path, &path_len,
      &minor_version,
      headers, &num_headers,
      0 /* last_len */);

  if (r < 0) return false;
  header_len = static_cast<size_t>(r);

  req.method = std::string(method, method_len);
  std::transform(req.method.begin(), req.method.end(), req.method.begin(),
                 ::toupper);

  // Split path and query string.
  std::string full_path(path, path_len);
  auto qmark = full_path.find('?');
  if (qmark != std::string::npos) {
    req.path = full_path.substr(0, qmark);
    req.raw_query = full_path.substr(qmark + 1);
  } else {
    req.path = full_path;
    req.raw_query.clear();
  }

  for (size_t i = 0; i < num_headers; ++i) {
    std::string hname(headers[i].name, headers[i].name_len);
    std::transform(hname.begin(), hname.end(), hname.begin(), ::tolower);
    req.headers[hname] = std::string(headers[i].value, headers[i].value_len);
  }

  return true;
}

// Handle a single connection: read request, push to queue, wait, send response.
template <typename Conn>
static void handle_connection(Conn& conn, RequestQueue* queue) {
  const auto started = SteadyClock::now();
  std::string raw = read_request_raw(conn, started);
  if (raw.empty()) return;

  HttpRequest req;
  size_t header_len = 0;
  if (!parse_http_request(raw, req, header_len)) return;

  // A body framed by Transfer-Encoding cannot be read: this server only
  // understands Content-Length framing. Refuse explicitly instead of falling
  // through to Content-Length, which RFC 9112 6.3 forbids precisely because
  // disagreeing with a front-end proxy about which header frames the body is
  // how requests get smuggled. Silently ignoring it also produced a puzzling
  // "invalid JSON body" for a request whose body was simply never read.
  auto te_it = req.headers.find("transfer-encoding");
  if (te_it != req.headers.end()) {
    std::string te = te_it->second;
    std::transform(te.begin(), te.end(), te.begin(), ::tolower);
    if (te != "identity") {
      HttpResponse resp;
      resp.status = Status::kNotImplemented;
      resp.body = "{\"message\":\"Transfer-Encoding is not supported; send "
                  "Content-Length\",\"details\":null,\"hint\":null,"
                  "\"code\":\"VSQL0006\"}";
      resp.headers.emplace_back("Content-Type", "application/json");
      std::string raw_resp = format_http_response(resp);
      conn.write_all(raw_resp.data(), static_cast<int>(raw_resp.size()));
      return;
    }
  }

  // Read body if Content-Length is present.
  auto cl_it = req.headers.find("content-length");
  if (cl_it != req.headers.end()) {
    long long content_length = 0;
    try { content_length = std::stoll(cl_it->second); } catch (...) {}
    // Bound the body: an attacker-supplied Content-Length must not drive an
    // unbounded req.body allocation (memory-exhaustion DoS). Reject early.
    if (content_length > kMaxRequestBody) {
      HttpResponse resp;
      resp.status = Status::kContentTooLarge;
      resp.body = "{\"message\":\"request body too large\",\"details\":null,"
                  "\"hint\":null,\"code\":\"VSQL0004\"}";
      resp.headers.emplace_back("Content-Type", "application/json");
      std::string raw_resp = format_http_response(resp);
      conn.write_all(raw_resp.data(), static_cast<int>(raw_resp.size()));
      return;
    }
    if (content_length > 0) {
      // How many body bytes are already in raw?
      size_t already = raw.size() > header_len ? raw.size() - header_len : 0;
      req.body = raw.substr(header_len, already);
      // Read remaining body bytes.
      long long remaining = content_length - static_cast<long long>(already);
      while (remaining > 0) {
        if (request_expired(started)) return;
        char tmp[4096];
        int n = conn.read(tmp, static_cast<int>(
            std::min(static_cast<long long>(sizeof(tmp)), remaining)));
        if (n <= 0) break;
        req.body.append(tmp, n);
        remaining -= n;
      }
    }
  }

  auto future = queue->enqueue(std::move(req));

  // Wait for the SQL executor to fulfill the response (with timeout).
  auto status = future.wait_for(std::chrono::seconds(30));
  HttpResponse resp;
  if (status == std::future_status::ready) {
    // future.get() throws if the queue was destroyed during shutdown
    // (RequestQueue's destructor sets an exception on pending promises).
    // An uncaught throw in this detached thread would call std::terminate
    // and abort the mysqld process — catch it and return a graceful 503.
    try {
      resp = future.get();
    } catch (const std::exception& e) {
      fprintf(stderr, "vsql_rest: request future failed: %s\n", e.what());
      resp.status = Status::kServiceUnavailable;
      resp.body = "{\"message\":\"server shutting down\",\"details\":null,"
                  "\"hint\":null,\"code\":\"VSQL0003\"}";
      resp.headers.emplace_back("Content-Type", "application/json");
    }
  } else {
    resp.status = Status::kServiceUnavailable;
    resp.body = "{\"message\":\"request timed out\",\"details\":null,"
                "\"hint\":null,\"code\":\"VSQL0002\"}";
    resp.headers.emplace_back("Content-Type", "application/json");
  }

  std::string raw_resp = format_http_response(resp);
  conn.write_all(raw_resp.data(), static_cast<int>(raw_resp.size()));
}

void accept_loop(int listen_fd, SSL_CTX* ssl_ctx,
                 std::shared_ptr<RequestQueue> queue,
                 std::atomic<bool>* running) {
  while (running->load(std::memory_order_relaxed)) {
    struct sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(listen_fd,
                           reinterpret_cast<struct sockaddr*>(&client_addr),
                           &addr_len);
    if (client_fd < 0) {
      // listen_fd was closed (DISABLE wakeup) or transient error.
      break;
    }

    struct timeval tv{kReadTimeoutSeconds, 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (g_active_connections.load(std::memory_order_relaxed) >= kMaxConnections) {
      // Refuse before spawning a thread — spawning one is the cost being
      // capped. A TLS client gets a bare close: answering it would require
      // completing the handshake this branch exists to avoid.
      if (!ssl_ctx) {
        HttpResponse resp;
        resp.status = Status::kServiceUnavailable;
        resp.body = "{\"message\":\"too many connections\",\"details\":null,"
                    "\"hint\":null,\"code\":\"VSQL0005\"}";
        resp.headers.emplace_back("Content-Type", "application/json");
        std::string raw_resp = format_http_response(resp);
        PlainConn conn(client_fd);
        conn.write_all(raw_resp.data(), static_cast<int>(raw_resp.size()));
      }
      close(client_fd);
      continue;
    }
    g_active_connections.fetch_add(1, std::memory_order_relaxed);

    // Detach a thread per connection. Each connection handles one request.
    // The thread holds a shared_ptr to the queue: DISABLE drops the extension's
    // reference while connection threads may still be mid-request, and one of
    // them reaching enqueue() on a destroyed queue crashes the server.
    try {
      if (ssl_ctx) {
        std::thread([client_fd, ssl_ctx, queue]() {
          ConnectionSlot slot;
          TlsConn conn(ssl_ctx, client_fd);
          if (!conn.accept()) return;
          handle_connection(conn, queue.get());
        }).detach();
      } else {
        std::thread([client_fd, queue]() {
          ConnectionSlot slot;
          PlainConn conn(client_fd);
          handle_connection(conn, queue.get());
          close(client_fd);
        }).detach();
      }
    } catch (const std::exception& e) {
      // Out of threads. Drop this connection but keep listening: the slot is
      // ours to release, since no ConnectionSlot was ever constructed.
      fprintf(stderr, "vsql_rest: cannot start connection thread: %s\n", e.what());
      g_active_connections.fetch_sub(1, std::memory_order_relaxed);
      close(client_fd);
    }
  }
}

}  // namespace vsql_rest
