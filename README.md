# VillageSQL REST Extension

Exposes VillageSQL database tables as HTTP and HTTPS REST endpoints. Uses a
PostgREST-compatible query interface adapted for MySQL idioms — filter with
`%` wildcards, call stored procedures via RPC, authenticate with JWT.

## Building

**Linux:**
```bash
VillageSQL_BUILD_DIR=$HOME/build/villagesql bash build.sh
```

**macOS:**
```bash
VillageSQL_BUILD_DIR=~/.villagesql/build bash build.sh
```

OpenSSL is required. On macOS with Homebrew:
```bash
cmake -S . -B build \
  -DVillageSQL_BUILD_DIR=~/.villagesql/build \
  -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
cmake --build build
cmake --install build
```

## Installing

```sql
INSTALL EXTENSION 'vsql_rest';
SET GLOBAL vsql_rest_schema = 'mydb';
SET GLOBAL vsql_rest_port = 3000;
SET GLOBAL vsql_rest_enabled = ON;
```

## Quick start

```sql
-- Tables in vsql_rest_schema are accessible over HTTP immediately:
-- GET  http://localhost:3000/customers
-- GET  http://localhost:3000/customers?name=eq.Alice&order=age.desc
-- POST http://localhost:3000/customers   (JSON body)
-- POST http://localhost:3000/rpc/my_function
```

See [TESTING.md](TESTING.md) for the full test suite.
