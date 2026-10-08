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

/* Unit tests of the dplcompat device without a transport: the inflight lease
 * conflict of FlushRemoteChunk, and the abort check given to the store. */

#include "gtest/gtest.h"
#include "include/bareos.h"
#include "stored/stored.h"
#include "stored/stored_globals.h"
#include "lib/thread_specific_data.h"
#include "stored/backends/dplcompat_device.h"
#include "chunk_state_probe.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <fmt/format.h>
#include <map>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>

namespace storagedaemon {

class EodDevice;
void ExpectEndAfterAFullChunk(bool io_threads, bool close_first);
void ExpectFailedLoadThenReload(int whence);
void ExpectEndWithAChunkInTransit(size_t transit_size,
                                  boffset_t expected_end,
                                  const std::string& last_chunk,
                                  const std::string& expected_last);

class DplcompatLeaseTest : public ::testing::Test {
  friend class EodDevice;
  friend void ExpectEndAfterAFullChunk(bool io_threads, bool close_first);
  friend void ExpectEndWithAChunkInTransit(size_t transit_size,
                                           boffset_t expected_end,
                                           const std::string& last_chunk,
                                           const std::string& expected_last);
  friend void ExpectFailedLoadThenReload(int whence);

 protected:
  void SetUp() override
  {
    dir_ = std::filesystem::temp_directory_path()
           / ("dplcompat_lease." + std::to_string(getpid()));
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

  static bool Flush(DropletCompatibleDevice& dev, chunk_io_request& request)
  { return dev.FlushRemoteChunk(&request); }
  static bool Lease(DropletCompatibleDevice& dev, chunk_io_request& request)
  { return dev.SetInflightChunk(&request); }
  static void Install(DropletCompatibleDevice& dev,
                      std::unique_ptr<ObjectStore> store)
  { dev.InstallStore(std::move(store)); }
  static void Release(DropletCompatibleDevice& dev, chunk_io_request& request)
  { dev.ClearInflightChunk(&request); }
  static void MarkSetUp(DropletCompatibleDevice& dev)
  { dev.m_setup_succeeded = true; }
  static void UseIoThreads(DropletCompatibleDevice& dev)
  {
    dev.io_threads_ = 1;
    dev.io_slots_ = 2;
  }
  static int Open(DropletCompatibleDevice& dev, int flags)
  { return dev.SetupChunk("Vol", flags, 0); }
  static boffset_t Seek(DropletCompatibleDevice& dev,
                        boffset_t offset,
                        int whence)
  { return dev.d_lseek(nullptr, offset, whence); }

  std::filesystem::path dir_;
  std::string path_;
  StorageResource storage_;
};

TEST_F(DplcompatLeaseTest, HeldLeaseIsReportedOnTheRequestOnly)
{
  DropletCompatibleDevice dev;
  dev.errmsg = GetPoolMemory(PM_EMSG);
  dev.errmsg[0] = 0;
  dev.prt_name = GetPoolMemory(PM_NAME);
  PmStrcpy(dev.prt_name, "\"lease\" (memory)");
  dev.dev_errno = 0;

  char data[16] = "chunk data";
  chunk_io_request holder{};
  holder.volname = "LeaseVolume";
  holder.chunk = 3;
  ASSERT_TRUE(Lease(dev, holder));

  chunk_io_request request{};
  request.volname = "LeaseVolume";
  request.chunk = 3;
  request.buffer = data;
  request.wbuflen = sizeof(data);

  EXPECT_FALSE(Flush(dev, request));
  EXPECT_TRUE(request.lease_conflict);
  EXPECT_STREQ(dev.errmsg, "");
  EXPECT_EQ(dev.dev_errno, 0);

  Release(dev, holder);
}


namespace {

// Keeps the abort check the device hands to its transport.
class RecordingStore : public ObjectStore {
 public:
  std::function<bool()> abort_check;
  bool cancel_uploads{false};

