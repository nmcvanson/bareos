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

#ifndef BAREOS_STORED_BACKENDS_FLUSH_WAIT_H_
#define BAREOS_STORED_BACKENDS_FLUSH_WAIT_H_

#include <chrono>
#include <cstdint>

namespace storagedaemon {

enum class FlushWaitResult
{
  kWritten,
  kWaiting,
  kCanceled,
  kReadOnly,
  kTimedOut
};

/* Decides how a wait for pending chunk uploads goes on: a read-only device
 * ends it before anything else. The wait times out when the upload progress
 * counter has not moved for the timeout. */
class FlushWaitTracker {
 public:
  using clock = std::chrono::steady_clock;

  constexpr FlushWaitTracker(std::chrono::seconds timeout,
                             clock::time_point now,
                             uint64_t progress)
      : timeout_{timeout}, last_progress_time_{now}, last_progress_{progress}
  {
  }

  constexpr FlushWaitResult Check(bool written,
                                  bool canceled,
                                  bool readonly,
                                  uint64_t progress,
                                  clock::time_point now)
  {
    if (readonly) { return FlushWaitResult::kReadOnly; }
    if (written) { return FlushWaitResult::kWritten; }
    if (canceled) { return FlushWaitResult::kCanceled; }
    if (progress != last_progress_) {
      last_progress_ = progress;
      last_progress_time_ = now;
    }
    if (now - last_progress_time_ >= timeout_) {
      return FlushWaitResult::kTimedOut;
    }
    return FlushWaitResult::kWaiting;
  }

 private:
  std::chrono::seconds timeout_;
  clock::time_point last_progress_time_;
  uint64_t last_progress_;
};

inline constexpr std::chrono::milliseconds kFirstUploadRetryPause{5000};
inline constexpr std::chrono::milliseconds kLongestUploadRetryPause{60000};
inline constexpr std::chrono::milliseconds kShortestUploadRetryPause{1000};

/* Pause before a failed chunk upload is tried again: doubles with each try
 * from the first to the longest pause, and is at most a quarter of the flush
 * timeout (but not below the shortest pause). */
constexpr std::chrono::milliseconds UploadRetryPause(
    unsigned tries,
    std::chrono::seconds flush_timeout)
{
  std::chrono::milliseconds pause = kFirstUploadRetryPause;
  for (unsigned i = 1; i < tries && pause < kLongestUploadRetryPause; ++i) {
    pause *= 2;
  }
  if (pause > kLongestUploadRetryPause) { pause = kLongestUploadRetryPause; }
  std::chrono::milliseconds bound
      = std::chrono::duration_cast<std::chrono::milliseconds>(flush_timeout)
        / 4;
  if (bound < kShortestUploadRetryPause) { bound = kShortestUploadRetryPause; }
  return pause < bound ? pause : bound;
}

}  // namespace storagedaemon

#endif  // BAREOS_STORED_BACKENDS_FLUSH_WAIT_H_
