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
 * Storage group policy implementation.
 */

#include <algorithm>
#include <memory>
#include <vector>

#include "include/bareos.h"
#include "dird/dird_conf.h"
#include "dird/director_jcr_impl.h"
#include "dird/jobq.h"
#include "dird/storage.h"
#include "dird/storage_group_policy.h"
#include "include/jcr.h"
#include "lib/alist.h"

namespace directordaemon {

namespace {

constexpr StorageGroupPolicyType kDefaultPolicy
    = StorageGroupPolicyType::kListedOrder;

const char* PolicyName(StorageGroupPolicyType type)
{
  switch (type) {
    case StorageGroupPolicyType::kLeastUsed:
      return "LeastUsed";
    case StorageGroupPolicyType::kListedOrder:
    default:
      return "ListedOrder";
  }
}

/**
 * A policy reorders the candidate list. It never removes an element and
 * never picks one: the caller takes the new front, and the failover loop
 * walks the rest in the same order.
 */
class StorageGroupPolicy {
 public:
  virtual ~StorageGroupPolicy() = default;
  virtual void Order(std::vector<StorageResource*>& candidates,
                     JobControlRecord* jcr)
      = 0;
};

/**
 * Configuration order. Deliberately a real class doing nothing, so the
 * default path and a declared policy are the same code path.
 */
class ListedOrderPolicy : public StorageGroupPolicy {
 public:
  void Order(std::vector<StorageResource*>&, JobControlRecord*) override {}
};

/**
 * Fewest concurrent jobs first.
 *
 * stable_sort, not sort: members with equal counters must keep their
 * configured order, which is what makes the policy degrade gracefully to
 * ListedOrder on an idle installation rather than to an arbitrary order.
 *
 * The counts are read one at a time, each under the queue mutex, so the set
 * is not a consistent snapshot. That is fine: this is an ordering hint, and
 * the queue re-checks the limit when the job is dispatched.
 */
class LeastUsedPolicy : public StorageGroupPolicy {
 public:
  void Order(std::vector<StorageResource*>& candidates,
             JobControlRecord*) override
  {
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](StorageResource* lhs, StorageResource* rhs) {
                       return GetStorageNumConcurrentJobs(lhs)
                              < GetStorageNumConcurrentJobs(rhs);
                     });
  }
};

std::unique_ptr<StorageGroupPolicy> MakePolicy(StorageGroupPolicyType type)
{
  switch (type) {
    case StorageGroupPolicyType::kLeastUsed:
      return std::make_unique<LeastUsedPolicy>();
    case StorageGroupPolicyType::kListedOrder:
    default:
      return std::make_unique<ListedOrderPolicy>();
  }
}

std::vector<StorageResource*> ToVector(alist<StorageResource*>* list)
{
  std::vector<StorageResource*> out;
  if (!list) { return out; }
  out.reserve(list->size());
  for (auto* store : list) {
    if (store) { out.push_back(store); }
  }
  return out;
}

/**
 * Replace the contents of an alist with the given order.
 *
 * alist offers no sort, only append / prepend / remove(index) / get(index),
 * so reordering means draining and refilling. Safe here because the write
 * storage list is created not_owned_by_alist (storage.cc CopyWstorage), so
 * removing an element does not delete the resource behind it.
 */
void RebuildAlist(alist<StorageResource*>* list,
                  const std::vector<StorageResource*>& order)
{
  while (!list->empty()) { list->remove(0); }
  for (auto* store : order) { list->append(store); }
}

/**
 * Drop members this job must not use, in place.
 *
 * Two filters today:
 *   - Enabled = no on the Storage. Declared since forever and read only by
 *     output filters, so honouring it here is new behaviour and better than
 *     Bacula, whose STORE::is_enabled() has no caller at all.
 *   - members on a different Storage Daemon than the first. Every member is
 *     announced down the single socket opened to write_storage, and the SD
 *     matches device names and media types, never Storage names, so a
 *     cross-daemon member fails confusingly rather than cleanly.
 *
 * Never empties the list: if every member would be dropped the original is
 * kept and the caller warns. A configuration mistake must not turn into a
 * failed job.
 */
void FilterCandidates(JobControlRecord* jcr,
                      std::vector<StorageResource*>& candidates)
{
  if (candidates.size() < 2) { return; }

  StorageResource* reference = candidates.front();
  std::vector<StorageResource*> kept;
  kept.reserve(candidates.size());

  for (auto* store : candidates) {
    if (!store->enabled) {
      Jmsg(jcr, M_WARNING, 0,
           T_("Storage group: skipping \"%s\", it is disabled.\n"),
           store->resource_name_);
      continue;
    }
    if (store != reference && !IsSameStorageDaemon(reference, store)) {
      Jmsg(jcr, M_WARNING, 0,
           T_("Storage group: skipping \"%s\", it is not on the same Storage "
              "Daemon as \"%s\".\n"),
           store->resource_name_, reference->resource_name_);
      continue;
    }
    kept.push_back(store);
  }

  if (kept.empty()) {
    Jmsg(jcr, M_WARNING, 0,
         T_("Storage group: every member was filtered out, using the "
            "configured list unchanged.\n"));
    return;
  }

  candidates.swap(kept);
}

} /* namespace */

StorageGroupPolicyType StorageGroupPolicyFromName(const char* name)
{
  if (!name) { return kDefaultPolicy; }
  if (Bstrcasecmp(name, "LeastUsed")) {
    return StorageGroupPolicyType::kLeastUsed;
  }
  if (Bstrcasecmp(name, "ListedOrder")) {
    return StorageGroupPolicyType::kListedOrder;
  }
  return kDefaultPolicy;
}

StorageGroupPolicyType ResolveStorageGroupPolicy(const JobResource* job,
                                                 const PoolResource* pool,
                                                 const char** name_out)
{
  const char* declared = nullptr;

  /* Pool wins over Job, matching the directive descriptions, the config
   * validator's comment and Bacula's job.c precedence. */
  if (pool && pool->storage_group_policy) {
    declared = pool->storage_group_policy;
  } else if (job && job->storage_group_policy) {
    declared = job->storage_group_policy;
  }

  StorageGroupPolicyType type = StorageGroupPolicyFromName(declared);
  if (name_out) { *name_out = PolicyName(type); }

  return type;
}

int ApplyStorageGroupPolicy(JobControlRecord* jcr)
{
  if (!jcr) { return 0; }

  alist<StorageResource*>* list = jcr->dir_impl->res.write_storage_list;
  if (!list) { return 0; }
  if (list->size() < 2) { return list->size(); }

  std::vector<StorageResource*> candidates = ToVector(list);
  FilterCandidates(jcr, candidates);

  StorageGroupPolicyType type = ResolveStorageGroupPolicy(
      jcr->dir_impl->res.job, jcr->dir_impl->res.pool, nullptr);
  MakePolicy(type)->Order(candidates, jcr);

  RebuildAlist(list, candidates);

  /* first() cannot be null here: FilterCandidates never empties the list. */
  SetCurrentWstorage(jcr, list->first());

  return list->size();
}

} /* namespace directordaemon */
