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

/* Device option "transport" of the dplcompat backend: devices from a test
 * configuration whose program is /bin/true, set up without any backing
 * store. */

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

#define CONFIG_SUBDIR "dplcompat_transport"
#include "sd_backend_tests.h"

using namespace storagedaemon;

namespace {
/* Creates the device of the named resource, which runs its setup, and keeps
 * what was printed meanwhile; true when the device could be created. */
bool CreateDevice(const char* name, std::string& output)
{
  JobControlRecord* jcr
      = SetupDummyJcr("dplcompat_transport_option", nullptr, nullptr);
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

TEST_F(sd, dplcompat_transport_defaults_to_program)
{
  std::string output;
  EXPECT_TRUE(CreateDevice("transport-default", output));
}

TEST_F(sd, dplcompat_accepts_transport_program)
{
  std::string output;
  EXPECT_TRUE(CreateDevice("transport-program", output));
}

TEST_F(sd, dplcompat_refuses_transport_native_until_it_exists)
{
  std::string output;
  EXPECT_FALSE(CreateDevice("transport-native", output));
  EXPECT_NE(output.find("invalid argument 'native' for option 'transport'"),
            std::string::npos)
      << output;
}

TEST_F(sd, dplcompat_refuses_unknown_transport)
{
  std::string output;
  EXPECT_FALSE(CreateDevice("transport-unknown", output));
  EXPECT_NE(output.find("invalid argument 'carrier-pigeon' for option "
                        "'transport'"),
            std::string::npos)
      << output;
}
