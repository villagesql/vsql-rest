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

#ifndef VSQL_REST_SCHEMA_CACHE_H
#define VSQL_REST_SCHEMA_CACHE_H

#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "types.h"
#include <villagesql/preview/sql_query.h>

namespace vsql_rest {

// Thread-safe schema cache. Populated from INFORMATION_SCHEMA on first use
// and refreshed after schema_ttl_seconds elapses. Used by sql_executor to
// validate table/column names (injection prevention) and build FK join queries.
class SchemaCache {
 public:
  SchemaCache() = default;

  // Ensure cache is populated. Refreshes if stale. Returns false on error.
  bool refresh_if_needed(vsql::preview_sql_query::Session& session,
                         const std::string& schema_name,
                         int ttl_seconds);

  // Force a full refresh regardless of TTL. Returns false on error.
  bool refresh(vsql::preview_sql_query::Session& session,
               const std::string& schema_name);

  bool table_exists(const std::string& name) const;
  const TableInfo* get_table(const std::string& name) const;

  bool routine_exists(const std::string& name) const;
  const RoutineInfo* get_routine(const std::string& name) const;

  // All table names for API discovery (GET /).
  std::vector<std::string> table_names() const;

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, TableInfo> tables_;
  std::unordered_map<std::string, RoutineInfo> routines_;
  std::time_t last_refresh_{0};
  bool populated_{false};
};

// Infer ColType from MySQL DATA_TYPE string.
ColType col_type_from_mysql(const std::string& data_type);

}  // namespace vsql_rest

#endif  // VSQL_REST_SCHEMA_CACHE_H
