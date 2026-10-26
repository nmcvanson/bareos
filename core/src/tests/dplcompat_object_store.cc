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

/* The ObjectStore interface as implemented by the program transport: the
 * command lines the program gets, the kind of every failure, and byte ranges,
 * against small fake wrapper programs written by the test. */

#include "gtest/gtest.h"
#include "include/bareos.h"

#include "stored/backends/crud_storage.h"
#include "stored/backends/object_store.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

class ObjectStoreProgram : public ::testing::Test {
 protected:
  fs::path dir_;
  CrudStorage storage_;
  ObjectStore& store_{storage_};

  void SetUp() override
  {
    dir_
        = fs::temp_directory_path()
          / ("dplcompat_object_store." + std::to_string(getpid()) + "."
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

  std::string ReadFile(const fs::path& path)
  {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
  }
};

TEST_F(ObjectStoreProgram, EveryOperationRunsTheProgramWithItsOldArguments)
{
  const fs::path log = dir_ / "calls.log";
  UseProgram(
      "echo \"$*\" >> \"$calls\"\n"
      "case \"$1\" in\n"
      "  stat) echo 5;;\n"
      "  list) echo '0000 5';;\n"
      "  upload) cat > /dev/null;;\n"
      "  download) printf abcde;;\n"
      "esac");
  ASSERT_TRUE(store_.set_option("calls", log.string()));

  std::string data = "abc";
  std::string buffer(5, '\0');
  EXPECT_TRUE(store_.test_connection());
  EXPECT_TRUE(store_.stat("Vol1", "0000"));
  EXPECT_TRUE(store_.list("Vol1"));
  EXPECT_TRUE(store_.upload("Vol1", "0001", {data.data(), data.size()}));
  EXPECT_TRUE(store_.download("Vol1", "0002", {buffer.data(), buffer.size()},
                              std::nullopt));
  EXPECT_TRUE(store_.remove("Vol1", "0003"));

  EXPECT_EQ(ReadFile(log),
            "testconnection\n"
            "stat Vol1 0000\n"
            "list Vol1\n"
            "upload Vol1 0001\n"
            "download Vol1 0002\n"
            "remove Vol1 0003\n");
}

TEST_F(ObjectStoreProgram, UploadSendsTheDataAndDownloadReturnsIt)
{
  const fs::path stored = dir_ / "stored.bin";
  UseProgram(
      "case \"$1\" in\n"
      "  upload) cat > \"$target\";;\n"
      "  download) cat \"$target\";;\n"
      "esac");
  ASSERT_TRUE(store_.set_option("target", stored.string()));

  std::string data(300 * 1024, 'x');
  data[7] = 'y';
  data.back() = 'z';
  ASSERT_TRUE(store_.upload("Vol1", "0000", {data.data(), data.size()}));
  EXPECT_EQ(ReadFile(stored), data);

  std::string buffer(data.size(), '\0');
  auto result = store_.download("Vol1", "0000", {buffer.data(), buffer.size()},
                                std::nullopt);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(result->size_bytes(), data.size());
  EXPECT_EQ(buffer, data);
}

TEST_F(ObjectStoreProgram, OptionsAreTheLinesTheProgramPrints)
{
  UseProgram("[ \"$1\" = options ] && printf 'bucket\\ns3cfg\\n'");
  auto options = store_.get_supported_options();
  ASSERT_TRUE(options) << options.error();
  ASSERT_EQ(options->size(), 2u);
  EXPECT_EQ(options->at(0), "bucket");
  EXPECT_EQ(options->at(1), "s3cfg");
}

TEST_F(ObjectStoreProgram, OptionsReachTheProgramAsEnvironment)
{
  UseProgram("echo \"$answer\"");
  ASSERT_TRUE(store_.set_option("answer", "42"));
  auto result = store_.stat("Vol1", "0000");
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(result->size, 42u);
}

TEST_F(ObjectStoreProgram, OptionNameThatIsNoVariableIsAConfigError)
{
  for (const char* name : {"", "1bad", "bad-name", "bad name", "bad=name"}) {
    auto result = store_.set_option(name, "x");
    ASSERT_FALSE(result) << name;
    EXPECT_EQ(result.error().code, StoreErrc::kConfig) << name;
  }
}

TEST_F(ObjectStoreProgram, StatOfAMissingObjectIsNotFound)
{
  UseProgram("echo");
  auto result = store_.stat("Vol1", "0000");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreErrc::kNotFound);
}

TEST_F(ObjectStoreProgram, StatThatFailsOrIsGarbageIsTransient)
{
  UseProgram("exit 1");
  auto failed = store_.stat("Vol1", "0000");
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().code, StoreErrc::kTransient);

