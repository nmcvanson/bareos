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

/* s3cmd configuration reader. Keys in the examples are made up. */

#include "gtest/gtest.h"

#include "stored/backends/s3cfg_reader.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace storagedaemon::s3;
namespace fs = std::filesystem;

namespace {

const char kTypicalCfg[]
    = "# Setup by the lab\n"
      "[default]\n"
      "access_key = AKTESTACCESS1\n"
      "secret_key = secret/with+chars=and spaces\n"
      "host_base = os.example.vn\n"
      "host_bucket = os.example.vn\n"
      "use_https = True\n"
      "bucket_location = vn-north-1\n"
      "signature_v2 = False\n"
      "ca_certs_file = /etc/ssl/certs/lab-ca.pem\n"
      "check_ssl_certificate = True\n";

S3Config MustParse(std::string_view text)
{
  auto c = ParseS3cfg(text);
  EXPECT_TRUE(c.has_value()) << (c ? "" : c.error());
  return c ? *c : S3Config{};
}

}  // namespace

TEST(s3cfg_parse, TypicalFile)
{
  S3Config c = MustParse(kTypicalCfg);
  EXPECT_EQ(c.access_key, "AKTESTACCESS1");
  EXPECT_EQ(c.secret_key, "secret/with+chars=and spaces");
  EXPECT_EQ(c.host_base, "os.example.vn");
  EXPECT_EQ(c.host_bucket, "os.example.vn");
  EXPECT_TRUE(c.use_https);
  EXPECT_EQ(c.bucket_location, "vn-north-1");
  EXPECT_EQ(c.Region(), "vn-north-1");
  EXPECT_EQ(c.ca_certs_file, "/etc/ssl/certs/lab-ca.pem");
  EXPECT_TRUE(c.check_ssl_certificate);
  EXPECT_TRUE(c.HasCredentials());
  EXPECT_TRUE(c.PathStyle());
}

TEST(s3cfg_parse, DefaultsOfAnEmptyFile)
{
  S3Config c = MustParse("");
  EXPECT_TRUE(c.access_key.empty());
  EXPECT_TRUE(c.secret_key.empty());
  EXPECT_EQ(c.host_base, "s3.amazonaws.com");
  EXPECT_EQ(c.host_bucket, "%(bucket)s.s3.amazonaws.com");
  EXPECT_TRUE(c.use_https);
  EXPECT_EQ(c.bucket_location, "US");
  EXPECT_EQ(c.Region(), "us-east-1");
  EXPECT_TRUE(c.ca_certs_file.empty());
  EXPECT_TRUE(c.check_ssl_certificate);
  EXPECT_FALSE(c.world_readable);
  EXPECT_FALSE(c.HasCredentials());
  EXPECT_FALSE(c.PathStyle());
}

TEST(s3cfg_parse, MissingFieldsKeepTheirDefaults)
{
  S3Config c = MustParse("[default]\naccess_key = A\nsecret_key = B\n");
  EXPECT_TRUE(c.HasCredentials());
  EXPECT_EQ(c.host_base, "s3.amazonaws.com");
  EXPECT_TRUE(c.use_https);
}

TEST(s3cfg_parse, OneCredentialIsNotEnough)
{
  EXPECT_FALSE(MustParse("access_key = A\n").HasCredentials());
  EXPECT_FALSE(MustParse("secret_key = B\n").HasCredentials());
  EXPECT_FALSE(MustParse("access_key =\nsecret_key = B\n").HasCredentials());
}

TEST(s3cfg_parse, CommentsBlankLinesAndWhitespace)
{
  S3Config c = MustParse(
      "\n# comment\n; other comment\n   \n[default]\n"
      "   access_key   =   A1  \n"
      "\tsecret_key\t=\tB2\t\n"
      "  # indented comment\n"
      "host_base=h.example\n");
  EXPECT_EQ(c.access_key, "A1");
  EXPECT_EQ(c.secret_key, "B2");
  EXPECT_EQ(c.host_base, "h.example");
}

