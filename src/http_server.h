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

#include <atomic>
#include <string>
#include <thread>

#include "request_queue.h"
#include "tls.h"

namespace vsql_rest {

// Create and bind a listening TCP socket. Pass port 0 for an OS-assigned
// port; the actual bound port is written to *bound_port when non-null.
// Returns the fd, or -1 on failure (errno set).
int create_listen_socket(int port, int* bound_port = nullptr);

// Format an HttpResponse as a raw HTTP/1.1 response string.
std::string format_http_response(const HttpResponse& resp);

// Accept loop: calls accept() on listen_fd in a loop. For each accepted
// connection, spawns a detached std::thread that reads one HTTP request,
// pushes it onto the queue, waits for the response, and sends it back.
// ssl_ctx may be null for plain HTTP.
// Runs until running is set to false OR listen_fd is closed.
void accept_loop(int listen_fd, SSL_CTX* ssl_ctx, RequestQueue* queue,
                 std::atomic<bool>* running);

}  // namespace vsql_rest

#endif  // VSQL_REST_HTTP_SERVER_H
