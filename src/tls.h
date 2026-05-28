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

#ifndef VSQL_REST_TLS_H
#define VSQL_REST_TLS_H

#include <memory>
#include <string>

#include <openssl/ssl.h>

namespace vsql_rest {

// RAII wrapper around SSL_CTX. Shared across all TLS connection threads.
class TlsContext {
 public:
  TlsContext() = default;
  ~TlsContext();

  TlsContext(const TlsContext&) = delete;
  TlsContext& operator=(const TlsContext&) = delete;

  // Load cert and key files. Returns false and populates error on failure.
  bool init(const std::string& cert_path, const std::string& key_path,
            std::string& error);

  bool is_valid() const noexcept { return ctx_ != nullptr; }
  SSL_CTX* get() const noexcept { return ctx_; }

  void reset();

 private:
  SSL_CTX* ctx_{nullptr};
};

// RAII wrapper around SSL* + underlying socket fd.
class TlsConn {
 public:
  explicit TlsConn(SSL_CTX* ctx, int fd);
  ~TlsConn();

  TlsConn(const TlsConn&) = delete;
  TlsConn& operator=(const TlsConn&) = delete;

  // Perform TLS server handshake. Returns false on failure.
  bool accept();

  // Read/write wrappers. Return bytes transferred, or -1 on error.
  int read(void* buf, int len);
  int write(const void* buf, int len);

  // Send all bytes in buf. Returns false if send fails partway.
  bool write_all(const void* buf, int len);

  bool is_valid() const noexcept { return ssl_ != nullptr; }

 private:
  SSL* ssl_{nullptr};
  int fd_{-1};
};

// Plain (non-TLS) socket wrapper with the same read/write interface.
class PlainConn {
 public:
  explicit PlainConn(int fd) : fd_(fd) {}
  ~PlainConn() = default;

  int read(void* buf, int len);
  int write(const void* buf, int len);
  bool write_all(const void* buf, int len);

 private:
  int fd_{-1};
};

}  // namespace vsql_rest

#endif  // VSQL_REST_TLS_H
