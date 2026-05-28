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

// TODO(villagesql): INFORMATION_SCHEMA introspection for tables, columns,
// and FK relationships. Builds an in-memory FK graph used for resource
// embedding (?select=*,relation(*)). Refreshed on TTL expiry (vsql_rest_schema_ttl).
// Column and table name whitelist used by sql_executor to prevent injection.

#endif  // VSQL_REST_SCHEMA_CACHE_H