  void set_abort_check(std::function<bool()> check) override
  { abort_check = std::move(check); }
  tl::expected<BStringList, StoreError> get_supported_options() override
  { return BStringList{}; }
  tl::expected<void, StoreError> set_option(const std::string&,
                                            const std::string&) override
  { return {}; }
  tl::expected<void, StoreError> test_connection() override { return {}; }
  tl::expected<ObjectStat, StoreError> stat(std::string_view,
                                            std::string_view) override
  { return ObjectStat{}; }
  tl::expected<ObjectList, StoreError> list(std::string_view) override
  { return ObjectList{}; }
  tl::expected<void, StoreError> upload(std::string_view,
                                        std::string_view,
                                        gsl::span<char>) override
  {
    if (cancel_uploads) {
      return tl::unexpected(StoreError{StoreErrc::kCanceled, "canceled"});
    }
    return {};
  }
  tl::expected<gsl::span<char>, StoreError> download(
      std::string_view,
      std::string_view,
      gsl::span<char> buffer,
      std::optional<ByteRange>) override
  { return buffer; }
  tl::expected<void, StoreError> remove(std::string_view,
                                        std::string_view) override
  { return {}; }
};

}  // namespace

TEST_F(DplcompatLeaseTest, InstalledStoreStopsWhenTheCallingJobIsCanceled)
{
  DropletCompatibleDevice dev;
  auto store = std::make_unique<RecordingStore>();
  RecordingStore* recorded = store.get();
  Install(dev, std::move(store));
  ASSERT_TRUE(recorded->abort_check);

  // An io-thread has no job: never aborts.
  EXPECT_FALSE(recorded->abort_check());

  auto running = std::make_shared<JobControlRecord>();
  SetJcrInThreadSpecificData(running.get());
  EXPECT_FALSE(recorded->abort_check());

  // A failed job still gets its kept-chunk tries.
  auto failed = std::make_shared<JobControlRecord>();
  failed->setJobStatus(JS_ErrorTerminated);
  SetJcrInThreadSpecificData(failed.get());
  EXPECT_FALSE(recorded->abort_check());
  failed->setJobStatus(JS_FatalError);
  EXPECT_FALSE(recorded->abort_check());

  auto canceled = std::make_shared<JobControlRecord>();
  canceled->setJobStatus(JS_Canceled);
  SetJcrInThreadSpecificData(canceled.get());
  EXPECT_TRUE(recorded->abort_check());

  // Another thread without the job is not told to stop.
  bool other_thread = true;
  std::thread([&] { other_thread = recorded->abort_check(); }).join();
  EXPECT_FALSE(other_thread);
  SetJcrInThreadSpecificData(nullptr);
}

TEST_F(DplcompatLeaseTest, CanceledUploadIsFlaggedOnTheRequest)
{
  DropletCompatibleDevice dev;
  dev.errmsg = GetPoolMemory(PM_EMSG);
  dev.errmsg[0] = 0;
  dev.prt_name = GetPoolMemory(PM_NAME);
  PmStrcpy(dev.prt_name, "\"cancel\" (memory)");
  auto store = std::make_unique<RecordingStore>();
  store->cancel_uploads = true;
  Install(dev, std::move(store));

  char data[16] = "chunk data";
  chunk_io_request request{};
  request.volname = "CancelVolume";
  request.chunk = 1;
  request.buffer = data;
  request.wbuflen = sizeof(data);

  EXPECT_FALSE(Flush(dev, request));
  EXPECT_TRUE(request.canceled);
  EXPECT_FALSE(request.lease_conflict);
  EXPECT_STREQ(dev.errmsg, "canceled");
}


namespace {

constexpr size_t kChunkBytes = 10 * 1024 * 1024;  // the smallest chunk size

// Objects of one volume in memory, named by chunk number ("0000").
class MemoryStore : public ObjectStore {
 public:
  std::mutex mutex;
  std::map<std::string, std::string> chunks;
  int uploads{0};
  std::atomic<int> upload_delay_ms{0};  // an upload takes this long
  std::atomic<bool> upload_started{false};
  bool missing_is_transient{false};  // like the program transport's stat
  int list_calls{0};
  int downloads{0};
  bool fail_downloads{false};
  int fail_list_from{0};  // the list call with this number (1-based) fails
  StoreErrc list_error{StoreErrc::kTransient};

