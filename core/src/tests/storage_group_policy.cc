/**
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

/* The storage group policy: name lookup, the Pool-then-Job-then-default
 * resolver, the Enabled filter, and ListedOrder's order. LeastUsed needs a
 * running Director and is covered by the system tests.
 *
 * Every test reparses the configuration, so a test may mutate a resource
 * (to disable a storage, or to move one to another daemon) without
 * affecting the next one. */

#include "testing_dir_common.h"

#include "dird/dird_conf.h"
#include "dird/director_jcr_impl.h"
#include "dird/jcr_util.h"
#include "dird/storage.h"
#include "dird/storage_group_policy.h"
#include "include/jcr.h"

using namespace directordaemon;

namespace {
constexpr const char* kConfig
    = "configs/bareos-configparser-tests/bareos-dir-StorageGroupPolicy.conf";

void Test_FreeJcr(JobControlRecord* jcr) { FreeJcr(jcr); }

using JcrPtr = std::unique_ptr<JobControlRecord, decltype(&Test_FreeJcr)>;

JobResource* GetJob(const char* name)
{
  return dynamic_cast<JobResource*>(my_config->GetResWithName(R_JOB, name));
}

PoolResource* GetPool(const char* name)
{
  return dynamic_cast<PoolResource*>(my_config->GetResWithName(R_POOL, name));
}

StorageResource* GetStorage(const char* name)
{
  return dynamic_cast<StorageResource*>(
      my_config->GetResWithName(R_STORAGE, name));
}

const char* NameAt(alist<StorageResource*>* list, int index)
{
  auto* store = static_cast<StorageResource*>(list->get(index));
  return store ? store->resource_name_ : "";
}

/* Build a job control record whose write storage list is the Job's Storage
 * list, which is the state DoNativeBackupInit hands to the policy. */
JcrPtr MakeJcrWithGroup(PConfigParser& config, const char* job_name)
{
  JcrPtr jcr(NewDirectorJcr(config->GetCurrentConfiguration()), &Test_FreeJcr);
  if (!jcr) { return jcr; }

  JobResource* job = GetJob(job_name);
  if (!job) { return jcr; }

  jcr->dir_impl->res.job = job;
  jcr->dir_impl->res.pool = job->pool;
  CopyWstorage(jcr.get(), job->storage, "Job resource");

  return jcr;
}
}  // namespace

/* ------------------------------------------------------------------ */
/* Name lookup                                                         */
/* ------------------------------------------------------------------ */

TEST(StorageGroupPolicy, NameLookupKnowsTheImplementedPolicies)
{
  EXPECT_EQ(StorageGroupPolicyFromName("ListedOrder"),
            StorageGroupPolicyType::kListedOrder);
  EXPECT_EQ(StorageGroupPolicyFromName("LeastUsed"),
            StorageGroupPolicyType::kLeastUsed);
}

/* The configuration validator uses Bstrcasecmp, so the lookup must agree
 * with it or a config that passes -t would resolve to the default. */
TEST(StorageGroupPolicy, NameLookupIsCaseInsensitive)
{
  EXPECT_EQ(StorageGroupPolicyFromName("listedorder"),
            StorageGroupPolicyType::kListedOrder);
  EXPECT_EQ(StorageGroupPolicyFromName("LEASTUSED"),
            StorageGroupPolicyType::kLeastUsed);
}

/* An unknown name cannot reach here through a valid config, but must not be
 * an error if it does: the name was already validated at config time. */
TEST(StorageGroupPolicy, NameLookupFallsBackToTheDefault)
{
  EXPECT_EQ(StorageGroupPolicyFromName(nullptr),
            StorageGroupPolicyType::kListedOrder);
  EXPECT_EQ(StorageGroupPolicyFromName("NoSuchPolicy"),
            StorageGroupPolicyType::kListedOrder);
}

/* ------------------------------------------------------------------ */
/* Resolver precedence                                                 */
/* ------------------------------------------------------------------ */

