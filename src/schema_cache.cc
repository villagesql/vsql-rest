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

#include "schema_cache.h"

#include <cstring>
#include "sql_executor.h"

namespace vsql_rest {

ColType col_type_from_mysql(const std::string& dt) {
  if (dt == "int" || dt == "bigint" || dt == "tinyint" || dt == "smallint" ||
      dt == "mediumint")
    return ColType::INTEGER;
  if (dt == "float" || dt == "double") return ColType::REAL;
  if (dt == "decimal" || dt == "numeric") return ColType::DECIMAL;
  if (dt == "date" || dt == "datetime" || dt == "timestamp" || dt == "time" ||
      dt == "year")
    return ColType::DATETIME;
  return ColType::TEXT;
}

bool SchemaCache::refresh(vsql::preview_sql_query::Session& session,
                          const std::string& schema_name) {
  std::unordered_map<std::string, TableInfo> new_tables;
  std::unordered_map<std::string, RoutineInfo> new_routines;

  const std::string esc_schema = mysql_escape(schema_name);

  // --- Columns ---
  std::string col_sql =
      "SELECT TABLE_NAME, COLUMN_NAME, DATA_TYPE, IS_NULLABLE, COLUMN_KEY "
      "FROM INFORMATION_SCHEMA.COLUMNS "
      "WHERE TABLE_SCHEMA = '" + esc_schema + "' "
      "ORDER BY TABLE_NAME, ORDINAL_POSITION";

  auto col_result = session.sql(col_sql).execute();
  if (col_result.has_error()) return false;

  while (col_result.next()) {
    auto tname = col_result.column_str(0);
    auto cname = col_result.column_str(1);
    auto dtype = col_result.column_str(2);
    auto nullable = col_result.column_str(3);
    auto col_key = col_result.column_str(4);

    if (tname.data() == nullptr || cname.data() == nullptr) continue;

    std::string tname_s(tname);
    auto& tbl = new_tables[tname_s];
    tbl.name = tname_s;

    ColInfo ci;
    ci.name = std::string(cname);
    ci.type = dtype.data() ? col_type_from_mysql(std::string(dtype)) : ColType::TEXT;
    ci.is_nullable = !nullable.data() || nullable != "NO";
    ci.is_primary_key = col_key.data() && col_key == "PRI";
    tbl.columns.push_back(std::move(ci));
  }

  // --- Foreign keys ---
  std::string fk_sql =
      "SELECT TABLE_NAME, COLUMN_NAME, REFERENCED_TABLE_NAME, "
      "REFERENCED_COLUMN_NAME "
      "FROM INFORMATION_SCHEMA.KEY_COLUMN_USAGE "
      "WHERE TABLE_SCHEMA = '" + esc_schema + "' "
      "AND REFERENCED_TABLE_NAME IS NOT NULL";

  auto fk_result = session.sql(fk_sql).execute();
  if (!fk_result.has_error()) {
    while (fk_result.next()) {
      auto tname = fk_result.column_str(0);
      auto cname = fk_result.column_str(1);
      auto ref_tname = fk_result.column_str(2);
      auto ref_cname = fk_result.column_str(3);

      if (!tname.data() || !cname.data() || !ref_tname.data() || !ref_cname.data())
        continue;

      std::string tname_s(tname);
      auto it = new_tables.find(tname_s);
      if (it == new_tables.end()) continue;

      FkInfo fk;
      fk.from_col = std::string(cname);
      fk.to_table = std::string(ref_tname);
      fk.to_col = std::string(ref_cname);
      it->second.fks.push_back(std::move(fk));
    }
  }

  // --- Routines ---
  std::string rtn_sql =
      "SELECT ROUTINE_NAME, ROUTINE_TYPE "
      "FROM INFORMATION_SCHEMA.ROUTINES "
      "WHERE ROUTINE_SCHEMA = '" + schema_name + "'";

  auto rtn_result = session.sql(rtn_sql).execute();
  if (!rtn_result.has_error()) {
    while (rtn_result.next()) {
      auto rname = rtn_result.column_str(0);
      auto rtype = rtn_result.column_str(1);
      if (!rname.data()) continue;

      RoutineInfo ri;
      ri.name = std::string(rname);
      ri.kind = (rtype.data() && rtype == "PROCEDURE")
                    ? RoutineKind::PROCEDURE
                    : RoutineKind::FUNCTION;
      new_routines[ri.name] = ri;
    }
  }

  // --- Routine parameters ---
  // ORDINAL_POSITION 0 is a function's return value, not a parameter.
  std::string prm_sql =
      "SELECT SPECIFIC_NAME, PARAMETER_NAME "
      "FROM INFORMATION_SCHEMA.PARAMETERS "
      "WHERE SPECIFIC_SCHEMA = '" + schema_name + "' "
      "AND ORDINAL_POSITION > 0 "
      "ORDER BY SPECIFIC_NAME, ORDINAL_POSITION";

  auto prm_result = session.sql(prm_sql).execute();
  if (!prm_result.has_error()) {
    while (prm_result.next()) {
      auto rname = prm_result.column_str(0);
      auto pname = prm_result.column_str(1);
      if (!rname.data() || !pname.data()) continue;
      auto it = new_routines.find(std::string(rname));
      if (it != new_routines.end()) {
        it->second.params.emplace_back(pname);
      }
    }
  }

  std::lock_guard<std::mutex> lock(mu_);
  tables_ = std::move(new_tables);
  routines_ = std::move(new_routines);
  last_refresh_ = std::time(nullptr);
  populated_ = true;
  return true;
}

bool SchemaCache::refresh_if_needed(vsql::preview_sql_query::Session& session,
                                    const std::string& schema_name,
                                    int ttl_seconds) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (populated_) {
      auto age = std::difftime(std::time(nullptr), last_refresh_);
      if (age < ttl_seconds) return true;
    }
  }
  return refresh(session, schema_name);
}

bool SchemaCache::table_exists(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mu_);
  return tables_.count(name) > 0;
}

const TableInfo* SchemaCache::get_table(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = tables_.find(name);
  return (it != tables_.end()) ? &it->second : nullptr;
}

bool SchemaCache::routine_exists(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mu_);
  return routines_.count(name) > 0;
}

const RoutineInfo* SchemaCache::get_routine(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = routines_.find(name);
  return (it != routines_.end()) ? &it->second : nullptr;
}

std::vector<std::string> SchemaCache::table_names() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> names;
  names.reserve(tables_.size());
  for (const auto& kv : tables_) names.push_back(kv.first);
  return names;
}

}  // namespace vsql_rest
