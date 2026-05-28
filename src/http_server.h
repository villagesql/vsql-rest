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

#ifndef VSQL_REST_HTTP_SERVER_H
#define VSQL_REST_HTTP_SERVER_H

// TODO(villagesql): TCP accept loop, connection thread dispatch.
// Starts a std::thread that calls accept() on the listen socket and spawns
// per-connection threads. Each connection thread parses the HTTP request via
// picohttpparser and pushes a ParsedRequest onto the request queue.

#endif  // VSQL_REST_HTTP_SERVER_H
