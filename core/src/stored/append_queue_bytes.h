/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026-2026 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#ifndef BAREOS_STORED_APPEND_QUEUE_BYTES_H_
#define BAREOS_STORED_APPEND_QUEUE_BYTES_H_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace storagedaemon {

/* Bytes of client messages a backup job holds in the storage daemon. The
 * reader waits while they reach the bound; 0 means no bound. */
class AppendQueueBytes {
 public:
  explicit AppendQueueBytes(std::size_t bound) : bound_{bound} {}

  // True once the held bytes are below the bound, false after the timeout.
  bool WaitBelowBound(std::chrono::milliseconds timeout)
  {
    if (bound_ == 0) { return true; }
    std::unique_lock<std::mutex> lock(mutex_);
    return below_bound_.wait_for(lock, timeout,
                                 [this] { return held_ < bound_; });
  }

  void Add(std::size_t bytes)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    held_ += bytes;
  }

  void Remove(std::size_t bytes)
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      held_ = bytes < held_ ? held_ - bytes : 0;
    }
    below_bound_.notify_all();
  }

  std::size_t Held()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return held_;
  }

 private:
  const std::size_t bound_;
  std::mutex mutex_;
  std::condition_variable below_bound_;
  std::size_t held_{};
};

}  // namespace storagedaemon

#endif  // BAREOS_STORED_APPEND_QUEUE_BYTES_H_
