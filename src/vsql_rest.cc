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

#include <villagesql/vsql.h>
#include <villagesql/preview/thread_worker.h>
#include <villagesql/preview/sql_query.h>
#include <villagesql/preview/sys_var.h>
#include <villagesql/preview/status_var.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <cerrno>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>

#include "http_server.h"
#include "json_emit.h"
#include "request_queue.h"
#include "schema_cache.h"
#include "sql_executor.h"

// ============================================================================
// Global state
// ============================================================================

namespace {

// Sys var backing storage. Written by the server when SET GLOBAL fires. The
// char* variables are never dereferenced here: read them with read_str_var().
static long long g_port        = 3000;
static long long g_ssl_port    = 3443;
static char*     g_ssl_cert    = nullptr;
static char*     g_ssl_key     = nullptr;
static char*     g_schema      = nullptr;
static bool      g_require_auth = false;
static char*     g_jwt_secret  = nullptr;
static char*     g_jwt_pubkey  = nullptr;
static long long g_schema_ttl       = 60;
static long long g_max_rows         = 1000;
static char*     g_allowed_tables    = nullptr;
static char*     g_allowed_routines  = nullptr;
static char*     g_table_methods     = nullptr;

// Status var backing storage. Written by the extension.
static long long g_requests_total     = 0;
static long long g_connections_total  = 0;
static long long g_requests_active    = 0;
static long long g_http_port_actual   = 0;
static long long g_https_port_actual  = 0;

// Runtime state — valid only between ENABLE and DISABLE.
static int                g_listen_fd     = -1;
static int                g_ssl_listen_fd = -1;
static vsql_rest::TlsContext g_tls_ctx;
// shared_ptr, not optional: detached connection threads are never joined and
// keep their own reference, so DISABLE dropping this one cannot free the queue
// out from under a thread still mid-request.
static std::shared_ptr<vsql_rest::RequestQueue> g_queue;
static std::atomic<bool>  g_running{false};
static std::thread        g_accept_thread;
static std::thread        g_ssl_accept_thread;
static vsql_rest::SchemaCache g_schema_cache;

}  // namespace

// ============================================================================
// Capabilities
// ============================================================================

namespace stv = vsql::preview_status_var;
namespace syv = vsql::preview_sys_var;
namespace sq  = vsql::preview_sql_query;
namespace tw  = vsql::preview_thread_worker;

static sq::SqlQueryCapability g_sql_query_cap;

static auto g_sys_vars = syv::make_capability({
  syv::make_int ("port",           "HTTP listen port (0 = OS-assigned)",  &g_port,     3000, 0, 65535),
  syv::make_int ("ssl_port",       "HTTPS listen port (0 = OS-assigned)", &g_ssl_port, 3443, 0, 65535),
  syv::make_str ("ssl_cert",       "Path to TLS cert file",     &g_ssl_cert,     ""),
  syv::make_str ("ssl_key",        "Path to TLS key file",      &g_ssl_key,      ""),
  syv::make_str ("schema",         "Exposed database schema",   &g_schema,       ""),
  syv::make_bool("require_auth",   "Require JWT on all requests",&g_require_auth, false),
  syv::make_str ("jwt_secret",     "HMAC secret for HS256",     &g_jwt_secret,   ""),
  syv::make_str ("jwt_public_key", "RSA public key path (RS256)",&g_jwt_pubkey,   ""),
  syv::make_int ("schema_ttl",     "Schema cache TTL (seconds)", &g_schema_ttl,   60, 1, 86400),
  syv::make_int ("max_rows",       "Default row limit",          &g_max_rows,    1000, 1, 1000000),
  syv::make_str ("allowed_tables",   "Comma-separated table allowlist (empty = all)",    &g_allowed_tables,   ""),
  syv::make_str ("allowed_routines", "Comma-separated routine allowlist (empty = all)",  &g_allowed_routines, ""),
  syv::make_str ("table_methods",    "Per-table method restrictions: tbl:GET,POST|tbl2:GET", &g_table_methods, ""),
});

static auto g_status_vars = stv::make_capability({
  stv::make_int("requests_total",    &g_requests_total),
  stv::make_int("connections_total", &g_connections_total),
  stv::make_int("requests_active",   &g_requests_active),
  stv::make_int("http_port",         &g_http_port_actual),
  stv::make_int("https_port",        &g_https_port_actual),
});

