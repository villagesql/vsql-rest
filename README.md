# VillageSQL REST Extension

Turns a VillageSQL database schema into a live HTTP/HTTPS REST API. Install
the extension, point it at a schema, and your tables are immediately
accessible as endpoints — no code required.

```sql
INSTALL EXTENSION vsql_rest;
SET GLOBAL vsql_rest.schema = 'mydb';
SET GLOBAL vsql_rest.port = 3000;
SET GLOBAL vsql_rest.vsql_rest_enabled = ON;
```

```bash
# Read
curl 'http://localhost:3000/orders?status=eq.pending&order=total.desc&limit=10'

# Write
curl -X POST -H 'Content-Type: application/json' \
     -d '{"name":"Alice","email":"alice@example.com"}' \
     http://localhost:3000/customers

# Call a stored function
curl -X POST -H 'Content-Type: application/json' \
     -d '{"a":3,"b":4}' http://localhost:3000/rpc/add_numbers
```

> **Preview APIs:** `vsql_rest` uses four VEF preview APIs (`thread_worker`, `sql_query`, `sys_var`, `status_var`) that may change API or ABI between VillageSQL releases. It's suitable for development and internal tooling — expect possible breaking changes when upgrading the server.

## Building

**Linux:**
```bash
VillageSQL_BUILD_DIR=$HOME/build/villagesql bash build.sh
```

**macOS:**
```bash
cmake -S . -B build \
  -DVillageSQL_BUILD_DIR=~/.villagesql/build \
  -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
cmake --build build
cmake --install build
```

Requires OpenSSL. Install on macOS with Homebrew: `brew install openssl@3`.

## Installing

```sql
INSTALL EXTENSION vsql_rest;
```

Then configure via `SET GLOBAL`:

| Variable | Default | Purpose |
|---|---|---|
| `vsql_rest.vsql_rest_enabled` | OFF | Start/stop the server |
| `vsql_rest.port` | 3000 | HTTP listen port (0 = OS-assigned) |
| `vsql_rest.ssl_port` | 3443 | HTTPS listen port (0 = OS-assigned) |
| `vsql_rest.ssl_cert` | `""` | Path to TLS certificate file |
| `vsql_rest.ssl_key` | `""` | Path to TLS private key file |
| `vsql_rest.schema` | `""` | Database schema to expose |
| `vsql_rest.require_auth` | OFF | Require JWT on all requests |
| `vsql_rest.jwt_secret` | `""` | HMAC secret for HS256 tokens |
| `vsql_rest.jwt_public_key` | `""` | RSA public key path for RS256 tokens |
| `vsql_rest.schema_ttl` | 60 | Schema cache TTL in seconds |
| `vsql_rest.max_rows` | 1000 | Default row cap when no `?limit` |
| `vsql_rest.allowed_tables` | `""` | Comma-separated table allowlist; empty means all tables are accessible |
| `vsql_rest.allowed_routines` | `""` | Comma-separated routine allowlist; empty means all routines are callable |
| `vsql_rest.table_methods` | `""` | Per-table HTTP method restrictions — format: `tbl:GET,POST\|tbl2:GET` |

Observability (via `SHOW STATUS LIKE 'vsql_rest%'`):

| Variable | Purpose |
|---|---|
| `vsql_rest.requests_total` | Total requests processed |
| `vsql_rest.connections_total` | Total TCP connections accepted |
| `vsql_rest.requests_active` | Requests currently queued |
| `vsql_rest.http_port` | Actual bound HTTP port (0 when not listening) |
| `vsql_rest.https_port` | Actual bound HTTPS port (0 when not listening) |

## Function Reference

### Reads: `GET /table`

| Parameter | Format | Example |
|---|---|---|
| Column filter | `col=op.value` | `?name=eq.Alice` |
| OR filter | `or=(f1,f2)` | `?or=(status.eq.pending,status.eq.shipped)` |
| Order | `order=col.dir` | `?order=age.desc` |
| Pagination | `limit=N&offset=M` | `?limit=10&offset=20` |
| Column select | `select=col1,col2` | `?select=id,name` |
| Embedding | `select=*,rel(*)` | `?select=*,orders(*)` |