  UseProgram("echo 'not a size'");
  auto garbage = store_.stat("Vol1", "0000");
  ASSERT_FALSE(garbage);
  EXPECT_EQ(garbage.error().code, StoreErrc::kTransient);
}

TEST_F(ObjectStoreProgram, ErrorTextNamesTheCommandLine)
{
  UseProgram("exit 3");
  auto result = store_.stat("Vol1", "0000");
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().message.find("stat \"Vol1\" \"0000\""),
            std::string::npos)
      << result.error();
  EXPECT_NE(result.error().message.find("returned 3"), std::string::npos)
      << result.error();
}

TEST_F(ObjectStoreProgram, ListThatFailsOrIsGarbageIsTransient)
{
  UseProgram("echo '0000 10'; exit 3");
  auto failed = store_.list("Vol1");
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().code, StoreErrc::kTransient);

  UseProgram("echo 'this is not a listing'");
  auto garbage = store_.list("Vol1");
  ASSERT_FALSE(garbage);
  EXPECT_EQ(garbage.error().code, StoreErrc::kTransient);
}

TEST_F(ObjectStoreProgram, SilentListIsATimeout)
{
  UseProgram("exec sleep 30");
  auto result = store_.list("Vol1");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreErrc::kTimeout);
}

TEST_F(ObjectStoreProgram, SilentStatIsATimeout)
{
  UseProgram("exec sleep 30");
  auto result = store_.stat("Vol1", "0000");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreErrc::kTimeout);
}

TEST_F(ObjectStoreProgram, UploadThatFailsIsTransient)
{
  UseProgram("cat > /dev/null; exit 1");
  std::string data = "abc";
  auto result = store_.upload("Vol1", "0000", {data.data(), data.size()});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreErrc::kTransient);
}

TEST_F(ObjectStoreProgram, SilentUploadIsATimeout)
{
  UseProgram("exec sleep 30");
  std::string data = "abc";
  auto result = store_.upload("Vol1", "0000", {data.data(), data.size()});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreErrc::kTimeout);
}

TEST_F(ObjectStoreProgram, ShortOrFailingDownloadIsTransient)
{
  std::string buffer(4, '\0');
  UseProgram("printf ab");
  auto too_short = store_.download(
      "Vol1", "0000", {buffer.data(), buffer.size()}, std::nullopt);
  ASSERT_FALSE(too_short);
  EXPECT_EQ(too_short.error().code, StoreErrc::kTransient);

  UseProgram("printf abcd; exit 3");
  auto failed = store_.download("Vol1", "0000", {buffer.data(), buffer.size()},
                                std::nullopt);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().code, StoreErrc::kTransient);

  UseProgram("printf abcdef");
  auto too_long = store_.download("Vol1", "0000",
                                  {buffer.data(), buffer.size()}, std::nullopt);
  ASSERT_FALSE(too_long);
  EXPECT_EQ(too_long.error().code, StoreErrc::kTransient);
}

TEST_F(ObjectStoreProgram, SilentDownloadIsATimeout)
{
  UseProgram("exec sleep 30");
  std::string buffer(4, '\0');
  auto result = store_.download("Vol1", "0000", {buffer.data(), buffer.size()},
                                std::nullopt);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreErrc::kTimeout);
}

TEST_F(ObjectStoreProgram, ProgramCannotDownloadARange)
{
  const fs::path marker = dir_ / "ran";
  UseProgram("touch \"$marker\"; printf abcd");
  ASSERT_TRUE(store_.set_option("marker", marker.string()));
  EXPECT_FALSE(store_.supports_range_download());

  std::string buffer(2, '\0');
  auto result = store_.download("Vol1", "0000", {buffer.data(), buffer.size()},
                                ByteRange{1, 2});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreErrc::kConfig);
  EXPECT_FALSE(fs::exists(marker)) << "the program must not run";
}

TEST_F(ObjectStoreProgram, TestConnectionFailureIsTransient)
{
  UseProgram("echo 'no route to host'; exit 1");
  auto result = store_.test_connection();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreErrc::kTransient);
}

TEST_F(ObjectStoreProgram, RemoveFailureIsTransient)
{
  UseProgram("exit 2");
  auto result = store_.remove("Vol1", "0000");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StoreErrc::kTransient);
}
