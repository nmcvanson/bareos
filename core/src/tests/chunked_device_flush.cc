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

/* Bounded waits of chunked devices: the wait decision and retry pause at
 * compile time, the bounded enqueue, and a chunked device whose fake store can
 * slow down, hold or fail its uploads. */

#include "gtest/gtest.h"
#include "include/fcntl_def.h"
#include "include/bareos.h"
#include "stored/stored.h"
#include "stored/stored_globals.h"
#include "lib/messages_resource.h"
#include "lib/thread_specific_data.h"
#include "stored/backends/chunked_device.h"
#include "stored/backends/flush_wait.h"
#include "stored/backends/ordered_cbuf.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace storagedaemon;
using namespace std::chrono_literals;

namespace {

// Gives the device code a working directory for its inflight markers.
class WorkingDirectory : public ::testing::Environment {
 public:
  void SetUp() override
  {
    dir_ = std::filesystem::temp_directory_path()
           / ("chunked_device_flush." + std::to_string(getpid()));
    std::filesystem::create_directories(dir_);
    path_ = dir_.string();
    storage_.working_directory = path_.data();
    me = &storage_;
  }
  void TearDown() override
  {
    me = nullptr;
    storage_.working_directory = nullptr;
    std::filesystem::remove_all(dir_);
  }

 private:
  std::filesystem::path dir_;
  std::string path_;
  StorageResource storage_;
};

const ::testing::Environment* const working_directory
    = ::testing::AddGlobalTestEnvironment(new WorkingDirectory);

using SteadyClock = FlushWaitTracker::clock;

constexpr SteadyClock::time_point At(int seconds)
{ return SteadyClock::time_point{std::chrono::seconds{seconds}}; }

constexpr bool ReadOnlyWinsOverEverything()
{
  FlushWaitTracker tracker{10s, At(0), 0};
  return tracker.Check(true, true, true, 0, At(100))
         == FlushWaitResult::kReadOnly;
}
static_assert(ReadOnlyWinsOverEverything());

constexpr bool WrittenWinsOverCancelAndTimeout()
{
  FlushWaitTracker tracker{10s, At(0), 0};
  return tracker.Check(true, true, false, 0, At(100))
         == FlushWaitResult::kWritten;
}
static_assert(WrittenWinsOverCancelAndTimeout());

constexpr bool CancelAndReadOnlyEndTheWait()
{
  FlushWaitTracker tracker{10s, At(0), 0};
  return tracker.Check(false, true, false, 0, At(1))
             == FlushWaitResult::kCanceled
         && tracker.Check(false, true, true, 0, At(1))
                == FlushWaitResult::kReadOnly
         && tracker.Check(false, false, true, 0, At(1))
                == FlushWaitResult::kReadOnly;
}
static_assert(CancelAndReadOnlyEndTheWait());

constexpr bool TimesOutWithoutProgress()
{
  FlushWaitTracker tracker{10s, At(0), 5};
  return tracker.Check(false, false, false, 5, At(9))
             == FlushWaitResult::kWaiting
         && tracker.Check(false, false, false, 5, At(10))
                == FlushWaitResult::kTimedOut;
}
static_assert(TimesOutWithoutProgress());

constexpr bool ProgressRestartsTheDeadline()
{
  FlushWaitTracker tracker{10s, At(0), 5};
  return tracker.Check(false, false, false, 6, At(9))
             == FlushWaitResult::kWaiting
         && tracker.Check(false, false, false, 6, At(18))
                == FlushWaitResult::kWaiting
         && tracker.Check(false, false, false, 6, At(19))
                == FlushWaitResult::kTimedOut;
}
static_assert(ProgressRestartsTheDeadline());

// Pauses with the code default flush_timeout: 5 s doubling up to 60 s.
static_assert(UploadRetryPause(1, 1800s) == 5s);
static_assert(UploadRetryPause(2, 1800s) == 10s);
static_assert(UploadRetryPause(3, 1800s) == 20s);
static_assert(UploadRetryPause(4, 1800s) == 40s);
static_assert(UploadRetryPause(5, 1800s) == 60s);
static_assert(UploadRetryPause(6, 1800s) == 60s);
static_assert(UploadRetryPause(255, 1800s) == 60s);
static_assert(UploadRetryPause(0, 1800s) == 5s);
// At most a quarter of the flush timeout, never below 1 s.
static_assert(UploadRetryPause(3, 120s) == 20s);
static_assert(UploadRetryPause(4, 120s) == 30s);
static_assert(UploadRetryPause(1, 30s) == 5s);
static_assert(UploadRetryPause(2, 30s) == 7500ms);
static_assert(UploadRetryPause(1, 2s) == 1s);
static_assert(UploadRetryPause(255, 0s) == 1s);

constexpr size_t kChunk = 10 * 1024 * 1024;

// A chunked device whose backing store is a map in memory.
class FakeChunkedDevice : public ChunkedDevice {
 public:
  std::atomic<bool> fail_uploads{false};
  std::atomic<bool> stale_busy{false};  // failed uploads leave EBUSY behind
  std::atomic<bool> hold_uploads{false};
  std::atomic<bool> fail_list{false};
  std::atomic<bool> writer_canceled{false};
  std::atomic<bool> fail_thread_start{false};
  std::atomic<int> upload_delay_ms{0};
  std::atomic<int> fail_next_uploads{0};
  std::atomic<int> conflict_next_uploads{0};  // report a lease conflict
  std::atomic<int> cancel_next_uploads{0};    // report a canceled job
  std::atomic<int> uploads_started{0};
  std::atomic<int> truncates{0};
  std::string reason;

  FakeChunkedDevice(uint8_t io_threads,
                    uint8_t retries,
                    uint32_t timeout,
                    uint8_t io_slots = 2)
  {
    io_threads_ = io_threads;
    io_slots_ = io_slots;
    retries_ = retries;
    flush_timeout_ = timeout;
    errmsg = GetPoolMemory(PM_EMSG);
    errmsg[0] = 0;
    prt_name = GetPoolMemory(PM_NAME);
    PmStrcpy(prt_name, "\"fake\" (memory)");
    bstrncpy(VolCatInfo.VolCatName, "TestVolume",
             sizeof(VolCatInfo.VolCatName));
  }

  // Lets the io-threads finish every upload before the device goes away.
  ~FakeChunkedDevice() override
  {
    fail_uploads = false;
    hold_uploads = false;
    fail_list = false;
    upload_delay_ms = 0;
    VolCatInfo.VolCatBytes = 0;
    for (int i = 0; i < 1200 && !Wait(); ++i) {
      std::this_thread::sleep_for(100ms);
    }
  }

