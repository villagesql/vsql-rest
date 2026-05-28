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

#include "json_emit.h"

#include <cstdio>

namespace vsql_rest {

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() * 2 + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

std::string emit_col_value(std::string_view sv, ColType type) {
  // Null string_view (data() == nullptr) → JSON null.
  if (sv.data() == nullptr) return "null";

  switch (type) {
    case ColType::INTEGER:
      // Emit as JSON number if value looks like an integer.
      if (!sv.empty()) {
        bool valid = true;
        size_t start = (sv[0] == '-') ? 1 : 0;
        if (start == sv.size()) valid = false;
        for (size_t i = start; i < sv.size() && valid; ++i) {
          if (sv[i] < '0' || sv[i] > '9') valid = false;
        }
        if (valid) return std::string(sv);
      }
      // Fall through to quoted string on unexpected format.
      return "\"" + json_escape(sv) + "\"";

    case ColType::REAL: {
      // Emit as JSON number.
      if (!sv.empty()) return std::string(sv);
      return "null";
    }

    case ColType::DECIMAL:
    case ColType::DATETIME:
    case ColType::TEXT:
    default:
      return "\"" + json_escape(sv) + "\"";
  }
}

std::string emit_result_array(vsql::preview_sql_query::Result& result,
                              const std::vector<ColInfo>& col_info) {
  std::string out;
  out.reserve(512);
  out += '[';
  bool first_row = true;

  while (result.next()) {
    if (!first_row) out += ',';
    first_row = false;
    out += '{';
    for (unsigned int i = 0; i < static_cast<unsigned int>(col_info.size()); ++i) {
      if (i > 0) out += ',';
      out += '"';
      out += json_escape(col_info[i].name);
      out += "\":";
      auto sv = result.column_str(i);
      out += emit_col_value(sv, col_info[i].type);
    }
    out += '}';
  }

  out += ']';
  return out;
}

std::string emit_result_array_untyped(vsql::preview_sql_query::Result& result) {
  unsigned int ncols = result.num_columns();
  std::string out;
  out.reserve(256);
  out += '[';
  bool first_row = true;

  while (result.next()) {
    if (!first_row) out += ',';
    first_row = false;
    out += '{';
    for (unsigned int i = 0; i < ncols; ++i) {
      if (i > 0) out += ',';
      // Column names not available from sql_query API; use col0, col1, ...
      char name_buf[16];
      snprintf(name_buf, sizeof(name_buf), "col%u", i);
      out += '"';
      out += name_buf;
      out += "\":";
      auto sv = result.column_str(i);
      if (sv.data() == nullptr) {
        out += "null";
      } else {
        out += '"';
        out += json_escape(sv);
        out += '"';
      }
    }
    out += '}';
  }

  out += ']';
  return out;
}

std::string emit_scalar(std::string_view sv) {
  if (sv.data() == nullptr) return "null";
  // Try to determine if this looks like a number.
  if (!sv.empty()) {
    bool looks_like_int = true;
    size_t start = (sv[0] == '-') ? 1 : 0;
    bool has_dot = false;
    for (size_t i = start; i < sv.size(); ++i) {
      if (sv[i] == '.' && !has_dot) {
        has_dot = true;
      } else if (sv[i] < '0' || sv[i] > '9') {
        looks_like_int = false;
        break;
      }
    }
    if (looks_like_int && start < sv.size()) return std::string(sv);
  }
  return "\"" + json_escape(sv) + "\"";
}

std::string emit_error(std::string_view message, std::string_view code) {
  std::string out;
  out.reserve(128);
  out += "{\"message\":\"";
  out += json_escape(message);
  out += "\",\"details\":null,\"hint\":null,\"code\":\"";
  out += json_escape(code);
  out += "\"}";
  return out;
}

}  // namespace vsql_rest
