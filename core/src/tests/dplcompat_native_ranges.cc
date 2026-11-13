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

#include "include/bareos.h"
#include "include/jcr.h"
#include "gtest/gtest.h"
#include "lib/thread_specific_data.h"
#include "stored/backends/dplcompat_device.h"
#include "stored/backends/hard_stop.h"
#include "stored/block.h"
#include "stored/stored_conf.h"
#include "stored/stored_globals.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace storagedaemon {
namespace {

constexpr size_t kChunkBytes = DEFAULT_CHUNK_SIZE;
constexpr size_t kBlockBytes = DEFAULT_BLOCK_SIZE;
constexpr char kPoison = '~';

// No expected byte is the poison used for unfilled download/caller buffers.
std::string Pattern(size_t size)
{
  std::string bytes(size, '\0');
  for (size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<char>(1 + (i * 37 + i / 257) % 125);
  }
  return bytes;
}

struct Download {
  std::string part;
  std::optional<ByteRange> range;
  bool worker{};  // a read-ahead worker, not the job thread
};

// A synchronous store records externally visible I/O, not device cache state.
class RangeStore : public ObjectStore {
 public:
  std::map<std::string, std::string> objects;
  std::vector<std::string> heads;
  std::vector<Download> downloads;
  size_t uploads{};
  bool ranges{true};
  bool short_next{};
  bool fail_listing{};
  std::optional<StoreError> head_error;
  std::string head_error_part;
  std::optional<StoreError> get_error;

  // A store that goes away under a running worker is a lifetime bug.
  ~RangeStore() override
  {
    if (shared_active->load() != 0) {
      ADD_FAILURE() << "the store was destroyed while a read-ahead worker ran";
    }
  }

  tl::expected<BStringList, StoreError> get_supported_options() override
  { return BStringList{}; }
  tl::expected<void, StoreError> set_option(const std::string&,
                                            const std::string&) override
  { return {}; }
  tl::expected<void, StoreError> test_connection() override { return {}; }
  bool supports_range_download() const override { return ranges; }

  tl::expected<ObjectStat, StoreError> stat(std::string_view,
                                            std::string_view part) override
  {
    std::lock_guard<std::mutex> lock(objects_mutex_);
    heads.emplace_back(part);
    if (head_error && (head_error_part.empty() || head_error_part == part)) {
      return tl::unexpected(*head_error);
    }
    auto found = objects.find(std::string(part));
    if (found == objects.end()) {
      return tl::unexpected(StoreError{StoreErrc::kNotFound, "missing chunk"});
    }
    return ObjectStat{found->second.size()};
  }

  tl::expected<ObjectList, StoreError> list(std::string_view) override
  {
    std::lock_guard<std::mutex> lock(objects_mutex_);
    if (fail_listing) {
      return tl::unexpected(
          StoreError{StoreErrc::kPermanent, "listing denied"});
    }
    ObjectList result;
    for (const auto& [part, bytes] : objects) {
      result.emplace(part, ObjectStat{bytes.size()});
    }
    return result;
  }

  // While held, uploads wait: a chunk handed to an io-thread stays queued.
  void HoldUploads(bool hold)
  {
    {
      std::lock_guard<std::mutex> lock(gate_mutex_);
      hold_uploads_ = hold;
    }
    gate_.notify_all();
  }