/* The Pool wins, as both directive descriptions say. */
TEST(StorageGroupPolicy, ResolverPrefersThePoolOverTheJob)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JobResource* job = GetJob("job-with-policy");
  PoolResource* pool = GetPool("pool-with-policy");
  ASSERT_NE(job, nullptr);
  ASSERT_NE(pool, nullptr);
  ASSERT_STREQ(job->storage_group_policy, "ListedOrder");
  ASSERT_STREQ(pool->storage_group_policy, "LeastUsed");

  const char* name = nullptr;
  EXPECT_EQ(ResolveStorageGroupPolicy(job, pool, &name),
            StorageGroupPolicyType::kLeastUsed);
  EXPECT_STREQ(name, "LeastUsed");
}

TEST(StorageGroupPolicy, ResolverFallsBackToTheJob)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JobResource* job = GetJob("job-with-policy");
  PoolResource* pool = GetPool("pool-with-policy");
  ASSERT_NE(job, nullptr);
  ASSERT_NE(pool, nullptr);

  /* a Pool that declares nothing must not mask the Job's choice */
  pool->storage_group_policy = nullptr;

  const char* name = nullptr;
  EXPECT_EQ(ResolveStorageGroupPolicy(job, pool, &name),
            StorageGroupPolicyType::kListedOrder);
  EXPECT_STREQ(name, "ListedOrder");

  /* no Pool at all takes the same branch */
  EXPECT_EQ(ResolveStorageGroupPolicy(job, nullptr, nullptr),
            StorageGroupPolicyType::kListedOrder);
}

TEST(StorageGroupPolicy, ResolverFallsBackToTheDefault)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JobResource* job = GetJob("job-without-policy");
  ASSERT_NE(job, nullptr);
  ASSERT_EQ(job->storage_group_policy, nullptr);

  const char* name = nullptr;
  EXPECT_EQ(ResolveStorageGroupPolicy(job, nullptr, &name),
            StorageGroupPolicyType::kListedOrder);
  EXPECT_STREQ(name, "ListedOrder");

  /* and with nothing at all */
  EXPECT_EQ(ResolveStorageGroupPolicy(nullptr, nullptr, nullptr),
            StorageGroupPolicyType::kListedOrder);
}

/* ------------------------------------------------------------------ */
/* ApplyStorageGroupPolicy                                             */
/* ------------------------------------------------------------------ */

/* A job without a group must not be touched at all: this is what keeps the
 * change a no-op for every existing single-storage installation. */
TEST(StorageGroupPolicy, ApplyIsANoOpForASingleMemberList)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JcrPtr jcr = MakeJcrWithGroup(director_config, "job-without-policy");
  ASSERT_NE(jcr.get(), nullptr);
  ASSERT_EQ(jcr->dir_impl->res.write_storage_list->size(), 1);

  EXPECT_EQ(ApplyStorageGroupPolicy(jcr.get()), 1);

  EXPECT_EQ(jcr->dir_impl->res.write_storage_list->size(), 1);
  EXPECT_STREQ(jcr->dir_impl->res.write_storage->resource_name_, "storage01");
}

/* ListedOrder is the default and reorders nothing, so the configured order
 * and the current pointer must both survive a full pass through filtering
 * and list rebuilding. */
TEST(StorageGroupPolicy, ApplyListedOrderKeepsTheConfiguredOrder)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JcrPtr jcr = MakeJcrWithGroup(director_config, "job-with-policy");
  ASSERT_NE(jcr.get(), nullptr);
  ASSERT_EQ(jcr->dir_impl->res.write_storage_list->size(), 2);

  EXPECT_EQ(ApplyStorageGroupPolicy(jcr.get()), 2);

  auto* list = jcr->dir_impl->res.write_storage_list;
  ASSERT_EQ(list->size(), 2);
  EXPECT_STREQ(NameAt(list, 0), "storage01");
  EXPECT_STREQ(NameAt(list, 1), "storage02");
  EXPECT_STREQ(jcr->dir_impl->res.write_storage->resource_name_, "storage01");
}

/* Enabled = no on a Storage takes it out of every group it belongs to: the
 * policy drops a disabled member before choosing. */
