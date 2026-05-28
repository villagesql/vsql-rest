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

#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace vsql_rest {

struct HttpRequest {
  std::string method;
  std::string path;
  std::string raw_query;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
};

struct HttpResponse {
  int status{200};
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

struct PendingRequest {
  HttpRequest req;
  std::promise<HttpResponse> promise;
};

// Thread-safe queue for HTTP requests. Connection threads enqueue requests and
// wait on the returned future. The thread_worker drains the queue on each
// wakeup and fulfills each promise after executing SQL.
class RequestQueue {
 public:
  RequestQueue();
  ~RequestQueue();

  RequestQueue(const RequestQueue&) = delete;
  RequestQueue& operator=(const RequestQueue&) = delete;

  // File descriptor to use as thread_worker poll_fd. Becomes readable when
  // at least one request is queued.
  int signal_fd() const noexcept { return pipe_[0]; }

  // Enqueue a request from a connection thread. Returns a future that
  // resolves when the thread_worker fulfills the response.
  std::future<HttpResponse> enqueue(HttpRequest req);

  // Drain all queued requests. Consumes pending signal bytes from the pipe.
  // Called from thread_worker only.
  std::vector<PendingRequest> drain();

 private:
  int pipe_[2]{-1, -1};
  std::mutex mu_;
  std::queue<PendingRequest> queue_;
};

}  // namespace vsql_rest

#endif  // VSQL_REST_REQUEST_QUEUE_H
