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

/* FlushRemoteChunk of the dplcompat device while another upload holds the
 * chunk's inflight lease: it reports a conflict on the request only. */

#include "gtest/gtest.h"
#include "include/bareos.h"
#include "stored/stored.h"
#include "stored/stored_globals.h"
#include "stored/backends/dplcompat_device.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
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

}  // namespace storagedaemon