// String sys vars are registered PLUGIN_VAR_MEMALLOC, so a SET GLOBAL frees the
// old buffer from its own thread while this extension may be reading it. get()
// asks the server, which copies the value under LOCK_global_system_variables.
// It fails for a value of 1023 bytes or more (the server reads into a 1 KiB
// buffer), so callers must treat false as an error, not as an empty value.
static bool read_str_var(const char* name, std::string& out) {
  return !g_sys_vars.get("vsql_rest", name, out);
}

// ============================================================================
// Access-control config parsers
// ============================================================================

static std::string trim_token(const std::string& s) {
  auto start = s.find_first_not_of(' ');
  if (start == std::string::npos) return {};
  return s.substr(start, s.find_last_not_of(' ') - start + 1);
}

static std::unordered_set<std::string> parse_allowed_tables(const char* sv) {
  std::unordered_set<std::string> result;
  if (!sv || !*sv) return result;
  std::istringstream ss(sv);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    auto t = trim_token(tok);
    if (!t.empty()) result.insert(std::move(t));
  }
  return result;
}

// Format: "orders:GET,POST|users:GET"
static std::unordered_map<std::string, std::unordered_set<std::string>>
parse_table_methods(const char* sv) {
  std::unordered_map<std::string, std::unordered_set<std::string>> result;
  if (!sv || !*sv) return result;
  std::istringstream pipes(sv);
  std::string entry;
  while (std::getline(pipes, entry, '|')) {
    auto colon = entry.find(':');
    if (colon == std::string::npos) continue;
    std::string table = trim_token(entry.substr(0, colon));
    if (table.empty()) continue;
    std::unordered_set<std::string> methods;
    std::istringstream ms(entry.substr(colon + 1));
    std::string m;
    while (std::getline(ms, m, ',')) {
      std::string meth = trim_token(m);
      if (meth.empty()) continue;
      for (char& c : meth) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      methods.insert(meth);
    }
    if (!methods.empty()) result[std::move(table)] = std::move(methods);
  }
  return result;
}

// ============================================================================
// Thread worker callback
// ============================================================================

// Answers each request with an error. Each connection thread waits on its
// request's promise (handle_connection in http_server.cc), so a wakeup that
// cannot run requests must still answer them; the undrained signal pipe would
// also keep the poll fd readable.
static void reject(std::vector<vsql_rest::PendingRequest> pending,
                   vsql_rest::Status status, const std::string& message,
                   std::string_view code = "VSQL0001") {
  for (auto& p : pending) {
    vsql_rest::HttpResponse resp;
    resp.status = status;
    resp.body = vsql_rest::emit_error(message, code);
    p.promise.set_value(std::move(resp));
  }
}

