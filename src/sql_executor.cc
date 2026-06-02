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

#include "sql_executor.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "json_emit.h"
#include "third_party/json.hpp"

namespace vsql_rest {

// --- Utility functions ---

// Map MySQL error numbers to HTTP status codes.
// Constraint violations (FK, duplicate key) → 409 Conflict.
// Everything else → 500 Internal Server Error.
static int sql_errno_to_http(uint32_t mysql_errno) {
  switch (mysql_errno) {
    case 1062:  // ER_DUP_ENTRY — unique/primary key violation
    case 1451:  // ER_ROW_IS_REFERENCED_2 — FK parent row referenced
    case 1452:  // ER_NO_REFERENCED_ROW_2 — FK child row missing
      return 409;
    default:
      return 500;
  }
}

std::string url_decode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hex = 0;
      auto r = std::from_chars(s.data() + i + 1, s.data() + i + 3, hex, 16);
      if (r.ec == std::errc{}) {
        out += static_cast<char>(hex);
        i += 2;
        continue;
      }
    }
    out += (s[i] == '+') ? ' ' : s[i];
  }
  return out;
}

std::string mysql_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 2 + 4);
  for (unsigned char c : s) {
    switch (c) {
      case '\'': out += "\\'"; break;
      case '\\': out += "\\\\"; break;
      case '\0': out += "\\0"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\032': out += "\\Z"; break;  // Ctrl+Z / SUB
      case '"':  out += "\\\""; break;
      default:   out += static_cast<char>(c);
    }
  }
  return out;
}

std::string backtick(const std::string& name) {
  std::string out;
  out.reserve(name.size() * 2 + 2);
  out += '`';
  for (char c : name) {
    if (c == '`') out += "``";
    else out += c;
  }
  out += '`';
  return out;
}

std::vector<std::pair<std::string, std::string>> parse_query_string(
    const std::string& raw) {
  std::vector<std::pair<std::string, std::string>> result;
  if (raw.empty()) return result;
  // raw_query never starts with '?' (http_server strips the '?').
  std::string_view sv(raw);
  while (!sv.empty()) {
    auto amp = sv.find('&');
    std::string_view token = sv.substr(0, amp);
    auto eq = token.find('=');
    if (eq == std::string_view::npos) {
      result.emplace_back(url_decode(std::string(token)), "");
    } else {
      result.emplace_back(url_decode(std::string(token.substr(0, eq))),
                          url_decode(std::string(token.substr(eq + 1))));
    }
    sv = (amp == std::string_view::npos) ? "" : sv.substr(amp + 1);
  }
  return result;
}

// --- Shared helpers ---

static void trim_inplace(std::string& s) {
  auto start = s.find_first_not_of(' ');
  if (start == std::string::npos) { s.clear(); return; }
  auto end = s.find_last_not_of(' ');
  s = s.substr(start, end - start + 1);
}

// Serialize a nlohmann::json value to a MySQL SQL literal (escaped).
static std::string json_val_to_sql_literal(const nlohmann::json& val) {
  if (val.is_null())             return "NULL";
  if (val.is_string())           return "'" + mysql_escape(val.get<std::string>()) + "'";
  if (val.is_number_integer())   return std::to_string(val.get<long long>());
  if (val.is_number_float())     return std::to_string(val.get<double>());
  if (val.is_boolean())          return val.get<bool>() ? "1" : "0";
  return "'" + mysql_escape(val.dump()) + "'";
}

// --- Filter parsing ---

enum class FilterOp {
  EQ, NEQ, LT, LTE, GT, GTE, LIKE, CS_LIKE, IN, IS_NULL, IS_NOT_NULL,
  UNKNOWN
};

struct Filter {
  std::string col;
  FilterOp op{FilterOp::UNKNOWN};
  std::string value;   // for IN: comma-separated items inside parens
};

static FilterOp parse_op(const std::string& op_str) {
  if (op_str == "eq")       return FilterOp::EQ;
  if (op_str == "neq")      return FilterOp::NEQ;
  if (op_str == "lt")       return FilterOp::LT;
  if (op_str == "lte")      return FilterOp::LTE;
  if (op_str == "gt")       return FilterOp::GT;
  if (op_str == "gte")      return FilterOp::GTE;
  if (op_str == "like")     return FilterOp::LIKE;
  if (op_str == "cs_like")  return FilterOp::CS_LIKE;
  if (op_str == "in")       return FilterOp::IN;
  if (op_str == "is")       return FilterOp::IS_NULL;  // refined below
  return FilterOp::UNKNOWN;
}

// Parse a single filter value "op.rest". Returns nullopt on error.
static std::optional<Filter> parse_filter(const std::string& col,
                                          const std::string& op_val) {
  auto dot = op_val.find('.');
  if (dot == std::string::npos) return std::nullopt;
  std::string op_str = op_val.substr(0, dot);
  std::string val_str = op_val.substr(dot + 1);

  Filter f;
  f.col = col;
  f.op = parse_op(op_str);
  if (f.op == FilterOp::UNKNOWN) return std::nullopt;

  if (f.op == FilterOp::IS_NULL) {
    if (val_str == "null") {
      f.op = FilterOp::IS_NULL;
    } else if (val_str == "not.null") {
      f.op = FilterOp::IS_NOT_NULL;
    } else {
      return std::nullopt;
    }
  }

  f.value = val_str;
  return f;
}