  using ChunkedDevice::ChunkedVolumeSize;
  using ChunkedDevice::ChunksNotUploaded;
  using ChunkedDevice::CloseChunk;
  using ChunkedDevice::ReadChunked;
  using ChunkedDevice::SetupChunk;
  using ChunkedDevice::TruncateChunkedVolume;
  using ChunkedDevice::WriteChunked;

  // Runs the flush wait and keeps the reason of a failure.
  bool Wait(const std::function<bool()>& is_canceled = [] { return false; })
  {
    reason.clear();
    return WaitUntilChunksWritten(is_canceled, reason);
  }

  std::string Stored(uint16_t chunk, const std::string& volume = "TestVolume")
  {
    std::lock_guard<std::mutex> lock(store_mutex_);
    auto it = stored_.find({volume, chunk});
    return it == stored_.end() ? std::string{} : it->second;
  }

  void Seed(const std::string& volume,
            uint16_t chunk,
            const std::vector<char>& data)
  {
    std::lock_guard<std::mutex> lock(store_mutex_);
    stored_[{volume, chunk}].assign(data.begin(), data.end());
  }

  // Makes the volume the one the device works on, with its catalog size.
  void SelectVolume(const char* volume, uint64_t bytes)
  {
    bstrncpy(VolCatInfo.VolCatName, volume, sizeof(VolCatInfo.VolCatName));
    VolCatInfo.VolCatBytes = bytes;
  }

  SteadyClock::time_point LastUploadEnd()
  {
    std::lock_guard<std::mutex> lock(store_mutex_);
    return last_upload_end_;
  }

  // Start times of the upload tries of non-empty chunks.
  std::vector<SteadyClock::time_point> Attempts()
  {
    std::lock_guard<std::mutex> lock(store_mutex_);
    return attempts_;
  }

  SeekMode GetSeekMode() const override { return SeekMode::BYTES; }
  int d_ioctl(int, ioctl_req_t, char*) override { return -1; }
  int d_open(const char*, int, int) override { return 0; }
  int d_close(int) override { return 0; }
  ssize_t d_read(int, void*, size_t) override { return -1; }
  ssize_t d_write(int, const void*, size_t) override { return -1; }
  boffset_t d_lseek(DeviceControlRecord*, boffset_t, int) override
  { return -1; }
  bool d_truncate(DeviceControlRecord*) override { return true; }

 protected:
  bool CheckRemoteConnection() override { return true; }
  bool WriterCanceled() override { return writer_canceled; }
  bool StartIoThreads() override
  { return !fail_thread_start && ChunkedDevice::StartIoThreads(); }

  bool FlushRemoteChunk(chunk_io_request* request) override
  {
    if (request->wbuflen == 0) { return true; }
    int left = conflict_next_uploads.load();
    if (left > 0
        && conflict_next_uploads.compare_exchange_strong(left, left - 1)) {
      PmStrcpy(errmsg, "stale message of another job\n");
      request->lease_conflict = true;
      return false;
    }
    left = cancel_next_uploads.load();
    if (left > 0
        && cancel_next_uploads.compare_exchange_strong(left, left - 1)) {
      PmStrcpy(errmsg, "canceled\n");
      request->canceled = true;
      return false;
    }
    auto inflight_lease = getInflightLease(request);
    if (!inflight_lease) {
      PmStrcpy(errmsg, "chunk is already being uploaded\n");
      request->lease_conflict = true;
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(store_mutex_);
      attempts_.push_back(SteadyClock::now());
    }
    ++uploads_started;
    while (hold_uploads) { std::this_thread::sleep_for(10ms); }
    std::this_thread::sleep_for(std::chrono::milliseconds{upload_delay_ms});
    int fail_next = fail_next_uploads.load();
    const bool fail_once = fail_next > 0
                           && fail_next_uploads.compare_exchange_strong(
                               fail_next, fail_next - 1);
    if (fail_uploads || fail_once) {
      PmStrcpy(errmsg, "injected upload failure\n");
      if (stale_busy) { dev_errno = EBUSY; }
      return false;
    }
    std::lock_guard<std::mutex> lock(store_mutex_);
    std::string& object = stored_[{request->volname, request->chunk}];
    if (object.size() <= request->wbuflen) {
      object.assign(request->buffer, request->wbuflen);
    }
    last_upload_end_ = SteadyClock::now();
    return true;
  }

  bool ReadRemoteChunk(chunk_io_request* request) override
  {
    std::lock_guard<std::mutex> lock(store_mutex_);
    auto it = stored_.find({request->volname, request->chunk});
    if (it == stored_.end()) {
      dev_errno = EIO;
      return false;
    }
    memcpy(request->buffer, it->second.data(), it->second.size());
    *request->rbuflen = it->second.size();
    return true;
  }

