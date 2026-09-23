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

/* The write storage list is what a storage group lives in, and the storage
 * group policy reorders it. These tests pin down the two
 * primitives that build and destroy it, so that a later change to either is
 * caught here rather than in a system test.
 *
 * Note on coverage: the console run path's guard lives in ResetRestoreContext,
 * which is static in ua_run.cc and cannot be reached from a unit test. What
 * is covered here is the behaviour that guard depends on -- CopyWstorage
 * keeping every member, SetWstorage destroying all but one, and the
 * JobMayUseStorageGroup predicate the guard now consults. The guard's own
 * wiring is covered by the systemtest. */

#include "testing_dir_common.h"

#include "dird/dird_conf.h"
#include "dird/director_jcr_impl.h"
#include "dird/jcr_util.h"
#include "dird/storage.h"
#include "include/jcr.h"
#include "include/job_status.h"
#include "lib/message.h"
#include "include/protocol_types.h"
#include "include/job_types.h"
#include "include/job_level.h"

using namespace directordaemon;

/* The storage group guard's truth table, checked at compile time: only a
 * native backup that is not a VirtualFull may keep a group. */
static_assert(JobAttributesMayUseStorageGroup(JT_BACKUP, PT_NATIVE, L_FULL));
static_assert(JobAttributesMayUseStorageGroup(JT_BACKUP,
                                              PT_NATIVE,
                                              L_INCREMENTAL));
static_assert(JobAttributesMayUseStorageGroup(JT_BACKUP,
                                              PT_NATIVE,
                                              L_DIFFERENTIAL));

static_assert(!JobAttributesMayUseStorageGroup(JT_BACKUP,
                                               PT_NATIVE,
                                               L_VIRTUAL_FULL));

static_assert(!JobAttributesMayUseStorageGroup(JT_VERIFY, PT_NATIVE, L_FULL));
static_assert(!JobAttributesMayUseStorageGroup(JT_RESTORE, PT_NATIVE, L_NONE));
static_assert(!JobAttributesMayUseStorageGroup(JT_MIGRATE, PT_NATIVE, L_FULL));
static_assert(!JobAttributesMayUseStorageGroup(JT_COPY, PT_NATIVE, L_FULL));
static_assert(!JobAttributesMayUseStorageGroup(JT_CONSOLIDATE,
                                               PT_NATIVE,
                                               L_FULL));

static_assert(!JobAttributesMayUseStorageGroup(JT_BACKUP,
                                               PT_NDMP_BAREOS,
                                               L_FULL));
static_assert(!JobAttributesMayUseStorageGroup(JT_BACKUP,
                                               PT_NDMP_NATIVE,
                                               L_FULL));

/* The per-member connect timeout: the group timeout for a group, the
 * general one for a single storage. */
static_assert(StorageCandidateConnectTimeout(true, 30, 180) == 30);
static_assert(StorageCandidateConnectTimeout(false, 30, 180) == 180);

/* A group timeout of 0 means unset: the general timeout applies. */
static_assert(StorageCandidateConnectTimeout(true, 0, 180) == 180);
static_assert(StorageCandidateConnectTimeout(false, 0, 180) == 180);

/* The device recorded for the bootstrap file: a real name, never the Just
 * In Time placeholder or an empty reply. */
static_assert(ReservedDeviceIsKnown("FileStorage"));
static_assert(!ReservedDeviceIsKnown("JustInTime Device"));
static_assert(!ReservedDeviceIsKnown(""));

namespace {
constexpr const char* kConfig
    = "configs/bareos-configparser-tests/bareos-dir-StorageGroupPolicy.conf";

void Test_FreeJcr(JobControlRecord* jcr) { FreeJcr(jcr); }

using JcrPtr = std::unique_ptr<JobControlRecord, decltype(&Test_FreeJcr)>;

JobResource* GetJob(const char* name)
{
  return dynamic_cast<JobResource*>(my_config->GetResWithName(R_JOB, name));
}

StorageResource* GetStorage(const char* name)
{
  return dynamic_cast<StorageResource*>(
      my_config->GetResWithName(R_STORAGE, name));
}
}  // namespace