TEST(StorageGroupPolicy, ApplyDropsADisabledMember)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JcrPtr jcr = MakeJcrWithGroup(director_config, "job-with-policy");
  ASSERT_NE(jcr.get(), nullptr);
  ASSERT_EQ(jcr->dir_impl->res.write_storage_list->size(), 2);

  StorageResource* first = GetStorage("storage01");
  ASSERT_NE(first, nullptr);
  first->enabled = false;

  EXPECT_EQ(ApplyStorageGroupPolicy(jcr.get()), 1);

  auto* list = jcr->dir_impl->res.write_storage_list;
  ASSERT_EQ(list->size(), 1);
  EXPECT_STREQ(NameAt(list, 0), "storage02");
  EXPECT_STREQ(jcr->dir_impl->res.write_storage->resource_name_, "storage02");
}

/* Filtering must never empty the list. A configuration mistake that
 * disables every member is a warning, not a failed job. */
TEST(StorageGroupPolicy, ApplyKeepsTheListWhenEveryMemberIsFiltered)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JcrPtr jcr = MakeJcrWithGroup(director_config, "job-with-policy");
  ASSERT_NE(jcr.get(), nullptr);

  GetStorage("storage01")->enabled = false;
  GetStorage("storage02")->enabled = false;

  EXPECT_EQ(ApplyStorageGroupPolicy(jcr.get()), 2);

  auto* list = jcr->dir_impl->res.write_storage_list;
  ASSERT_EQ(list->size(), 2);
  EXPECT_STREQ(NameAt(list, 0), "storage01");
  EXPECT_STREQ(NameAt(list, 1), "storage02");
}

/* A disabled first member is dropped, even on another Storage Daemon, and
 * the other member is kept. */
TEST(StorageGroupPolicy, DisabledFirstMemberIsDroppedWhicheverDaemonItIsOn)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JcrPtr jcr = MakeJcrWithGroup(director_config, "job-with-policy");
  ASSERT_NE(jcr.get(), nullptr);
  ASSERT_EQ(jcr->dir_impl->res.write_storage_list->size(), 2);

  StorageResource* first = GetStorage("storage01");
  StorageResource* second = GetStorage("storage02");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  first->enabled = false;
  second->SDport = first->SDport + 1;

  EXPECT_EQ(ApplyStorageGroupPolicy(jcr.get()), 1);

  auto* list = jcr->dir_impl->res.write_storage_list;
  ASSERT_EQ(list->size(), 1);
  EXPECT_STREQ(NameAt(list, 0), "storage02");
  EXPECT_STREQ(jcr->dir_impl->res.write_storage->resource_name_, "storage02");
}

/* A group may span Storage Daemons: every member is kept. */
TEST(StorageGroupPolicy, CrossDaemonGroupKeepsEveryMember)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JcrPtr jcr = MakeJcrWithGroup(director_config, "job-with-policy");
  ASSERT_NE(jcr.get(), nullptr);
  ASSERT_EQ(jcr->dir_impl->res.write_storage_list->size(), 2);

  StorageResource* first = GetStorage("storage01");
  StorageResource* second = GetStorage("storage02");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  /* both enabled, neither on the other's daemon */
  second->SDport = first->SDport + 1;

  EXPECT_EQ(ApplyStorageGroupPolicy(jcr.get()), 2);

  auto* list = jcr->dir_impl->res.write_storage_list;
  ASSERT_EQ(list->size(), 2);
  EXPECT_STREQ(NameAt(list, 0), "storage01");
  EXPECT_STREQ(NameAt(list, 1), "storage02")
      << "the second daemon's member is the one failover exists to reach";
}

TEST(StorageGroupPolicy, ApplyRejectsMissingInputs)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  EXPECT_EQ(ApplyStorageGroupPolicy(nullptr), 0);

  /* a job control record with no write storage list yet */
  JcrPtr jcr(NewDirectorJcr(director_config->GetCurrentConfiguration()),
             &Test_FreeJcr);
  ASSERT_NE(jcr.get(), nullptr);
  ASSERT_EQ(jcr->dir_impl->res.write_storage_list, nullptr);
  EXPECT_EQ(ApplyStorageGroupPolicy(jcr.get()), 0);
}