  std::optional<ssize_t> RemoteVolumeSize() override
  {
    if (fail_list) {
      PmStrcpy(errmsg, "injected list failure\n");
      return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(store_mutex_);
    ssize_t size = -1;
    for (const auto& [key, object] : stored_) {
      if (key.first == VolCatInfo.VolCatName) {
        size = std::max<ssize_t>(size, 0) + static_cast<ssize_t>(object.size());
      }
    }
    return size;
  }

  bool TruncateRemoteVolume(DeviceControlRecord*) override
  {
    std::lock_guard<std::mutex> lock(store_mutex_);
    for (auto it = stored_.begin(); it != stored_.end();) {
      it = it->first.first == VolCatInfo.VolCatName ? stored_.erase(it)
                                                    : std::next(it);
    }
    ++truncates;
    return true;
  }

 private:
  std::mutex store_mutex_;
  std::map<std::pair<std::string, uint16_t>, std::string> stored_;
  std::vector<SteadyClock::time_point> attempts_;
  SteadyClock::time_point last_upload_end_{};
};

std::vector<char> Pattern(size_t size)
{
  std::vector<char> data(size);
  for (size_t i = 0; i < size; ++i) { data[i] = static_cast<char>(i * 7); }
  return data;
}

// Opens the volume and writes the data in blocks, as the SD does.
void WriteVolume(FakeChunkedDevice& dev, const std::vector<char>& data)
{
  ASSERT_EQ(dev.SetupChunk("TestVolume", O_CREAT | O_RDWR, 0640), 0);
  constexpr size_t block = 1024 * 1024;
  for (size_t done = 0; done < data.size(); done += block) {
    const size_t count = std::min(block, data.size() - done);
    ASSERT_EQ(dev.WriteChunked(0, data.data() + done, count),
              static_cast<ssize_t>(count));
  }
  dev.VolCatInfo.VolCatBytes = data.size();
}

/* Writes the data in blocks until a write fails or the time is up; returns
 * the number of bytes written and the errno of the failed write. */
size_t WriteUntilRefused(FakeChunkedDevice& dev,
                         const std::vector<char>& data,
                         std::chrono::seconds limit,
                         int& write_errno)
{
  constexpr size_t block = 1024 * 1024;
  const auto give_up = SteadyClock::now() + limit;
  size_t written = 0;
  write_errno = 0;
  while (SteadyClock::now() < give_up) {
    const size_t count = std::min(block, data.size() - written);
    if (count == 0) { std::this_thread::sleep_for(100ms); }
    if (dev.WriteChunked(0, data.data() + written, count) < 0) {
      write_errno = errno;
      break;
    }
    written += count;
  }
  return written;
}

// Waits until the store holds the chunk or the time is up.
bool WaitStored(FakeChunkedDevice& dev, uint16_t chunk, size_t size)
{
  const auto give_up = SteadyClock::now() + 60s;
  while (dev.Stored(chunk).size() != size && SteadyClock::now() < give_up) {
    std::this_thread::sleep_for(100ms);
  }
  return dev.Stored(chunk).size() == size;
}

// Waits until zero-byte writes are taken again or the time is up.
bool WaitWritable(FakeChunkedDevice& dev, const std::vector<char>& data)
{
  const auto give_up = SteadyClock::now() + 60s;
  while (dev.WriteChunked(0, data.data(), 0) < 0
         && SteadyClock::now() < give_up) {
    std::this_thread::sleep_for(100ms);
  }
  return dev.WriteChunked(0, data.data(), 0) == 0;
}

// Waits until the store saw the number of upload tries and they ended.
bool WaitAttempts(FakeChunkedDevice& dev, size_t tries)
{
  const auto give_up = SteadyClock::now() + 60s;
  while (dev.Attempts().size() < tries && SteadyClock::now() < give_up) {
    std::this_thread::sleep_for(10ms);
  }
  std::this_thread::sleep_for(200ms);
  return dev.Attempts().size() >= tries;
}

bool Contains(const std::string& text, const char* part)
{ return text.find(part) != std::string::npos; }

int CompareInts(ocbuf_item* a, ocbuf_item* b)
{
  const int left = *static_cast<int*>(a->data);
  const int right = *static_cast<int*>(b->data);
  return left == right ? 0 : (left < right ? -1 : 1);
}

void KeepOld(void*, void*) {}

struct timespec AfterMilliseconds(int ms)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  const int64_t nsec = ts.tv_nsec + static_cast<int64_t>(ms % 1000) * 1000000;
  ts.tv_sec += ms / 1000 + nsec / 1000000000;
  ts.tv_nsec = nsec % 1000000000;
  return ts;
}

}  // namespace

TEST(ordered_circbuf, FullEnqueueWaitsUntilTheGivenTime)
{
  ordered_circbuf cb(1);
  int a = 1, b = 2;
  ASSERT_EQ(cb.enqueue(&a, sizeof(a), CompareInts, KeepOld), &a);

  const auto start = SteadyClock::now();
  bool was_full = false;
  const struct timespec until = AfterMilliseconds(300);
  EXPECT_EQ(cb.enqueue(&b, sizeof(b), CompareInts, KeepOld, false, false,
                       &until, &was_full),
            nullptr);
  EXPECT_TRUE(was_full);
  EXPECT_GE(SteadyClock::now() - start, 250ms);
}

TEST(ordered_circbuf, IgnoreFullQueuesPastTheCapacityAndStaysFull)
{
  ordered_circbuf cb(1);
  int a = 1, b = 2, c = 3;
  ASSERT_EQ(cb.enqueue(&a, sizeof(a), CompareInts, KeepOld), &a);
  bool was_full = true;
  EXPECT_EQ(cb.enqueue(&b, sizeof(b), CompareInts, KeepOld, false, false,
                       nullptr, &was_full, true),
            &b);
  EXPECT_FALSE(was_full);
  EXPECT_TRUE(cb.full());

  // One item taken out still leaves the buffer over its capacity.
  EXPECT_NE(cb.dequeue(), nullptr);
  EXPECT_TRUE(cb.full());
  const struct timespec until = AfterMilliseconds(200);
  EXPECT_EQ(cb.enqueue(&c, sizeof(c), CompareInts, KeepOld, false, false,
                       &until, &was_full),
            nullptr);
  EXPECT_TRUE(was_full);
  EXPECT_NE(cb.dequeue(), nullptr);
  EXPECT_FALSE(cb.full());
}

TEST(ordered_circbuf, TimedEnqueueWakesWhenASlotIsFreed)
{
  ordered_circbuf cb(1);
  int a = 1, b = 2;
  ASSERT_EQ(cb.enqueue(&a, sizeof(a), CompareInts, KeepOld), &a);

  std::thread consumer([&cb] {
    std::this_thread::sleep_for(300ms);
    cb.dequeue(true /* reserve_slot */);
    cb.unreserve_slot();
  });
  const auto start = SteadyClock::now();
  bool was_full = true;
  const struct timespec until = AfterMilliseconds(10000);
  EXPECT_EQ(cb.enqueue(&b, sizeof(b), CompareInts, KeepOld, false, false,
                       &until, &was_full),
            &b);
  EXPECT_FALSE(was_full);
  EXPECT_LT(SteadyClock::now() - start, 5s);
  consumer.join();
}

TEST(chunked_device_flush, ReleasesWithinOneSecondOfTheLastUpload)
{
  FakeChunkedDevice dev{1, 0, 60};
  dev.upload_delay_ms = 200;
  const auto data = Pattern(2 * kChunk + 12345);
  WriteVolume(dev, data);

  ASSERT_TRUE(dev.Wait()) << dev.reason;
  EXPECT_LT(SteadyClock::now() - dev.LastUploadEnd(), 1s);

  EXPECT_EQ(dev.Stored(0), std::string(data.data(), kChunk));
  EXPECT_EQ(dev.Stored(1), std::string(data.data() + kChunk, kChunk));
  EXPECT_EQ(dev.Stored(2), std::string(data.data() + 2 * kChunk, 12345));
  EXPECT_EQ(dev.CloseChunk(), 0);
}

TEST(chunked_device_flush, WaitEndsWhenNoUploadSucceedsAndKeepsTheChunk)
{
  FakeChunkedDevice dev{1, 0, 2};
  dev.fail_uploads = true;
  const auto data = Pattern(4096);
  WriteVolume(dev, data);

  const auto start = SteadyClock::now();
  EXPECT_FALSE(dev.Wait());
  EXPECT_GE(SteadyClock::now() - start, 2s);
  EXPECT_LT(SteadyClock::now() - start, 5s);
  EXPECT_TRUE(Contains(dev.reason, "made no progress")) << dev.reason;
  EXPECT_TRUE(Contains(dev.reason, "injected upload failure")) << dev.reason;

  // The device frees its own buffer; the queued copy is uploaded later.
  EXPECT_EQ(dev.CloseChunk(), 0);
  dev.fail_uploads = false;
  EXPECT_TRUE(WaitStored(dev, 0, data.size()));
  EXPECT_EQ(dev.Stored(0), std::string(data.data(), data.size()));
}