// Build SQL condition fragment for a single filter. table_qualifier is
// pre-computed as backtick(schema) + "." + backtick(table).
// col must already be validated against the schema cache.
static std::string filter_to_sql(const Filter& f,
                                 const std::string& table_qualifier) {
  std::string col_expr = table_qualifier + "." + backtick(f.col);

  switch (f.op) {
    case FilterOp::EQ:
      return col_expr + " = '" + mysql_escape(f.value) + "'";
    case FilterOp::NEQ:
      return col_expr + " != '" + mysql_escape(f.value) + "'";
    case FilterOp::LT:
      return col_expr + " < '" + mysql_escape(f.value) + "'";
    case FilterOp::LTE:
      return col_expr + " <= '" + mysql_escape(f.value) + "'";
    case FilterOp::GT:
      return col_expr + " > '" + mysql_escape(f.value) + "'";
    case FilterOp::GTE:
      return col_expr + " >= '" + mysql_escape(f.value) + "'";
    case FilterOp::LIKE:
      return col_expr + " LIKE '" + mysql_escape(f.value) + "'";
    case FilterOp::CS_LIKE:
      return col_expr + " LIKE BINARY '" + mysql_escape(f.value) + "'";
    case FilterOp::IS_NULL:
      return col_expr + " IS NULL";
    case FilterOp::IS_NOT_NULL:
      return col_expr + " IS NOT NULL";
    case FilterOp::IN: {
      // Value format: (val1,val2,val3)
      std::string v = f.value;
      if (!v.empty() && v.front() == '(') v = v.substr(1);
      if (!v.empty() && v.back() == ')') v.pop_back();
      std::string in_list;
      std::istringstream ss(v);
      std::string item;
      while (std::getline(ss, item, ',')) {
        trim_inplace(item);
        if (!in_list.empty()) in_list += ',';
        in_list += "'" + mysql_escape(item) + "'";
      }
      return col_expr + " IN (" + in_list + ")";
    }
    default:
      return {};
  }
}

// Parse the or(filter1,filter2) syntax.
// Returns list of Filter structs, or empty on parse error.
static std::vector<Filter> parse_or_filters(
    const std::string& or_value, const TableInfo& table_info) {
  // Expected format: (col.op.val,col2.op.val2)
  std::vector<Filter> filters;
  std::string v = or_value;
  if (!v.empty() && v.front() == '(') v = v.substr(1);
  if (!v.empty() && v.back() == ')') v.pop_back();

  // Split by comma (simple split; values with commas inside parens not handled).
  std::istringstream ss(v);
  std::string token;
  while (std::getline(ss, token, ',')) {
    // token is "col.op.val" — find first dot for col, rest is op.val.
    auto d = token.find('.');
    if (d == std::string::npos) return {};
    std::string col = token.substr(0, d);
    std::string op_val = token.substr(d + 1);

    // Validate column.
    bool col_valid = false;
    for (const auto& ci : table_info.columns) {
      if (ci.name == col) { col_valid = true; break; }
    }
    if (!col_valid) return {};

    auto f = parse_filter(col, op_val);
    if (!f) return {};
    filters.push_back(*f);
  }
  return filters;
}

// Build WHERE clause from filters. Returns empty string if no filters.
// or_filters are OR-combined; regular_filters are AND-combined.
static std::string build_where(
    const std::vector<Filter>& and_filters,
    const std::vector<std::vector<Filter>>& or_groups,
    const std::string& schema_name,
    const std::string& table_name) {
  // Pre-compute table qualifier once; filters repeat it per column.
  const std::string tq = backtick(schema_name) + "." + backtick(table_name);
  std::vector<std::string> conditions;

  for (const auto& f : and_filters) {
    auto cond = filter_to_sql(f, tq);
    if (!cond.empty()) conditions.push_back(cond);
  }

  for (const auto& group : or_groups) {
    if (group.empty()) continue;
    std::string or_cond;
    for (const auto& f : group) {
      auto cond = filter_to_sql(f, tq);
      if (cond.empty()) continue;
      if (!or_cond.empty()) or_cond += " OR ";
      or_cond += cond;
    }
    if (!or_cond.empty()) conditions.push_back("(" + or_cond + ")");
  }

  if (conditions.empty()) return {};
  std::string where = " WHERE ";
  for (size_t i = 0; i < conditions.size(); ++i) {
    if (i > 0) where += " AND ";
    where += conditions[i];
  }
  return where;
}

// --- Select column parsing ---

struct SelectSpec {
  std::vector<std::string> columns;    // explicit columns (empty = SELECT *)
  std::vector<std::string> embeddings; // relation names to embed
};