  // Waits until io-threads have stored the given number of objects.
  bool WaitForObjects(size_t count)
  {
    for (int i = 0; i < 200; ++i) {
      {
        std::lock_guard<std::mutex> lock(objects_mutex_);
        if (objects.size() >= count) { return true; }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return false;
  }

  tl::expected<void, StoreError> upload(std::string_view,
                                        std::string_view part,
                                        gsl::span<char> bytes) override
  {
    {
      std::unique_lock<std::mutex> lock(gate_mutex_);
      gate_.wait(lock, [this] { return !hold_uploads_; });
    }
    std::lock_guard<std::mutex> lock(objects_mutex_);
    ++uploads;
    objects[std::string(part)] = std::string(bytes.data(), bytes.size());
    return {};
  }

  void set_abort_check(std::function<bool()> check) override
  { abort_check_ = std::move(check); }

  // While held, a GET of the part waits; a hard stop or a cancel ends it.
  void HoldPart(const std::string& part, bool hold)
  {
    {
      std::lock_guard<std::mutex> lock(gate_mutex_);
      if (hold) {
        held_parts_.insert(part);
      } else {
        held_parts_.erase(part);
      }
    }
    gate_.notify_all();
  }

  // The next whole-object GETs of the part by a worker thread fail.
  void FailWorkerGets(const std::string& part, int count)
  {
    std::lock_guard<std::mutex> lock(objects_mutex_);
    fail_worker_[part] = count;
  }

  // Lets every held GET and upload go.
  void ReleaseAll()
  {
    {
      std::lock_guard<std::mutex> lock(gate_mutex_);
      held_parts_.clear();
      hold_uploads_ = false;
    }
    gate_.notify_all();
  }

  size_t HeadCount()
  {
    std::lock_guard<std::mutex> lock(objects_mutex_);
    return heads.size();
  }

  std::vector<Download> DownloadsSnapshot()
  {
    std::lock_guard<std::mutex> lock(objects_mutex_);
    return downloads;
  }

  // Parts fetched by worker threads, in the order they finished.
  std::vector<std::string> WorkerFinished()
  {
    std::lock_guard<std::mutex> lock(objects_mutex_);
    return worker_finished_;
  }

  tl::expected<gsl::span<char>, StoreError> download(
      std::string_view,
      std::string_view part,
      gsl::span<char> buffer,
      std::optional<ByteRange> range) override
  {
    const bool worker = std::this_thread::get_id() != owner_;
    struct WorkerCount {
      RangeStore* store;
      bool counted;
      ~WorkerCount()
      {
        if (counted) { --store->active_workers; }
      }
    } worker_count{this, worker};
    if (worker) {
      const int now = ++active_workers;
      int seen = max_workers.load();
      while (now > seen && !max_workers.compare_exchange_weak(seen, now)) {}
    }
    {
      std::lock_guard<std::mutex> lock(objects_mutex_);
      downloads.push_back({std::string(part), range, worker});
    }
    {
      std::unique_lock<std::mutex> lock(gate_mutex_);
      while (held_parts_.count(std::string(part)) > 0) {
        if ((tls_hard_stop && tls_hard_stop->load())
            || (abort_check_ && abort_check_())) {
          return tl::unexpected(StoreError{StoreErrc::kCanceled, "canceled"});
        }
        gate_.wait_for(lock, std::chrono::milliseconds{10});
      }
    }
    std::lock_guard<std::mutex> lock(objects_mutex_);
    if (worker && !range) {
      auto fail = fail_worker_.find(std::string(part));
      if (fail != fail_worker_.end() && fail->second > 0) {
        --fail->second;
        return tl::unexpected(StoreError{StoreErrc::kTransient, "injected"});
      }
    }
    const size_t poison_size
        = range ? std::min(range->length, buffer.size()) : buffer.size();
    std::fill_n(buffer.data(), poison_size, kPoison);
    if (get_error) { return tl::unexpected(*get_error); }
    auto found = objects.find(std::string(part));
    if (found == objects.end()) {
      return tl::unexpected(StoreError{StoreErrc::kNotFound, "missing GET"});
    }
    const size_t offset = range ? range->offset : 0;
    size_t length = range ? range->length : found->second.size();
    if (offset > found->second.size() || length > found->second.size() - offset
        || length > buffer.size()) {
      return tl::unexpected(StoreError{StoreErrc::kPermanent, "invalid GET"});
    }
    if (short_next && length > 0) {
      --length;
      short_next = false;
    }
    std::memcpy(buffer.data(), found->second.data() + offset, length);
    if (worker) { worker_finished_.emplace_back(part); }
    return buffer.first(length);
  }

  // The counter outlives the store, for a test that destroys the device.
  std::shared_ptr<std::atomic<int>> shared_active{
      std::make_shared<std::atomic<int>>(0)};
  std::atomic<int>& active_workers{*shared_active};
  std::atomic<int> max_workers{0};

  tl::expected<void, StoreError> remove(std::string_view,
                                        std::string_view part) override
  {
    std::lock_guard<std::mutex> lock(objects_mutex_);
    objects.erase(std::string(part));
    return {};
  }

 private:
  std::thread::id owner_{std::this_thread::get_id()};
  std::function<bool()> abort_check_;
  std::mutex gate_mutex_;
  std::condition_variable gate_;
  bool hold_uploads_{};
  std::set<std::string> held_parts_;
  std::mutex objects_mutex_;
  std::map<std::string, int> fail_worker_;
  std::vector<std::string> worker_finished_;
};

}  // namespace

// The existing installer friend is defined separately in each test binary.
class DplcompatLeaseTest : public ::testing::Test {
 protected:
  void SetUp() override
  {
    previous_jcr_ = GetJcrFromThreadSpecificData();
    previous_storage_ = me;
    me = &storage_;
    directory_
        = std::filesystem::temp_directory_path()
          / ("dplcompat_ranges." + std::to_string(getpid()) + "."
             + std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    owns_directory_ = std::filesystem::create_directory(directory_);
    ASSERT_TRUE(owns_directory_);
    working_path_ = directory_.string();
    storage_.working_directory = working_path_.data();
    job_ = std::make_unique<JobControlRecord>();
    job_->setJobType(JT_BACKUP);
    job_->setJobStatusWithPriorityCheck(JS_Running);
    SetJcrInThreadSpecificData(job_.get());
    dev_->errmsg = GetPoolMemory(PM_EMSG);
    dev_->errmsg[0] = 0;
    dev_->prt_name = GetPoolMemory(PM_NAME);
    PmStrcpy(dev_->prt_name, "\"ranges\" (memory)");
    bstrncpy(dev_->VolCatInfo.VolCatName, "RangeVolume",
             sizeof(dev_->VolCatInfo.VolCatName));
    auto store = std::make_unique<RangeStore>();
    store_ = store.get();
    Install(std::move(store));
  }

  void TearDown() override
  {
    if (store_) { store_->ReleaseAll(); }
    if (opened_ && dev_) { dev_->d_close(0); }
    SetJcrInThreadSpecificData(previous_jcr_);
    job_.reset();
    me = previous_storage_;
    storage_.working_directory = nullptr;
    if (owns_directory_) { std::filesystem::remove_all(directory_); }
  }

  void Install(std::unique_ptr<ObjectStore> store)
  {
    dev_->m_setup_succeeded = true;
    dev_->InstallStore(std::move(store));
  }

  // The test bodies are not friends of the device: the fixture sets this.
  static void UseIoThreads(DropletCompatibleDevice& dev)
  {
    dev.io_threads_ = 1;
    dev.io_slots_ = 3;
  }

  // The test bodies are not friends of the device: the fixture asks.
  static bool NeedsFlushing(DropletCompatibleDevice& dev)
  { return dev.CurrentChunkNeedsFlushing(); }

  int Open(int flags)
  {
    const int result = dev_->d_open("memory/RangeVolume", flags, 0640);
    opened_ = result == 0;
    return result;
  }

  // Chunks 0..full-1 are full, a tail chunk of tail bytes may follow.
  std::string MakeVolume(size_t full, size_t tail)
  {
    std::string data = Pattern(full * kChunkBytes + tail);
    for (size_t chunk = 0; chunk * kChunkBytes < data.size(); ++chunk) {
      char name[16];
      snprintf(name, sizeof(name), "%04zu", chunk);
      store_->objects[name] = data.substr(chunk * kChunkBytes, kChunkBytes);
    }
    dev_->VolCatInfo.VolCatBytes = data.size();
    return data;
  }

  // Reads [from, from + length) sequentially in blocks and compares each.
  void ReadBlocks(const std::string& expected, size_t from, size_t length)
  {
    std::string block(kBlockBytes, kPoison);
    for (size_t done = 0; done < length;) {
      const size_t want = std::min(kBlockBytes, length - done);
      const ssize_t got = dev_->d_read(0, block.data(), want);
      ASSERT_EQ(got, static_cast<ssize_t>(want)) << "read at " << from + done;
      ASSERT_EQ(std::memcmp(block.data(), expected.data() + from + done, want),
                0)
          << "data at " << from + done;
      done += want;
    }
  }

  template <typename Predicate> static bool WaitFor(Predicate predicate)
  {
    for (int i = 0; i < 500; ++i) {
      if (predicate()) { return true; }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
  }

  void Read(const std::string& expected, size_t offset, size_t length)
  {
    std::string output(length, kPoison);
    ASSERT_EQ(dev_->d_read(0, output.data(), output.size()),
              static_cast<ssize_t>(length));
    EXPECT_EQ(std::memcmp(output.data(), expected.data() + offset, length), 0)
        << "read at " << offset;
  }

  std::unique_ptr<DropletCompatibleDevice> dev_{
      std::make_unique<DropletCompatibleDevice>()};
  RangeStore* store_{};
  StorageResource storage_;
  StorageResource* previous_storage_{};
  JobControlRecord* previous_jcr_{};
  std::unique_ptr<JobControlRecord> job_;
  bool opened_{};
  std::filesystem::path directory_;
  std::string working_path_;
  bool owns_directory_{};
};

TEST_F(DplcompatLeaseTest, MetadataOnlyOpenForBothAccessModes)
{
  store_->objects["0000"] = Pattern(4 * kBlockBytes);
  for (int flags : {O_RDONLY, O_RDWR}) {
    ASSERT_EQ(Open(flags), 0);
    EXPECT_TRUE(store_->downloads.empty());
    EXPECT_EQ(dev_->d_close(0), 0);
    EXPECT_EQ(store_->uploads, 0);
  }
  EXPECT_EQ(store_->HeadCount(), 2);
}

TEST_F(DplcompatLeaseTest, FirstLabelRangeUsesDefaultBlockSize)
{
  const std::string data = Pattern(4 * kBlockBytes);
  store_->objects["0000"] = data;
  ASSERT_EQ(Open(O_RDONLY), 0);
  Read(data, 0, 64);
  ASSERT_EQ(store_->downloads.size(), 1);
  ASSERT_TRUE(store_->downloads[0].range);
  EXPECT_EQ(store_->downloads[0].range->offset, 0);
  EXPECT_EQ(store_->downloads[0].range->length, kBlockBytes);
  EXPECT_EQ(store_->HeadCount(), 1);
}

TEST_F(DplcompatLeaseTest, ConfiguredBlockSizeBoundsFirstRange)
{
  const std::string data = Pattern(5 * kBlockBytes);
  store_->objects["0000"] = data;
  dev_->max_block_size = 2 * kBlockBytes;
  ASSERT_EQ(Open(O_RDONLY), 0);
  Read(data, 0, 64);
  ASSERT_EQ(store_->downloads.size(), 1);
  ASSERT_TRUE(store_->downloads[0].range);
  EXPECT_EQ(store_->downloads[0].range->length, 2 * kBlockBytes);
}

TEST_F(DplcompatLeaseTest, ZeroCountAndEmptyObjectsDoNotDownload)
{
  for (size_t size : {size_t{0}, 4 * kBlockBytes}) {
    store_->objects["0000"] = Pattern(size);
    ASSERT_EQ(Open(O_RDONLY), 0);
    char output = kPoison;
    EXPECT_EQ(dev_->d_read(0, &output, 0), 0);
    if (size == 0) { EXPECT_EQ(dev_->d_read(0, &output, 1), 0); }
    EXPECT_EQ(output, kPoison);
    EXPECT_TRUE(store_->downloads.empty());
    EXPECT_EQ(dev_->d_close(0), 0);
  }
}

TEST_F(DplcompatLeaseTest, SequentialReadingFetchesOneSuffix)
{
  const std::string data = Pattern(3 * kBlockBytes + 31);
  store_->objects["0000"] = data;
  ASSERT_EQ(Open(O_RDONLY), 0);
  Read(data, 0, 64);
  Read(data, 64, kBlockBytes - 64);
  ASSERT_EQ(store_->downloads.size(), 1);
  Read(data, kBlockBytes, 64);
  Read(data, kBlockBytes + 64, data.size() - kBlockBytes - 64);
  ASSERT_EQ(store_->downloads.size(), 2);
  ASSERT_TRUE(store_->downloads[1].range);
  EXPECT_EQ(store_->downloads[1].range->offset, kBlockBytes);
  EXPECT_EQ(store_->downloads[1].range->length, data.size() - kBlockBytes);
  EXPECT_EQ(store_->HeadCount(), 1);
}

TEST_F(DplcompatLeaseTest, LateSameChunkSeekReadsOnlyItsRange)
{
  const std::string data = Pattern(kChunkBytes);
  store_->objects["0000"] = data;
  const size_t target = kChunkBytes - 2 * kBlockBytes - 17;
  ASSERT_EQ(Open(O_RDONLY), 0);
  ASSERT_EQ(dev_->d_lseek(nullptr, target, SEEK_SET),
            static_cast<boffset_t>(target));
  EXPECT_TRUE(store_->downloads.empty());
  Read(data, target, 256);
  ASSERT_EQ(store_->downloads.size(), 1);
  ASSERT_TRUE(store_->downloads[0].range);
  EXPECT_EQ(store_->downloads[0].range->offset, target);
  EXPECT_EQ(store_->downloads[0].range->length, kBlockBytes);
  EXPECT_EQ(store_->HeadCount(), 1);
}

TEST_F(DplcompatLeaseTest, RandomSeeksReplaceOneWindowAndRewindReusesLabel)
{
  const std::string data = Pattern(8 * kBlockBytes);
  store_->objects["0000"] = data;
  const size_t target = 4 * kBlockBytes + 13;
  ASSERT_EQ(Open(O_RDONLY), 0);
  Read(data, 0, 64);
  ASSERT_EQ(dev_->d_lseek(nullptr, target, SEEK_SET),
            static_cast<boffset_t>(target));
  Read(data, target, 128);
  ASSERT_EQ(dev_->d_lseek(nullptr, 0, SEEK_SET), 0);
  Read(data, 0, 64);
  ASSERT_EQ(store_->downloads.size(), 3);
  ASSERT_TRUE(store_->downloads[1].range);
  EXPECT_EQ(store_->downloads[1].range->offset, target);
  ASSERT_TRUE(store_->downloads[2].range);
  EXPECT_EQ(store_->downloads[2].range->offset, 0);
  ASSERT_EQ(dev_->d_lseek(nullptr, 0, SEEK_SET), 0);
  Read(data, 0, 64);
  EXPECT_EQ(store_->downloads.size(), 3);
  EXPECT_EQ(store_->HeadCount(), 1);
}

TEST_F(DplcompatLeaseTest, ShortRangeInvalidatesUntilSeekRecovery)
{
  const std::string data = Pattern(3 * kBlockBytes);
  store_->objects["0000"] = data;
  ASSERT_EQ(Open(O_RDONLY), 0);
  store_->short_next = true;
  std::array<char, 64> output;
  output.fill(kPoison);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
  EXPECT_EQ(dev_->dev_errno, EIO);
  EXPECT_EQ(job_->getJobStatus(), JS_FatalError);
  EXPECT_TRUE(std::all_of(output.begin(), output.end(),
                          [](char byte) { return byte == kPoison; }));
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
  EXPECT_EQ(store_->downloads.size(), 1);
  ASSERT_EQ(dev_->d_lseek(nullptr, 0, SEEK_SET), 0);
  Read(data, 0, output.size());
  EXPECT_EQ(store_->downloads.size(), 2);
}

TEST_F(DplcompatLeaseTest, DeniedRangeInvalidatesWithoutReturningBytes)
{
  store_->objects["0000"] = Pattern(3 * kBlockBytes);
  ASSERT_EQ(Open(O_RDONLY), 0);
  store_->get_error = StoreError{StoreErrc::kPermanent, "GET denied"};
  std::array<char, 64> output;
  output.fill(kPoison);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
  EXPECT_EQ(job_->getJobStatus(), JS_FatalError);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
  EXPECT_EQ(store_->downloads.size(), 1);
  EXPECT_TRUE(std::all_of(output.begin(), output.end(),
                          [](char byte) { return byte == kPoison; }));
}

TEST_F(DplcompatLeaseTest, MissingRangeAfterSuccessfulMetadataIsNotEof)
{
  store_->objects["0000"] = Pattern(3 * kBlockBytes);
  ASSERT_EQ(Open(O_RDONLY), 0);
  store_->get_error = StoreError{StoreErrc::kNotFound, "GET missing"};
  std::array<char, 64> output;
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
  EXPECT_EQ(dev_->dev_errno, EIO);
  EXPECT_EQ(job_->getJobStatus(), JS_FatalError);
  EXPECT_EQ(store_->uploads, 0);
}

TEST_F(DplcompatLeaseTest, MissingNextChunkAtCatalogEndIsEof)
{
  const std::string data = Pattern(kChunkBytes);
  store_->objects["0000"] = data;
  dev_->VolCatInfo.VolCatBytes = data.size();
  ASSERT_EQ(Open(O_RDONLY), 0);
  std::string output(kChunkBytes + 64, kPoison);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()),
            static_cast<ssize_t>(data.size()));
  EXPECT_EQ(std::memcmp(output.data(), data.data(), data.size()), 0);
  EXPECT_EQ(dev_->d_read(0, output.data(), 64), 0);
  EXPECT_NE(job_->getJobStatus(), JS_FatalError);
}

TEST_F(DplcompatLeaseTest, MissingNextChunkBeforeCatalogEndIsAnError)
{
  store_->objects["0000"] = Pattern(kChunkBytes);
  dev_->VolCatInfo.VolCatBytes = kChunkBytes + 64;
  ASSERT_EQ(Open(O_RDONLY), 0);
  std::string output(kChunkBytes + 64, kPoison);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
  EXPECT_EQ(dev_->dev_errno, EIO);
  EXPECT_EQ(dev_->d_read(0, output.data(), 64), -1);
}

TEST_F(DplcompatLeaseTest, PresentUnreadableNextChunkIsNotEof)
{
  store_->objects["0000"] = Pattern(kChunkBytes);
  store_->objects["0001"] = "present but denied";
  store_->head_error = StoreError{StoreErrc::kPermanent, "HEAD denied"};
  store_->head_error_part = "0001";
  dev_->VolCatInfo.VolCatBytes = kChunkBytes;
  ASSERT_EQ(Open(O_RDONLY), 0);
  std::string output(kChunkBytes + 64, kPoison);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
  EXPECT_EQ(dev_->dev_errno, EIO);
  EXPECT_EQ(dev_->d_read(0, output.data(), 64), -1);
}

TEST_F(DplcompatLeaseTest, UnknownListingCannotDeclareEof)
{
  store_->objects["0000"] = Pattern(kChunkBytes);
  store_->fail_listing = true;
  dev_->VolCatInfo.VolCatBytes = kChunkBytes;
  ASSERT_EQ(Open(O_RDONLY), 0);
  std::string output(kChunkBytes + 64, kPoison);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
  EXPECT_EQ(dev_->dev_errno, EIO);
}

TEST_F(DplcompatLeaseTest, ReadsAcrossBoundaryAndShortTail)
{
  const std::string first = Pattern(kChunkBytes);
  const std::string tail(4096, '\x7f');
  store_->objects["0000"] = first;
  store_->objects["0001"] = tail;
  dev_->VolCatInfo.VolCatBytes = first.size() + tail.size();
  ASSERT_EQ(Open(O_RDONLY), 0);
  ASSERT_EQ(dev_->d_lseek(nullptr, kChunkBytes - 32, SEEK_SET),
            static_cast<boffset_t>(kChunkBytes - 32));
  std::string output(kBlockBytes, kPoison);
  const std::string expected = first.substr(first.size() - 32) + tail;
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()),
            static_cast<ssize_t>(expected.size()));
  EXPECT_EQ(std::memcmp(output.data(), expected.data(), expected.size()), 0);
  EXPECT_EQ(output[expected.size()], kPoison);
  EXPECT_EQ(dev_->d_read(0, output.data(), 1), 0);
}

