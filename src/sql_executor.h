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

#ifndef VSQL_REST_SQL_EXECUTOR_H
#define VSQL_REST_SQL_EXECUTOR_H

#include <optional>
#include <string>
#include <vector>

#include "request_queue.h"
#include "schema_cache.h"
#include "jwt_auth.h"
#include <villagesql/preview/sql_query.h>

namespace vsql_rest {

// Percent-decode a URL-encoded string.
std::string url_decode(const std::string& s);

// MySQL string escape: escape ', \, NUL, \n, \r, \Z, " per MySQL spec.
// Returns the escaped value WITHOUT surrounding quotes.
std::string mysql_escape(const std::string& s);

// Wrap an identifier in backticks, escaping any backticks in the name.
std::string backtick(const std::string& name);

// Parse the raw query string into (key, value) pairs.
// Handles repeated keys (e.g. two ?age= params become two entries).
std::vector<std::pair<std::string, std::string>> parse_query_string(
    const std::string& raw);

// Build the HTTP response for a given request using the SQL session.
// Sets JWT user variables, validates table/column names against the schema
// cache, generates and executes SQL, and returns a filled HttpResponse.
HttpResponse execute_request(vsql::preview_sql_query::Session& session,
                             const HttpRequest& req,
                             const std::string& schema_name,
                             SchemaCache& schema,
                             const std::string& jwt_secret,
                             const std::string& jwt_pubkey_path,
                             bool require_auth,
                             long long max_rows);

}  // namespace vsql_rest

#endif  // VSQL_REST_SQL_EXECUTOR_H