/* A Job declaring "Storage = storage01, storage02" must reach the job control
 * record with both members present, and write_storage pointing at the first.
 * This is the state the storage group policy reorders. */
TEST(StorageGroupList, CopyWstorageKeepsEveryMember)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JcrPtr jcr(NewDirectorJcr(director_config->GetCurrentConfiguration()),
             &Test_FreeJcr);
  ASSERT_NE(jcr.get(), nullptr);

  JobResource* job = GetJob("job-with-policy");
  ASSERT_NE(job, nullptr);
  ASSERT_NE(job->storage, nullptr);
  ASSERT_EQ(job->storage->size(), 2);

  CopyWstorage(jcr.get(), job->storage, "Job resource");

  ASSERT_NE(jcr->dir_impl->res.write_storage_list, nullptr);
  EXPECT_EQ(jcr->dir_impl->res.write_storage_list->size(), 2);

  ASSERT_NE(jcr->dir_impl->res.write_storage, nullptr);
  EXPECT_STREQ(jcr->dir_impl->res.write_storage->resource_name_, "storage01")
      << "write_storage must start on the first listed member";
  EXPECT_STREQ(jcr->dir_impl->res.wstore_source, "Job resource");
}

/* SetWstorage is destructive: it frees the whole list before assigning. This
 * is why the console run path has to be guarded -- calling it with a
 * defaulted storage silently reduced a group of two to one. */
TEST(StorageGroupList, SetWstorageCollapsesTheList)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JcrPtr jcr(NewDirectorJcr(director_config->GetCurrentConfiguration()),
             &Test_FreeJcr);
  ASSERT_NE(jcr.get(), nullptr);

  JobResource* job = GetJob("job-with-policy");
  ASSERT_NE(job, nullptr);

  CopyWstorage(jcr.get(), job->storage, "Job resource");
  ASSERT_EQ(jcr->dir_impl->res.write_storage_list->size(), 2);

  StorageResource* second = dynamic_cast<StorageResource*>(
      my_config->GetResWithName(R_STORAGE, "storage02"));
  ASSERT_NE(second, nullptr);

  UnifiedStorageResource ustore;
  ustore.store = second;
  PmStrcpy(ustore.store_source, "command line");

  SetWstorage(jcr.get(), &ustore);

  EXPECT_EQ(jcr->dir_impl->res.write_storage_list->size(), 1)
      << "SetWstorage frees the list before assigning";
  EXPECT_STREQ(jcr->dir_impl->res.write_storage->resource_name_, "storage02");
}

/* A Job listing a single storage is not a group. The run path guard must leave
 * this case on the original code path, so confirm the list really is 1. */
TEST(StorageGroupList, SingleStorageJobIsNotAGroup)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JcrPtr jcr(NewDirectorJcr(director_config->GetCurrentConfiguration()),
             &Test_FreeJcr);
  ASSERT_NE(jcr.get(), nullptr);

  JobResource* job = GetJob("job-without-policy");
  ASSERT_NE(job, nullptr);
  ASSERT_NE(job->storage, nullptr);

  CopyWstorage(jcr.get(), job->storage, "Job resource");

  ASSERT_NE(jcr->dir_impl->res.write_storage_list, nullptr);
  EXPECT_EQ(jcr->dir_impl->res.write_storage_list->size(), 1);
  EXPECT_STREQ(jcr->dir_impl->res.write_storage->resource_name_, "storage01");
}

/* JobMayUseStorageGroup decides whether ResetRestoreContext keeps a group
 * alive or collapses it the way it always did. Only a native backup may keep
 * one: it is the only job type that reaches the policy, and the only one that
 * rebuilds its write list from the Pool afterwards.
 *
 * Verify is the case that made this necessary. Its read storage list is never
 * rebuilt, so a surviving group would switch it from the Pool's storage to the
 * Job's -- SetJcrDefaults prefers the Job, GetJobStorage prefers the Pool. */