TEST_F(DplcompatLeaseTest, FullSequentialChunkUsesOneFullGetForTheNextChunk)
{
  const std::string first = Pattern(kChunkBytes);
  const std::string second(kChunkBytes, '\x7f');
  store_->objects["0000"] = first;
  store_->objects["0001"] = second;
  dev_->VolCatInfo.VolCatBytes = first.size() + second.size();
  ASSERT_EQ(Open(O_RDONLY), 0);
  Read(first, 0, 64);
  Read(first, 64, kBlockBytes - 64);
  Read(first, kBlockBytes, first.size() - kBlockBytes);
  ASSERT_EQ(store_->downloads.size(), 2);
  Read(second, 0, 64);
  Read(second, 64, 4096);
  ASSERT_EQ(store_->downloads.size(), 3);
  EXPECT_EQ(store_->downloads.back().part, "0001");
  EXPECT_FALSE(store_->downloads.back().range)
      << "established sequential reading should not add a range GET per chunk";
  EXPECT_EQ(store_->HeadCount(), 2);
}

TEST_F(DplcompatLeaseTest, PartialBoundaryKeepsNextChunkRanged)
{
  const std::string first = Pattern(kChunkBytes);
  const std::string second(kChunkBytes, '\x7f');
  store_->objects["0000"] = first;
  store_->objects["0001"] = second;
  dev_->VolCatInfo.VolCatBytes = first.size() + second.size();
  ASSERT_EQ(Open(O_RDONLY), 0);
  ASSERT_EQ(dev_->d_lseek(nullptr, kChunkBytes - 32, SEEK_SET),
            static_cast<boffset_t>(kChunkBytes - 32));
  std::string output(4096, kPoison);
  const std::string expected
      = first.substr(first.size() - 32) + second.substr(0, output.size() - 32);
  ASSERT_EQ(dev_->d_read(0, output.data(), output.size()),
            static_cast<ssize_t>(output.size()));
  EXPECT_EQ(std::memcmp(output.data(), expected.data(), expected.size()), 0);
  ASSERT_EQ(store_->downloads.size(), 2);
  EXPECT_EQ(store_->downloads.back().part, "0001");
  ASSERT_TRUE(store_->downloads.back().range);
  EXPECT_EQ(store_->downloads.back().range->offset, 0);
  EXPECT_EQ(store_->downloads.back().range->length, kBlockBytes);
}