TEST(chunked_device_flush, TransientOutageShorterThanTheTimeoutSucceeds)
{
  FakeChunkedDevice dev{1, 0, 10};
  dev.fail_uploads = true;
  const auto data = Pattern(kChunk + 100);
  WriteVolume(dev, data);

  std::thread recover([&dev] {
    std::this_thread::sleep_for(1500ms);
    dev.fail_uploads = false;
  });
  const auto start = SteadyClock::now();
  EXPECT_TRUE(dev.Wait()) << dev.reason;
  EXPECT_LT(SteadyClock::now() - start, 10s);
  recover.join();

  EXPECT_EQ(dev.Stored(0), std::string(data.data(), kChunk));
  EXPECT_EQ(dev.Stored(1), std::string(data.data() + kChunk, 100));
  EXPECT_EQ(dev.CloseChunk(), 0);
}

/* A lease conflict on an io-thread is no failed try: with retries = 1 a
 * counted try would make the device read-only, and no error is reported. */
TEST(chunked_device_flush, LeaseConflictOnAnIoThreadIsRequeuedWithoutATry)
{
  FakeChunkedDevice dev{1, 1, 30};
  dev.conflict_next_uploads = 2;
  const auto data = Pattern(4096);
  WriteVolume(dev, data);

  EXPECT_TRUE(dev.Wait()) << dev.reason;
  EXPECT_EQ(dev.conflict_next_uploads.load(), 0);
  EXPECT_EQ(dev.Stored(0), std::string(data.data(), data.size()));
  EXPECT_EQ(dev.Attempts().size(), 1u);
  EXPECT_FALSE(Contains(dev.reason, "stale message")) << dev.reason;
  EXPECT_EQ(dev.CloseChunk(), 0);
}

TEST(chunked_device_flush, CancelEndsTheWait)
{
  FakeChunkedDevice dev{1, 0, 30};
  dev.fail_uploads = true;
  WriteVolume(dev, Pattern(4096));

  std::atomic<bool> canceled{false};
  std::thread cancel([&canceled] {
    std::this_thread::sleep_for(1s);
    canceled = true;
  });
  const auto start = SteadyClock::now();
  EXPECT_FALSE(dev.Wait([&canceled] { return canceled.load(); }));
  EXPECT_LT(SteadyClock::now() - start, 5s);
  EXPECT_TRUE(Contains(dev.reason, "job canceled")) << dev.reason;
  cancel.join();
}

TEST(chunked_device_flush, CancelWhileAnUploadRunsKeepsTheWrittenBytes)
{
  FakeChunkedDevice dev{1, 0, 30};
  dev.hold_uploads = true;
  const auto data = Pattern(4096);
  WriteVolume(dev, data);

  std::atomic<bool> canceled{false};
  std::thread cancel([&canceled] {
    std::this_thread::sleep_for(1s);
    canceled = true;
  });
  EXPECT_FALSE(dev.Wait([&canceled] { return canceled.load(); }));
  cancel.join();
  EXPECT_GE(dev.uploads_started.load(), 1);

  // The device frees its buffer while the copy is still being uploaded.
  EXPECT_EQ(dev.CloseChunk(), 0);
  std::vector<char> reuse(kChunk, '\xff');
  ASSERT_EQ(reuse.size(), kChunk);
  dev.hold_uploads = false;
  EXPECT_TRUE(WaitStored(dev, 0, data.size()));
  EXPECT_EQ(dev.Stored(0), std::string(data.data(), data.size()));
}

TEST(chunked_device_flush, FailedUploadsAreRetriedAfterAGrowingPause)
{
  // Two io-threads; flush_timeout 40 s caps the pause at 10 s: 5, 10, 10 s.
  FakeChunkedDevice dev{2, 0, 40};
  dev.fail_uploads = true;
  WriteVolume(dev, Pattern(kChunk + 10));

  std::this_thread::sleep_for(17s);
  const auto attempts = dev.Attempts();
  ASSERT_GE(attempts.size(), 3u);
  EXPECT_LE(attempts.size(), 4u);
  EXPECT_GE(attempts[1] - attempts[0], 4500ms);
  EXPECT_LT(attempts[1] - attempts[0], 7s);
  EXPECT_GE(attempts[2] - attempts[1], 9500ms);
  EXPECT_LT(attempts[2] - attempts[1], 12s);
}

TEST(chunked_device_flush, RetriesKeepTheChunkAndRefuseWritesUntilDrained)
{
  FakeChunkedDevice dev{1, 2, 30};
  dev.fail_uploads = true;
  const auto data = Pattern(2 * kChunk);
  ASSERT_EQ(dev.SetupChunk("TestVolume", O_CREAT | O_RDWR, 0640), 0);

  // Writes go on until the device turns read-only after two failed tries.
  int write_errno = 0;
  const size_t written = WriteUntilRefused(dev, data, 30s, write_errno);
  EXPECT_EQ(write_errno, EIO);
  EXPECT_EQ(dev.dev_errno, EIO);
  EXPECT_GE(written, kChunk);

  EXPECT_FALSE(dev.Wait());
  EXPECT_TRUE(Contains(dev.reason, "refuses writes")) << dev.reason;
  EXPECT_TRUE(Contains(dev.reason, "chunk 0 of volume TestVolume"))
      << dev.reason;

  // A write to the read-only device fails with EIO.
  dev.dev_errno = 0;
  EXPECT_LT(dev.WriteChunked(0, data.data(), 0), 0);
  EXPECT_EQ(dev.dev_errno, EIO);

  // Once the store works again the kept chunk is uploaded and writes resume.
  dev.fail_uploads = false;
  EXPECT_TRUE(WaitStored(dev, 0, kChunk));
  EXPECT_EQ(dev.Stored(0), std::string(data.data(), kChunk));
  EXPECT_TRUE(WaitWritable(dev, data));
}

