# Testing

## Prerequisites

- VillageSQL build directory (with `vsql_rest.veb` installed)
- The extension must be installed before running: `cmake --install build`
- `curl`, `python3`, and `openssl` must be in PATH
- A running VillageSQL server

## Environment variables

| Variable | Purpose |
|---|---|
| `VillageSQL_BUILD_DIR` | Path to VillageSQL build tree (required for build) |
| `OPENSSL_ROOT_DIR` | OpenSSL prefix (macOS with Homebrew usually needs this) |

## Build and install

```bash
# Linux
VillageSQL_BUILD_DIR=$HOME/build/villagesql bash build.sh

# macOS
cmake -S . -B build \
  -DVillageSQL_BUILD_DIR=~/.villagesql/build \
  -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
cmake --build build
cmake --install build
```

## Run the full test suite

The suite must be staged as `mysql-test/suite/vsql-rest/` inside the server
**source** tree and run by suite name. Tests that shell out to a helper script
resolve it through `$MYSQL_TEST_DIR/suite/vsql-rest/t/`, which only exists once
the suite is staged there — passing this directory to `--suite=` by path
instead makes those tests fail to find their helper. This is the same layout
`villagesql/bld_tools/test_extension_vebs.sh` sets up for CI.

```bash
ln -s /path/to/vsql-rest/mysql-test \
      /path/to/villagesql-server/mysql-test/suite/vsql-rest
cd /path/to/villagesql/build/mysql-test
perl mysql-test-run.pl \
  --suite=vsql-rest \
  --mysqld=--vsql_allow_preview_extensions=ON
```

`rest_timeouts` waits out the 20-second read deadline, so the suite takes
roughly 25 seconds longer than the other tests would suggest.

## Regenerate result files after code changes

```bash
cd /path/to/villagesql/build/mysql-test
perl mysql-test-run.pl \
  --suite=vsql-rest \
  --mysqld=--vsql_allow_preview_extensions=ON \
  --record
```

## Test files

| File | Criteria covered |
|---|---|
| `rest_lifecycle.test` | Server start (C1), server stop on disable (C2) |
| `rest_filters.test` | All filter operators: eq, neq, lt/lte/gt/gte, like, cs_like, in, is.null/is.not.null, or, AND, empty result (C3-C13) |
| `rest_pagination.test` | Ordering, limit+offset, Content-Range header, Prefer: count=exact, column select (C14-C18) |
| `rest_writes.test` | POST single row, POST with Prefer: return=representation, bulk POST, PATCH/204, PATCH with repr, DELETE/204, unfiltered DELETE/PATCH guard (C19-C24, C34-C35) |
| `rest_embedding.test` | Forward FK embed (customers→orders), reverse FK embed (orders→customers) (C25-C26) |
| `rest_rpc.test` | Stored function via RPC, DML stored procedure via RPC, Content-Type and Content-Length headers (C27-C30) |
| `rest_errors.test` | 404 for unknown table, 400 for invalid JSON body, 400 for unknown operator, unfiltered DELETE/PATCH guard (C31-C35) |
| `rest_auth.test` | 401 with no token, 200 with valid HS256 JWT, 401 with expired JWT, JWT sub injected as user variable (C36-C40) |
| `rest_https.test` | HTTPS on ssl_port, HTTP and HTTPS simultaneous (C41-C42) |
| `rest_discovery.test` | GET / returns OpenAPI-compatible definitions (C43) |

## Shared includes

- `rest_setup.inc` — installs extension, creates `test_rest` database, loads test data, enables the server on an OS-assigned port (`port = 0`) discovered via the `vsql_rest.http_port` status var
- `rest_teardown.inc` — disables server, uninstalls extension, drops database
- `rest_wait_http.inc` / `rest_wait_https.inc` — wait for the listener to bind, then capture the OS-assigned port into `$REST_PORT` / `$REST_SSL_PORT` (re-source after re-enabling, since the port changes on every enable)
- `rest_jwt_helper.py` — Python helper for generating HS256 JWT tokens at test runtime (used by `rest_auth.test`)

## Notes

- Tests bind OS-assigned ports (`port`/`ssl_port` = 0) and read the actual port from the `vsql_rest.http_port` / `vsql_rest.https_port` status vars, so parallel MTR workers never collide on a fixed port.
- The HTTPS test generates a temporary self-signed certificate via `openssl req` and uses `curl -k` to skip verification.
- The auth test generates JWT tokens at runtime using Python's `hmac` module. The secret `test-secret-key` is hardcoded in both the test and the helper script.
- Stored procedures that return result sets via `SELECT` are not testable via `/rpc/` — see Known Limitations in README.md.