TEST_F(DplcompatLeaseTest, WriterLabelSeekEndLoadsSameChunkBeforeAppend)
{
  const std::string data = Pattern(4 * kBlockBytes + 19);
  const std::string appended = "new backup records";
  store_->objects["0000"] = data;
  ASSERT_EQ(Open(O_RDWR), 0);
  Read(data, 0, 64);
  ASSERT_EQ(store_->downloads.size(), 1);
  ASSERT_EQ(dev_->d_lseek(nullptr, 0, SEEK_END),
            static_cast<boffset_t>(data.size()));
  ASSERT_EQ(store_->downloads.size(), 2);
  ASSERT_TRUE(store_->downloads.back().range);
  EXPECT_EQ(store_->downloads.back().range->offset, 0);
  EXPECT_EQ(store_->downloads.back().range->length, data.size());
  EXPECT_EQ(store_->HeadCount(), 1);
  ASSERT_EQ(dev_->d_write(0, appended.data(), appended.size()),
            static_cast<ssize_t>(appended.size()));
  ASSERT_EQ(dev_->d_close(0), 0);
  EXPECT_EQ(store_->objects["0000"], data + appended);
  EXPECT_EQ(store_->uploads, 1);
}

TEST_F(DplcompatLeaseTest, MiddleWriteAfterPartialReadPreservesAllOtherBytes)
{
  const std::string data = Pattern(4 * kBlockBytes + 19);
  const std::string patch = "replace just these bytes";
  const size_t target = 2 * kBlockBytes + 17;
  std::string expected = data;
  expected.replace(target, patch.size(), patch);
  store_->objects["0000"] = data;
  ASSERT_EQ(Open(O_RDWR), 0);
  Read(data, 0, 64);
  ASSERT_EQ(dev_->d_lseek(nullptr, target, SEEK_SET),
            static_cast<boffset_t>(target));
  ASSERT_EQ(dev_->d_write(0, patch.data(), patch.size()),
            static_cast<ssize_t>(patch.size()));
  ASSERT_EQ(dev_->d_close(0), 0);
  EXPECT_EQ(store_->objects["0000"], expected);
  EXPECT_EQ(store_->uploads, 1);
}

