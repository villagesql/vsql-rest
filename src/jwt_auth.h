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

#ifndef VSQL_REST_JWT_AUTH_H
#define VSQL_REST_JWT_AUTH_H

// TODO(villagesql): JWT parse, HS256 (HMAC-SHA256) and RS256 (RSA-SHA256)
// signature verification via OpenSSL. Extracts claims and returns them for
// injection as MySQL user variables (@vsql_rest_jwt_sub, etc.) before query
// execution. Validates exp claim; returns 401-appropriate error on failure.

#endif  // VSQL_REST_JWT_AUTH_H
