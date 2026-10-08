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

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>

namespace storagedaemon {

class DplcompatLeaseTest : public ::testing::Test {
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

}  // namespace storagedaemon
