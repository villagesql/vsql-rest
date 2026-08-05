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

#ifndef VSQL_REST_TYPES_H
#define VSQL_REST_TYPES_H

#include <string>
#include <vector>

namespace vsql_rest {

enum class ColType {
  INTEGER,   // INT, BIGINT, TINYINT, SMALLINT, MEDIUMINT
  REAL,      // FLOAT, DOUBLE
  DECIMAL,   // DECIMAL, NUMERIC — emit as JSON string to preserve precision
  TEXT,      // VARCHAR, CHAR, TEXT, ENUM, SET, JSON, etc.
  DATETIME,  // DATE, DATETIME, TIMESTAMP, TIME, YEAR — emit as JSON string
};

struct ColInfo {
  std::string name;
  ColType type{ColType::TEXT};
  bool is_nullable{true};
  bool is_primary_key{false};
};

struct FkInfo {
  std::string from_col;
  std::string to_table;
  std::string to_col;
};

struct TableInfo {
  std::string name;
  std::vector<ColInfo> columns;
  std::vector<FkInfo> fks;  // FK relationships where this table is the child
};

enum class RoutineKind { FUNCTION, PROCEDURE };

struct RoutineInfo {
  std::string name;
  RoutineKind kind{RoutineKind::FUNCTION};
  // Parameter names in declared order. RPC arguments are positional in SQL, so
  // this is what lets a JSON object be bound by name instead of by the order
  // the JSON parser happens to iterate in.
  std::vector<std::string> params;
};

}  // namespace vsql_rest

#endif  // VSQL_REST_TYPES_H