  tl::expected<BStringList, StoreError> get_supported_options() override
  { return BStringList{}; }
  tl::expected<void, StoreError> set_option(const std::string&,
                                            const std::string&) override
  { return {}; }
  tl::expected<void, StoreError> test_connection() override { return {}; }
  tl::expected<ObjectStat, StoreError> stat(std::string_view,
                                            std::string_view part) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = chunks.find(std::string(part));
    if (it == chunks.end()) {
      return tl::unexpected(StoreError{
          missing_is_transient ? StoreErrc::kTransient : StoreErrc::kNotFound,
          "no such chunk"});
    }
    return ObjectStat{it->second.size()};
  }
  tl::expected<ObjectList, StoreError> list(std::string_view) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (++list_calls == fail_list_from) {
      return tl::unexpected(StoreError{list_error, "list failed"});
    }
    ObjectList result;
    for (const auto& [name, data] : chunks) {
      result[name] = ObjectStat{data.size()};
    }
    return result;
  }
  tl::expected<void, StoreError> upload(std::string_view,
                                        std::string_view part,
                                        gsl::span<char> data) override
  {
    upload_started = true;
    std::this_thread::sleep_for(std::chrono::milliseconds{upload_delay_ms});
    std::lock_guard<std::mutex> lock(mutex);
    ++uploads;
    chunks[std::string(part)] = std::string(data.data(), data.size());
    return {};
  }
  tl::expected<gsl::span<char>, StoreError> download(
      std::string_view,
      std::string_view part,
      gsl::span<char> buffer,
      std::optional<ByteRange>) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    ++downloads;
    if (fail_downloads) {
      return tl::unexpected(
          StoreError{StoreErrc::kTransient, "download failed"});
    }
    const std::string& data = chunks.at(std::string(part));
    std::memcpy(buffer.data(), data.data(), data.size());
    return buffer.first(data.size());
  }
  tl::expected<void, StoreError> remove(std::string_view,
                                        std::string_view part) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    chunks.erase(std::string(part));
    return {};
  }
};

std::string Chunk(char fill, size_t size = kChunkBytes)
{ return std::string(size, fill); }

std::string ChunkName(int number) { return fmt::format("{:04d}", number); }

}  // namespace

// A device on a MemoryStore, with the volume "Vol" mounted.
class EodDevice {
 public:
  EodDevice()
  {
    dev.errmsg = GetPoolMemory(PM_EMSG);
    dev.errmsg[0] = 0;
    dev.prt_name = GetPoolMemory(PM_NAME);
    PmStrcpy(dev.prt_name, "\"eod\" (memory)");
    bstrncpy(dev.VolCatInfo.VolCatName, "Vol",
             sizeof(dev.VolCatInfo.VolCatName));
    auto memory = std::make_unique<MemoryStore>();
    store = memory.get();
    DplcompatLeaseTest::MarkSetUp(dev);
    DplcompatLeaseTest::Install(dev, std::move(memory));
  }

  DropletCompatibleDevice dev;
  MemoryStore* store;
};