**Filter operators:**

| Operator | SQL | Example |
|---|---|---|
| `eq` | `=` | `?age=eq.25` |
| `neq` | `!=` | `?status=neq.cancelled` |
| `lt` | `<` | `?age=lt.30` |
| `lte` | `<=` | `?age=lte.30` |
| `gt` | `>` | `?age=gt.18` |
| `gte` | `>=` | `?age=gte.18` |
| `like` | `LIKE` (case-insensitive, MySQL default) | `?name=like.A%25` |
| `cs_like` | `LIKE BINARY` (case-sensitive) | `?name=cs_like.Alice` |
| `in` | `IN (...)` | `?id=in.(1,2,3)` |
| `is` | `IS NULL` / `IS NOT NULL` | `?age=is.null` / `?age=is.not.null` |

Note: `like` uses MySQL's `%` and `_` wildcards (not `*`). URL-encode `%` as `%25`.

**Resource embedding** — use FK relationships to nest related rows:

```bash
# One-to-many: each customer includes their orders
curl 'http://localhost:3000/customers?select=id,name,orders(*)'

# Many-to-one: each order includes the related customer
curl 'http://localhost:3000/orders?select=id,total,customers(name,email)'
```

### Writes

| Method | Endpoint | Body | Response |
|---|---|---|---|
| `POST` | `/table` | JSON object or array | `201 Created` + `Location` header |
| `POST` + `Prefer: return=representation` | `/table` | JSON object | `200` + inserted row |
| `PATCH` | `/table?filter` | JSON object (partial update) | `204 No Content` |
| `PATCH` + `Prefer: return=representation` | `/table?filter` | JSON object | `200` + updated rows |
| `DELETE` | `/table?filter` | — | `204 No Content` |

`PATCH` and `DELETE` require at least one filter — filterless operations return `400` to prevent accidental full-table changes.

**Bulk insert** — POST an array to insert multiple rows in one request:

```bash
curl -X POST -H 'Content-Type: application/json' \
     -d '[{"name":"Bob"},{"name":"Carol"}]' \
     http://localhost:3000/customers
```

### RPC: `POST /rpc/routine_name`

Calls a stored function or DML stored procedure. Pass parameters as a JSON object.

```bash
# Stored function — returns scalar
curl -X POST -H 'Content-Type: application/json' \
     -d '{"a":7,"b":5}' http://localhost:3000/rpc/add_numbers
# Response: 12

# DML stored procedure — side effects only
curl -X POST -H 'Content-Type: application/json' \
     -d '{"order_id":42}' http://localhost:3000/rpc/mark_shipped
# Response: 200
```

### `Content-Range` header

Every `GET` response includes a `Content-Range` header:

- Default: `Content-Range: 0-N/*` (total unknown)
- With `Prefer: count=exact`: `Content-Range: 0-N/TOTAL`

### API Discovery: `GET /`

Returns an OpenAPI-compatible JSON schema with all exposed tables and their column types.

## Authentication (JWT)

Set `vsql_rest.require_auth = ON` to require a valid JWT on every request.

```sql
SET GLOBAL vsql_rest.jwt_secret = 'your-secret';
SET GLOBAL vsql_rest.require_auth = ON;
```

Send the token in the `Authorization` header:

```bash
curl -H 'Authorization: Bearer <token>' http://localhost:3000/customers
```

Supported algorithms: **HS256** (HMAC-SHA256) and **RS256** (RSA-SHA256). For RS256, set `vsql_rest.jwt_public_key` to a PEM file path.

**`exp` is required.** Verification fails closed on expiry: a token whose `exp`
claim is in the past — or that carries no `exp` claim at all — is rejected with
`401`. This is a behavior change from 0.0.2, which accepted tokens with no `exp`
claim and never expired them. If your issuer mints tokens without `exp`, add one
before upgrading; otherwise those callers start receiving:

