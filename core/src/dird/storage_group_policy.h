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
/**
 * @file
 * Storage group policy: choosing one Storage out of a Job's Storage list.
 *
 * A Job or Pool may declare several Storage resources ("Storage = A, B").
 * The Director keeps them in write_storage_list and has always used the
 * first one. A policy reorders that list so a different member can come
 * first, and points write_storage at the new head.
 *
 * The policies only ever REORDER. They never select a single element and
 * never shorten the list, so the failover loop can reuse the same order.
 */

#ifndef BAREOS_DIRD_STORAGE_GROUP_POLICY_H_
#define BAREOS_DIRD_STORAGE_GROUP_POLICY_H_

class JobControlRecord;

namespace directordaemon {

class JobResource;
class PoolResource;

/* The policies this release implements. The names accepted by the
 * configuration parser are validated separately in dird_conf.cc; keep the
 * two in step. FreeSpace and FreeSpaceLeastUsed are not here: they need
 * free space tracking in the Storage Daemon, which does not exist. */
enum class StorageGroupPolicyType
{
  kListedOrder, /**< configuration order, the default */
  kLeastUsed,   /**< fewest concurrent jobs first */
};

/* Resolve the effective policy for a job: Pool first, then Job, then the
 * default, as both directive descriptions say.
 *
 * name_out, when given, receives the canonical name of the returned policy
 * as a static string. It is never null and never needs freeing. */
StorageGroupPolicyType ResolveStorageGroupPolicy(const JobResource* job,
                                                 const PoolResource* pool,
                                                 const char** name_out
                                                 = nullptr);

/* Map a configured name to a policy. Matching is case insensitive, to agree
 * with the configuration validator. An unknown or null name gives the
 * default rather than an error: the name was validated at config time. */
StorageGroupPolicyType StorageGroupPolicyFromName(const char* name);

/* Filter and reorder jcr's write_storage_list in place, then point
 * write_storage at the new first member.
 *
 * Does nothing when the list holds fewer than two members, so a job without
 * a storage group behaves exactly as it did before.
 *
 * Returns the number of candidates left after filtering, which is what the
 * caller reports to the user. */
int ApplyStorageGroupPolicy(JobControlRecord* jcr);

} /* namespace directordaemon */

#endif  // BAREOS_DIRD_STORAGE_GROUP_POLICY_H_