TEST(chunked_device_flush, FullQueueWithoutProgressRefusesTheWriteAndKeepsIt)
{
  // One io-thread with one slot: the second chunk waits for a free slot.
  FakeChunkedDevice dev{1, 0, 3, 1};
  dev.fail_uploads = true;
  const auto data = Pattern(3 * kChunk);
  ASSERT_EQ(dev.SetupChunk("TestVolume", O_CREAT | O_RDWR, 0640), 0);

  const auto start = SteadyClock::now();
  int write_errno = 0;
  const size_t written = WriteUntilRefused(dev, data, 30s, write_errno);
  const auto refused_after = SteadyClock::now() - start;
  EXPECT_EQ(write_errno, EIO);
  EXPECT_EQ(dev.dev_errno, EIO);
  EXPECT_GE(written, kChunk);
  EXPECT_GE(refused_after, 3s);
  EXPECT_LT(refused_after, 8s);

  // Both full chunks are kept and uploaded once the store works again.
  dev.fail_uploads = false;
  EXPECT_TRUE(WaitStored(dev, 0, kChunk));
  EXPECT_TRUE(WaitStored(dev, 1, kChunk));
  EXPECT_EQ(dev.Stored(1), std::string(data.data() + kChunk, kChunk));
  EXPECT_TRUE(WaitWritable(dev, data));
}

TEST(chunked_device_flush, CancelEndsTheWaitForAFreeSlot)
{
  FakeChunkedDevice dev{1, 0, 60, 1};
  dev.fail_uploads = true;
  const auto data = Pattern(3 * kChunk);
  ASSERT_EQ(dev.SetupChunk("TestVolume", O_CREAT | O_RDWR, 0640), 0);

  std::thread cancel([&dev] {
    std::this_thread::sleep_for(1s);
    dev.writer_canceled = true;
  });
  const auto start = SteadyClock::now();
  int write_errno = 0;
  WriteUntilRefused(dev, data, 30s, write_errno);
  EXPECT_EQ(write_errno, EIO);
  EXPECT_LT(SteadyClock::now() - start, 4s);
  cancel.join();

  dev.writer_canceled = false;
  dev.fail_uploads = false;
  EXPECT_TRUE(WaitStored(dev, 1, kChunk));
}

TEST(chunked_device_flush, FullQueueWakesTheWriterWhenAnUploadEnds)
{
  FakeChunkedDevice dev{1, 0, 60, 1};
  dev.upload_delay_ms = 200;
  const auto start = SteadyClock::now();
  WriteVolume(dev, Pattern(6 * kChunk));
  // Five chunk uploads of 0.2 s; one-second slices would need over 4 s.
  EXPECT_LT(SteadyClock::now() - start, 3500ms);
  ASSERT_TRUE(dev.Wait()) << dev.reason;
}

TEST(chunked_device_flush, OlderFailedChunkNeverReplacesANewerQueuedOne)
{
  FakeChunkedDevice dev{1, 0, 2};
  dev.hold_uploads = true;
  dev.fail_uploads = true;
  const auto data = Pattern(8192);
  ASSERT_EQ(dev.SetupChunk("TestVolume", O_CREAT | O_RDWR, 0640), 0);

  // A copy of the first half is being uploaded when the second half arrives.
  ASSERT_EQ(dev.WriteChunked(0, data.data(), 4096), 4096);
  EXPECT_FALSE(dev.Wait());
  ASSERT_GE(dev.uploads_started.load(), 1);
  ASSERT_EQ(dev.WriteChunked(0, data.data() + 4096, 4096), 4096);
  EXPECT_FALSE(dev.Wait());

  // The older, smaller copy fails and must not take the newer one's place.
  dev.hold_uploads = false;
  std::this_thread::sleep_for(500ms);
  dev.fail_uploads = false;
  EXPECT_TRUE(WaitStored(dev, 0, data.size()));
  EXPECT_EQ(dev.Stored(0), std::string(data.data(), data.size()));
}

TEST(chunked_device_flush, ListErrorIsNeverReadAsSizeZero)
{
  FakeChunkedDevice dev{1, 0, 2};
  WriteVolume(dev, Pattern(4096));
  ASSERT_TRUE(dev.Wait()) << dev.reason;

  dev.fail_list = true;
  dev.VolCatInfo.VolCatBytes = 0;
  EXPECT_EQ(dev.ChunkedVolumeSize(), -1);
  EXPECT_FALSE(dev.Wait());
  EXPECT_TRUE(Contains(dev.reason, "made no progress")) << dev.reason;
  EXPECT_TRUE(Contains(dev.reason, "injected list failure")) << dev.reason;
}

TEST(chunked_device_flush, VolumeWithoutChunksIsNotWritten)
{
  FakeChunkedDevice dev{1, 0, 2};
  ASSERT_EQ(dev.SetupChunk("TestVolume", O_CREAT | O_RDWR, 0640), 0);
  dev.VolCatInfo.VolCatBytes = 100;
  EXPECT_FALSE(dev.Wait());
}

TEST(chunked_device_flush, BlockingUploadsWaitForTheListing)
{
  FakeChunkedDevice dev{0, 0, 60};
  const auto data = Pattern(kChunk + 10);
  WriteVolume(dev, data);
  EXPECT_TRUE(dev.Wait()) << dev.reason;
  EXPECT_EQ(dev.Stored(1), std::string(data.data() + kChunk, 10));
}

TEST(chunked_device_flush, BlockingUploadFailureRefusesTheWriteAtOnce)
{
  FakeChunkedDevice dev{0, 0, 60};
  dev.fail_uploads = true;
  const auto data = Pattern(2 * kChunk);
  ASSERT_EQ(dev.SetupChunk("TestVolume", O_CREAT | O_RDWR, 0640), 0);
  int write_errno = 0;
  const auto start = SteadyClock::now();
  WriteUntilRefused(dev, data, 10s, write_errno);
  EXPECT_EQ(write_errno, EIO);
  EXPECT_EQ(dev.dev_errno, EIO);
  EXPECT_LT(SteadyClock::now() - start, 2s);
}

TEST(chunked_device_flush, QueueThatCannotStartFailsTheJobAndKeepsTheChunk)
{
  auto jcr = std::make_shared<JobControlRecord>();
  SetJcrInThreadSpecificData(jcr.get());
  {
    FakeChunkedDevice dev{1, 0, 60};
    const auto data = Pattern(2 * kChunk);
    dev.Seed("TestVolume", 0, {});
    ASSERT_EQ(dev.SetupChunk("TestVolume", O_CREAT | O_RDWR, 0640), 0);

    // The first full chunk cannot be queued: the write fails, the job too.
    dev.fail_thread_start = true;
    int write_errno = 0;
    const size_t written = WriteUntilRefused(dev, data, 10s, write_errno);
    EXPECT_EQ(write_errno, EIO);
    EXPECT_EQ(dev.dev_errno, EIO);
    EXPECT_EQ(written, kChunk);
    EXPECT_EQ(jcr->getJobStatus(), JS_FatalError);

    // The chunk stayed in the device and is uploaded once threads start.
    dev.fail_thread_start = false;
    dev.SelectVolume("TestVolume", kChunk);
    EXPECT_TRUE(dev.Wait()) << dev.reason;
    EXPECT_EQ(dev.Stored(0), std::string(data.data(), kChunk));
  }
  SetJcrInThreadSpecificData(nullptr);
}