TEST(s3cfg_parse, CrLfFileAndByteOrderMark)
{
  S3Config c = MustParse(
      "\xEF\xBB\xBF[default]\r\naccess_key = A\r\nsecret_key = B\r\n"
      "host_base = h\r\nuse_https = False\r\n");
  EXPECT_EQ(c.access_key, "A");
  EXPECT_EQ(c.secret_key, "B");
  EXPECT_EQ(c.host_base, "h");
  EXPECT_FALSE(c.use_https);
}

TEST(s3cfg_parse, LastLineWithoutNewline)
{ EXPECT_EQ(MustParse("[default]\nsecret_key = tail").secret_key, "tail"); }

TEST(s3cfg_parse, ColonDelimiterNamesAreCaseInsensitiveLastValueWins)
{
  S3Config c = MustParse("ACCESS_KEY: first\nAccess_Key = second\n");
  EXPECT_EQ(c.access_key, "second");
}

TEST(s3cfg_parse, ValueKeepsInnerEqualsSignsAndHashes)
{
  S3Config c = MustParse("secret_key = ab=cd#ef;gh\n");
  EXPECT_EQ(c.secret_key, "ab=cd#ef;gh");
}

TEST(s3cfg_parse, OnlyTheDefaultSectionCounts)
{
  S3Config c = MustParse(
      "[other]\naccess_key = WRONG\n[default]\naccess_key = RIGHT\n"
      "[third]\nsecret_key = WRONG\n");
  EXPECT_EQ(c.access_key, "RIGHT");
  EXPECT_TRUE(c.secret_key.empty());
}

TEST(s3cfg_parse, KeysBeforeAnySectionAreAccepted)
{ EXPECT_EQ(MustParse("access_key = A\n[other]\nx = y\n").access_key, "A"); }

TEST(s3cfg_parse, UnknownKeysAreIgnored)
{
  S3Config c = MustParse(
      "[default]\nproxy_host = p\nmultipart_chunk_size_mb = 15\n"
      "signature_v2 = False\naccess_key = A\n");
  EXPECT_EQ(c.access_key, "A");
}

TEST(s3cfg_parse, BooleanSpellings)
{
  for (const char* t : {"True", "true", "YES", "on", "1"}) {
    EXPECT_TRUE(MustParse(std::string("use_https = ") + t + "\n").use_https)
        << t;
  }
  for (const char* f : {"False", "false", "No", "OFF", "0"}) {
    EXPECT_FALSE(MustParse(std::string("use_https = ") + f + "\n").use_https)
        << f;
  }
  S3Config c = MustParse("check_ssl_certificate = no\n");
  EXPECT_FALSE(c.check_ssl_certificate);
  EXPECT_TRUE(c.use_https);
}

TEST(s3cfg_parse, RegionMapping)
{
  EXPECT_EQ(MustParse("bucket_location = US\n").Region(), "us-east-1");
  EXPECT_EQ(MustParse("bucket_location = EU\n").Region(), "eu-west-1");
  EXPECT_EQ(MustParse("bucket_location = \n").Region(), "us-east-1");
  EXPECT_EQ(MustParse("bucket_location = ap-southeast-1\n").Region(),
            "ap-southeast-1");
}

TEST(s3cfg_parse, HostBucketDecidesTheAddressingStyle)
{
  EXPECT_FALSE(
      MustParse("host_bucket = %(bucket)s.os.example.vn\n").PathStyle());
  EXPECT_TRUE(MustParse("host_bucket = os.example.vn\n").PathStyle());
}

TEST(s3cfg_parse, MalformedLinesAreErrors)
{
  EXPECT_FALSE(ParseS3cfg("[default\naccess_key = A\n").has_value());
  EXPECT_FALSE(
      ParseS3cfg("[default]\nthis line has no delimiter\n").has_value());
  EXPECT_FALSE(ParseS3cfg("use_https = maybe\n").has_value());
  EXPECT_FALSE(ParseS3cfg("check_ssl_certificate = 2\n").has_value());
  EXPECT_FALSE(ParseS3cfg("host_base = \n").has_value());
  EXPECT_FALSE(ParseS3cfg("host_bucket =\n").has_value());
}