```json
{"message":"token expired","details":null,"hint":null,"code":"VSQL0001"}
```

**JWT claims as user variables** — after verification, claims are injected as MySQL user variables before query execution:

| JWT claim | MySQL variable |
|---|---|
| `sub` | `@vsql_rest_jwt_sub` |
| `role` | `@vsql_rest_jwt_role` |
| Any string claim | `@vsql_rest_jwt_<claim>` |

These can be used for row-level filtering via views. MySQL views cannot reference user variables directly; use a helper function:

```sql
CREATE FUNCTION vsql_rest_jwt_sub() RETURNS VARCHAR(255)
NOT DETERMINISTIC READS SQL DATA
BEGIN RETURN @vsql_rest_jwt_sub; END;

CREATE VIEW my_view AS
  SELECT * FROM customers WHERE email = vsql_rest_jwt_sub();
```

## Access Control

vsql_rest executes all queries through an internal session — MySQL's `GRANT`/`REVOKE` system doesn't restrict what it can read or write. Three sys vars let you lock down the exposed surface without touching JWT or view configuration.

**Table allowlist** — expose only the tables you intend to:

```sql
SET GLOBAL vsql_rest.allowed_tables = 'orders,customers';
```

Tables not in the list return `404`. The API discovery endpoint (`GET /`) also filters to the allowlist. Empty string (the default) allows all tables in the schema.

**Routine allowlist** — restrict which stored procedures and functions are callable via `/rpc/`:

```sql
SET GLOBAL vsql_rest.allowed_routines = 'add_numbers,mark_shipped';
```

Routines not in the list return `404`. Empty string allows all routines.

**Per-table method restrictions** — make specific tables read-only (or write-only):

```sql
-- customers: read-only. orders: read and create only.
SET GLOBAL vsql_rest.table_methods = 'customers:GET|orders:GET,POST';
```

Blocked methods return `405`. Tables absent from this config are unrestricted. The format is pipe-separated `table:METHOD,METHOD` entries; method names are case-insensitive.

All three vars take effect on the next request cycle (~50ms) with no restart. They can be combined:

```sql
SET GLOBAL vsql_rest.allowed_tables   = 'orders,customers';
SET GLOBAL vsql_rest.allowed_routines = 'get_summary';
SET GLOBAL vsql_rest.table_methods    = 'customers:GET|orders:GET,POST';
```

Note: `allowed_tables` also prevents FK-embedded related tables from being fetched in `?select=*,related(*)` queries. If `related` isn't in the allowlist, that embedding is silently skipped rather than returning an error.

## HTTPS

Set `ssl_cert` and `ssl_key` to enable HTTPS. Both HTTP and HTTPS listeners run simultaneously on their respective ports. If a TLS listener is configured but fails to bind or initialize, the server refuses to start (fail-closed) rather than falling back to plaintext-only.

```sql
SET GLOBAL vsql_rest.ssl_cert = '/path/to/cert.pem';
SET GLOBAL vsql_rest.ssl_key  = '/path/to/key.pem';
SET GLOBAL vsql_rest.ssl_port = 3443;
-- Restart to pick up TLS config:
SET GLOBAL vsql_rest.vsql_rest_enabled = OFF;
SET GLOBAL vsql_rest.vsql_rest_enabled = ON;
```

For production deployments, a TLS-terminating reverse proxy (nginx, Caddy) in front of the plain HTTP port is also a valid approach.

## Known Limitations