TEST(chunked_device_flush, FatalMessagesRoutedNowhereStillFailTheJob)
{
  MessagesResource routes_nothing;
  auto jcr = std::make_shared<JobControlRecord>();
  jcr->jcr_msgs = &routes_nothing;
  SetJcrInThreadSpecificData(jcr.get());
  {
    FakeChunkedDevice dev{0, 0, 60};
    dev.fail_uploads = true;
    ASSERT_EQ(dev.SetupChunk("TestVolume", O_CREAT | O_RDWR, 0640), 0);
    int write_errno = 0;
    WriteUntilRefused(dev, Pattern(2 * kChunk), 10s, write_errno);
    EXPECT_EQ(write_errno, EIO);
  }
  EXPECT_EQ(jcr->getJobStatus(), JS_FatalError);
  SetJcrInThreadSpecificData(nullptr);
  jcr->jcr_msgs = nullptr;
}

TEST(chunked_device_flush, ReadOpenReleasesAtOnceWhileOtherChunksWait)
{
  FakeChunkedDevice dev{1, 0, 2};
  dev.fail_uploads = true;
  WriteVolume(dev, Pattern(4096));
  EXPECT_FALSE(dev.Wait());
  EXPECT_EQ(dev.CloseChunk(), 0);

  // A restore of another volume does not wait for those chunks.
  const auto other = Pattern(5000);
  dev.Seed("OtherVolume", 0, other);
  dev.SelectVolume("OtherVolume", other.size());
  ASSERT_EQ(dev.SetupChunk("OtherVolume", O_RDONLY, 0), 0);
  const auto start = SteadyClock::now();
  EXPECT_TRUE(dev.Wait()) << dev.reason;
  EXPECT_LT(SteadyClock::now() - start, 500ms);
  EXPECT_EQ(dev.CloseChunk(), 0);

  // A writer on the same device still waits for the queue.
  dev.SelectVolume("TestVolume", 4096);
  ASSERT_EQ(dev.SetupChunk("TestVolume", O_CREAT | O_RDWR, 0640), 0);
  EXPECT_FALSE(dev.Wait());
  EXPECT_TRUE(Contains(dev.reason, "made no progress")) << dev.reason;
}

TEST(chunked_device_flush, TruncateWaitsForTheQueuedChunksOfTheVolume)
{
  // Failed tries are 5 s apart, so none overwrites errmsg during the checks.
  FakeChunkedDevice dev{1, 0, 60};
  dev.fail_uploads = true;
  const auto data = Pattern(8192);
  ASSERT_EQ(dev.SetupChunk("TestVolume", O_CREAT | O_RDWR, 0640), 0);

  // Two flushes of the same chunk leave one chunk waiting.
  for (size_t part = 0; part < 2; ++part) {
    ASSERT_EQ(dev.WriteChunked(0, data.data() + part * 4096, 4096), 4096);
    EXPECT_FALSE(dev.Wait([] { return true; }));
    ASSERT_TRUE(WaitAttempts(dev, part + 1));
  }
  dev.VolCatInfo.VolCatBytes = data.size();

  EXPECT_FALSE(dev.TruncateChunkedVolume(nullptr));
  EXPECT_EQ(dev.dev_errno, EBUSY);
  EXPECT_TRUE(Contains(dev.errmsg, "Volume TestVolume")) << dev.errmsg;
  EXPECT_TRUE(Contains(dev.errmsg, "1 chunk(s) waiting")) << dev.errmsg;
  EXPECT_EQ(dev.truncates.load(), 0);

  // Another volume of the device may be truncated.
  dev.SelectVolume("OtherVolume", 0);
  EXPECT_TRUE(dev.TruncateChunkedVolume(nullptr));
  EXPECT_EQ(dev.truncates.load(), 1);

  // The kept chunk is uploaded unchanged; then the volume may be truncated.
  dev.SelectVolume("TestVolume", data.size());
  dev.fail_uploads = false;
  EXPECT_TRUE(WaitStored(dev, 0, data.size()));
  EXPECT_EQ(dev.Stored(0), std::string(data.data(), data.size()));
  EXPECT_TRUE(dev.Wait()) << dev.reason;
  EXPECT_TRUE(dev.TruncateChunkedVolume(nullptr));
  EXPECT_EQ(dev.truncates.load(), 2);
}

TEST(chunked_device_flush, TruncateWaitsForAChunkBeingUploaded)
{
  FakeChunkedDevice dev{1, 0, 2};
  dev.hold_uploads = true;
  WriteVolume(dev, Pattern(4096));
  EXPECT_FALSE(dev.Wait());
  ASSERT_GE(dev.uploads_started.load(), 1);

  EXPECT_FALSE(dev.TruncateChunkedVolume(nullptr));
  EXPECT_EQ(dev.dev_errno, EBUSY);
  EXPECT_TRUE(Contains(dev.errmsg, "1 chunk(s) waiting")) << dev.errmsg;
  EXPECT_EQ(dev.truncates.load(), 0);
}

/* Without io-threads, writes a full chunk whose upload fails: the write is
 * refused and the job of the thread fails. */
void FailFirstBlockingChunk(FakeChunkedDevice& dev,
                            const std::vector<char>& data)
{
  dev.fail_uploads = true;
  ASSERT_EQ(dev.SetupChunk("TestVolume", O_CREAT | O_RDWR, 0640), 0);
  int write_errno = 0;
  EXPECT_EQ(WriteUntilRefused(dev, data, 10s, write_errno), kChunk);
  EXPECT_EQ(write_errno, EIO);
  dev.SelectVolume("TestVolume", kChunk);
}

TEST(chunked_device_flush, BlockingUploadFailureKeepsTheChunkAndRefusesWrites)
{
  auto jcr = std::make_shared<JobControlRecord>();
  SetJcrInThreadSpecificData(jcr.get());
  {
    FakeChunkedDevice dev{0, 0, 5};
    const auto data = Pattern(2 * kChunk);
    FailFirstBlockingChunk(dev, data);
    EXPECT_EQ(jcr->getJobStatus(), JS_FatalError);

    // Every other writer of the device is refused while the chunk is kept.
    auto other = std::make_shared<JobControlRecord>();
    SetJcrInThreadSpecificData(other.get());
    EXPECT_LT(dev.WriteChunked(0, data.data(), 0), 0);
    EXPECT_EQ(dev.dev_errno, EIO);
    EXPECT_EQ(other->getJobStatus(), JS_FatalError);
    SetJcrInThreadSpecificData(jcr.get());

    // Once the store is back the release wait uploads the kept chunk.
    dev.fail_uploads = false;
    EXPECT_TRUE(dev.Wait()) << dev.reason;
    EXPECT_EQ(dev.Stored(0), std::string(data.data(), kChunk));
    EXPECT_EQ(dev.WriteChunked(0, data.data(), 0), 0);
  }
  SetJcrInThreadSpecificData(nullptr);
}