// A refused seek says EIO, keeps the position and drops the chunk: reads and
// writes fail and upload nothing until a seek loads a chunk again.
void ExpectRefusedSeek(EodDevice& d,
                       boffset_t offset,
                       int whence,
                       bool chunk_zero_exists = true)
{
  const ChunkState before = ChunkStateProbe::Capture(d.dev);
  errno = 0;
  EXPECT_EQ(d.dev.d_lseek(nullptr, offset, whence), -1);
  EXPECT_EQ(errno, EIO);
  const ChunkState after = ChunkStateProbe::Capture(d.dev);
  EXPECT_EQ(after.offset, before.offset);
  EXPECT_EQ(after.start_offset, -1);
  EXPECT_EQ(after.end_offset, -1);
  EXPECT_EQ(after.buflen, 0u);
  EXPECT_FALSE(after.chunk_setup);
  EXPECT_FALSE(after.need_flushing);
  EXPECT_EQ(after.writing, before.writing);
  EXPECT_EQ(after.opened, before.opened);

  // Nothing is read or written: a write must not restart at chunk 0.
  const int downloads = d.store->downloads;
  const int uploads = d.store->uploads;
  char buffer[16];
  errno = 0;
  EXPECT_EQ(d.dev.d_write(0, "x", 1), -1);
  EXPECT_EQ(errno, EIO);
  errno = 0;
  EXPECT_EQ(d.dev.d_read(0, buffer, sizeof(buffer)), -1);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(d.store->downloads, downloads);
  EXPECT_EQ(d.store->uploads, uploads);
  if (!chunk_zero_exists) { return; }

  // A seek that loads a chunk makes the device usable again: chunk 0 is read
  // from the store.
  EXPECT_EQ(d.dev.d_lseek(nullptr, 0, SEEK_SET), 0);
  EXPECT_EQ(d.dev.d_read(0, buffer, sizeof(buffer)),
            static_cast<ssize_t>(sizeof(buffer)));
  EXPECT_EQ(d.store->downloads, downloads + 1);
  EXPECT_EQ(std::string(buffer, sizeof(buffer)), std::string(16, 'a'));
}

// Opens for writing and moves to the end, as the daemon does to append.
TEST_F(DplcompatLeaseTest, EndOfAVolumeOfFullChunksIsTheNextChunk)
{
  for (int full : {1, 3}) {
    EodDevice d;
    for (int i = 0; i < full; ++i) {
      d.store->chunks[ChunkName(i)] = Chunk('a' + i);
    }
    ASSERT_EQ(Open(d.dev, O_RDWR), 0);
    ASSERT_EQ(Seek(d.dev, 0, SEEK_END),
              static_cast<boffset_t>(full * kChunkBytes));
    EXPECT_EQ(d.dev.d_write(0, "new bytes", 9), 9);
    EXPECT_EQ(d.dev.d_close(0), 0);

    EXPECT_EQ(d.store->chunks.at(ChunkName(full)), "new bytes");
    for (int i = 0; i < full; ++i) {
      EXPECT_EQ(d.store->chunks.at(ChunkName(i)), Chunk('a' + i));
    }
    EXPECT_EQ(d.store->chunks.size(), static_cast<size_t>(full) + 1);
  }
}

TEST_F(DplcompatLeaseTest, EndOfAVolumeWithAPartialLastChunkIsInThatChunk)
{
  EodDevice d;
  d.store->chunks["0000"] = Chunk('a');
  d.store->chunks["0001"] = Chunk('b', 1000);
  ASSERT_EQ(Open(d.dev, O_RDWR), 0);
  ASSERT_EQ(Seek(d.dev, 0, SEEK_END),
            static_cast<boffset_t>(kChunkBytes + 1000));
  EXPECT_EQ(d.dev.d_write(0, "tail", 4), 4);
  EXPECT_EQ(d.dev.d_close(0), 0);
  EXPECT_EQ(d.store->chunks.at("0001"), Chunk('b', 1000) + "tail");
  EXPECT_EQ(d.store->chunks.at("0000"), Chunk('a'));
}

TEST_F(DplcompatLeaseTest, EndWithAMissingChunkInTheListingIsNotAccepted)
{
  EodDevice d;
  // Chunk 0 is half full and chunk 2 exists: the sizes add up to one chunk,
  // but there is a hole.
  d.store->chunks["0000"] = Chunk('a', kChunkBytes / 2);
  d.store->chunks["0002"] = Chunk('c', kChunkBytes / 2);
  ASSERT_EQ(Open(d.dev, O_RDWR), 0);
  ExpectRefusedSeek(d, 0, SEEK_END);
  // A second try is refused as well: the first one left nothing behind.
  ExpectRefusedSeek(d, 0, SEEK_END);
  EXPECT_EQ(d.store->chunks.size(), 2u);
}

