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

/* A second job joins a dplcompat device that a first job is appending to
 * (Maximum Concurrent Jobs > 1): the mount closes and opens the device, reads
 * the volume label and moves to the end of the volume. The first job's bytes
 * must reach the store intact, also when it holds unflushed bytes of a chunk
 * after the first. */

#include "gtest/gtest.h"
#include "include/bareos.h"
#include "stored/stored.h"
#include "stored/stored_globals.h"
#include "stored/backends/dplcompat_device.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

namespace storagedaemon {

// Same name as the friend of DropletCompatibleDevice, for access to its parts.
class DplcompatLeaseTest : public ::testing::Test {
 public:
  static void MarkSetUp(DropletCompatibleDevice& dev)
  { dev.m_setup_succeeded = true; }
  static void Install(DropletCompatibleDevice& dev,
                      std::unique_ptr<ObjectStore> store)
  { dev.InstallStore(std::move(store)); }
  static void SetIoThreads(DropletCompatibleDevice& dev,
                           uint8_t threads,
                           uint8_t slots)
  {
    dev.io_threads_ = threads;
    dev.io_slots_ = slots;
  }
  static int Open(DropletCompatibleDevice& dev, int flags)
  { return dev.SetupChunk("Vol", flags, 0); }

 protected:
  void SetUp() override
  {
    dir_ = std::filesystem::temp_directory_path()
           / ("chunked_mcj_label." + std::to_string(getpid()));
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

  std::filesystem::path dir_;
  std::string path_;
  StorageResource storage_;
};

namespace {

constexpr size_t kChunkBytes = 10 * 1024 * 1024;  // the smallest chunk size
constexpr size_t kBlockBytes = 1024 * 1024;       // size of one block write
constexpr size_t kLabelBytes = 64;                // what the label read takes

// Objects of one volume in memory, named by chunk number ("0000").
class LockedStore : public ObjectStore {
 public:
  std::string Get(const std::string& part)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = chunks_.find(part);
    return it == chunks_.end() ? std::string{} : it->second;
  }
  bool Has(const std::string& part)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return chunks_.count(part) > 0;
  }
  size_t Count()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return chunks_.size();
  }

  tl::expected<BStringList, StoreError> get_supported_options() override
  { return BStringList{}; }
  tl::expected<void, StoreError> set_option(const std::string&,
                                            const std::string&) override
  { return {}; }
  tl::expected<void, StoreError> test_connection() override { return {}; }
  tl::expected<ObjectStat, StoreError> stat(std::string_view,
                                            std::string_view part) override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = chunks_.find(std::string(part));
    if (it == chunks_.end()) {
      return tl::unexpected(StoreError{StoreErrc::kNotFound, "no such chunk"});
    }
    return ObjectStat{it->second.size()};
  }
  tl::expected<ObjectList, StoreError> list(std::string_view) override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ObjectList result;
    for (const auto& [name, data] : chunks_) {
      result[name] = ObjectStat{data.size()};
    }
    return result;
  }
  tl::expected<void, StoreError> upload(std::string_view,
                                        std::string_view part,
                                        gsl::span<char> data) override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_[std::string(part)] = std::string(data.data(), data.size());
    return {};
  }
  tl::expected<gsl::span<char>, StoreError> download(
      std::string_view,
      std::string_view part,
      gsl::span<char> buffer,
      std::optional<ByteRange>) override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = chunks_.find(std::string(part));
    if (it == chunks_.end()) {
      return tl::unexpected(StoreError{StoreErrc::kNotFound, "no such chunk"});
    }
    std::memcpy(buffer.data(), it->second.data(), it->second.size());
    return buffer.first(it->second.size());
  }
  tl::expected<void, StoreError> remove(std::string_view,
                                        std::string_view) override
  { return {}; }

 private:
  std::mutex mutex_;
  std::map<std::string, std::string> chunks_;
};

// A device on a LockedStore, with the volume "Vol" mounted.
class McjDevice {
 public:
  explicit McjDevice(uint8_t io_threads)
  {
    dev.errmsg = GetPoolMemory(PM_EMSG);
    dev.errmsg[0] = 0;
    dev.prt_name = GetPoolMemory(PM_NAME);
    PmStrcpy(dev.prt_name, "\"mcj\" (memory)");
    bstrncpy(dev.VolCatInfo.VolCatName, "Vol",
             sizeof(dev.VolCatInfo.VolCatName));
    auto memory = std::make_unique<LockedStore>();
    store = memory.get();
    DplcompatLeaseTest::MarkSetUp(dev);
    DplcompatLeaseTest::Install(dev, std::move(memory));
    if (io_threads > 0) {
      DplcompatLeaseTest::SetIoThreads(dev, io_threads, 2);
    }
  }

  DropletCompatibleDevice dev;
  LockedStore* store;
};