TEST(s3cfg_parse, ErrorsNameTheLineButNeverTheValue)
{
  auto c = ParseS3cfg(
      "[default]\nsecret_key = TOPSECRETVALUE\nuse_https = TOPSECRETBOOL\n");
  ASSERT_FALSE(c.has_value());
  EXPECT_NE(c.error().find("line 3"), std::string::npos);
  EXPECT_NE(c.error().find("use_https"), std::string::npos);
  EXPECT_EQ(c.error().find("TOPSECRET"), std::string::npos);

  c = ParseS3cfg("secret_key = TOPSECRETVALUE\nbroken line TOPSECRET\n");
  ASSERT_FALSE(c.has_value());
  EXPECT_EQ(c.error().find("TOPSECRET"), std::string::npos);
}

TEST(s3cfg_parse, EmptyHostBaseErrorNamesTheKey)
{
  auto c = ParseS3cfg("host_base = \n");
  ASSERT_FALSE(c.has_value());
  EXPECT_NE(c.error().find("host_base"), std::string::npos);
}

class s3cfg_file : public ::testing::Test {
 protected:
  void SetUp() override
  {
    dir_ = fs::temp_directory_path()
           / ("s3cfg_reader_test_" + std::to_string(getpid()));
    fs::create_directories(dir_);
  }
  void TearDown() override
  {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  std::string Write(const std::string& name,
                    const std::string& content,
                    mode_t mode)
  {
    fs::path p = dir_ / name;
    {
      std::ofstream out(p, std::ios::binary);
      out << content;
    }
    chmod(p.c_str(), mode);
    return p.string();
  }
  fs::path dir_;
};

TEST_F(s3cfg_file, ReadsAPrivateFile)
{
  auto c = ReadS3cfgFile(Write("private.cfg", kTypicalCfg, 0600));
  ASSERT_TRUE(c.has_value()) << c.error();
  EXPECT_EQ(c->access_key, "AKTESTACCESS1");
  EXPECT_FALSE(c->world_readable);
}

TEST_F(s3cfg_file, GroupReadableIsNotWorldReadable)
{
  auto c = ReadS3cfgFile(Write("group.cfg", kTypicalCfg, 0640));
  ASSERT_TRUE(c.has_value()) << c.error();
  EXPECT_FALSE(c->world_readable);
}

TEST_F(s3cfg_file, WorldReadableFileIsFlagged)
{
  auto c = ReadS3cfgFile(Write("world.cfg", kTypicalCfg, 0644));
  ASSERT_TRUE(c.has_value()) << c.error();
  EXPECT_TRUE(c->world_readable);
  EXPECT_TRUE(c->HasCredentials());
}

TEST_F(s3cfg_file, MissingFileAndDirectoryAreErrors)
{
  auto missing = ReadS3cfgFile((dir_ / "nope.cfg").string());
  ASSERT_FALSE(missing.has_value());
  EXPECT_NE(missing.error().find("nope.cfg"), std::string::npos);
  EXPECT_FALSE(ReadS3cfgFile(dir_.string()).has_value());
  EXPECT_FALSE(ReadS3cfgFile("").has_value());
}

TEST_F(s3cfg_file, OversizedFileIsRefused)
{
  std::string big = "[default]\n# " + std::string(2 * 1024 * 1024, 'x') + "\n";
  EXPECT_FALSE(ReadS3cfgFile(Write("big.cfg", big, 0600)).has_value());
}

TEST_F(s3cfg_file, ParseErrorNamesTheFileButNotTheValue)
{
  auto c = ReadS3cfgFile(
      Write("bad.cfg", "secret_key = TOPSECRETVALUE\nuse_https = ???\n", 0600));
  ASSERT_FALSE(c.has_value());
  EXPECT_NE(c.error().find("bad.cfg"), std::string::npos);
  EXPECT_EQ(c.error().find("TOPSECRET"), std::string::npos);
}

TEST_F(s3cfg_file, EmptyFileGivesDefaultsWithoutCredentials)
{
  auto c = ReadS3cfgFile(Write("empty.cfg", "", 0600));
  ASSERT_TRUE(c.has_value()) << c.error();
  EXPECT_FALSE(c->HasCredentials());
}
