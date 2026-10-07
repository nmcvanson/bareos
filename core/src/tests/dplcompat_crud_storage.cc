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

/* CrudStorage, the program transport of dplcompat: parsing of "list" output
 * and the behaviour against small fake wrapper programs written by the test
 * (slow, silent, empty, garbage and failing output). */

#include "gtest/gtest.h"
#include "include/bareos.h"

#include "stored/backends/crud_storage.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

TEST(dplcompat_list_output, EmptyOutputIsEmptyListing)
{
  auto result = CrudStorage::parse_list_output("");
  ASSERT_TRUE(result) << result.error();
  EXPECT_TRUE(result->empty());
}

TEST(dplcompat_list_output, ParsesNameAndSize)
{
  auto result = CrudStorage::parse_list_output("0000 10485760\n0001 42\n");
  ASSERT_TRUE(result) << result.error();
  ASSERT_EQ(result->size(), 2u);
  EXPECT_EQ(result->at("0000").size, 10485760u);
  EXPECT_EQ(result->at("0001").size, 42u);
}

TEST(dplcompat_list_output, AcceptsCrlfBlankLinesAndNoFinalNewline)
{
  auto result = CrudStorage::parse_list_output("\r\n0000\t7 \r\n\n0001  8");
  ASSERT_TRUE(result) << result.error();
  ASSERT_EQ(result->size(), 2u);
  EXPECT_EQ(result->at("0000").size, 7u);
  EXPECT_EQ(result->at("0001").size, 8u);
}

TEST(dplcompat_list_output, RejectsMalformedLines)
{
  for (const char* output :
       {"0000\n", "0000 12x\n", "0000 -1\n", "0000 1 2\n",
        "0000 99999999999999999999999\n", "0000 1\nWARNING: oops\n"}) {
    EXPECT_FALSE(CrudStorage::parse_list_output(output)) << output;
  }
}

class CrudStorageProgram : public ::testing::Test {
 protected:
  fs::path dir_;
  CrudStorage storage_;

  void SetUp() override
  {
    dir_
        = fs::temp_directory_path()
          / ("dplcompat_crud_storage." + std::to_string(getpid()) + "."
             + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    storage_.set_program_timeout(2s);
  }

  void TearDown() override { fs::remove_all(dir_); }

  // Writes an executable shell script and makes it the storage's program.
  void UseProgram(const std::string& body)
  {
    const fs::path program = dir_ / "wrapper.sh";
    std::ofstream(program) << "#!/bin/sh\n" << body << "\n";
    fs::permissions(program, fs::perms::owner_all);
    ASSERT_TRUE(storage_.set_program(program.string()));
  }
};

TEST_F(CrudStorageProgram, ListThatKeepsPrintingOutlivesTheTimeout)
{
  UseProgram(
      "for i in 0 1 2 3 4 5; do printf '%04d 10\\n' \"$i\"; sleep 1; done");
  const auto start = std::chrono::steady_clock::now();
  auto result = storage_.list("Vol1");
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(result->size(), 6u);
  EXPECT_GE(std::chrono::steady_clock::now() - start, 5s);
}

TEST_F(CrudStorageProgram, SilentListIsKilledAndIsAnError)
{
  UseProgram("exec sleep 30");
  auto result = storage_.list("Vol1");
  EXPECT_FALSE(result);
}

TEST_F(CrudStorageProgram, EmptyListIsAnEmptyListing)
{
  UseProgram("exit 0");
  auto result = storage_.list("Vol1");
  ASSERT_TRUE(result) << result.error();
  EXPECT_TRUE(result->empty());
}

TEST_F(CrudStorageProgram, GarbageListIsAnError)
{
  UseProgram("echo 'this is not a listing'");
  EXPECT_FALSE(storage_.list("Vol1"));
}

TEST_F(CrudStorageProgram, FailingListIsAnErrorEvenWithValidLines)
{
  UseProgram("echo '0000 10'; exit 3");
  EXPECT_FALSE(storage_.list("Vol1"));
}

TEST_F(CrudStorageProgram, ListPassesTheVolumeName)
{
  UseProgram("[ \"$1\" = list ] && [ \"$2\" = Vol1 ] && echo '0000 5'");
  auto result = storage_.list("Vol1");
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(result->at("0000").size, 5u);
}

TEST_F(CrudStorageProgram, StatOfMissingObjectIsAnErrorNotASize)
{
  UseProgram("exit 1");
  EXPECT_FALSE(storage_.stat("Vol1", "0000"));
}

TEST_F(CrudStorageProgram, StatReturnsTheSize)
{
  UseProgram("echo 1234");
  auto result = storage_.stat("Vol1", "0000");
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(result->size, 1234u);
}

TEST_F(CrudStorageProgram, TestConnectionFollowsTheExitCode)
{
  UseProgram("[ \"$1\" = testconnection ] && echo connected");
  EXPECT_TRUE(storage_.test_connection());
  UseProgram("echo 'no route to host'; exit 1");
  EXPECT_FALSE(storage_.test_connection());
}

TEST_F(CrudStorageProgram, RemoveFollowsTheExitCode)
{
  UseProgram("exit 0");
  EXPECT_TRUE(storage_.remove("Vol1", "0000"));
  UseProgram("exit 2");
  EXPECT_FALSE(storage_.remove("Vol1", "0000"));
}
