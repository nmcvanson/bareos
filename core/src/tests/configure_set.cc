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

#include "testing_dir_common.h"

#include "dird/ua.h"
#include "dird/ua_input.h"
#include "include/jcr.h"
#include "dird/ua_configure.cc"

#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdlib.h>

namespace fs = std::filesystem;

namespace {

bool CaptureSink(void* ctx, const char* fmt, ...) PRINTF_LIKE(2, 3);
bool CaptureSink(void* ctx, const char* fmt, ...)
{
  PoolMem buf(PM_MESSAGE);
  va_list ap;
  va_start(ap, fmt);
  buf.Bvsprintf(fmt, ap);
  va_end(ap);
  *static_cast<std::string*>(ctx) += buf.c_str();
  return true;
}

std::string Slurp(const fs::path& p)
{
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void Spit(const fs::path& p, const std::string& s)
{
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << s;
}

const char* kJobFile
    = "# managed-by: bareos-configure\n"
      "Job {\n"
      "  Name = web-1\n"
      "  Type = Backup\n"
      "  Client = client1\n"
      "  FileSet = fileset1\n"
      "  Pool = pool1\n"
      "  Messages = Standard\n"
      "  Storage = storage01\n"
      "}\n";

// Runs one console line through ConfigureCmd with captured output.
bool RunLine(const char* line,
             std::string& out,
             const std::function<void(directordaemon::UaContext&)>& prep
             = nullptr)
{
  JobControlRecord jcr{};
  directordaemon::UaContext ua(&jcr);
  out.clear();
  if (prep) { prep(ua); }
  ua.send = std::make_unique<OutputFormatter>(CaptureSink, &out, nullptr,
                                              nullptr);
  PmStrcpy(ua.cmd, line);
  ParseUaArgs(&ua);
  bool r = ConfigureCmd(&ua, line);
  ua.send->SendBuffer();
  return r;
}

class ConfigureSetTest : public ::testing::Test {
 protected:
  void SetUp() override
  {
    InitDirGlobals();
    char tmpl[] = "/tmp/cfgset-XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    root = tmpl;
    d = root / "bareos-dir.d";
    Spit(d / "director/bareos-dir.conf",
         "Director {\n  Name = bareos-dir\n  Password = secret\n"
         "  Messages = Standard\n}\n");
    Spit(d / "messages/Standard.conf",
         "Messages {\n  Name = Standard\n  console = all\n}\n");
    for (const char* s : {"storage01", "storage02"}) {
      Spit(d / "storage" / (std::string(s) + ".conf"),
           std::string("Storage {\n  Name = ") + s
               + "\n  Address = localhost\n  Password = secret\n"
                 "  Device = dev1\n  MediaType = Type\n}\n");
    }
    Spit(d / "client/client1.conf",
         "Client {\n  Name = client1\n  Address = localhost\n"
         "  Password = secret\n}\n");
    Spit(d / "fileset/fileset1.conf",
         "FileSet {\n  Name = fileset1\n  Include {\n    File = /tmp\n  }\n}\n");
    Spit(d / "pool/pool1.conf",
         "Pool {\n  Name = pool1\n  Pool Type = Backup\n}\n");
    Spit(d / "job/web-1.conf", kJobFile);
    Spit(d / "job/web-2.conf", std::string(kJobFile).replace(
                                   std::string(kJobFile).find("web-1"), 5,
                                   "web-2"));
    Spit(d / "job/hand.conf",
         "Job {\n  Name = hand\n  Type = Backup\n  Client = client1\n"
         "  FileSet = fileset1\n  Pool = pool1\n  Messages = Standard\n"
         "  Storage = storage01\n}\n");
    config = DirectorPrepareResources(root.string());
    ASSERT_NE(config.get(), nullptr);
    directordaemon::config_change_wait_ms = 300;
  }

  void TearDown() override
  {
    config.reset();
    directordaemon::my_config = nullptr;
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  bool Run(const char* line,
           const std::function<void(directordaemon::UaContext&)>& prep
           = nullptr)
  {
    return RunLine(line, out, prep);
  }

  fs::path root, d;
  std::string out;
  PConfigParser config;
};

}  // namespace

TEST_F(ConfigureSetTest, PreviewWritesNothing)
{
  EXPECT_TRUE(
      Run("configure set job=web-1 storagegrouppolicy=leastused preview"));
  EXPECT_NE(out.find("LeastUsed"), std::string::npos) << out;
  EXPECT_EQ(Slurp(d / "job/web-1.conf"), kJobFile);
  EXPECT_FALSE(fs::exists(d / "job/web-1.conf.tmp"));
}

TEST_F(ConfigureSetTest, RefusalsWriteNothing)
{
  const std::string before = Slurp(d / "job/web-1.conf");
  // no marker in the file
  EXPECT_FALSE(Run("configure set job=hand storagegrouppolicy=LeastUsed"));
  EXPECT_FALSE(Run("configure set job=web-1 storagegrouppolicy=Nope"));
  EXPECT_FALSE(Run("configure set job=web-1 storage=storage01,missing"));
  EXPECT_FALSE(Run("configure set job=nojob storagegrouppolicy=LeastUsed"));
  EXPECT_FALSE(Run("configure set job=../x storagegrouppolicy=LeastUsed"));
  EXPECT_FALSE(Run("configure set job=web-1 priority=5"));
  EXPECT_FALSE(Run("configure set storage=storage01 enabled=maybe"));
  EXPECT_FALSE(Run("configure set job=web-1 storagegrouppolicy="));
  EXPECT_EQ(Slurp(d / "job/web-1.conf"), before);
}

TEST_F(ConfigureSetTest, SymlinkedIncludeDirectoryRefused)
{
  fs::path real = root / "real-include";
  fs::rename(d, real);
  fs::create_directory_symlink(real, d);
  EXPECT_FALSE(Run("configure set job=web-1 storagegrouppolicy=LeastUsed"));
  EXPECT_EQ(Slurp(real / "job/web-1.conf"), kJobFile);
}

TEST_F(ConfigureSetTest, SymlinkedFileRefused)
{
  fs::path target = root / "target.conf";
  fs::rename(d / "job/web-1.conf", target);
  fs::create_symlink(target, d / "job/web-1.conf");
  EXPECT_FALSE(Run("configure set job=web-1 storagegrouppolicy=LeastUsed"));
  EXPECT_EQ(Slurp(target), kJobFile);
  EXPECT_TRUE(fs::is_symlink(d / "job/web-1.conf"));
}

TEST_F(ConfigureSetTest, DotDotInNameRefused)
{
  EXPECT_FALSE(Run("configure set job=web..1 storagegrouppolicy=LeastUsed"));
}

TEST_F(ConfigureSetTest, ExistingTmpIsKeptAndStopsTheCommand)
{
  Spit(d / "job/web-1.conf.tmp", "keep me");
  EXPECT_FALSE(Run("configure set job=web-1 storagegrouppolicy=LeastUsed"));
  EXPECT_EQ(Slurp(d / "job/web-1.conf.tmp"), "keep me");
  EXPECT_EQ(Slurp(d / "job/web-1.conf"), kJobFile);
}

TEST_F(ConfigureSetTest, FailedReloadRestoresTheFile)
{
  // A broken file elsewhere in the tree makes the reload fail.
  Spit(d / "pool/bad.conf", "Pool {\n  Name = bad\n  Bogus = 1\n}\n");
  EXPECT_FALSE(Run("configure set job=web-1 storagegrouppolicy=LeastUsed"));
  EXPECT_EQ(Slurp(d / "job/web-1.conf"), kJobFile);
  EXPECT_FALSE(fs::exists(d / "job/web-1.conf.tmp"));
  EXPECT_NE(out.find("restored"), std::string::npos) << out;
  EXPECT_TRUE(directordaemon::my_config->GetResWithName(
      directordaemon::R_JOB, "web-1"));
}

TEST_F(ConfigureSetTest, SetAppliesAndReloads)
{
  EXPECT_TRUE(Run("configure set job=web-1 storagegrouppolicy=leastused"));
  EXPECT_NE(out.find("Reloaded: true"), std::string::npos) << out;
  EXPECT_NE(Slurp(d / "job/web-1.conf").find("StorageGroupPolicy = LeastUsed"),
            std::string::npos);
  auto* job = static_cast<directordaemon::JobResource*>(
      directordaemon::my_config->GetResWithName(directordaemon::R_JOB,
                                                "web-1"));
  ASSERT_NE(job, nullptr);
  ASSERT_NE(job->storage_group_policy, nullptr);
  EXPECT_STREQ(job->storage_group_policy, "LeastUsed");
}

TEST_F(ConfigureSetTest, ReloadRefusedWhileChangeHoldsTheGuard)
{
  auto token = directordaemon::TryBeginConfigChange();
  ASSERT_TRUE(token);
  EXPECT_FALSE(directordaemon::TryBeginConfigChange());
  EXPECT_FALSE(directordaemon::DoReloadConfig());
  EXPECT_FALSE(Run("configure set job=web-1 storagegrouppolicy=LeastUsed"));
  EXPECT_NE(out.find("busy"), std::string::npos) << out;
  EXPECT_FALSE(Run("configure add pool name=p2"));
  token.reset();
  EXPECT_TRUE(directordaemon::TryBeginConfigChange());
}

TEST_F(ConfigureSetTest, SetWaitsForARunningChange)
{
  directordaemon::config_change_wait_ms = 5000;
  auto token = directordaemon::TryBeginConfigChange();
  ASSERT_TRUE(token);
  std::thread releaser([&token] {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    token.reset();
  });
  // Waits, then gets the guard; preview needs no reload.
  EXPECT_TRUE(
      Run("configure set job=web-1 storagegrouppolicy=LeastUsed preview"));
  releaser.join();
}

TEST_F(ConfigureSetTest, TwoSetsRunOneAfterTheOther)
{
  directordaemon::config_change_wait_ms = 10000;
  bool a = false, b = false;
  std::string oa, ob;
  std::thread t1([&] {
    a = RunLine("configure set job=web-1 storagegrouppolicy=LeastUsed preview",
                oa);
  });
  std::thread t2([&] {
    b = RunLine("configure set job=web-1 storage=storage01,storage02 preview",
                ob);
  });
  t1.join();
  t2.join();
  EXPECT_TRUE(a);
  EXPECT_TRUE(b);
}

namespace {

// Runs a line in JSON mode and returns the finalized JSON-RPC text.
std::string RunJson(const char* line, bool& ok)
{
  JobControlRecord jcr{};
  directordaemon::UaContext ua(&jcr);
  std::string out;
  ua.send = std::make_unique<OutputFormatter>(CaptureSink, &out, nullptr,
                                              nullptr);
  ua.api = API_MODE_JSON;
  ua.send->SetMode(API_MODE_JSON);
  ua.send->SetCompact(true);
  PmStrcpy(ua.cmd, line);
  ParseUaArgs(&ua);
  ok = ConfigureCmd(&ua, line);
  ua.send->FinalizeResult(ok);
  return out;
}

void SaveEnvelope(const char* name, const std::string& json)
{
  const char* dir = getenv("CONFIGURE_ENVELOPE_DIR");
  if (dir) { Spit(fs::path(dir) / name, json); }
}

}  // namespace

TEST_F(ConfigureSetTest, JsonErrorEnvelopeCarriesTheObject)
{
  Spit(d / "pool/bad.conf",
       "Pool {\n  Name = bad\n  Bogus = 1\n}\n");
  bool ok = true;
  std::string set_json
      = RunJson("configure set job=web-1 storagegrouppolicy=LeastUsed", ok);
  EXPECT_FALSE(ok);
  SaveEnvelope("set-restored.json", set_json);
  EXPECT_NE(set_json.find("\"error\""), std::string::npos) << set_json;
  EXPECT_NE(set_json.find("\"rollback\":\"restored\""), std::string::npos)
      << set_json;
}

TEST_F(ConfigureSetTest, TwoRealSetsOnDifferentFilesBothLand)
{
  directordaemon::config_change_wait_ms = 20000;
  bool a = false, b = false;
  std::string oa, ob;
  std::thread t1([&] {
    a = RunLine("configure set job=web-1 storagegrouppolicy=LeastUsed", oa);
  });
  std::thread t2([&] {
    b = RunLine("configure set job=web-2 storage=storage01,storage02", ob);
  });
  t1.join();
  t2.join();
  EXPECT_TRUE(a) << oa;
  EXPECT_TRUE(b) << ob;
  EXPECT_NE(Slurp(d / "job/web-1.conf").find("StorageGroupPolicy = LeastUsed"),
            std::string::npos);
  EXPECT_NE(Slurp(d / "job/web-2.conf").find("Storage = storage01,storage02"),
            std::string::npos);
  auto* j1 = static_cast<directordaemon::JobResource*>(
      directordaemon::my_config->GetResWithName(directordaemon::R_JOB,
                                                "web-1"));
  auto* j2 = static_cast<directordaemon::JobResource*>(
      directordaemon::my_config->GetResWithName(directordaemon::R_JOB,
                                                "web-2"));
  ASSERT_TRUE(j1 && j2);
  ASSERT_NE(j1->storage_group_policy, nullptr);
  EXPECT_STREQ(j1->storage_group_policy, "LeastUsed");
  ASSERT_NE(j2->storage, nullptr);
  EXPECT_EQ(j2->storage->size(), 2);
}

TEST_F(ConfigureSetTest, RealSetWaitsForAHeldTokenThenPublishes)
{
  directordaemon::config_change_wait_ms = 5000;
  auto token = directordaemon::TryBeginConfigChange();
  ASSERT_TRUE(token);
  std::thread releaser([&token] {
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    token.reset();
  });
  EXPECT_TRUE(Run("configure set job=web-1 storagegrouppolicy=LeastUsed"))
      << out;
  releaser.join();
  EXPECT_NE(Slurp(d / "job/web-1.conf").find("StorageGroupPolicy = LeastUsed"),
            std::string::npos);
  auto* job = static_cast<directordaemon::JobResource*>(
      directordaemon::my_config->GetResWithName(directordaemon::R_JOB,
                                                "web-1"));
  ASSERT_TRUE(job && job->storage_group_policy);
  EXPECT_STREQ(job->storage_group_policy, "LeastUsed");
}

namespace {

// A console that may use everything except what the test restricts.
void RestrictedConsole(directordaemon::UaContext& ua,
                       std::vector<std::string> jobs,
                       std::vector<std::string> storages)
{
  ua.user_acl = std::make_unique<directordaemon::UserAcl>();
  for (auto& l : ua.user_acl->acl_lists) { l = {"*all*"}; }
  ua.user_acl->acl_lists[directordaemon::Job_ACL] = jobs;
  ua.user_acl->acl_lists[directordaemon::Storage_ACL] = storages;
}

}  // namespace

TEST_F(ConfigureSetTest, RestrictedConsoleCannotSetUnpermittedJobOrStorage)
{
  const std::string before = Slurp(d / "job/web-1.conf");
  EXPECT_FALSE(Run("configure set job=web-1 storagegrouppolicy=LeastUsed",
                   [](directordaemon::UaContext& ua) {
                     RestrictedConsole(ua, {"hand"}, {"*all*"});
                   }));
  EXPECT_FALSE(Run("configure set job=web-1 storage=storage01,storage02",
                   [](directordaemon::UaContext& ua) {
                     RestrictedConsole(ua, {"*all*"}, {"storage01"});
                   }));
  EXPECT_EQ(Slurp(d / "job/web-1.conf"), before);
  // The same console may set what it is permitted to.
  EXPECT_TRUE(Run("configure set job=web-1 storagegrouppolicy=LeastUsed "
                  "preview",
                  [](directordaemon::UaContext& ua) {
                    RestrictedConsole(ua, {"web-1"}, {"*all*"});
                  }))
      << out;
}

namespace {
std::string g_original_file;
bool FailRestore(const std::string&, const std::string& data)
{
  return data == g_original_file;
}
}  // namespace

TEST_F(ConfigureSetTest, RollbackFailedIsReportedWithThePath)
{
  Spit(d / "pool/bad.conf",
       "Pool {\n  Name = bad\n  Bogus = 1\n}\n");
  g_original_file = Slurp(d / "job/web-1.conf");
  directordaemon::ConfigureWriteFailHook = FailRestore;
  EXPECT_FALSE(Run("configure set job=web-1 storagegrouppolicy=LeastUsed"));
  directordaemon::ConfigureWriteFailHook = nullptr;
  EXPECT_NE(out.find("Reloaded: false"), std::string::npos) << out;
  EXPECT_NE(out.find("Rollback: failed"), std::string::npos) << out;
  EXPECT_NE(out.find("ROLLBACK FAILED"), std::string::npos) << out;
  EXPECT_NE(out.find((d / "job/web-1.conf").string()), std::string::npos)
      << out;
  // The restore could not run, so the published text is still on disk.
  EXPECT_NE(Slurp(d / "job/web-1.conf").find("StorageGroupPolicy = LeastUsed"),
            std::string::npos);
  EXPECT_FALSE(fs::exists(d / "job/web-1.conf.tmp"));
}

TEST(ConfigChangeTokenTest, MovedFromTokenIsRejectedAndReleaseHappensOnce)
{
  auto first = directordaemon::TryBeginConfigChange();
  ASSERT_TRUE(first);
  EXPECT_FALSE(directordaemon::TryBeginConfigChange());
  {
    directordaemon::ConfigChangeToken owner(std::move(*first));
    EXPECT_FALSE(first->active());
    EXPECT_TRUE(owner.active());
    EXPECT_FALSE(directordaemon::ReloadConfigHeld(*first));
    // The moved-from token must not end the change on destruction or reset.
    first.reset();
    EXPECT_FALSE(directordaemon::TryBeginConfigChange());
  }
  auto again = directordaemon::TryBeginConfigChange();
  EXPECT_TRUE(again);
  EXPECT_FALSE(directordaemon::TryBeginConfigChange());
  again.reset();
  EXPECT_TRUE(directordaemon::TryBeginConfigChange());
}

TEST(ConfigChangeTokenTest, MoveAssignmentTransfersOwnership)
{
  auto a = directordaemon::TryBeginConfigChange();
  ASSERT_TRUE(a);
  directordaemon::ConfigChangeToken target(std::move(*a));
  directordaemon::ConfigChangeToken other(std::move(target));
  EXPECT_FALSE(target.active());
  EXPECT_TRUE(other.active());
  target = std::move(other);
  EXPECT_TRUE(target.active());
  EXPECT_FALSE(other.active());
  EXPECT_FALSE(directordaemon::TryBeginConfigChange());
}

