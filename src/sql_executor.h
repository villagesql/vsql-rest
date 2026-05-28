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

// TODO(villagesql): SQL generation for SELECT/INSERT/UPDATE/DELETE/CALL/SELECT-func.
// All user-supplied values are MySQL-escaped before interpolation.
// Column and table names are validated against the schema cache whitelist
// before use — never interpolated directly from request input.

#endif  // VSQL_REST_SQL_EXECUTOR_H
