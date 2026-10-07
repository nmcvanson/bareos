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

/* Device options of the dplcompat backend: devices from a test configuration
 * whose program is /bin/true, set up without any backing store. */

#include "gtest/gtest.h"
#include "include/bareos.h"

#define STORAGE_DAEMON 1
#include "include/jcr.h"
#include "lib/parse_conf.h"
#include "stored/butil.h"
#include "stored/device_control_record.h"
#include "stored/stored_jcr_impl.h"
#include "stored/stored.h"
#include "stored/stored_globals.h"
#include "stored/sd_backends.h"

#include <string>

#define CONFIG_SUBDIR "dplcompat_device_options"
#include "sd_backend_tests.h"

using namespace storagedaemon;

namespace {
/* Creates the device of the named resource, which runs its setup, and keeps
 * what was printed meanwhile; true when the device could be created. */
bool CreateDevice(const char* name, std::string& output)
{
  JobControlRecord* jcr
      = SetupDummyJcr("dplcompat_device_options", nullptr, nullptr);
  auto* resource = (DeviceResource*)my_config->GetResWithName(R_DEVICE, name);
  EXPECT_NE(resource, nullptr) << name;
  Device* dev = nullptr;
  if (resource) {
    testing::internal::CaptureStdout();
    dev = FactoryCreateDevice(jcr, resource);
    output = testing::internal::GetCapturedStdout();
  }
  if (dev) {
    EXPECT_TRUE(dev->setup());
    // Without a mounted volume there is nothing to flush.
    EXPECT_TRUE(dev->d_flush(nullptr));
    delete dev;
  }
  FreeJcr(jcr);
  return dev != nullptr;
}
}  // namespace

TEST_F(sd, dplcompat_refuses_flush_timeout_zero)
{
  std::string output;
  EXPECT_FALSE(CreateDevice("flush-timeout-zero", output));
  EXPECT_NE(output.find("Option 'flush_timeout' must be at least 1"),
            std::string::npos);
}

TEST_F(sd, dplcompat_refuses_flush_timeout_junk)
{
  std::string output;
  EXPECT_FALSE(CreateDevice("flush-timeout-junk", output));
  EXPECT_NE(output.find("for option 'flush_timeout'"), std::string::npos);
}

TEST_F(sd, dplcompat_accepts_flush_timeout)
{
  std::string output;
  EXPECT_TRUE(CreateDevice("flush-timeout-thirty", output));
}

TEST_F(sd, dplcompat_default_flush_timeout)
{
  std::string output;
  EXPECT_TRUE(CreateDevice("flush-timeout-default", output));
}
