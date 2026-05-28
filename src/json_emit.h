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

#ifndef VSQL_REST_JSON_EMIT_H
#define VSQL_REST_JSON_EMIT_H

#include <string>
#include <string_view>
#include <vector>

#include "types.h"
#include <villagesql/preview/sql_query.h>

namespace vsql_rest {

// Escape s for use as a JSON string value (without surrounding quotes).
std::string json_escape(std::string_view s);

// Emit a single column value. Null sv (data() == nullptr) → "null".
// Type determines whether to emit as number or quoted string.
std::string emit_col_value(std::string_view sv, ColType type);

// Serialize a full result set to a JSON array of objects.
// col_info must match the column count of the result.
// If include_embedding is set, rows from a joined subquery are nested
// under the relation key.
std::string emit_result_array(vsql::preview_sql_query::Result& result,
                              const std::vector<ColInfo>& col_info);

// Serialize a result set to a JSON array for RPC procedure responses.
// Uses column names from the result metadata; types default to TEXT.
std::string emit_result_array_untyped(vsql::preview_sql_query::Result& result);

// Emit a scalar value returned from an RPC function call.
// sv may be null (data() == nullptr) → "null".
std::string emit_scalar(std::string_view sv);

// Emit a standard error response body.
// {"message":"...","details":null,"hint":null,"code":"..."}
std::string emit_error(std::string_view message,
                       std::string_view code = "VSQL0001");

}  // namespace vsql_rest

#endif  // VSQL_REST_JSON_EMIT_H