TEST_F(DplcompatLeaseTest, MetadataErrorWithCreateDoesNotOpenOrUpload)
{
  const std::string data = Pattern(3 * kBlockBytes);
  store_->objects["0000"] = data;
  store_->head_error = StoreError{StoreErrc::kPermanent, "HEAD denied"};
  EXPECT_EQ(Open(O_CREAT | O_RDWR), -1);
  EXPECT_EQ(job_->getJobStatus(), JS_FatalError);
  EXPECT_TRUE(store_->downloads.empty());
  EXPECT_EQ(store_->uploads, 0);
  EXPECT_EQ(store_->objects["0000"], data);
}

TEST_F(DplcompatLeaseTest, MissingCreateAllowsWritingANewLabel)
{
  const std::string label = "new volume label";
  ASSERT_EQ(Open(O_CREAT | O_RDWR), 0);
  // The absent chunk 0 is an error, never zero bytes or EOF.
  char output = kPoison;
  EXPECT_EQ(dev_->d_read(0, &output, 1), -1);
  EXPECT_EQ(output, kPoison);
  ASSERT_EQ(dev_->d_write(0, label.data(), label.size()),
            static_cast<ssize_t>(label.size()));
  ASSERT_EQ(dev_->d_close(0), 0);
  EXPECT_EQ(store_->objects["0000"], label);
  EXPECT_EQ(store_->uploads, 1);
  EXPECT_TRUE(store_->downloads.empty());
  EXPECT_NE(job_->getJobStatus(), JS_FatalError);
}

TEST_F(DplcompatLeaseTest, DeniedWriterLabelFailsJobBeforeAutolabel)
{
  const std::string data = Pattern(3 * kBlockBytes);
  store_->objects["0000"] = data;
  dev_->VolCatInfo.VolCatBytes = data.size();
  ASSERT_EQ(Open(O_RDWR), 0);
  store_->get_error = StoreError{StoreErrc::kPermanent, "label GET denied"};
  std::array<char, 64> output;
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
  EXPECT_EQ(job_->getJobStatus(), JS_FatalError);
  ASSERT_EQ(dev_->d_close(0), 0);
  EXPECT_EQ(store_->uploads, 0);
  EXPECT_EQ(store_->objects["0000"], data);
  EXPECT_EQ(dev_->VolCatInfo.VolCatBytes, data.size());
}

TEST_F(DplcompatLeaseTest, ShortWriterLabelFailsJobBeforeAutolabel)
{
  const std::string data = Pattern(3 * kBlockBytes);
  store_->objects["0000"] = data;
  dev_->VolCatInfo.VolCatBytes = data.size();
  ASSERT_EQ(Open(O_RDWR), 0);
  store_->short_next = true;
  std::array<char, 64> output;
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
  EXPECT_EQ(job_->getJobStatus(), JS_FatalError);
  EXPECT_EQ(dev_->d_write(0, "replacement", 11), -1);
  ASSERT_EQ(dev_->d_close(0), 0);
  EXPECT_EQ(store_->uploads, 0);
  EXPECT_EQ(store_->objects["0000"], data);
  EXPECT_EQ(dev_->VolCatInfo.VolCatBytes, data.size());
}

TEST_F(DplcompatLeaseTest, ReadStraddlingTheLoadedEndFetchesTheSuffix)
{
  const std::string data = Pattern(3 * kBlockBytes + 31);
  store_->objects["0000"] = data;
  ASSERT_EQ(Open(O_RDONLY), 0);
  Read(data, 0, 64);
  Read(data, 64, kBlockBytes + 100);
  ASSERT_EQ(store_->downloads.size(), 2);
  ASSERT_TRUE(store_->downloads[1].range);
  EXPECT_EQ(store_->downloads[1].range->offset, kBlockBytes);
  EXPECT_EQ(store_->downloads[1].range->length, data.size() - kBlockBytes);
}

TEST_F(DplcompatLeaseTest, ConsecutiveAppendsKeepTheDirtyBytes)
{
  const std::string data = Pattern(4 * kBlockBytes + 19);
  const std::string first = "first appended record";
  const std::string second = "second appended record";
  store_->objects["0000"] = data;
  ASSERT_EQ(Open(O_RDWR), 0);
  Read(data, 0, 64);
  ASSERT_EQ(dev_->d_lseek(nullptr, 0, SEEK_END),
            static_cast<boffset_t>(data.size()));
  ASSERT_EQ(dev_->d_write(0, first.data(), first.size()),
            static_cast<ssize_t>(first.size()));
  ASSERT_EQ(dev_->d_write(0, second.data(), second.size()),
            static_cast<ssize_t>(second.size()));
  ASSERT_EQ(dev_->d_close(0), 0);
  EXPECT_EQ(store_->objects["0000"], data + first + second);
  EXPECT_EQ(store_->uploads, 1);
}

TEST_F(DplcompatLeaseTest, PartialReadAtTheCatalogEndStopsAtEof)
{
  const std::string data = Pattern(kChunkBytes);
  store_->objects["0000"] = data;
  dev_->VolCatInfo.VolCatBytes = data.size();
  ASSERT_EQ(Open(O_RDONLY), 0);
  ASSERT_EQ(dev_->d_lseek(nullptr, kChunkBytes - 32, SEEK_SET),
            static_cast<boffset_t>(kChunkBytes - 32));
  std::string output(64, kPoison);
  ASSERT_EQ(dev_->d_read(0, output.data(), output.size()), 32);
  EXPECT_EQ(std::memcmp(output.data(), data.data() + kChunkBytes - 32, 32), 0);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), 0);
  EXPECT_NE(job_->getJobStatus(), JS_FatalError);
}

TEST_F(DplcompatLeaseTest, PartialReadBeforeTheCatalogEndIsAnError)
{
  store_->objects["0000"] = Pattern(kChunkBytes);
  dev_->VolCatInfo.VolCatBytes = kChunkBytes + 64;
  ASSERT_EQ(Open(O_RDONLY), 0);
  ASSERT_EQ(dev_->d_lseek(nullptr, kChunkBytes - 32, SEEK_SET),
            static_cast<boffset_t>(kChunkBytes - 32));
  std::string output(64, kPoison);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
  EXPECT_EQ(dev_->dev_errno, EIO);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
}