TEST(chunked_device_flush, KeptChunkIsRetriedUntilTheFlushTimeout)
{
  FakeChunkedDevice dev{0, 0, 3};
  const auto data = Pattern(2 * kChunk);
  FailFirstBlockingChunk(dev, data);

  const auto start = SteadyClock::now();
  EXPECT_FALSE(dev.Wait());
  EXPECT_GE(SteadyClock::now() - start, 3s);
  EXPECT_LT(SteadyClock::now() - start, 10s);
  EXPECT_TRUE(Contains(dev.reason, "chunk 0 of volume TestVolume"))
      << dev.reason;
  EXPECT_TRUE(Contains(dev.reason, "injected upload failure")) << dev.reason;
  EXPECT_GE(dev.Attempts().size(), 3u);
  EXPECT_LT(dev.WriteChunked(0, data.data(), 0), 0);

  // The chunk is still kept: a later wait uploads it.
  dev.fail_uploads = false;
  EXPECT_TRUE(dev.Wait()) << dev.reason;
  EXPECT_EQ(dev.Stored(0), std::string(data.data(), kChunk));
}

TEST(chunked_device_flush, CancelEndsTheRetryOfAKeptChunk)
{
  FakeChunkedDevice dev{0, 0, 600};
  FailFirstBlockingChunk(dev, Pattern(2 * kChunk));

  const auto start = SteadyClock::now();
  EXPECT_FALSE(dev.Wait([start] { return SteadyClock::now() - start > 1s; }));
  EXPECT_LT(SteadyClock::now() - start, 3s);
  EXPECT_TRUE(Contains(dev.reason, "canceled")) << dev.reason;
}

TEST(chunked_device_flush, CloseKeepsTheChunkAndTheNextOpenUploadsIt)
{
  auto jcr = std::make_shared<JobControlRecord>();
  SetJcrInThreadSpecificData(jcr.get());
  {
    FakeChunkedDevice dev{0, 0, 60};
    const auto data = Pattern(2 * kChunk);
    FailFirstBlockingChunk(dev, data);
    dev.CloseChunk();

    // An open while the store still fails is refused and fails that job.
    auto next = std::make_shared<JobControlRecord>();
    SetJcrInThreadSpecificData(next.get());
    EXPECT_EQ(dev.SetupChunk("TestVolume", O_RDWR, 0640), -1);
    EXPECT_EQ(dev.dev_errno, EROFS);
    EXPECT_EQ(next->getJobStatus(), JS_FatalError);

    // The next open after the outage uploads it and takes writes again.
    auto last = std::make_shared<JobControlRecord>();
    SetJcrInThreadSpecificData(last.get());
    dev.fail_uploads = false;
    EXPECT_EQ(dev.SetupChunk("TestVolume", O_RDWR, 0640), 0);
    EXPECT_EQ(dev.Stored(0), std::string(data.data(), kChunk));
    EXPECT_EQ(dev.WriteChunked(0, data.data(), 0), 0);
    EXPECT_NE(last->getJobStatus(), JS_FatalError);
  }
  SetJcrInThreadSpecificData(nullptr);
}

TEST(chunked_device_flush, TruncateIsRefusedWhileAChunkIsKept)
{
  FakeChunkedDevice dev{0, 0, 60};
  FailFirstBlockingChunk(dev, Pattern(2 * kChunk));

  EXPECT_FALSE(dev.TruncateChunkedVolume(nullptr));
  EXPECT_EQ(dev.dev_errno, EBUSY);
  EXPECT_TRUE(Contains(dev.errmsg, "1 chunk(s) waiting")) << dev.errmsg;
  EXPECT_EQ(dev.truncates.load(), 0);
}

/* An EBUSY left in dev_errno by another job is not a lease conflict: the
 * failed upload still fails the writer and the chunk is kept and retried. */
TEST(chunked_device_flush, StaleBusyErrnoIsNotALeaseConflict)
{
  FakeChunkedDevice dev{0, 0, 60};
  dev.stale_busy = true;
  const auto data = Pattern(2 * kChunk);
  FailFirstBlockingChunk(dev, data);

  dev.fail_uploads = false;
  EXPECT_TRUE(dev.Wait()) << dev.reason;
  EXPECT_EQ(dev.Stored(0), std::string(data.data(), kChunk));
}

TEST(chunked_device_flush, KeptChunkOfAnotherVolumeKeepsTheReleaseWaiting)
{
  FakeChunkedDevice dev{0, 0, 60};
  const auto data = Pattern(2 * kChunk);
  FailFirstBlockingChunk(dev, data);

  // A job on another volume does not end OK while the chunk is not stored.
  dev.SelectVolume("OtherVolume", 0);
  const auto start = SteadyClock::now();
  EXPECT_FALSE(dev.Wait([start] { return SteadyClock::now() - start > 1s; }));

  dev.fail_uploads = false;
  EXPECT_TRUE(dev.Wait()) << dev.reason;
  EXPECT_EQ(dev.Stored(0, "TestVolume"), std::string(data.data(), kChunk));
}

TEST(chunked_device_flush, ReadOfAnotherVolumeLeavesTheKeptChunkIntact)
{
  FakeChunkedDevice dev{0, 0, 60};
  const auto data = Pattern(2 * kChunk);
  FailFirstBlockingChunk(dev, data);
  dev.CloseChunk();

  // A restore loads other data into the device's chunk buffer.
  const std::vector<char> other(5000, 'x');
  dev.Seed("OtherVolume", 0, other);
  dev.SelectVolume("OtherVolume", other.size());
  ASSERT_EQ(dev.SetupChunk("OtherVolume", O_RDONLY, 0), 0);
  std::vector<char> read(other.size());
  EXPECT_EQ(dev.ReadChunked(0, read.data(), read.size()),
            static_cast<ssize_t>(read.size()));
  EXPECT_EQ(read, other);
  dev.CloseChunk();

  // A reader does not wait for it; the next open for writing uploads it.
  EXPECT_TRUE(dev.Wait()) << dev.reason;
  dev.fail_uploads = false;
  dev.SelectVolume("TestVolume", kChunk);
  EXPECT_EQ(dev.SetupChunk("TestVolume", O_RDWR, 0640), 0);
  EXPECT_EQ(dev.Stored(0), std::string(data.data(), kChunk));
}

