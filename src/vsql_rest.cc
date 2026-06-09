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
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <sys/socket.h>
#include <unistd.h>

#include "http_server.h"
#include "request_queue.h"
#include "schema_cache.h"
#include "sql_executor.h"

// ============================================================================
// Global state
// ============================================================================

namespace {

// Sys var backing storage. Written by the server when SET GLOBAL fires.
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

// Runtime state — valid only between ENABLE and DISABLE.
static int                g_listen_fd     = -1;
static int                g_ssl_listen_fd = -1;
static vsql_rest::TlsContext g_tls_ctx;
static std::optional<vsql_rest::RequestQueue> g_queue;
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
  syv::make_int ("port",           "HTTP listen port",          &g_port,         3000, 1, 65535),
  syv::make_int ("ssl_port",       "HTTPS listen port",         &g_ssl_port,     3443, 1, 65535),
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
});

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

static vef_next_wakeup_t rest_worker(vef_wakeup_reason_t reason,
                                     vef_thread_handle_t* handle,
                                     void* /*arg*/) {
  switch (reason) {
    case VEF_WAKEUP_ENABLE: {
      // handle is NULL at ENABLE — cannot open SQL session here.
      // Try to bind listen sockets BEFORE setting any state. If HTTP bind
      // fails (e.g. port in use), leave the extension cleanly disabled
      // rather than half-enabled with no listener.
      int http_fd = vsql_rest::create_listen_socket(static_cast<int>(g_port));
      if (http_fd < 0) {
        // Bind failed. Don't touch g_queue / g_running. DISABLE on this
        // state will be a clean no-op.
        return {};
      }

      g_queue.emplace();
      g_running.store(true, std::memory_order_relaxed);
      g_listen_fd = http_fd;

      // HTTPS listener (only if cert+key are configured).
      std::string cert = g_ssl_cert ? g_ssl_cert : "";
      std::string key  = g_ssl_key  ? g_ssl_key  : "";
      if (!cert.empty() && !key.empty()) {
        std::string tls_err;
        if (g_tls_ctx.init(cert, key, tls_err)) {
          g_ssl_listen_fd = vsql_rest::create_listen_socket(
              static_cast<int>(g_ssl_port));
        }
      }

      // Start accept threads.
      g_accept_thread = std::thread(vsql_rest::accept_loop,
                                    g_listen_fd, nullptr,
                                    &*g_queue, &g_running);
      if (g_ssl_listen_fd >= 0 && g_tls_ctx.is_valid()) {
        g_ssl_accept_thread = std::thread(vsql_rest::accept_loop,
                                          g_ssl_listen_fd,
                                          g_tls_ctx.get(),
                                          &*g_queue, &g_running);
      }

      // Re-arm on signal pipe with 50ms fallback.
      return {50, g_queue->signal_fd()};
    }

    case VEF_WAKEUP_POLL_FD:
    case VEF_WAKEUP_PERIODIC: {
      if (!g_queue || !handle) return {};

      std::string schema_name = g_schema ? g_schema : "";
      if (schema_name.empty()) return {};

      // Open SQL session for this wakeup.
      auto session = g_sql_query_cap.open(handle);
      if (!session) return {};

      // Refresh schema cache if needed.
      g_schema_cache.refresh_if_needed(session, schema_name,
                                       static_cast<int>(g_schema_ttl));

      // Parse access-control config once per wakeup (cheap; may change via SET GLOBAL).
      auto allowed_tables   = parse_allowed_tables(g_allowed_tables);
      auto allowed_routines = parse_allowed_tables(g_allowed_routines);
      auto table_methods    = parse_table_methods(g_table_methods);

      // Drain and process all queued requests.
      auto pending = g_queue->drain();
      g_requests_active += static_cast<long long>(pending.size());

      for (auto& p : pending) {
        ++g_requests_total;
        vsql_rest::HttpResponse resp = vsql_rest::execute_request(
            session, p.req,
            schema_name, g_schema_cache,
            g_jwt_secret  ? g_jwt_secret  : "",
            g_jwt_pubkey  ? g_jwt_pubkey  : "",
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

      g_tls_ctx.reset();

      g_queue.reset();

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