TEST_F(DplcompatLeaseTest, EndWithAShortChunkBeforeItIsNotAccepted)
{
  EodDevice d;
  d.store->chunks["0000"] = Chunk('a');
  d.store->chunks["0001"] = Chunk('b', kChunkBytes / 2);
  d.store->chunks["0003"] = Chunk('d', kChunkBytes / 2);
  ASSERT_EQ(Open(d.dev, O_RDWR), 0);
  ExpectRefusedSeek(d, 0, SEEK_END);
}

TEST_F(DplcompatLeaseTest, EndOfAnEmptyVolumeStillFails)
{
  EodDevice d;
  ASSERT_EQ(Open(d.dev, O_RDWR | O_CREAT), 0);
  ExpectRefusedSeek(d, 0, SEEK_END, false);
}

// A new volume has no chunk 0 yet: the rewind that precedes the label read
// is not a refused seek (the open already positioned on chunk 0), nothing is
// dropped, and the label can be written.
TEST_F(DplcompatLeaseTest, RewindOfAFreshVolumeIsNotRefused)
{
  EodDevice d;
  ASSERT_EQ(Open(d.dev, O_RDWR | O_CREAT), 0);
  errno = 0;
  EXPECT_EQ(d.dev.d_lseek(nullptr, 0, SEEK_SET), 0);
  char buffer[16];
  EXPECT_EQ(d.dev.d_read(0, buffer, sizeof(buffer)), 0);  // no label yet

  ASSERT_EQ(d.dev.d_lseek(nullptr, 0, SEEK_SET), 0);
  EXPECT_EQ(d.dev.d_write(0, "label", 5), 5);
  EXPECT_EQ(d.dev.d_close(0), 0);
  EXPECT_EQ(d.store->chunks.at("0000"), "label");
}

// After a truncate (a relabel) the chunk is empty and set up: the rewind and
// the label read are not refused and the read gives end of file.
TEST_F(DplcompatLeaseTest, RewindAfterATruncateIsNotRefused)
{
  EodDevice d;
  d.store->chunks["0000"] = Chunk('a');
  ASSERT_EQ(Open(d.dev, O_RDWR), 0);
  ASSERT_TRUE(d.dev.d_truncate(nullptr));
  EXPECT_TRUE(d.store->chunks.empty());

  EXPECT_EQ(d.dev.d_lseek(nullptr, 0, SEEK_SET), 0);
  char buffer[16];
  EXPECT_EQ(d.dev.d_read(0, buffer, sizeof(buffer)), 0);  // no label
  ASSERT_EQ(d.dev.d_lseek(nullptr, 0, SEEK_SET), 0);
  EXPECT_EQ(d.dev.d_write(0, "label", 5), 5);
  EXPECT_EQ(d.dev.d_close(0), 0);
  EXPECT_EQ(d.store->chunks.at("0000"), "label");
}

TEST_F(DplcompatLeaseTest, FailedOrCanceledListingIsNotAnEnd)
{
  for (StoreErrc code : {StoreErrc::kTransient, StoreErrc::kCanceled}) {
    EodDevice d;
    d.store->chunks["0000"] = Chunk('a');
    ASSERT_EQ(Open(d.dev, O_RDWR), 0);
    // The seek lists twice: for the size, then fresh after the failed load.
    d.store->list_calls = 0;
    d.store->fail_list_from = 2;
    d.store->list_error = code;
    ExpectRefusedSeek(d, 0, SEEK_END);
    EXPECT_EQ(d.store->chunks.size(), 1u);
  }
}

TEST_F(DplcompatLeaseTest, EndForAReaderOrWithAnOffsetIsNotAccepted)
{
  EodDevice reader;
  reader.store->chunks["0000"] = Chunk('a');
  ASSERT_EQ(Open(reader.dev, O_RDONLY), 0);
  ExpectRefusedSeek(reader, 0, SEEK_END);

  EodDevice writer;
  writer.store->chunks["0000"] = Chunk('a');
  ASSERT_EQ(Open(writer.dev, O_RDWR), 0);
  ExpectRefusedSeek(writer, 5, SEEK_END);

  // An explicit position in the missing chunk fails as before.
  EodDevice seeker;
  seeker.store->chunks["0000"] = Chunk('a');
  ASSERT_EQ(Open(seeker.dev, O_RDWR), 0);
  ExpectRefusedSeek(seeker, kChunkBytes, SEEK_SET);
}

