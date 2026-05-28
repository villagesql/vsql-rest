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

#include "request_queue.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>

namespace vsql_rest {

RequestQueue::RequestQueue() {
  if (pipe(pipe_) != 0) {
    pipe_[0] = pipe_[1] = -1;
    return;
  }
  // Write end non-blocking so connection threads never block on a full pipe.
  fcntl(pipe_[1], F_SETFL, O_NONBLOCK);
  // Read end non-blocking so drain() can read until empty without blocking.
  fcntl(pipe_[0], F_SETFL, O_NONBLOCK);
}

RequestQueue::~RequestQueue() {
  if (pipe_[0] >= 0) close(pipe_[0]);
  if (pipe_[1] >= 0) close(pipe_[1]);
  // Drain under lock, fail promises outside the lock to avoid holding mu_
  // while promise destructors or waiting threads run.
  std::vector<PendingRequest> drained;
  {
    std::lock_guard<std::mutex> lock(mu_);
    while (!queue_.empty()) {
      drained.push_back(std::move(queue_.front()));
      queue_.pop();
    }
  }
  for (auto& p : drained) {
    p.promise.set_exception(std::make_exception_ptr(
        std::runtime_error("request queue destroyed")));
  }
}

std::future<HttpResponse> RequestQueue::enqueue(HttpRequest req) {
  PendingRequest pending;
  pending.req = std::move(req);
  auto future = pending.promise.get_future();

  {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.push(std::move(pending));
  }

  // Signal the thread_worker. One byte is sufficient; non-blocking write
  // silently drops if pipe buffer is full (means signal already pending).
  char byte = 1;
  write(pipe_[1], &byte, 1);
  return future;
}

std::vector<PendingRequest> RequestQueue::drain() {
  // Drain all signal bytes. Non-blocking read loops until EAGAIN.
  char buf[256];
  while (read(pipe_[0], buf, sizeof(buf)) > 0) {
  }

  std::vector<PendingRequest> result;
  std::lock_guard<std::mutex> lock(mu_);
  while (!queue_.empty()) {
    result.push_back(std::move(queue_.front()));
    queue_.pop();
  }
  return result;
}

}  // namespace vsql_rest
