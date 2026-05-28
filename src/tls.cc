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

#include "tls.h"

#include <unistd.h>

#include <openssl/err.h>

namespace vsql_rest {

// TlsContext

TlsContext::~TlsContext() { reset(); }

void TlsContext::reset() {
  if (ctx_) {
    SSL_CTX_free(ctx_);
    ctx_ = nullptr;
  }
}

bool TlsContext::init(const std::string& cert_path, const std::string& key_path,
                      std::string& error) {
  reset();
  ctx_ = SSL_CTX_new(TLS_server_method());
  if (!ctx_) {
    error = "SSL_CTX_new failed";
    return false;
  }

  // Disable deprecated protocols.
  SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

  if (SSL_CTX_use_certificate_file(ctx_, cert_path.c_str(),
                                   SSL_FILETYPE_PEM) != 1) {
    error = "cannot load certificate: " + cert_path;
    reset();
    return false;
  }

  if (SSL_CTX_use_PrivateKey_file(ctx_, key_path.c_str(),
                                  SSL_FILETYPE_PEM) != 1) {
    error = "cannot load private key: " + key_path;
    reset();
    return false;
  }

  if (SSL_CTX_check_private_key(ctx_) != 1) {
    error = "certificate and private key do not match";
    reset();
    return false;
  }

  return true;
}

// TlsConn

TlsConn::TlsConn(SSL_CTX* ctx, int fd) : fd_(fd) {
  if (!ctx) return;
  ssl_ = SSL_new(ctx);
  if (ssl_) SSL_set_fd(ssl_, fd);
}

TlsConn::~TlsConn() {
  if (ssl_) {
    SSL_shutdown(ssl_);
    SSL_free(ssl_);
  }
  if (fd_ >= 0) close(fd_);
}

bool TlsConn::accept() {
  if (!ssl_) return false;
  return SSL_accept(ssl_) == 1;
}

int TlsConn::read(void* buf, int len) {
  if (!ssl_) return -1;
  return SSL_read(ssl_, buf, len);
}

int TlsConn::write(const void* buf, int len) {
  if (!ssl_) return -1;
  return SSL_write(ssl_, buf, len);
}

bool TlsConn::write_all(const void* buf, int len) {
  const char* p = static_cast<const char*>(buf);
  int remaining = len;
  while (remaining > 0) {
    int n = SSL_write(ssl_, p, remaining);
    if (n <= 0) return false;
    p += n;
    remaining -= n;
  }
  return true;
}

// PlainConn

int PlainConn::read(void* buf, int len) {
  return static_cast<int>(::read(fd_, buf, len));
}

int PlainConn::write(const void* buf, int len) {
  return static_cast<int>(::write(fd_, buf, len));
}

bool PlainConn::write_all(const void* buf, int len) {
  const char* p = static_cast<const char*>(buf);
  int remaining = len;
  while (remaining > 0) {
    int n = static_cast<int>(::write(fd_, p, remaining));
    if (n <= 0) return false;
    p += n;
    remaining -= n;
  }
  return true;
}

}  // namespace vsql_rest
