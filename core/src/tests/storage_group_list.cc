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

/* A.3: the write storage list is what a storage group lives in, and the
 * policy added in stage B reorders it. These tests pin down the two
 * primitives that build and destroy it, so that a later change to either is
 * caught here rather than in a system test.
 *
 * Note on coverage: the guard added in A.3 lives in ResetRestoreContext,
 * which is static in ua_run.cc and cannot be reached from a unit test. What
 * is covered here is the behaviour that guard depends on -- CopyWstorage
 * keeping every member, and SetWstorage destroying all but one. The guard
 * itself is covered by the systemtest. */

#include "testing_dir_common.h"

#include "dird/dird_conf.h"
#include "dird/director_jcr_impl.h"
#include "dird/jcr_util.h"
#include "dird/storage.h"
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
}  // namespace

/* A Job declaring "Storage = storage01, storage02" must reach the job control
 * record with both members present, and write_storage pointing at the first.
 * This is the state stage B's policy reorders. */
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
 * is why the console run path had to be guarded in A.3 -- calling it with a
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

/* A Job listing a single storage is not a group. The A.3 guard must leave
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