/* A release flush copies chunk 0 with 4096 bytes and its upload is held; the
 * writer then fills chunk 0, whose upload meets the copy in flight and is
 * kept. Returns the release wait's thread, still waiting. */
std::thread KeepTwoCopiesOfChunkZero(FakeChunkedDevice& dev,
                                     const std::vector<char>& data,
                                     bool& waited_ok,
                                     std::string& stored_when_done)
{
  EXPECT_EQ(dev.SetupChunk("TestVolume", O_CREAT | O_RDWR, 0640), 0);
  EXPECT_EQ(dev.WriteChunked(0, data.data(), 4096), 4096);
  dev.SelectVolume("TestVolume", 4096);
  dev.hold_uploads = true;
  std::thread release([&dev, &waited_ok, &stored_when_done] {
    waited_ok = dev.Wait();
    stored_when_done = dev.Stored(0);
  });
  while (dev.uploads_started.load() < 1) { std::this_thread::sleep_for(10ms); }

  // The writer goes on: the same chunk in flight is not a failed upload.
  constexpr size_t block = 1024 * 1024;
  for (size_t done = 4096; done < data.size(); done += block) {
    const size_t count = std::min(block, data.size() - done);
    EXPECT_EQ(dev.WriteChunked(0, data.data() + done, count),
              static_cast<ssize_t>(count));
  }
  return release;
}

TEST(chunked_device_flush, ReleaseWaitDuringAKeepIsNeverWrittenEarly)
{
  auto jcr = std::make_shared<JobControlRecord>();
  SetJcrInThreadSpecificData(jcr.get());
  {
    FakeChunkedDevice dev{0, 0, 10};
    const auto data = Pattern(2 * kChunk);
    bool waited_ok = false;
    std::string stored_when_done;
    std::thread release
        = KeepTwoCopiesOfChunkZero(dev, data, waited_ok, stored_when_done);
    EXPECT_NE(jcr->getJobStatus(), JS_FatalError);
    EXPECT_EQ(dev.WriteChunked(0, data.data(), 0), 0);

    /* The copy uploads and its size is what the release wait expects, while
     * the kept full chunk is not due for a retry yet: the wait must go on. */
    dev.hold_uploads = false;
    while (dev.Stored(0).empty()) { std::this_thread::sleep_for(10ms); }
    std::this_thread::sleep_for(300ms);

    // Both jobs now wait for the kept full chunk.
    dev.SelectVolume("TestVolume", data.size());
    EXPECT_TRUE(dev.Wait()) << dev.reason;
    release.join();
    EXPECT_TRUE(waited_ok) << dev.reason;
    EXPECT_EQ(stored_when_done, std::string(data.data(), kChunk));
    EXPECT_EQ(dev.Stored(1), std::string(data.data() + kChunk, kChunk));
  }
  SetJcrInThreadSpecificData(nullptr);
}

TEST(chunked_device_flush, LargerCopyOfAKeptChunkWinsInEitherOrder)
{
  for (const bool smaller_last : {false, true}) {
    FakeChunkedDevice dev{0, 0, 10};
    const auto data = Pattern(2 * kChunk);
    bool waited_ok = false;
    std::string stored_when_done;
    dev.fail_uploads = true;
    std::thread release
        = KeepTwoCopiesOfChunkZero(dev, data, waited_ok, stored_when_done);
    dev.hold_uploads = false;
    release.join();
    EXPECT_FALSE(waited_ok);

    /* All retries are due (pauses are at most 2.5 s here) and the small copy
     * is tried first; it fails once when it must be stored last. */
    std::this_thread::sleep_for(3s);
    dev.fail_next_uploads = smaller_last ? 1 : 0;
    dev.fail_uploads = false;
    dev.SelectVolume("TestVolume", data.size());
    EXPECT_TRUE(dev.Wait()) << dev.reason;
    EXPECT_EQ(dev.Stored(0), std::string(data.data(), kChunk));
  }
}

TEST(chunked_device_flush, CancelWaitsForAtMostOneKeptChunkUpload)
{
  FakeChunkedDevice dev{0, 0, 10};
  const auto data = Pattern(2 * kChunk);
  bool waited_ok = false;
  std::string stored_when_done;
  dev.fail_uploads = true;
  std::thread release
      = KeepTwoCopiesOfChunkZero(dev, data, waited_ok, stored_when_done);
  dev.hold_uploads = false;
  release.join();

  // Every kept chunk is due; each try takes 1.5 s and fails.
  std::this_thread::sleep_for(3s);
  dev.upload_delay_ms = 1500;
  const auto start = SteadyClock::now();
  EXPECT_FALSE(
      dev.Wait([start] { return SteadyClock::now() - start > 100ms; }));
  EXPECT_LT(SteadyClock::now() - start, 2500ms);
  EXPECT_TRUE(Contains(dev.reason, "canceled")) << dev.reason;
  dev.upload_delay_ms = 0;
}

TEST(chunked_device_flush, ChunksNotUploadedNameQueuedAndKeptChunks)
{
  {
    FakeChunkedDevice dev{1, 0, 60};
    dev.hold_uploads = true;
    WriteVolume(dev, Pattern(2 * kChunk + 10));
    while (dev.uploads_started.load() < 1) {
      std::this_thread::sleep_for(10ms);
    }
    // Chunk 0 is with the io-thread; chunk 1 waits in the queue.
    const auto chunks = dev.ChunksNotUploaded();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].first, "TestVolume");
    EXPECT_EQ(chunks[0].second, 1);
    dev.hold_uploads = false;
  }
  {
    FakeChunkedDevice dev{0, 0, 60};
    FailFirstBlockingChunk(dev, Pattern(2 * kChunk));
    const auto chunks = dev.ChunksNotUploaded();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].first, "TestVolume");
    EXPECT_EQ(chunks[0].second, 0);
  }
}

/* An upload try of a kept chunk that a canceled job ends does not count as a
 * failure: the next try comes after a second, not after a longer pause. */
TEST(chunked_device_flush, CanceledTryOfAKeptChunkKeepsItsRetryPause)
{
  FakeChunkedDevice dev{0, 0, 60};
  const auto data = Pattern(2 * kChunk);
  FailFirstBlockingChunk(dev, data);

  dev.cancel_next_uploads = 1;
  dev.SetupChunk("TestVolume", O_RDONLY, 0);  // one try of every kept chunk
  EXPECT_EQ(dev.cancel_next_uploads.load(), 0);
  ASSERT_EQ(dev.ChunksNotUploaded().size(), 1u);

  dev.fail_uploads = false;
  const auto start = SteadyClock::now();
  EXPECT_TRUE(dev.Wait()) << dev.reason;
  EXPECT_LT(SteadyClock::now() - start, 5s);
  EXPECT_EQ(dev.Stored(0), std::string(data.data(), kChunk));
}
