# AGENTS.md

Guidance for AI coding assistants working in this repository.

**Note**: Also check `AGENTS.local.md` for machine-specific paths when present.

## Project Overview

`vsql_rest` is a VillageSQL extension that runs an embedded HTTP/HTTPS server
inside the database process, exposing tables as REST endpoints. It uses four
preview VEF capabilities: `thread_worker` (background server loop),
`sql_query` (execute SQL from that loop), `sys_var` (configuration), and
`status_var` (observability counters).

Install name: `vsql_rest`. Repo/directory name: `vsql-rest`.

## Build

```bash
VillageSQL_BUILD_DIR=/path/to/villagesql/build bash build.sh
```

Requires OpenSSL. On macOS:
```bash
cmake -S . -B build \
  -DVillageSQL_BUILD_DIR=/path/to/build \
  -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
cmake --build build
cmake --install build
```

## Install and enable

```sql
INSTALL EXTENSION vsql_rest;
SET GLOBAL vsql_rest.schema = 'mydb';
SET GLOBAL vsql_rest.port = 3000;
SET GLOBAL vsql_rest.vsql_rest_enabled = ON;
```

## Run tests

Stage the suite inside the server source tree first and run it by name. Tests
with helper scripts resolve them via `$MYSQL_TEST_DIR/suite/vsql-rest/t/`, so
passing this directory to `--suite=` by path makes them fail to find the helper.

```bash
ln -s /path/to/vsql-rest/mysql-test \
      /path/to/villagesql-server/mysql-test/suite/vsql-rest
cd /path/to/villagesql/build/mysql-test
perl mysql-test-run.pl \
  --suite=vsql-rest \
  --mysqld=--vsql_allow_preview_extensions=ON
perl mysql-test-run.pl \
  --suite=vsql-rest \
  --mysqld=--vsql_allow_preview_extensions=ON \
  --record
```

## Architecture

**Concurrency model**: `thread_worker` owns a single SQL session (via `sql_query`).
Worker `std::thread`s handle TLS accept + HTTP parsing and enqueue requests onto a
mutex-protected queue. The thread_worker drains the queue, executes SQL, and
fulfills per-request `std::promise<Response>`. A signal pipe wakes the
thread_worker when requests are ready.

**`sql_query` constraint**: `SqlQueryCapability::open(handle)` is only valid
inside `POLL_FD` and `PERIODIC` wakeup callbacks — not ENABLE (handle is NULL
there) and not from arbitrary pthreads. The session is opened on each callback
and closed via RAII at callback exit.

**SQL injection prevention**: `sql_query` executes string SQL with no parameter
binding. All user-supplied values are escaped before interpolation. Column and
table names are validated against the schema cache whitelist — never interpolated
directly from request input.

## Source files

| File | Purpose |
|---|---|
| `src/vsql_rest.cc` | VEF entry point, capability globals, thread_worker callback |
| `src/http_server.cc` / `.h` | TCP accept loop, connection thread dispatch |
| `src/tls.cc` / `.h` | OpenSSL SSL_CTX init, TLS handshake, read/write wrappers |
| `src/sql_executor.cc` / `.h` | SQL generation, escaping, result fetching |
| `src/jwt_auth.cc` / `.h` | JWT parse, HS256/RS256 verify, claim extraction |
| `src/json_emit.cc` / `.h` | Result rows → JSON array serialization |
| `src/schema_cache.cc` / `.h` | INFORMATION_SCHEMA introspection, FK graph, TTL refresh |
| `src/request_queue.cc` / `.h` | Thread-safe request/response queue + pipe signal |
| `src/third_party/picohttpparser.h` | MIT HTTP/1.1 parser (~500 lines, header-only) |
| `src/third_party/json.hpp` | nlohmann/json single-header for request body parsing |

## Sys vars

| Variable | Type | Default | Purpose |
|---|---|---|---|
| `vsql_rest.vsql_rest_enabled` | BOOL | ON | Start/stop the server (thread_worker control var) |
| `vsql_rest.port` | INT | 3000 | HTTP listen port (0 = OS-assigned) |
| `vsql_rest.ssl_port` | INT | 3443 | HTTPS listen port (0 = OS-assigned) |
| `vsql_rest.ssl_cert` | STR | `""` | Path to TLS cert file |
| `vsql_rest.ssl_key` | STR | `""` | Path to TLS key file |
| `vsql_rest.schema` | STR | `""` | Exposed database schema |
| `vsql_rest.require_auth` | BOOL | OFF | Require JWT on all requests |
| `vsql_rest.jwt_secret` | STR | `""` | HMAC secret for HS256 JWTs |
| `vsql_rest.jwt_public_key` | STR | `""` | Path to RSA public key for RS256 JWTs |
| `vsql_rest.schema_ttl` | INT | 60 | Schema cache TTL in seconds |
| `vsql_rest.max_rows` | INT | 1000 | Default row cap when no ?limit given |

## Status vars (SHOW STATUS LIKE 'vsql_rest%')

- `vsql_rest.requests_total` — total requests processed
- `vsql_rest.connections_total` — total TCP connections accepted
- `vsql_rest.requests_active` — requests currently queued
- `vsql_rest.http_port` — actual bound HTTP port (0 when not listening)
- `vsql_rest.https_port` — actual bound HTTPS port (0 when not listening)

## Preview APIs in use

All four are from `villagesql/preview/`. Their API and ABI may change between
VillageSQL releases. Building requires `include-dev/` to precede `include/` in
the compiler include path (handled by CMakeLists.txt).

- `preview/thread_worker` — background server loop
- `preview/sql_query` — execute SQL from thread_worker context
- `preview/sys_var` — configuration variables
- `preview/status_var` — observability counters

## VEF API conventions

- Include `<villagesql/vsql.h>`. No `abi/` headers.
- Typed wrappers: `StringArg`, `IntArg`, `StringResult`, `IntResult`, etc.
- `make_extension().with(cap)` to register preview capabilities.
- Every VDF entry point is wrapped in `try/catch (...)`.
- Copyright header (GPL-2.0) on every `.cc`, `.h`, `CMakeLists.txt`.