TEST(StorageGroupList, OnlyNativeBackupMayKeepAGroup)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JcrPtr jcr(NewDirectorJcr(director_config->GetCurrentConfiguration()),
             &Test_FreeJcr);
  ASSERT_NE(jcr.get(), nullptr);

  jcr->setJobProtocol(PT_NATIVE);
  jcr->setJobLevel(L_FULL);

  jcr->setJobType(JT_BACKUP);
  EXPECT_TRUE(JobMayUseStorageGroup(jcr.get()));

  for (int job_type : {JT_VERIFY, JT_RESTORE, JT_MIGRATE, JT_COPY, JT_ADMIN,
                       JT_CONSOLIDATE, JT_ARCHIVE}) {
    jcr->setJobType(job_type);
    EXPECT_FALSE(JobMayUseStorageGroup(jcr.get()))
        << "job type '" << static_cast<char>(job_type)
        << "' must not keep a storage group";
  }

  /* A non-native backup does not reach DoNativeBackupInit either. */
  jcr->setJobType(JT_BACKUP);
  jcr->setJobProtocol(PT_NDMP_BAREOS);
  EXPECT_FALSE(JobMayUseStorageGroup(jcr.get()));
}

/* The wrapper reads the level from the job: every backup level may keep a
 * group except VirtualFull. */
TEST(StorageGroupList, VirtualFullMayNotKeepAGroup)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JcrPtr jcr(NewDirectorJcr(director_config->GetCurrentConfiguration()),
             &Test_FreeJcr);
  ASSERT_NE(jcr.get(), nullptr);

  jcr->setJobType(JT_BACKUP);
  jcr->setJobProtocol(PT_NATIVE);

  for (int level : {L_FULL, L_INCREMENTAL, L_DIFFERENTIAL}) {
    jcr->setJobLevel(level);
    EXPECT_TRUE(JobMayUseStorageGroup(jcr.get()))
        << "level '" << static_cast<char>(level)
        << "' is a real backup and may keep a group";
  }

  jcr->setJobLevel(L_VIRTUAL_FULL);
  EXPECT_FALSE(JobMayUseStorageGroup(jcr.get()));
}

/* A null job control record must not crash the guard. */
TEST(StorageGroupList, JobMayUseStorageGroupHandlesNull)
{
  EXPECT_FALSE(JobMayUseStorageGroup(nullptr));
}

/* SetCurrentWstorage moves write_storage within the list the job already
 * has, where SetWstorage frees the list and rebuilds it around one member.
 * The policy and the failover loop both need the non-destructive form. */
TEST(StorageGroupList, SetCurrentWstorageMovesThePointer)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JcrPtr jcr(NewDirectorJcr(director_config->GetCurrentConfiguration()),
             &Test_FreeJcr);
  ASSERT_NE(jcr.get(), nullptr);

  JobResource* job = GetJob("job-with-policy");
  ASSERT_NE(job, nullptr);
  CopyWstorage(jcr.get(), job->storage, "Job resource");
  ASSERT_EQ(jcr->dir_impl->res.write_storage_list->size(), 2);
  ASSERT_STREQ(jcr->dir_impl->res.write_storage->resource_name_, "storage01");

  StorageResource* second = GetStorage("storage02");
  ASSERT_NE(second, nullptr);

  EXPECT_TRUE(SetCurrentWstorage(jcr.get(), second));

  EXPECT_EQ(jcr->dir_impl->res.write_storage, second);
  /* the list must be untouched: same size, same members, same order */
  ASSERT_EQ(jcr->dir_impl->res.write_storage_list->size(), 2);
  EXPECT_STREQ(((StorageResource*)jcr->dir_impl->res.write_storage_list->get(0))
                   ->resource_name_,
               "storage01");
  EXPECT_STREQ(((StorageResource*)jcr->dir_impl->res.write_storage_list->get(1))
                   ->resource_name_,
               "storage02");
}

/* A storage that is not in the list must be refused, and nothing may move.
 * The failover loop relies on this to skip a candidate rather than point the
 * job at a storage the policy never approved. */