// A seek into a chunk that cannot be loaded (a restore probing the volume)
// fails, and the reads that follow fail too: they must neither return 0 nor
// the bytes of the next chunk. A seek that works loads the chunk again.
void ExpectFailedLoadThenReload(int whence)
{
  EodDevice d;
  d.store->chunks["0000"] = Chunk('a');
  d.store->chunks["0001"] = Chunk('b');
  d.store->chunks["0002"] = Chunk('c');
  ASSERT_EQ(DplcompatLeaseTest::Open(d.dev, O_RDONLY), 0);
  ASSERT_EQ(d.dev.d_lseek(nullptr, 0, SEEK_SET), 0);

  d.store->fail_downloads = true;
  const boffset_t into_chunk_one = kChunkBytes + 5;
  errno = 0;
  EXPECT_EQ(d.dev.d_lseek(nullptr, into_chunk_one, whence), -1);
  EXPECT_EQ(errno, EIO);
  char buffer[16];
  memset(buffer, 'z', sizeof(buffer));
  errno = 0;
  EXPECT_EQ(d.dev.d_read(0, buffer, sizeof(buffer)), -1);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(std::string(buffer, sizeof(buffer)), std::string(16, 'z'));

  d.store->fail_downloads = false;
  const int downloads = d.store->downloads;
  EXPECT_EQ(d.dev.d_lseek(nullptr, kChunkBytes + 5, SEEK_SET),
            static_cast<boffset_t>(kChunkBytes + 5));
  EXPECT_EQ(d.dev.d_read(0, buffer, sizeof(buffer)),
            static_cast<ssize_t>(sizeof(buffer)));
  EXPECT_EQ(d.store->downloads, downloads + 1);
  EXPECT_EQ(std::string(buffer, sizeof(buffer)), std::string(16, 'b'));
}

TEST_F(DplcompatLeaseTest, SeekSetIntoAChunkThatFailsToLoadThenReloads)
{ ExpectFailedLoadThenReload(SEEK_SET); }

TEST_F(DplcompatLeaseTest, SeekCurIntoAChunkThatFailsToLoadThenReloads)
{ ExpectFailedLoadThenReload(SEEK_CUR); }

TEST_F(DplcompatLeaseTest, EndIsNotAcceptedWhileAnotherUploadHoldsTheNextChunk)
{
  EodDevice d;
  d.store->chunks["0000"] = Chunk('a');
  ASSERT_EQ(Open(d.dev, O_RDWR), 0);

  chunk_io_request holder{};
  holder.volname = "Vol";
  holder.chunk = 1;
  ASSERT_TRUE(Lease(d.dev, holder));
  ExpectRefusedSeek(d, 0, SEEK_END);
  ExpectRefusedSeek(d, 0, SEEK_END);
  Release(d.dev, holder);
  EXPECT_EQ(Seek(d.dev, 0, SEEK_END), static_cast<boffset_t>(kChunkBytes));
}

TEST_F(DplcompatLeaseTest, EndIgnoresObjectsThatAreNotChunks)
{
  EodDevice d;
  d.store->chunks["0000"] = Chunk('a');
  d.store->chunks["note"] = "not a chunk";
  d.store->chunks["00001"] = "not a chunk either";
  ASSERT_EQ(Open(d.dev, O_RDWR), 0);
  EXPECT_EQ(Seek(d.dev, 0, SEEK_END), static_cast<boffset_t>(kChunkBytes));
}

TEST_F(DplcompatLeaseTest, EndWorksWhenTheStoreNeverReportsNotFound)
{
  EodDevice d;
  d.store->missing_is_transient = true;
  d.store->chunks["0000"] = Chunk('a');
  ASSERT_EQ(Open(d.dev, O_RDWR), 0);
  EXPECT_EQ(Seek(d.dev, 0, SEEK_END), static_cast<boffset_t>(kChunkBytes));
}

