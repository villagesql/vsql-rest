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

// TODO(villagesql): custom JSON emitter for result row → JSON array serialization.
// All string values are JSON-escaped. NULL columns emit JSON null.
// DECIMAL columns emit JSON strings (preserves precision).
// Numeric and boolean columns emit JSON numbers/booleans.

#endif  // VSQL_REST_JSON_EMIT_H
