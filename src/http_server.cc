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
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "third_party/picohttpparser.h"

namespace vsql_rest {

int create_listen_socket(int port) {
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
  return fd;
}

std::string format_http_response(const HttpResponse& resp) {
  std::string out;
  out.reserve(256 + resp.body.size());

  // Status line.
  const char* reason = "OK";
  switch (resp.status) {
    case 200: reason = "OK"; break;
    case 201: reason = "Created"; break;
    case 204: reason = "No Content"; break;
    case 400: reason = "Bad Request"; break;
    case 401: reason = "Unauthorized"; break;
    case 404: reason = "Not Found"; break;
    case 405: reason = "Method Not Allowed"; break;
    case 409: reason = "Conflict"; break;
    case 500: reason = "Internal Server Error"; break;
    default:  reason = "Unknown"; break;
  }

  char status_line[64];
  snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d %s\r\n",
           resp.status, reason);
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
static std::string read_request_raw(Conn& conn) {
  std::string buf;
  buf.reserve(4096);
  char tmp[4096];
  while (buf.size() < 65536) {
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
  std::string raw = read_request_raw(conn);
  if (raw.empty()) return;

  HttpRequest req;
  size_t header_len = 0;
  if (!parse_http_request(raw, req, header_len)) return;

  // Read body if Content-Length is present.
  auto cl_it = req.headers.find("content-length");
  if (cl_it != req.headers.end()) {
    long long content_length = 0;
    try { content_length = std::stoll(cl_it->second); } catch (...) {}
    if (content_length > 0) {
      // How many body bytes are already in raw?
      size_t already = raw.size() > header_len ? raw.size() - header_len : 0;
      req.body = raw.substr(header_len, already);
      // Read remaining body bytes.
      long long remaining = content_length - static_cast<long long>(already);
      while (remaining > 0) {
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
      resp.status = 503;
      resp.body = "{\"message\":\"server shutting down\",\"details\":null,"
                  "\"hint\":null,\"code\":\"VSQL0003\"}";
      resp.headers.emplace_back("Content-Type", "application/json");
    }
  } else {
    resp.status = 503;
    resp.body = "{\"message\":\"request timed out\",\"details\":null,"
                "\"hint\":null,\"code\":\"VSQL0002\"}";
    resp.headers.emplace_back("Content-Type", "application/json");
  }

  std::string raw_resp = format_http_response(resp);
  conn.write_all(raw_resp.data(), static_cast<int>(raw_resp.size()));
}

void accept_loop(int listen_fd, SSL_CTX* ssl_ctx, RequestQueue* queue,
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

    // Detach a thread per connection. Each connection handles one request.
    if (ssl_ctx) {
      std::thread([client_fd, ssl_ctx, queue]() {
        TlsConn conn(ssl_ctx, client_fd);
        if (!conn.accept()) return;
        handle_connection(conn, queue);
      }).detach();
    } else {
      std::thread([client_fd, queue]() {
        PlainConn conn(client_fd);
        handle_connection(conn, queue);
        close(client_fd);
      }).detach();
    }
  }
}

}  // namespace vsql_rest