TEST(StorageGroupList, SetCurrentWstorageRefusesANonMember)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JcrPtr jcr(NewDirectorJcr(director_config->GetCurrentConfiguration()),
             &Test_FreeJcr);
  ASSERT_NE(jcr.get(), nullptr);

  JobResource* job = GetJob("job-without-policy");
  ASSERT_NE(job, nullptr);
  CopyWstorage(jcr.get(), job->storage, "Job resource");
  ASSERT_EQ(jcr->dir_impl->res.write_storage_list->size(), 1);

  StorageResource* before = jcr->dir_impl->res.write_storage;
  StorageResource* outsider = GetStorage("storage02");
  ASSERT_NE(outsider, nullptr);
  ASSERT_NE(outsider, before);

  EXPECT_FALSE(SetCurrentWstorage(jcr.get(), outsider));

  EXPECT_EQ(jcr->dir_impl->res.write_storage, before);
  EXPECT_EQ(jcr->dir_impl->res.write_storage_list->size(), 1);
}

/* A one-member list is the common case in the field and must still work:
 * selecting the only member succeeds and is a no-op. */
TEST(StorageGroupList, SetCurrentWstorageOnASingleMemberList)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JcrPtr jcr(NewDirectorJcr(director_config->GetCurrentConfiguration()),
             &Test_FreeJcr);
  ASSERT_NE(jcr.get(), nullptr);

  JobResource* job = GetJob("job-without-policy");
  ASSERT_NE(job, nullptr);
  CopyWstorage(jcr.get(), job->storage, "Job resource");
  ASSERT_EQ(jcr->dir_impl->res.write_storage_list->size(), 1);

  StorageResource* only = jcr->dir_impl->res.write_storage;
  ASSERT_NE(only, nullptr);

  EXPECT_TRUE(SetCurrentWstorage(jcr.get(), only));
  EXPECT_EQ(jcr->dir_impl->res.write_storage, only);
  EXPECT_EQ(jcr->dir_impl->res.write_storage_list->size(), 1);
}

/* Null arguments, and a job that has no list at all, must be refused rather
 * than dereferenced. DoNativeBackupInit can reach the policy before the list
 * exists if a config is malformed. */
TEST(StorageGroupList, SetCurrentWstorageRejectsMissingInputs)
{
  InitDirGlobals();
  PConfigParser director_config(DirectorPrepareResources(kConfig));
  ASSERT_TRUE(director_config);

  JcrPtr jcr(NewDirectorJcr(director_config->GetCurrentConfiguration()),
             &Test_FreeJcr);
  ASSERT_NE(jcr.get(), nullptr);

  StorageResource* any = GetStorage("storage01");
  ASSERT_NE(any, nullptr);

  /* no list built yet */
  ASSERT_EQ(jcr->dir_impl->res.write_storage_list, nullptr);
  EXPECT_FALSE(SetCurrentWstorage(jcr.get(), any));

  EXPECT_FALSE(SetCurrentWstorage(nullptr, any));
  EXPECT_FALSE(SetCurrentWstorage(jcr.get(), nullptr));
}

/* M_FATAL during a storage group attempt is a warning; outside one it
 * still fails the job. */
TEST(StorageGroupList, FatalDuringACandidateAttemptDoesNotCondemnTheJob)
{
  InitMsg(nullptr, nullptr);

  JobControlRecord jcr;
  jcr.JobId = 0; /* keep the message out of any job log */
  jcr.setJobStatusWithPriorityCheck(JS_Running);

  jcr.trying_storage_candidate = true;
  Jmsg(&jcr, M_FATAL, 0, "storage group candidate attempt\n");

  EXPECT_EQ(jcr.getJobStatus(), JS_Running)
      << "a failed candidate must leave the job runnable";
  EXPECT_EQ(jcr.JobErrors, 0u)
      << "a failed candidate is not an error against the job";
  EXPECT_EQ(jcr.JobWarnings, 1u)
      << "it is reported, as a warning rather than silently";
}

TEST(StorageGroupList, FatalOutsideACandidateAttemptCondemnsTheJob)
{
  InitMsg(nullptr, nullptr);

  JobControlRecord jcr;
  jcr.JobId = 0;
  jcr.setJobStatusWithPriorityCheck(JS_Running);

  /* The default, and the path every other job takes. */
  ASSERT_FALSE(jcr.trying_storage_candidate);
  Jmsg(&jcr, M_FATAL, 0, "the last candidate has failed\n");

  EXPECT_EQ(jcr.getJobStatus(), JS_FatalError);
  EXPECT_EQ(jcr.JobErrors, 1u);
}