TEST_F(DplcompatLeaseTest, QueuedChunkIsClonedAndNeverRangedFromTheStore)
{
  UseIoThreads(*dev_);
  store_->HoldUploads(true);
  const std::string data = Pattern(3 * kChunkBytes + 1);
  ASSERT_EQ(Open(O_CREAT | O_RDWR), 0);
  for (size_t offset = 0; offset < data.size(); offset += kBlockBytes) {
    const size_t length = std::min(kBlockBytes, data.size() - offset);
    ASSERT_EQ(dev_->d_write(0, data.data() + offset, length),
              static_cast<ssize_t>(length));
  }
  // Chunk 1 waits in the upload queue; the unfinished chunk 3 is abandoned.
  const size_t target = kChunkBytes + 12345;
  const size_t heads_before = store_->HeadCount();
  ASSERT_EQ(dev_->d_lseek(nullptr, target, SEEK_SET),
            static_cast<boffset_t>(target));
  Read(data, target, 4096);
  EXPECT_FALSE(NeedsFlushing(*dev_));
  EXPECT_TRUE(store_->downloads.empty());
  EXPECT_EQ(store_->HeadCount(), heads_before);
  store_->HoldUploads(false);
  ASSERT_TRUE(dev_->d_flush(nullptr));
  ASSERT_TRUE(store_->WaitForObjects(3));
  ASSERT_EQ(dev_->d_close(0), 0);
  opened_ = false;
  for (int chunk = 0; chunk < 3; ++chunk) {
    char name[8];
    snprintf(name, sizeof(name), "%04d", chunk);
    // Compared without printing: a mismatch of 10 MiB strings is unreadable.
    EXPECT_TRUE(store_->objects[name]
                == data.substr(chunk * kChunkBytes, kChunkBytes))
        << "chunk " << chunk;
  }
}

// Names of the chunks fetched by read-ahead workers.
static std::set<std::string> WorkerParts(RangeStore& store)
{
  std::set<std::string> parts;
  for (const auto& download : store.DownloadsSnapshot()) {
    if (download.worker) { parts.insert(download.part); }
  }
  return parts;
}

TEST_F(DplcompatLeaseTest, SequentialRestoreReadsAheadAtMostTwoChunks)
{
  const std::string data = MakeVolume(5, 100);
  ASSERT_EQ(Open(O_RDONLY), 0);
  ReadBlocks(data, 0, 2 * kChunkBytes + 1000);
  ASSERT_TRUE(WaitFor([&] { return WorkerParts(*store_).count("0004") > 0; }));
  EXPECT_EQ(WorkerParts(*store_),
            (std::set<std::string>{"0002", "0003", "0004"}));
  ReadBlocks(data, 2 * kChunkBytes + 1000,
             data.size() - 2 * kChunkBytes - 1000);
  char byte;
  EXPECT_EQ(dev_->d_read(0, &byte, 1), 0);
  EXPECT_EQ(WorkerParts(*store_),
            (std::set<std::string>{"0002", "0003", "0004", "0005"}));
  EXPECT_LE(store_->max_workers.load(), 2);
  size_t foreground_whole = 0;
  size_t foreground_all = 0;
  for (const auto& download : store_->DownloadsSnapshot()) {
    if (!download.worker) { ++foreground_all; }
    if (!download.worker && !download.range) {
      ++foreground_whole;
      EXPECT_EQ(download.part, "0001");
    }
  }
  EXPECT_EQ(foreground_whole, 1);
  // The label range and its suffix of chunk 0, and the whole chunk 1: every
  // other chunk came from a read-ahead and was not fetched again.
  EXPECT_EQ(foreground_all, 3);
  EXPECT_NE(job_->getJobStatus(), JS_FatalError);
}

TEST_F(DplcompatLeaseTest, ReadAheadCompletingOutOfOrderDeliversTheRightBytes)
{
  const std::string data = MakeVolume(4, 100);
  store_->HoldPart("0002", true);
  ASSERT_EQ(Open(O_RDONLY), 0);
  ReadBlocks(data, 0, kChunkBytes + 1000);
  ASSERT_TRUE(WaitFor([&] { return !store_->WorkerFinished().empty(); }));
  EXPECT_EQ(store_->WorkerFinished(), (std::vector<std::string>{"0003"}));
  std::thread release([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    store_->HoldPart("0002", false);
  });
  ReadBlocks(data, kChunkBytes + 1000, data.size() - kChunkBytes - 1000);
  release.join();
  EXPECT_EQ(store_->WorkerFinished().front(), "0003");
}

TEST_F(DplcompatLeaseTest, FailedSpeculativeReadIsRetriedByTheConsumer)
{
  const std::string data = MakeVolume(4, 100);
  store_->FailWorkerGets("0003", 1);
  ASSERT_EQ(Open(O_RDONLY), 0);
  ReadBlocks(data, 0, kChunkBytes + 1000);
  ASSERT_TRUE(WaitFor([&] { return store_->WorkerFinished().size() >= 1; }));
  EXPECT_NE(job_->getJobStatus(), JS_FatalError);
  ReadBlocks(data, kChunkBytes + 1000, data.size() - kChunkBytes - 1000);
  size_t whole_0003 = 0;
  for (const auto& download : store_->DownloadsSnapshot()) {
    if (download.part == "0003" && !download.range) { ++whole_0003; }
  }
  EXPECT_EQ(whole_0003, 2);
  EXPECT_NE(job_->getJobStatus(), JS_FatalError);
}

TEST_F(DplcompatLeaseTest, SpeculativeFailureOnlyFailsWhenTheChunkIsRead)
{
  const std::string data = MakeVolume(4, 100);
  store_->FailWorkerGets("0002", 1);
  store_->FailWorkerGets("0003", 1);
  ASSERT_EQ(Open(O_RDONLY), 0);
  ReadBlocks(data, 0, kChunkBytes + 1000);
  ASSERT_TRUE(WaitFor([&] {
    return store_->active_workers.load() == 0
           && store_->DownloadsSnapshot().size() >= 5;
  }));
  EXPECT_NE(job_->getJobStatus(), JS_FatalError);
  ReadBlocks(data, kChunkBytes + 1000, 1000);
  store_->get_error = StoreError{StoreErrc::kPermanent, "GET denied"};
  std::string output(2 * kChunkBytes, kPoison);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
  EXPECT_EQ(dev_->dev_errno, EIO);
}

TEST_F(DplcompatLeaseTest, SeekEndsReadAheadWithinABoundedTime)
{
  const std::string data = MakeVolume(4, 100);
  store_->HoldPart("0002", true);
  store_->HoldPart("0003", true);
  ASSERT_EQ(Open(O_RDONLY), 0);
  ReadBlocks(data, 0, kChunkBytes + 1000);
  ASSERT_TRUE(WaitFor([&] { return store_->active_workers.load() == 2; }));
  const auto begin = std::chrono::steady_clock::now();
  ASSERT_EQ(dev_->d_lseek(nullptr, 0, SEEK_SET), 0);
  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds{2});
  EXPECT_EQ(store_->active_workers.load(), 0);
  store_->ReleaseAll();
  ReadBlocks(data, 0, 1000);
}