// Bytes that differ from place to place, so a moved block shows up.
std::string Pattern(size_t size)
{
  std::string data(size, '\0');
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<char>((i * 131) + ((i >> 16) * 17) + (i >> 20));
  }
  return data;
}

std::string ChunkName(size_t number)
{
  char name[8];
  snprintf(name, sizeof(name), "%04zu", number);
  return name;
}

// Index of the first byte that differs, or -1.
int64_t FirstDifference(const std::string& got, const std::string& want)
{
  const size_t common = std::min(got.size(), want.size());
  for (size_t i = 0; i < common; ++i) {
    if (got[i] != want[i]) { return static_cast<int64_t>(i); }
  }
  return got.size() == want.size() ? -1 : static_cast<int64_t>(common);
}

// Writes data[from, to) in blocks, as the SD does.
void WriteRange(DropletCompatibleDevice& dev,
                const std::string& data,
                size_t from,
                size_t to)
{
  for (size_t done = from; done < to; done += kBlockBytes) {
    const size_t count = std::min(kBlockBytes, to - done);
    ASSERT_EQ(dev.d_write(0, data.data() + done, count),
              static_cast<ssize_t>(count))
        << "write at " << done;
  }
}

// Waits until the store holds the chunks before the one the job is filling.
void WaitUploaded(McjDevice& d, size_t chunks)
{
  for (int i = 0; i < 600; ++i) {
    bool all = true;
    for (size_t c = 0; c < chunks; ++c) {
      all = all && d.store->Has(ChunkName(c));
    }
    if (all) { return; }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

/* The first job writes the first bytes of the volume, then a second job joins.
 * Its mount closes the open device (MountNextWriteVolume), opens it again,
 * rewinds to read the label and moves to the end of the volume. The first job
 * then writes the rest and closes. The store must end with exactly the bytes
 * the first job wrote. */
void RunSecondJobJoin(uint8_t io_threads, size_t join_at, size_t total)
{
  McjDevice d(io_threads);
  const std::string data = Pattern(total);

  ASSERT_EQ(DplcompatLeaseTest::Open(d.dev, O_RDWR | O_CREAT), 0);
  WriteRange(d.dev, data, 0, join_at);
  WaitUploaded(d, join_at / kChunkBytes);

  // The second job's mount: close, open, rewind, read the label, go to the end.
  EXPECT_EQ(d.dev.d_close(0), 0);
  ASSERT_EQ(DplcompatLeaseTest::Open(d.dev, O_RDWR), 0);
  EXPECT_EQ(d.dev.d_lseek(nullptr, 0, SEEK_SET), 0);
  char label[kLabelBytes];
  EXPECT_EQ(d.dev.d_read(0, label, sizeof(label)),
            static_cast<ssize_t>(sizeof(label)));
  EXPECT_EQ(std::memcmp(label, data.data(), sizeof(label)), 0)
      << "the label is the first bytes of the volume";
  EXPECT_EQ(d.dev.d_lseek(nullptr, 0, SEEK_END),
            static_cast<boffset_t>(join_at))
      << "the end of the volume is where the first job stopped writing";

  // The first job goes on.
  WriteRange(d.dev, data, join_at, total);
  // As ReleaseDevice does: wait for the uploads first, then close.
  EXPECT_TRUE(d.dev.d_flush(nullptr));
  EXPECT_EQ(d.dev.d_close(0), 0);

  const size_t chunks = (total + kChunkBytes - 1) / kChunkBytes;
  EXPECT_EQ(d.store->Count(), chunks);
  for (size_t c = 0; c < chunks; ++c) {
    const size_t start = c * kChunkBytes;
    const std::string want
        = data.substr(start, std::min(kChunkBytes, total - start));
    const std::string got = d.store->Get(ChunkName(c));
    EXPECT_EQ(got.size(), want.size()) << "size of chunk " << c;
    EXPECT_EQ(FirstDifference(got, want), -1)
        << "chunk " << c << ": first differing byte";
  }
}

}  // namespace

// Control: the first job is still in chunk 0 when the second job joins.
TEST_F(DplcompatLeaseTest, SecondJobJoiningInChunkZeroKeepsTheData)
{ RunSecondJobJoin(0, kChunkBytes / 2, 2 * kChunkBytes + kChunkBytes / 2); }

// The first job is in chunk 1 with unflushed bytes when the second job joins.
TEST_F(DplcompatLeaseTest, SecondJobJoiningPastChunkZeroKeepsTheData)
{
  RunSecondJobJoin(0, kChunkBytes + kChunkBytes / 2,
                   2 * kChunkBytes + kChunkBytes / 2);
}

TEST_F(DplcompatLeaseTest, SecondJobJoiningPastChunkZeroKeepsTheDataIoThreads)
{
  RunSecondJobJoin(1, kChunkBytes + kChunkBytes / 2,
                   2 * kChunkBytes + kChunkBytes / 2);
}

}  // namespace storagedaemon