// Waits until the store holds the chunk (an io-thread uploads it).
bool WaitForChunk(MemoryStore& store, const std::string& name)
{
  for (int i = 0; i < 100; ++i) {
    {
      std::lock_guard<std::mutex> lock(store.mutex);
      if (store.chunks.count(name) > 0) { return true; }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  return false;
}

// An exactly full chunk: the end of the volume is the start of the next chunk.
// With close_first the job closes the device and opens it again (as the mount
// of a second job does) before it moves to the end; the full chunk is then
// stored (with io-threads: waited for, as chunk 0 is what the open loads).
// Without it the full chunk is still unflushed and holds the end.
void ExpectEndAfterAFullChunk(bool io_threads, bool close_first)
{
  EodDevice d;
  if (io_threads) { DplcompatLeaseTest::UseIoThreads(d.dev); }
  ASSERT_EQ(DplcompatLeaseTest::Open(d.dev, O_RDWR | O_CREAT), 0);
  const std::string full = Chunk('f');
  ASSERT_EQ(d.dev.d_write(0, full.data(), full.size()),
            static_cast<ssize_t>(full.size()));
  if (close_first) {
    ASSERT_EQ(d.dev.d_close(0), 0);
    if (io_threads) { ASSERT_TRUE(WaitForChunk(*d.store, "0000")); }
    ASSERT_EQ(DplcompatLeaseTest::Open(d.dev, O_RDWR), 0);
  }

  EXPECT_EQ(DplcompatLeaseTest::Seek(d.dev, 0, SEEK_END),
            static_cast<boffset_t>(kChunkBytes));
  EXPECT_EQ(d.dev.d_write(0, "tail", 4), 4);
  EXPECT_EQ(d.dev.d_close(0), 0);
  // With io-threads the last upload may still be on its way.
  for (int i = 0; i < 100; ++i) {
    {
      std::lock_guard<std::mutex> lock(d.store->mutex);
      if (d.store->chunks.count("0001") > 0) { break; }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }

  std::lock_guard<std::mutex> lock(d.store->mutex);
  EXPECT_EQ(d.store->chunks.at("0000"), full);
  EXPECT_EQ(d.store->chunks.at("0001"), "tail");
}

TEST_F(DplcompatLeaseTest, EndAfterAFullChunkClosedAndOpenedAgain)
{ ExpectEndAfterAFullChunk(false, true); }

TEST_F(DplcompatLeaseTest, EndAfterAFullChunkClosedAndOpenedAgainWithThreads)
{ ExpectEndAfterAFullChunk(true, true); }

/* Two full chunks are still on their way to the store when the device is opened
 * again (the open loads chunk 0, which is stored): chunk 1 is uploading and
 * chunk 2 is queued. The move to the end waits for both uploads and then starts
 * the fourth chunk. */
TEST_F(DplcompatLeaseTest, EndAfterFullChunksStillQueuedWaitsForTheirUploads)
{
  EodDevice d;
  DplcompatLeaseTest::UseIoThreads(d.dev);
  d.store->chunks["0000"] = Chunk('a');
  ASSERT_EQ(DplcompatLeaseTest::Open(d.dev, O_RDWR), 0);
  ASSERT_EQ(DplcompatLeaseTest::Seek(d.dev, 0, SEEK_END),
            static_cast<boffset_t>(kChunkBytes));

  d.store->upload_delay_ms = 500;
  const std::string second = Chunk('b');
  const std::string third = Chunk('c');
  for (const std::string* chunk : {&second, &third}) {
    ASSERT_EQ(d.dev.d_write(0, chunk->data(), chunk->size()),
              static_cast<ssize_t>(chunk->size()));
  }
  ASSERT_EQ(d.dev.d_close(0), 0);
  // The io-thread holds chunk 1; chunk 2 waits in the queue behind it.
  for (int i = 0; i < 100 && !d.store->upload_started; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }
  ASSERT_TRUE(d.store->upload_started);
  ASSERT_EQ(DplcompatLeaseTest::Open(d.dev, O_RDWR), 0);

  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(DplcompatLeaseTest::Seek(d.dev, 0, SEEK_END),
            static_cast<boffset_t>(3 * kChunkBytes));
  EXPECT_GE(std::chrono::steady_clock::now() - start,
            std::chrono::milliseconds{400});
  EXPECT_EQ(d.dev.d_write(0, "tail", 4), 4);
  EXPECT_EQ(d.dev.d_close(0), 0);
  ASSERT_TRUE(WaitForChunk(*d.store, "0003"));

  std::lock_guard<std::mutex> lock(d.store->mutex);
  EXPECT_EQ(d.store->chunks.at("0000"), Chunk('a'));
  EXPECT_EQ(d.store->chunks.at("0001"), second);
  EXPECT_EQ(d.store->chunks.at("0002"), third);
  EXPECT_EQ(d.store->chunks.at("0003"), "tail");
}

/* Chunk 1 is in transit: counted as pending, but neither queued, uploading nor
 * stored (an io-thread between its dequeue and its lease), so the first size
 * of the volume misses it. The move to the end waits for it, sizes the volume
 * again and ends where the chunk ends. */
void ExpectEndWithAChunkInTransit(size_t transit_size,
                                  boffset_t expected_end,
                                  const std::string& last_chunk,
                                  const std::string& expected_last)
{
  EodDevice d;
  DplcompatLeaseTest::UseIoThreads(d.dev);
  d.store->chunks["0000"] = Chunk('a');
  ASSERT_EQ(DplcompatLeaseTest::Open(d.dev, O_RDWR), 0);
  ChunkStateProbe::CountPending(d.dev, "Vol", 1);

  std::atomic<boffset_t> end{-2};
  const auto seek = &DplcompatLeaseTest::Seek;  // named here: friend access
  std::thread seeker([&] { end = seek(d.dev, 0, SEEK_END); });
  std::this_thread::sleep_for(std::chrono::milliseconds{500});
  EXPECT_EQ(end.load(), -2) << "the move to the end waits for the chunk";
  {
    std::lock_guard<std::mutex> lock(d.store->mutex);
    d.store->chunks["0001"] = Chunk('b', transit_size);
  }
  ChunkStateProbe::CountPending(d.dev, "Vol", -1);
  seeker.join();
  EXPECT_EQ(end.load(), expected_end);

  EXPECT_EQ(d.dev.d_write(0, "tail", 4), 4);
  EXPECT_EQ(d.dev.d_close(0), 0);
  for (int i = 0; i < 100; ++i) {
    {
      std::lock_guard<std::mutex> lock(d.store->mutex);
      auto it = d.store->chunks.find(last_chunk);
      if (it != d.store->chunks.end() && it->second == expected_last) { break; }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  std::lock_guard<std::mutex> lock(d.store->mutex);
  EXPECT_EQ(d.store->chunks.at("0000"), Chunk('a'));
  EXPECT_EQ(d.store->chunks.at(last_chunk), expected_last);
}

// The chunk in transit is full: the end is the start of the next chunk.
TEST_F(DplcompatLeaseTest, EndWithAFullChunkInTransitIsAcceptedAfterTheWait)
{ ExpectEndWithAChunkInTransit(kChunkBytes, 2 * kChunkBytes, "0002", "tail"); }

// The chunk in transit is partial: the end is inside it, after its data.
TEST_F(DplcompatLeaseTest, EndWithAPartialChunkInTransitIsLoadedAfterTheWait)
{
  const size_t part = kChunkBytes / 2;
  ExpectEndWithAChunkInTransit(part, static_cast<boffset_t>(kChunkBytes + part),
                               "0001", Chunk('b', part) + "tail");
}

TEST_F(DplcompatLeaseTest, EndAfterAFullUnflushedChunkIsItsOwnEnd)
{ ExpectEndAfterAFullChunk(false, false); }

TEST_F(DplcompatLeaseTest, EndAfterAFullUnflushedChunkIsItsOwnEndWithThreads)
{ ExpectEndAfterAFullChunk(true, false); }

}  // namespace storagedaemon