TEST_F(DplcompatLeaseTest, CloseJoinsTheReadAheadWorkers)
{
  const std::string data = MakeVolume(4, 100);
  store_->HoldPart("0002", true);
  store_->HoldPart("0003", true);
  ASSERT_EQ(Open(O_RDONLY), 0);
  ReadBlocks(data, 0, kChunkBytes + 1000);
  ASSERT_TRUE(WaitFor([&] { return store_->active_workers.load() == 2; }));
  const auto begin = std::chrono::steady_clock::now();
  ASSERT_EQ(dev_->d_close(0), 0);
  opened_ = false;
  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds{2});
  EXPECT_EQ(store_->active_workers.load(), 0);
}

TEST_F(DplcompatLeaseTest, ReopeningAnotherVolumeDropsTheReadAhead)
{
  const std::string data = MakeVolume(4, 100);
  store_->HoldPart("0002", true);
  store_->HoldPart("0003", true);
  ASSERT_EQ(Open(O_RDONLY), 0);
  ReadBlocks(data, 0, kChunkBytes + 1000);
  ASSERT_TRUE(WaitFor([&] { return store_->active_workers.load() == 2; }));
  bstrncpy(dev_->VolCatInfo.VolCatName, "OtherVolume",
           sizeof(dev_->VolCatInfo.VolCatName));
  ASSERT_EQ(Open(O_RDONLY), 0);
  EXPECT_EQ(store_->active_workers.load(), 0);
}

TEST_F(DplcompatLeaseTest, ReadAheadNeverGoesPastTheCatalogSize)
{
  const std::string data = MakeVolume(4, 0);
  dev_->VolCatInfo.VolCatBytes = 3 * kChunkBytes;
  ASSERT_EQ(Open(O_RDONLY), 0);
  ReadBlocks(data, 0, kChunkBytes + 1000);
  ASSERT_TRUE(WaitFor([&] { return WorkerParts(*store_).count("0002") > 0; }));
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  EXPECT_EQ(WorkerParts(*store_), (std::set<std::string>{"0002"}));
}

TEST_F(DplcompatLeaseTest, CancelEndsTheWaitForASlowReadAhead)
{
  const std::string data = MakeVolume(4, 100);
  store_->HoldPart("0002", true);
  ASSERT_EQ(Open(O_RDONLY), 0);
  ReadBlocks(data, 0, kChunkBytes + 1000);
  ASSERT_TRUE(WaitFor([&] { return store_->active_workers.load() >= 1; }));
  std::thread cancel([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    job_->setJobStatusWithPriorityCheck(JS_Canceled);
  });
  std::string output(2 * kChunkBytes, kPoison);
  const auto begin = std::chrono::steady_clock::now();
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
  cancel.join();
  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds{5});
}

TEST_F(DplcompatLeaseTest, DestroyingTheDeviceJoinsTheReadAheadWorkers)
{
  const std::string data = MakeVolume(4, 100);
  store_->HoldPart("0002", true);
  store_->HoldPart("0003", true);
  auto active = store_->shared_active;
  ASSERT_EQ(Open(O_RDONLY), 0);
  ReadBlocks(data, 0, kChunkBytes + 1000);
  ASSERT_TRUE(WaitFor([&] { return active->load() == 2; }));
  // The device goes away without a close: no worker may outlive its store.
  opened_ = false;
  dev_.reset();
  store_ = nullptr;
  EXPECT_EQ(active->load(), 0);
}

TEST_F(DplcompatLeaseTest, WriterOpenedDeviceNeverReadsAhead)
{
  const std::string data = MakeVolume(4, 100);
  ASSERT_EQ(Open(O_RDWR), 0);
  ReadBlocks(data, 0, 2 * kChunkBytes + 1000);
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  EXPECT_TRUE(WorkerParts(*store_).empty());
}

TEST_F(DplcompatLeaseTest, PartiallyReadChunkDoesNotStartReadAhead)
{
  const std::string data = MakeVolume(4, 100);
  ASSERT_EQ(Open(O_RDONLY), 0);
  ASSERT_EQ(dev_->d_lseek(nullptr, kChunkBytes - 100000, SEEK_SET),
            static_cast<boffset_t>(kChunkBytes - 100000));
  ReadBlocks(data, kChunkBytes - 100000, 100000 + 4096);
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  EXPECT_TRUE(WorkerParts(*store_).empty());
}

TEST_F(DplcompatLeaseTest, MissingChunkBeforeTheCatalogEndStaysAReadError)
{
  const std::string data = MakeVolume(3, 0);
  dev_->VolCatInfo.VolCatBytes = 4 * kChunkBytes;
  ASSERT_EQ(Open(O_RDONLY), 0);
  ReadBlocks(data, 0, 3 * kChunkBytes);
  std::string output(64, kPoison);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
  EXPECT_EQ(dev_->dev_errno, EIO);
}

TEST_F(DplcompatLeaseTest, UnreadableLastChunkFailsTheJobAtSeekEnd)
{
  const std::string first = Pattern(kChunkBytes);
  const std::string last(4096, 'z');
  store_->objects["0000"] = first;
  store_->objects["0001"] = last;
  dev_->VolCatInfo.VolCatBytes = first.size() + last.size();
  ASSERT_EQ(Open(O_RDWR), 0);
  store_->get_error = StoreError{StoreErrc::kPermanent, "GET denied"};
  EXPECT_EQ(dev_->d_lseek(nullptr, 0, SEEK_END), -1);
  EXPECT_EQ(job_->getJobStatus(), JS_FatalError);
  EXPECT_EQ(store_->uploads, 0);
  EXPECT_EQ(store_->objects["0001"], last);
}

TEST_F(DplcompatLeaseTest, TransientHeadErrorAtTheEndOfAReadIsStillTheEnd)
{
  const std::string data = Pattern(kChunkBytes);
  store_->objects["0000"] = data;
  store_->head_error = StoreError{StoreErrc::kTransient, "HEAD timed out"};
  store_->head_error_part = "0001";
  dev_->VolCatInfo.VolCatBytes = data.size();
  ASSERT_EQ(Open(O_RDONLY), 0);
  ASSERT_EQ(dev_->d_lseek(nullptr, kChunkBytes - 32, SEEK_SET),
            static_cast<boffset_t>(kChunkBytes - 32));
  std::string output(64, kPoison);
  ASSERT_EQ(dev_->d_read(0, output.data(), output.size()), 32);
  EXPECT_EQ(std::memcmp(output.data(), data.data() + kChunkBytes - 32, 32), 0);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), 0);
  EXPECT_NE(job_->getJobStatus(), JS_FatalError);
}