static SelectSpec parse_select(const std::string& select_val) {
  SelectSpec spec;
  if (select_val.empty() || select_val == "*") return spec;

  // Split on top-level commas only (ignore commas inside parentheses).
  std::vector<std::string> tokens;
  int depth = 0;
  std::string cur;
  for (char c : select_val) {
    if (c == '(') { ++depth; cur += c; }
    else if (c == ')') { --depth; cur += c; }
    else if (c == ',' && depth == 0) {
      tokens.push_back(cur); cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) tokens.push_back(cur);

  for (auto& token : tokens) {
    trim_inplace(token);
    if (token.empty()) continue;

    auto paren = token.find('(');
    if (paren != std::string::npos) {
      spec.embeddings.push_back(token.substr(0, paren));
    } else {
      spec.columns.push_back(token);
    }
  }
  return spec;
}

// Validate column name against table info. Returns false if not found.
static bool validate_column(const std::string& col, const TableInfo& info) {
  if (col == "*") return true;
  for (const auto& ci : info.columns) {
    if (ci.name == col) return true;
  }
  return false;
}

// Collect filter params from a parsed query string into and_filters/or_groups.
// Skips pagination/ordering keys. Returns a filled HttpResponse on error (status != 0),
// or an empty HttpResponse (status == 0) on success.
static HttpResponse collect_filters(
    const std::vector<std::pair<std::string, std::string>>& params,
    const TableInfo& table_info,
    std::vector<Filter>& and_filters,
    std::vector<std::vector<Filter>>& or_groups) {
  HttpResponse err;
  err.status = 0;
  for (const auto& [k, v] : params) {
    if (k == "select" || k == "order" || k == "limit" || k == "offset") continue;
    if (k == "or") {
      auto grp = parse_or_filters(v, table_info);
      if (!grp.empty()) or_groups.push_back(grp);
      continue;
    }
    if (!validate_column(k, table_info)) {
      err.status = 400;
      err.body = emit_error("unknown column: " + k);
      return err;
    }
    auto f = parse_filter(k, v);
    if (!f) {
      err.status = 400;
      err.body = emit_error("invalid filter operator in: " + k + "=" + v);
      return err;
    }
    and_filters.push_back(*f);
  }
  return err;
}

// --- GET handler ---

static HttpResponse handle_get(vsql::preview_sql_query::Session& session,
                                const std::string& table_name,
                                const std::string& raw_query,
                                const std::string& schema_name,
                                const TableInfo& table_info,
                                long long max_rows,
                                bool count_exact,
                                SchemaCache& schema_cache) {
  HttpResponse resp;
  auto params = parse_query_string(raw_query);

  std::vector<Filter> and_filters;
  std::vector<std::vector<Filter>> or_groups;
  SelectSpec select_spec;
  std::string order_col, order_dir;
  long long limit_val = max_rows;
  long long offset_val = 0;
  bool limit_set = false;
  bool prefer_count = false;  // set by caller based on Prefer header

  for (const auto& [k, v] : params) {
    if (k == "select") {
      select_spec = parse_select(v);
    } else if (k == "order") {
      // format: col.asc or col.desc
      auto d = v.rfind('.');
      if (d != std::string::npos) {
        order_col = v.substr(0, d);
        order_dir = v.substr(d + 1);
        if (!validate_column(order_col, table_info)) {
          resp.status = 400;
          resp.body = emit_error("unknown column: " + order_col);
          return resp;
        }
      }
    } else if (k == "limit") {
      try { limit_val = std::stoll(v); limit_set = true; } catch (...) {}
    } else if (k == "offset") {
      try { offset_val = std::stoll(v); } catch (...) {}
    } else {
      // Delegate remaining params (column filters + or) to shared helper.
      // Pass as a single-element params list to reuse collect_filters.
      std::vector<std::pair<std::string,std::string>> one{{k, v}};
      auto err = collect_filters(one, table_info, and_filters, or_groups);
      if (err.status != 0) return err;
    }
  }

  // Validate select columns.
  for (const auto& col : select_spec.columns) {
    if (!validate_column(col, table_info)) {
      resp.status = 400;
      resp.body = emit_error("unknown column: " + col);
      return resp;
    }
  }

  // Build SELECT column list.
  // When embeddings need FK columns not in the explicit select list, we fetch
  // those extra columns internally but exclude them from the JSON output.
  std::string col_list;
  std::vector<ColInfo> result_cols;    // columns included in JSON output
  std::vector<ColInfo> internal_cols;  // all fetched columns (superset)

  if (select_spec.columns.empty()) {
    col_list = backtick(schema_name) + "." + backtick(table_name) + ".*";
    result_cols = table_info.columns;
    internal_cols = table_info.columns;
  } else {
    // Start with user-requested columns.
    std::vector<std::string> fetch_cols = select_spec.columns;

    // If embeddings are requested, ensure FK columns are fetched.
    // (Forward FK: table_info.fks → to embed_table; we need from_col.)
    // (Reverse FK resolved later in embedding loop — needs PK.)
    // Always add PK if not present when there are embeddings (needed for reverse FK).
    if (!select_spec.embeddings.empty()) {
      // Add PK column if not already in fetch list.
      for (const auto& ci : table_info.columns) {
        if (ci.is_primary_key) {
          bool found = false;
          for (const auto& c : fetch_cols) {
            if (c == ci.name) { found = true; break; }
          }
          if (!found) fetch_cols.push_back(ci.name);
          break;
        }
      }
      // Add FK from_col columns needed for forward FK embeddings.
      for (const auto& embed_name : select_spec.embeddings) {
        for (const auto& fk : table_info.fks) {
          if (fk.to_table == embed_name) {
            bool found = false;
            for (const auto& c : fetch_cols) {
              if (c == fk.from_col) { found = true; break; }
            }
            if (!found) fetch_cols.push_back(fk.from_col);
          }
        }
      }
    }

    for (size_t i = 0; i < fetch_cols.size(); ++i) {
      if (i > 0) col_list += ", ";
      col_list += backtick(schema_name) + "." + backtick(table_name) +
                  "." + backtick(fetch_cols[i]);
    }
    for (const auto& cname : fetch_cols) {
      for (const auto& ci : table_info.columns) {
        if (ci.name == cname) { internal_cols.push_back(ci); break; }
      }
    }
    // result_cols: only user-requested columns (for JSON output).
    for (const auto& cname : select_spec.columns) {
      for (const auto& ci : table_info.columns) {
        if (ci.name == cname) { result_cols.push_back(ci); break; }
      }
    }
  }

  std::string where = build_where(and_filters, or_groups, schema_name, table_name);

  // Determine total count for Content-Range if needed.
  // (count query uses same WHERE but no LIMIT/OFFSET)
  long long total_count = -1;  // -1 = unknown
  // (will be queried if Prefer: count=exact header is present — handled by caller)

  // Build ORDER BY.
  std::string order_clause;
  if (!order_col.empty()) {
    order_clause = " ORDER BY " + backtick(schema_name) + "." +
                   backtick(table_name) + "." + backtick(order_col);
    if (order_dir == "desc") order_clause += " DESC";
    else order_clause += " ASC";
  }

  // Build main SQL.
  std::string sql = "SELECT " + col_list + " FROM " +
                    backtick(schema_name) + "." + backtick(table_name) +
                    where + order_clause +
                    " LIMIT " + std::to_string(limit_val) +
                    " OFFSET " + std::to_string(offset_val);

  auto result = session.sql(sql).execute();
  if (result.has_error()) {
    resp.status = sql_errno_to_http(result.error().errno_);
    resp.body = emit_error(std::string(result.error().message));
    return resp;
  }

  // Collect rows into memory once. Optional<string> for SQL NULL (avoids sentinel).
  using OptStr = std::optional<std::string>;
  using Row = std::vector<std::pair<std::string, OptStr>>;
  std::vector<Row> rows;
  while (result.next()) {
    Row row;
    for (size_t ci = 0; ci < internal_cols.size(); ++ci) {
      auto sv = result.column_str(static_cast<unsigned int>(ci));
      row.emplace_back(internal_cols[ci].name,
                       sv.data() ? OptStr{std::string(sv)} : OptStr{});
    }
    rows.push_back(std::move(row));
  }
  long long returned = static_cast<long long>(rows.size());

  // Build embeddings if requested.
  std::vector<std::pair<std::string, std::vector<std::vector<Row>>>> embeddings_data;
  std::vector<std::vector<ColInfo>> embeddings_cols;
  if (!select_spec.embeddings.empty()) {
    for (const auto& embed_name : select_spec.embeddings) {
      const auto* embed_tbl = schema_cache.get_table(embed_name);
      if (!embed_tbl) continue;

      const FkInfo* forward_fk = nullptr;
      for (const auto& f : table_info.fks) {
        if (f.to_table == embed_name) { forward_fk = &f; break; }
      }
      const FkInfo* reverse_fk = nullptr;
      for (const auto& f : embed_tbl->fks) {
        if (f.to_table == table_name) { reverse_fk = &f; break; }
      }
      if (!forward_fk && !reverse_fk) continue;

      const std::vector<ColInfo>& embed_cols = embed_tbl->columns;
      std::vector<std::vector<Row>> all_embed_rows;

      for (const auto& parent_row : rows) {
        std::string child_sql;
        if (reverse_fk) {
          std::string pk_val;
          for (const auto& ci : table_info.columns) {
            if (ci.is_primary_key) {
              for (const auto& [col, val] : parent_row) {
                if (col == ci.name) { if (val) pk_val = *val; break; }
              }
              break;
            }
          }
          child_sql = "SELECT * FROM " + backtick(schema_name) + "." +
              backtick(embed_name) + " WHERE " +
              backtick(schema_name) + "." + backtick(embed_name) + "." +
              backtick(reverse_fk->from_col) + " = '" + mysql_escape(pk_val) + "'";
        } else {
          std::string fk_val;
          for (const auto& [col, val] : parent_row) {
            if (col == forward_fk->from_col) { if (val) fk_val = *val; break; }
          }
          child_sql = "SELECT * FROM " + backtick(schema_name) + "." +
              backtick(embed_name) + " WHERE " +
              backtick(schema_name) + "." + backtick(embed_name) + "." +
              backtick(forward_fk->to_col) + " = '" + mysql_escape(fk_val) + "'";
        }

        auto child_result = session.sql(child_sql).execute();
        std::vector<Row> child_rows;
        if (!child_result.has_error()) {
          unsigned int ncols = child_result.num_columns();
          while (child_result.next()) {
            Row crow;
            for (unsigned int ci = 0; ci < ncols; ++ci) {
              auto sv = child_result.column_str(ci);
              std::string cname = (ci < embed_cols.size())
                  ? embed_cols[ci].name : "col" + std::to_string(ci);
              crow.emplace_back(cname, sv.data() ? OptStr{std::string(sv)} : OptStr{});
            }
            child_rows.push_back(std::move(crow));
          }
        }
        all_embed_rows.push_back(std::move(child_rows));
      }
      embeddings_cols.push_back(embed_cols);
      embeddings_data.emplace_back(embed_name, std::move(all_embed_rows));
    }
  }

  // Serialize rows to JSON (with or without embeddings).
  std::string body;
  if (embeddings_data.empty()) {
    // Fast path: emit directly from result_cols.
    std::string out;
    out.reserve(result_cols.size() * 32 * rows.size() + 16);
    out += '[';
    for (size_t ri = 0; ri < rows.size(); ++ri) {
      if (ri > 0) out += ',';
      out += '{';
      for (size_t ci = 0; ci < result_cols.size(); ++ci) {
        if (ci > 0) out += ',';
        out += '"'; out += json_escape(result_cols[ci].name); out += "\":";
        const auto& val = rows[ri][ci].second;
        if (!val) out += "null";
        else out += emit_col_value(*val, result_cols[ci].type);
      }
      out += '}';
    }
    out += ']';
    body = std::move(out);
  } else {
    std::string out;
    out.reserve(1024);
    out += '[';
    for (size_t ri = 0; ri < rows.size(); ++ri) {
      if (ri > 0) out += ',';
      out += '{';
      bool first = true;
      for (const auto& rc : result_cols) {
        if (!first) out += ','; first = false;
        out += '"'; out += json_escape(rc.name); out += "\":";
        OptStr val_opt;
        for (const auto& [cname, cval] : rows[ri]) {
          if (cname == rc.name) { val_opt = cval; break; }
        }
        if (!val_opt) out += "null";
        else out += emit_col_value(*val_opt, rc.type);
      }
      for (size_t ei = 0; ei < embeddings_data.size(); ++ei) {
        if (!first) out += ','; first = false;
        const auto& embed_name = embeddings_data[ei].first;
        const auto& all_rows   = embeddings_data[ei].second;
        const auto& ecols      = embeddings_cols[ei];
        out += '"'; out += json_escape(embed_name); out += "\":[";
        const auto& child_rows = all_rows[ri];
        for (size_t cr = 0; cr < child_rows.size(); ++cr) {
          if (cr > 0) out += ',';
          out += '{';
          for (size_t cc = 0; cc < child_rows[cr].size(); ++cc) {
            if (cc > 0) out += ',';
            const auto& col_name = child_rows[cr][cc].first;
            const auto& cval     = child_rows[cr][cc].second;
            out += '"'; out += json_escape(col_name); out += "\":";
            if (!cval) { out += "null"; continue; }
            ColType ctype = ColType::TEXT;
            for (const auto& ci : ecols) {
              if (ci.name == col_name) { ctype = ci.type; break; }
            }
            out += emit_col_value(*cval, ctype);
          }
          out += '}';
        }
        out += ']';
      }
      out += '}';
    }
    out += ']';
    body = std::move(out);
  }

  // Compute Content-Range.
  long long range_start = offset_val;
  long long range_end = range_start + returned - 1;
  if (range_end < range_start) range_end = range_start;

  if (count_exact) {
    std::string count_sql = "SELECT COUNT(*) FROM " + backtick(schema_name) +
                            "." + backtick(table_name) + where;
    auto count_result = session.sql(count_sql).execute();
    if (!count_result.has_error() && count_result.next()) {
      auto sv = count_result.column_str(0);
      if (sv.data()) {
        try { total_count = std::stoll(std::string(sv)); } catch (...) {}
      }
    }
  }

  char range_buf[64];
  if (total_count >= 0) {
    snprintf(range_buf, sizeof(range_buf), "%lld-%lld/%lld",
             range_start, range_end, total_count);
  } else {
    snprintf(range_buf, sizeof(range_buf), "%lld-%lld/*",
             range_start, range_end);
  }

  resp.headers.emplace_back("Content-Range", range_buf);
  resp.headers.emplace_back("Content-Type", "application/json");
  resp.body = std::move(body);
  return resp;
}

// --- POST handler ---

static HttpResponse handle_post(vsql::preview_sql_query::Session& session,
                                 const std::string& table_name,
                                 const std::string& body_str,
                                 const std::string& schema_name,
                                 const TableInfo& table_info,
                                 bool return_repr) {
  HttpResponse resp;

  if (body_str.empty()) {
    resp.status = 400;
    resp.body = emit_error("request body is required for POST");
    return resp;
  }

  nlohmann::json body_json;
  try {
    body_json = nlohmann::json::parse(body_str);
  } catch (const std::exception& e) {
    resp.status = 400;
    resp.body = emit_error(std::string("invalid JSON: ") + e.what());
    return resp;
  }

  // Normalize to array.
  std::vector<nlohmann::json> rows;
  if (body_json.is_array()) {
    for (auto& row : body_json) rows.push_back(row);
  } else if (body_json.is_object()) {
    rows.push_back(body_json);
  } else {
    resp.status = 400;
    resp.body = emit_error("POST body must be a JSON object or array");
    return resp;
  }

  // Build column set from first row and validate all columns.
  if (rows.empty()) {
    resp.status = 400;
    resp.body = emit_error("empty array");
    return resp;
  }

  // Insert each row.
  long long last_insert_id = 0;
  for (const auto& row : rows) {
    if (!row.is_object()) {
      resp.status = 400;
      resp.body = emit_error("each element must be a JSON object");
      return resp;
    }
    std::string cols_sql, vals_sql;
    bool first = true;
    for (auto& [key, val] : row.items()) {
      if (!validate_column(key, table_info)) {
        resp.status = 400;
        resp.body = emit_error("unknown column: " + key);
        return resp;
      }
      if (!first) { cols_sql += ", "; vals_sql += ", "; }
      first = false;
      cols_sql += backtick(key);
      vals_sql += json_val_to_sql_literal(val);
    }

    std::string insert_sql =
        "INSERT INTO " + backtick(schema_name) + "." + backtick(table_name) +
        " (" + cols_sql + ") VALUES (" + vals_sql + ")";
    auto r = session.sql(insert_sql).execute();
    if (r.has_error()) {
      resp.status = sql_errno_to_http(r.error().errno_);
      resp.body = emit_error(std::string(r.error().message));
      return resp;
    }

    // Get LAST_INSERT_ID.
    auto lid_r = session.sql("SELECT LAST_INSERT_ID()").execute();
    if (!lid_r.has_error() && lid_r.next()) {
      auto sv = lid_r.column_str(0);
      if (sv.data()) {
        try { last_insert_id = std::stoll(std::string(sv)); } catch (...) {}
      }
    }
  }

  // Find primary key column name.
  std::string pk_col;
  for (const auto& ci : table_info.columns) {
    if (ci.is_primary_key) { pk_col = ci.name; break; }
  }

  if (return_repr && !pk_col.empty() && last_insert_id > 0) {
    // Return the inserted row(s).
    std::string sel_sql =
        "SELECT * FROM " + backtick(schema_name) + "." + backtick(table_name) +
        " WHERE " + backtick(pk_col) + " = " + std::to_string(last_insert_id);
    auto sel_r = session.sql(sel_sql).execute();
    if (!sel_r.has_error()) {
      resp.status = 200;
      resp.body = emit_result_array(sel_r, table_info.columns);
      resp.headers.emplace_back("Content-Type", "application/json");
      return resp;
    }
  }

  resp.status = 201;
  if (!pk_col.empty() && last_insert_id > 0) {
    resp.headers.emplace_back(
        "Location", "/" + table_name + "?" + pk_col +
                        "=eq." + std::to_string(last_insert_id));
  }
  resp.headers.emplace_back("Content-Type", "application/json");
  resp.body = "";
  return resp;
}

// --- PATCH handler ---

static HttpResponse handle_patch(vsql::preview_sql_query::Session& session,
                                  const std::string& table_name,
                                  const std::string& raw_query,
                                  const std::string& body_str,
                                  const std::string& schema_name,
                                  const TableInfo& table_info,
                                  bool return_repr) {
  HttpResponse resp;

  if (body_str.empty()) {
    resp.status = 400;
    resp.body = emit_error("request body is required for PATCH");
    return resp;
  }

  // Reject unfiltered PATCH — would update every row.
  auto params = parse_query_string(raw_query);
  bool has_filter = false;
  for (const auto& [k, v] : params) {
    if (k != "select" && k != "order" && k != "limit" && k != "offset") {
      has_filter = true;
      break;
    }
  }
  if (!has_filter) {
    resp.status = 400;
    resp.body = emit_error("PATCH without a filter would update all rows; add at least one filter");
    return resp;
  }

  nlohmann::json body_json;
  try {
    body_json = nlohmann::json::parse(body_str);
  } catch (const std::exception& e) {
    resp.status = 400;
    resp.body = emit_error(std::string("invalid JSON: ") + e.what());
    return resp;
  }

  if (!body_json.is_object()) {
    resp.status = 400;
    resp.body = emit_error("PATCH body must be a JSON object");
    return resp;
  }

  // Build SET clause.
  std::string set_clause;
  for (auto& [key, val] : body_json.items()) {
    bool found = false;
    for (const auto& ci : table_info.columns) {
      if (ci.name == key) { found = true; break; }
    }
    if (!found) {
      resp.status = 400;
      resp.body = emit_error("unknown column: " + key);
      return resp;
    }
    if (!set_clause.empty()) set_clause += ", ";
    set_clause += backtick(schema_name) + "." + backtick(table_name) +
                  "." + backtick(key) + " = " + json_val_to_sql_literal(val);
  }

  // Parse filters.
  std::vector<Filter> and_filters;
  std::vector<std::vector<Filter>> or_groups;
  auto ferr = collect_filters(params, table_info, and_filters, or_groups);
  if (ferr.status != 0) return ferr;

  std::string where = build_where(and_filters, or_groups,
                                  schema_name, table_name);
  std::string update_sql =
      "UPDATE " + backtick(schema_name) + "." + backtick(table_name) +
      " SET " + set_clause + where;

  auto r = session.sql(update_sql).execute();
  if (r.has_error()) {
    resp.status = sql_errno_to_http(r.error().errno_);
    resp.body = emit_error(std::string(r.error().message));
    return resp;
  }

  if (return_repr) {
    std::string sel_sql =
        "SELECT * FROM " + backtick(schema_name) + "." + backtick(table_name) +
        where;
    auto sel_r = session.sql(sel_sql).execute();
    if (!sel_r.has_error()) {
      resp.status = 200;
      resp.body = emit_result_array(sel_r, table_info.columns);
      resp.headers.emplace_back("Content-Type", "application/json");
      return resp;
    }
  }

  resp.status = 204;
  return resp;
}

// --- DELETE handler ---

static HttpResponse handle_delete(vsql::preview_sql_query::Session& session,
                                   const std::string& table_name,
                                   const std::string& raw_query,
                                   const std::string& schema_name,
                                   const TableInfo& table_info) {
  HttpResponse resp;

  auto params = parse_query_string(raw_query);
  bool has_filter = false;
  for (const auto& [k, v] : params) {
    if (k != "select" && k != "order" && k != "limit" && k != "offset") {
      has_filter = true;
      break;
    }
  }
  if (!has_filter) {
    resp.status = 400;
    resp.body = emit_error("DELETE without a filter would delete all rows; add at least one filter");
    return resp;
  }

  std::vector<Filter> and_filters;
  std::vector<std::vector<Filter>> or_groups;
  auto ferr2 = collect_filters(params, table_info, and_filters, or_groups);
  if (ferr2.status != 0) return ferr2;

  std::string where = build_where(and_filters, or_groups,
                                  schema_name, table_name);
  std::string del_sql =
      "DELETE FROM " + backtick(schema_name) + "." + backtick(table_name) + where;

  auto r = session.sql(del_sql).execute();
  if (r.has_error()) {
    resp.status = sql_errno_to_http(r.error().errno_);
    resp.body = emit_error(std::string(r.error().message));
    return resp;
  }

  resp.status = 204;
  return resp;
}

// --- RPC handler ---

static HttpResponse handle_rpc(vsql::preview_sql_query::Session& session,
                                const std::string& routine_name,
                                const std::string& body_str,
                                const std::string& schema_name,
                                const RoutineInfo& routine) {
  HttpResponse resp;
  resp.headers.emplace_back("Content-Type", "application/json");

  // Parse JSON body for parameters.
  nlohmann::json params_json;
  if (!body_str.empty()) {
    try {
      params_json = nlohmann::json::parse(body_str);
    } catch (...) {
      resp.status = 400;
      resp.body = emit_error("invalid JSON body");
      return resp;
    }
  }

  // Build argument list from JSON object.
  std::string args;
  if (params_json.is_object()) {
    bool first = true;
    for (auto& [key, val] : params_json.items()) {
      if (!first) args += ", ";
      first = false;
      args += json_val_to_sql_literal(val);
    }
  }

  if (routine.kind == RoutineKind::PROCEDURE) {
    std::string call_sql =
        "CALL " + backtick(schema_name) + "." +
        backtick(routine_name) + "(" + args + ")";
    auto r = session.sql(call_sql).execute();
    if (r.has_error()) {
      resp.status = sql_errno_to_http(r.error().errno_);
      resp.body = emit_error(std::string(r.error().message));
      return resp;
    }
    resp.body = emit_result_array_untyped(r);
  } else {
    std::string sel_sql =
        "SELECT " + backtick(schema_name) + "." +
        backtick(routine_name) + "(" + args + ") AS result";
    auto r = session.sql(sel_sql).execute();
    if (r.has_error()) {
      resp.status = sql_errno_to_http(r.error().errno_);
      resp.body = emit_error(std::string(r.error().message));
      return resp;
    }
    if (r.next()) {
      auto sv = r.column_str(0);
      resp.body = emit_scalar(sv);
    } else {
      resp.body = "null";
    }
  }

  resp.status = 200;
  return resp;
}

// --- API discovery ---

static HttpResponse handle_discovery(const std::string& schema_name,
                                     SchemaCache& schema) {
  HttpResponse resp;
  resp.headers.emplace_back("Content-Type", "application/json");

  auto names = schema.table_names();
  std::string out;
  out.reserve(512);
  out += "{\"definitions\":{";
  bool first = true;
  for (const auto& tname : names) {
    if (!first) out += ',';
    first = false;
    out += '"';
    out += json_escape(tname);
    out += "\":{\"properties\":{";
    const auto* tbl = schema.get_table(tname);
    bool fc = true;
    if (tbl) {
      for (const auto& ci : tbl->columns) {
        if (!fc) out += ',';
        fc = false;
        out += '"';
        out += json_escape(ci.name);
        out += "\":{\"type\":\"";
        switch (ci.type) {
          case ColType::INTEGER: out += "integer"; break;
          case ColType::REAL:    out += "number"; break;
          case ColType::DECIMAL: out += "string"; break;
          case ColType::DATETIME: out += "string"; break;
          default:               out += "string"; break;
        }
        out += "\"}";
      }
    }
    out += "}}";
  }
  out += "}}";

  resp.body = out;
  return resp;
}

// --- Main dispatcher ---

HttpResponse execute_request(vsql::preview_sql_query::Session& session,
                             const HttpRequest& req,
                             const std::string& schema_name,
                             SchemaCache& schema,
                             const std::string& jwt_secret,
                             const std::string& jwt_pubkey_path,
                             bool require_auth,
                             long long max_rows) {
  HttpResponse resp;
  resp.headers.emplace_back("Content-Type", "application/json");

  // --- Auth ---
  JwtClaims claims;
  bool authed = !require_auth;
  auto it = req.headers.find("authorization");
  if (it != req.headers.end()) {
    const std::string& auth = it->second;
    if (auth.size() > 7 && auth.compare(0, 7, "Bearer ") == 0) {
      std::string token = auth.substr(7);
      auto jwt_res = verify_jwt(token, jwt_secret, jwt_pubkey_path);
      if (!jwt_res.ok) {
        resp.status = 401;
        resp.body = emit_error(jwt_res.error);
        return resp;
      }
      claims = jwt_res.claims;
      authed = true;

      // Inject JWT claims as MySQL user variables.
      if (!claims.sub.empty()) {
        auto sv = session.sql(
            "SET @vsql_rest_jwt_sub = '" + mysql_escape(claims.sub) + "'").execute();
        (void)sv;
      }
      if (!claims.role.empty()) {
        auto sv = session.sql(
            "SET @vsql_rest_jwt_role = '" + mysql_escape(claims.role) + "'").execute();
        (void)sv;
      }
      for (const auto& [k, v] : claims.extra) {
        auto sv = session.sql(
            "SET @vsql_rest_jwt_" + mysql_escape(k) + " = '" +
            mysql_escape(v) + "'").execute();
        (void)sv;
      }
    }
  }

  if (!authed) {
    resp.status = 401;
    resp.body = emit_error("authentication required");
    return resp;
  }

  // --- Prefer header ---
  bool return_repr = false;
  bool count_exact = false;
  auto pref_it = req.headers.find("prefer");
  if (pref_it != req.headers.end()) {
    const auto& pref = pref_it->second;
    if (pref.find("return=representation") != std::string::npos)
      return_repr = true;
    if (pref.find("count=exact") != std::string::npos)
      count_exact = true;
  }

  // --- Routing ---
  const std::string& path = req.path;

  // GET /
  if (path == "/" || path.empty()) {
    return handle_discovery(schema_name, schema);
  }

  // POST /rpc/routine_name
  if (req.method == "POST" && path.size() > 5 &&
      path.compare(0, 5, "/rpc/") == 0) {
    std::string rname = path.substr(5);
    const auto* ri = schema.get_routine(rname);
    if (!ri) {
      resp.status = 404;
      resp.body = emit_error("routine not found: " + rname);
      return resp;
    }
    return handle_rpc(session, rname, req.body, schema_name, *ri);
  }

  // Table operations: path is /table_name
  if (path.size() < 2 || path[0] != '/') {
    resp.status = 400;
    resp.body = emit_error("invalid path: " + path);
    return resp;
  }
  std::string table_name = path.substr(1);
  // Strip trailing slash.
  while (!table_name.empty() && table_name.back() == '/') table_name.pop_back();

  if (!schema.table_exists(table_name)) {
    resp.status = 404;
    resp.body = emit_error("table not found: " + table_name);
    return resp;
  }

  const auto* tbl = schema.get_table(table_name);
  if (!tbl) {
    resp.status = 500;
    resp.body = emit_error("internal error: table info unavailable");
    return resp;
  }

  if (req.method == "GET") {
    auto r = handle_get(session, table_name, req.raw_query, schema_name,
                        *tbl, max_rows, count_exact, schema);
    return r;
  } else if (req.method == "POST") {
    return handle_post(session, table_name, req.body, schema_name, *tbl,
                       return_repr);
  } else if (req.method == "PATCH") {
    return handle_patch(session, table_name, req.raw_query, req.body,
                        schema_name, *tbl, return_repr);
  } else if (req.method == "DELETE") {
    return handle_delete(session, table_name, req.raw_query, schema_name, *tbl);
  } else {
    resp.status = 405;
    resp.body = emit_error("method not allowed: " + req.method);
    return resp;
  }
}

}  // namespace vsql_rest