1. **SQL execution serialized** — all concurrent requests queue through a single SQL session. High write concurrency will experience queueing delay. Addressed when VEF exposes multi-session thread pool support. See: [VillageSQL issue tracker](https://github.com/villagesql/villagesql-server/issues)

2. **No parameterized queries** — `sql_query` executes string SQL with no parameter binding. Injection prevention relies on value escaping and column/table whitelist validation against the schema cache.

3. **Schema cache TTL** — DDL changes (new tables, ALTER TABLE, new views) are not reflected until the next cache refresh (default 60s, configurable via `vsql_rest.schema_ttl`).

4. **JWT user variable workaround** — MySQL views cannot reference user variables directly. Use a stored function wrapper (see Authentication section above).

5. **All four preview APIs are unstable** — `thread_worker`, `sql_query`, `sys_var`, and `status_var` may change API/ABI between VillageSQL releases.

6. **No in-server cert management** — cert/key must be files on disk. Cert rotation requires disabling and re-enabling the server.

7. **CALL for result-set-returning procedures not supported** — stored procedures that use `SELECT` to return result sets cannot be called via `/rpc/`. DML procedures (INSERT/UPDATE/DELETE) work. Workaround: write stored functions returning JSON instead.

8. **`jwt_secret` visible as plaintext in `SHOW GLOBAL VARIABLES`** — VEF does not support masked sys vars. Avoid setting `vsql_rest.jwt_secret` on shared or audited servers; use RS256 (`jwt_public_key`) instead, since the public key path is not sensitive.

9. **Extension upgrade path** — no `ALTER EXTENSION` command. Version upgrades require `UNINSTALL EXTENSION vsql_rest` then `INSTALL EXTENSION vsql_rest`. Tracked: [#12 Extension upgrades](https://github.com/villagesql/villagesql-server/issues/12) — 👍 it to signal demand.

10. **Request bodies are capped at 8 MiB** — the cap bounds the allocation driven
    by a client-supplied `Content-Length`, so an oversized header cannot exhaust
    server memory. A request declaring a `Content-Length` above 8 MiB is rejected
    with `413` before the body is read, and the size is not configurable:

    ```json
    {"message":"request body too large","details":null,"hint":null,"code":"VSQL0004"}
    ```

    Large bulk inserts must be split across multiple requests.

## Security Considerations

- **MySQL grants are bypassed** — vsql_rest executes queries through an internal session; `GRANT`/`REVOKE` have no effect on what the REST API can read or write. Use `allowed_tables`, `allowed_routines`, and `table_methods` to restrict the exposed surface, and JWT + view-based row filtering for per-caller access control. See [Access Control](#access-control).
- **JWT secret exposure** — `vsql_rest.jwt_secret` is visible as plaintext in `SHOW GLOBAL VARIABLES`. On shared or audited servers, use RS256 instead: set `vsql_rest.jwt_public_key` to a PEM file path. The public key path is not sensitive.
- **TLS in production** — the built-in HTTPS listener is suitable for development and internal use. For production, a TLS-terminating reverse proxy (nginx, Caddy) in front of the plain HTTP port gives you certificate rotation, OCSP stapling, and modern cipher control without restarting the extension.
- **SQL injection** — user-supplied values are escaped via `mysql_escape()` before interpolation. Table and column names are validated against the schema cache whitelist and never taken directly from request input. Because that escaping is backslash-based, `NO_BACKSLASH_ESCAPES` would defeat it (`\'` would leave the quote live), so the extension strips that mode from its own internal session at session open; other modes, such as strict mode, are preserved. The security guarantee depends on all three: if you find a bypass, please report it.
- **Network exposure** — vsql_rest listens on all interfaces by default. In production, bind the database host to a private network or use firewall rules to restrict access to the REST port.

## Testing

See [TESTING.md](TESTING.md).

## Contributing

See the [VillageSQL Contributing Guide](https://github.com/villagesql/villagesql-server/blob/main/CONTRIBUTING.md).

## Reporting Bugs and Requesting Features

Open an issue at [github.com/villagesql/vsql-rest/issues](https://github.com/villagesql/vsql-rest/issues).

## Contact

- Discord: [discord.gg/KSr6whd3Fr](https://discord.gg/KSr6whd3Fr)
- GitHub Issues: [github.com/villagesql/vsql-rest/issues](https://github.com/villagesql/vsql-rest/issues)

## License

GPL-2.0. See [LICENSE](LICENSE).