static vef_next_wakeup_t rest_worker(vef_wakeup_reason_t reason,
                                     vef_thread_handle_t* handle,
                                     void* /*arg*/) {
  switch (reason) {
    case VEF_WAKEUP_ENABLE: {
      // handle is NULL at ENABLE — cannot open SQL session here.
      // Bring up every required listener BEFORE committing state. When
      // cert+key are set the HTTPS listener is required: any failure tears
      // down and leaves the extension disabled rather than silently serving
      // plaintext-only.
      int http_port = 0;
      int http_fd = vsql_rest::create_listen_socket(static_cast<int>(g_port),
                                                    &http_port);
      if (http_fd < 0) {
        fprintf(stderr, "[vsql_rest] HTTP bind failed on port %lld: %s\n",
                g_port, strerror(errno));
        return {};
      }

      int ssl_fd = -1;
      int ssl_port = 0;
      std::string cert;
      std::string key;
      if (!read_str_var("ssl_cert", cert) || !read_str_var("ssl_key", key)) {
        fprintf(stderr, "[vsql_rest] cannot read ssl_cert or ssl_key\n");
        close(http_fd);
        return {};
      }
      if (!cert.empty() && !key.empty()) {
        std::string tls_err;
        if (!g_tls_ctx.init(cert, key, tls_err)) {
          fprintf(stderr, "[vsql_rest] TLS init failed: %s\n", tls_err.c_str());
          close(http_fd);
          return {};
        }
        ssl_fd = vsql_rest::create_listen_socket(static_cast<int>(g_ssl_port),
                                                 &ssl_port);
        if (ssl_fd < 0) {
          fprintf(stderr, "[vsql_rest] HTTPS bind failed on port %lld: %s\n",
                  g_ssl_port, strerror(errno));
          close(http_fd);
          g_tls_ctx.reset();
          return {};
        }
      }

      // All required listeners are up — commit state.
      g_queue = std::make_shared<vsql_rest::RequestQueue>();
      g_running.store(true, std::memory_order_relaxed);
      g_listen_fd = http_fd;
      g_http_port_actual = http_port;
      g_ssl_listen_fd = ssl_fd;
      g_https_port_actual = (ssl_fd >= 0) ? ssl_port : 0;

      // std::thread's constructor can throw std::system_error (e.g. EAGAIN
      // when the process/thread-count limit is hit) -- a real condition
      // under load, not exotic. The VEF SDK does not catch exceptions at the
      // thread_worker entry-point boundary, so an escaping exception here
      // would crash the whole server rather than just fail this ENABLE.
      // accept_loop() itself already guards its own per-connection
      // std::thread(...) the same way (http_server.cc); this brings ENABLE's
      // two accept-thread constructions in line with that.
      try {
        g_accept_thread = std::thread(vsql_rest::accept_loop,
                                      g_listen_fd, nullptr,
                                      g_queue, &g_running);
        if (g_ssl_listen_fd >= 0) {
          g_ssl_accept_thread = std::thread(vsql_rest::accept_loop,
                                            g_ssl_listen_fd,
                                            g_tls_ctx.get(),
                                            g_queue, &g_running);
        }
      } catch (const std::exception &e) {
        fprintf(stderr, "[vsql_rest] failed to start accept thread: %s\n",
                e.what());
        g_running.store(false, std::memory_order_relaxed);
        if (g_listen_fd >= 0) {
          shutdown(g_listen_fd, SHUT_RDWR);
          close(g_listen_fd);
          g_listen_fd = -1;
        }
        if (g_ssl_listen_fd >= 0) {
          shutdown(g_ssl_listen_fd, SHUT_RDWR);
          close(g_ssl_listen_fd);
          g_ssl_listen_fd = -1;
        }
        if (g_accept_thread.joinable()) g_accept_thread.join();
        if (g_ssl_accept_thread.joinable()) g_ssl_accept_thread.join();
        g_tls_ctx.reset();
        g_queue.reset();
        g_http_port_actual = 0;
        g_https_port_actual = 0;
        return {};
      }

      // Re-arm on signal pipe with 50ms fallback.
      return {50, g_queue->signal_fd()};
    }

    case VEF_WAKEUP_POLL_FD:
    case VEF_WAKEUP_PERIODIC: {
      if (!g_queue) return {};

      std::string schema_name;
      if (!read_str_var("schema", schema_name)) {
        reject(g_queue->drain(), vsql_rest::Status::kInternalServerError,
               "cannot read vsql_rest.schema");
        return {};
      }
      if (schema_name.empty()) {
        reject(g_queue->drain(), vsql_rest::Status::kServiceUnavailable,
               "vsql_rest.schema is not set", "VSQL0007");
        return {};
      }

      // Open SQL session for this wakeup.
      if (!handle) {
        reject(g_queue->drain(), vsql_rest::Status::kServiceUnavailable,
               "SQL session unavailable");
        return {};
      }
      auto session = g_sql_query_cap.open(handle);
      if (!session) {
        reject(g_queue->drain(), vsql_rest::Status::kServiceUnavailable,
               "SQL session unavailable");
        return {};
      }

      // The SQL executor escapes user input with backslash escaping. If the
      // server runs with NO_BACKSLASH_ESCAPES that escaping is defeated (\\'
      // leaves the quote live -> SQL injection), so strip that mode from this
      // internal session. Comma-guarded so removing it never leaves a stray
      // separator; other modes (e.g. strict) are preserved.
      session.sql("SET SESSION sql_mode = TRIM(BOTH ',' FROM REPLACE("
                  "CONCAT(',', @@SESSION.sql_mode, ','), "
                  "',NO_BACKSLASH_ESCAPES,', ','))").execute();

      // Refresh schema cache if needed.
      g_schema_cache.refresh_if_needed(session, schema_name,
                                       static_cast<int>(g_schema_ttl));

      // Drain and process all queued requests.
      auto pending = g_queue->drain();
      if (pending.empty()) {
        return reason == VEF_WAKEUP_POLL_FD
                   ? vef_next_wakeup_t{0, g_queue->signal_fd()}
                   : vef_next_wakeup_t{};
      }

      std::string jwt_secret;
      std::string jwt_pubkey;
      std::string allowed_tables_sv;
      std::string allowed_routines_sv;
      std::string table_methods_sv;
      const std::pair<const char*, std::string*> str_vars[] = {
          {"jwt_secret", &jwt_secret},
          {"jwt_public_key", &jwt_pubkey},
          {"allowed_tables", &allowed_tables_sv},
          {"allowed_routines", &allowed_routines_sv},
          {"table_methods", &table_methods_sv}};
      for (const auto& [name, out] : str_vars) {
        // An unreadable allowlist must not be taken as empty, which allows
        // every table.
        if (!read_str_var(name, *out)) {
          reject(std::move(pending), vsql_rest::Status::kInternalServerError,
                 std::string("cannot read vsql_rest.") + name);
          return {};
        }
      }

      // Parse access-control config once per wakeup (cheap; may change via SET GLOBAL).
      auto allowed_tables   = parse_allowed_tables(allowed_tables_sv.c_str());
      auto allowed_routines = parse_allowed_tables(allowed_routines_sv.c_str());
      auto table_methods    = parse_table_methods(table_methods_sv.c_str());

      g_requests_active += static_cast<long long>(pending.size());

      for (auto& p : pending) {
        ++g_requests_total;
        vsql_rest::HttpResponse resp = vsql_rest::execute_request(
            session, p.req,
            schema_name, g_schema_cache,
            jwt_secret,
            jwt_pubkey,
            g_require_auth,
            g_max_rows,
            allowed_tables,
            allowed_routines,
            table_methods);
        --g_requests_active;

        p.promise.set_value(std::move(resp));
      }

      if (reason == VEF_WAKEUP_POLL_FD) {
        return {0, g_queue->signal_fd()};
      }
      return {};
    }

    case VEF_WAKEUP_DISABLE: {
      // Signal accept threads to stop, close sockets.
      g_running.store(false, std::memory_order_relaxed);
      g_http_port_actual = 0;
      g_https_port_actual = 0;

      // shutdown() before close() so accept() returns immediately on Linux.
      // close() alone does not wake a thread blocked in accept() on the same
      // fd — the call keeps blocking and the subsequent join() hangs forever.
      // macOS happens to wake on close, which is why this only surfaces on
      // Linux runners.
      if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
      }
      if (g_ssl_listen_fd >= 0) {
        shutdown(g_ssl_listen_fd, SHUT_RDWR);
        close(g_ssl_listen_fd);
        g_ssl_listen_fd = -1;
      }

      if (g_accept_thread.joinable()) g_accept_thread.join();
      if (g_ssl_accept_thread.joinable()) g_ssl_accept_thread.join();

      // Detached connection threads outlive the accept loop and run code from
      // this library, which UNINSTALL is free to unload once DISABLE returns.
      if (!vsql_rest::drain_connections()) {
        fprintf(stderr,
                "[vsql_rest] timed out waiting for connection threads\n");
      }

      g_tls_ctx.reset();

      g_queue.reset();

      // g_schema_cache is static and outlives DISABLE, and even UNINSTALL and
      // INSTALL in one server, so a table created while the server was off
      // would otherwise stay invisible until the TTL expires.
      g_schema_cache.clear();

      return {};
    }
  }
  return {};
}

static tw::ThreadWorkerCapability<&rest_worker> g_worker{"rest",
                                                         "vsql_rest_enabled"};

// ============================================================================
// VEF entry point
// ============================================================================

VEF_GENERATE_ENTRY_POINTS(
    vsql::make_extension()
        .with(g_worker)
        .with(g_sql_query_cap)
        .with(g_sys_vars)
        .with(g_status_vars))