TEST_F(DplcompatLeaseTest, ShortWholeChunkGetIsAnErrorNotTheEnd)
{
  const std::string first = Pattern(kChunkBytes);
  const std::string second(kChunkBytes, 'z');
  store_->objects["0000"] = first;
  store_->objects["0001"] = second;
  dev_->VolCatInfo.VolCatBytes = first.size() + second.size();
  ASSERT_EQ(Open(O_RDONLY), 0);
  Read(first, 0, 64);
  Read(first, 64, kBlockBytes - 64);
  Read(first, kBlockBytes, first.size() - kBlockBytes - 100);
  store_->short_next = true;  // the next chunk is a whole-object GET
  std::string output(256, kPoison);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
  EXPECT_EQ(dev_->dev_errno, EIO);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), -1);
}

TEST_F(DplcompatLeaseTest, FailedWriterOpenDoesNotMakeALaterReaderAWriter)
{
  const std::string data = Pattern(kChunkBytes);
  store_->objects["0000"] = data;
  dev_->VolCatInfo.VolCatBytes = data.size();
  store_->head_error = StoreError{StoreErrc::kPermanent, "HEAD denied"};
  store_->head_error_part = "0000";
  EXPECT_EQ(Open(O_RDWR), -1);
  EXPECT_EQ(job_->getJobStatus(), JS_FatalError);

  // A restore on the same device: a HEAD error at its volume end is no fault.
  store_->head_error = StoreError{StoreErrc::kTransient, "HEAD timed out"};
  store_->head_error_part = "0001";
  auto restore_job = std::make_unique<JobControlRecord>();
  restore_job->setJobType(JT_RESTORE);
  restore_job->setJobStatusWithPriorityCheck(JS_Running);
  SetJcrInThreadSpecificData(restore_job.get());
  ASSERT_EQ(Open(O_RDONLY), 0);
  ASSERT_EQ(dev_->d_lseek(nullptr, kChunkBytes - 32, SEEK_SET),
            static_cast<boffset_t>(kChunkBytes - 32));
  std::string output(64, kPoison);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), 32);
  EXPECT_EQ(dev_->d_read(0, output.data(), output.size()), 0);
  EXPECT_NE(restore_job->getJobStatus(), JS_FatalError);
  SetJcrInThreadSpecificData(job_.get());
}

TEST_F(DplcompatLeaseTest, DamagedLayoutKeepsTheMountsVolumeErrorHandling)
{
  // Chunk 1 is missing between a full chunk and a later one: not the end of a
  // healthy volume, so the job is not failed here and the mount rules apply.
  store_->objects["0000"] = Pattern(kChunkBytes);
  store_->objects["0002"] = "tail";
  dev_->VolCatInfo.VolCatBytes = kChunkBytes + 4;
  ASSERT_EQ(Open(O_RDWR), 0);
  EXPECT_EQ(dev_->d_lseek(nullptr, 0, SEEK_END), -1);
  EXPECT_NE(job_->getJobStatus(), JS_FatalError);
  EXPECT_EQ(store_->uploads, 0);
}

TEST_F(DplcompatLeaseTest, SeekEndAfterAFullChunkAppendsToTheNextChunk)
{
  const std::string data = Pattern(kChunkBytes);
  store_->objects["0000"] = data;
  dev_->VolCatInfo.VolCatBytes = data.size();
  ASSERT_EQ(Open(O_RDWR), 0);
  ASSERT_EQ(dev_->d_lseek(nullptr, 0, SEEK_END),
            static_cast<boffset_t>(kChunkBytes));
  EXPECT_NE(job_->getJobStatus(), JS_FatalError);
  ASSERT_EQ(dev_->d_write(0, "tail", 4), 4);
  ASSERT_EQ(dev_->d_close(0), 0);
  opened_ = false;
  EXPECT_EQ(store_->objects["0001"], "tail");
  EXPECT_EQ(store_->uploads, 1);
  EXPECT_TRUE(store_->objects["0000"] == data);
}

TEST_F(DplcompatLeaseTest, ProgramTransportDoesNotFailTheJobAtAFailedSeekEnd)
{
  store_->ranges = false;
  const std::string first = Pattern(kChunkBytes);
  const std::string last(4096, 'z');
  store_->objects["0000"] = first;
  store_->objects["0001"] = last;
  dev_->VolCatInfo.VolCatBytes = first.size() + last.size();
  ASSERT_EQ(Open(O_RDWR), 0);
  store_->get_error = StoreError{StoreErrc::kPermanent, "GET denied"};
  EXPECT_EQ(dev_->d_lseek(nullptr, 0, SEEK_END), -1);
  EXPECT_NE(job_->getJobStatus(), JS_FatalError);
  EXPECT_EQ(store_->uploads, 0);
}

TEST_F(DplcompatLeaseTest, ShortMiddleChunkIsADamagedLayoutAtSeekEnd)
{
  store_->objects["0000"] = Pattern(kChunkBytes - 1);
  store_->objects["0001"] = "tail";
  dev_->VolCatInfo.VolCatBytes = kChunkBytes + 3;
  ASSERT_EQ(Open(O_RDWR), 0);
  store_->get_error = StoreError{StoreErrc::kPermanent, "GET denied"};
  EXPECT_EQ(dev_->d_lseek(nullptr, 0, SEEK_END), -1);
  EXPECT_NE(job_->getJobStatus(), JS_FatalError);
  EXPECT_EQ(store_->uploads, 0);
}

TEST_F(DplcompatLeaseTest, OversizeLastChunkIsADamagedLayoutAtSeekEnd)
{
  store_->objects["0000"] = Pattern(kChunkBytes);
  store_->objects["0001"] = std::string(kChunkBytes + 1, 'z');
  dev_->VolCatInfo.VolCatBytes = 2 * kChunkBytes + 1;
  ASSERT_EQ(Open(O_RDWR), 0);
  EXPECT_EQ(dev_->d_lseek(nullptr, 0, SEEK_END), -1);
  EXPECT_NE(job_->getJobStatus(), JS_FatalError);
  EXPECT_EQ(store_->uploads, 0);
}

TEST_F(DplcompatLeaseTest, ShortMiddleChunkIsNotTakenFromTheReadAhead)
{
  const std::string data = MakeVolume(4, 100);
  store_->objects["0002"].resize(kChunkBytes - 1);
  ASSERT_EQ(Open(O_RDONLY), 0);
  ReadBlocks(data, 0, 2 * kChunkBytes + 1000);
  // The speculative read of chunk 2 was short, so the job read it itself.
  size_t foreground_0002 = 0;
  for (const auto& download : store_->DownloadsSnapshot()) {
    if (!download.worker && download.part == "0002" && !download.range) {
      ++foreground_0002;
    }
  }
  EXPECT_EQ(foreground_0002, 1);
}

TEST_F(DplcompatLeaseTest, ProgramCompatibleStoreStillDownloadsTheWholeObject)
{
  const std::string data = Pattern(3 * kBlockBytes);
  store_->ranges = false;
  store_->objects["0000"] = data;
  ASSERT_EQ(Open(O_RDONLY), 0);
  ASSERT_EQ(store_->downloads.size(), 1);
  EXPECT_FALSE(store_->downloads[0].range);
  Read(data, 0, 64);
  EXPECT_EQ(store_->downloads.size(), 1);
}

}  // namespace storagedaemon
