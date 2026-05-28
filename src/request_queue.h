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

#ifndef VSQL_REST_REQUEST_QUEUE_H
#define VSQL_REST_REQUEST_QUEUE_H

// TODO(villagesql): thread-safe ParsedRequest/Response queue.
// Connection threads push ParsedRequest + std::promise<Response> onto the queue
// and write a byte to signal_pipe[1] to wake the thread_worker.
// The thread_worker drains the queue, executes SQL, and fulfills each promise.
// Includes the signal pipe (pipe[2]) used as the thread_worker poll_fd.

#endif  // VSQL_REST_REQUEST_QUEUE_H
