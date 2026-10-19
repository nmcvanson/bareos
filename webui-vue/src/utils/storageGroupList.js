/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026-2026 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
 */

export function resolveGroupPolicy(explicitPolicy, source, fallbackPolicy = null, fallbackSource = null) {
  if (explicitPolicy && typeof explicitPolicy === 'string' && explicitPolicy.trim()) {
    return {
      policy: explicitPolicy.trim(),
      policySource: source,
    }
  }
  if (fallbackPolicy && typeof fallbackPolicy === 'string' && fallbackPolicy.trim()) {
    return {
      policy: fallbackPolicy.trim(),
      policySource: fallbackSource,
    }
  }
  return {
    policy: 'ListedOrder',
    policySource: 'default',
  }
}

// The Director resolves the member list and the policy independently
// (storage_group_policy.cc): a Pool storage list of any length wins over the
// Job's list; the policy is the Pool's if set, else the Job's (JobDefs
// included), else ListedOrder. A card shows the owner's stored policy; jobs
// whose effective policy differs are listed in policyOverrides.
export function listGroups(jobsJson, poolsJson, jobdefsJson = {}) {
  const rawJobs = jobsJson?.result?.jobs ?? jobsJson?.jobs ?? jobsJson ?? {}
  const rawPools = poolsJson?.result?.pools ?? poolsJson?.pools ?? poolsJson ?? {}
  const rawJobdefs = jobdefsJson?.result?.jobdefs ?? jobdefsJson?.jobdefs ?? jobdefsJson ?? {}

  const toMap = (raw) => (Array.isArray(raw)
    ? Object.fromEntries(raw.map((r) => [r.name, r]))
    : (raw && typeof raw === 'object' ? raw : {}))
  const pools = toMap(rawPools)
  const jobdefs = toMap(rawJobdefs)
  const jobs = toMap(rawJobs)

  const groupsMap = new Map()
  const register = (ownerType, ownerName, members, stored) => {
    const key = `${ownerType}:${ownerName}`
    if (!groupsMap.has(key)) {
      groupsMap.set(key, {
        key,
        ownerType,
        ownerName,
        members: [...members],
        policy: stored.policy,
        policySource: stored.policySource,
        jobs: [],
        policyOverrides: [],
      })
    }
    return groupsMap.get(key)
  }

  // 1. Every JobDefs and Pool with a storage group, used by a job or not.
  for (const [defKey, def] of Object.entries(jobdefs)) {
    if (!def || typeof def !== 'object') continue
    if (Array.isArray(def.storage) && def.storage.length >= 2) {
      register('JobDefs', def.name ?? defKey, def.storage,
        resolveGroupPolicy(def.storagegrouppolicy, 'JobDefs'))
    }
  }
  for (const [poolKey, pool] of Object.entries(pools)) {
    if (!pool || typeof pool !== 'object') continue
    if (Array.isArray(pool.storage) && pool.storage.length >= 2) {
      register('Pool', pool.name ?? poolKey, pool.storage,
        resolveGroupPolicy(pool.storagegrouppolicy, 'Pool'))
    }
  }

  // 2. Attach each job to the owner of its effective member list.
  for (const [jobKey, job] of Object.entries(jobs)) {
    if (!job || typeof job !== 'object') continue
    const jobName = job.name ?? jobKey
    const pool = job.pool ? pools[job.pool] : undefined
    const def = job.jobdefs ? jobdefs[job.jobdefs] : undefined
    const poolList = Array.isArray(pool?.storage) && pool.storage.length > 0 ? pool.storage : null

    let group = null
    if (poolList) {
      // The Pool list wins, whatever its length; fewer than two is no group.
      if (poolList.length >= 2) group = groupsMap.get(`Pool:${pool.name ?? job.pool}`)
    } else {
      const defHasGroup = def && Array.isArray(def.storage) && def.storage.length >= 2
      const matchesDef = defHasGroup && (
        !job.storage
        || (Array.isArray(job.storage)
          && job.storage.length === def.storage.length
          && job.storage.every((v, i) => v === def.storage[i]))
      )
      if (matchesDef) {
        group = groupsMap.get(`JobDefs:${def.name ?? job.jobdefs}`)
      } else if (Array.isArray(job.storage) && job.storage.length >= 2) {
        group = register('Job', jobName, job.storage,
          resolveGroupPolicy(job.storagegrouppolicy, 'Job', def?.storagegrouppolicy, 'JobDefs'))
      }
    }
    if (!group) continue

    if (!group.jobs.includes(jobName)) group.jobs.push(jobName)
    const effective = pool?.storagegrouppolicy
      ? resolveGroupPolicy(pool.storagegrouppolicy, 'Pool')
      : resolveGroupPolicy(job.storagegrouppolicy, 'Job', def?.storagegrouppolicy, 'JobDefs')
    if (effective.policy !== group.policy || effective.policySource !== group.policySource) {
      group.policyOverrides.push({ job: jobName, policy: effective.policy, policySource: effective.policySource })
    }
  }

  const result = Array.from(groupsMap.values())
  for (const entry of result) {
    entry.jobs.sort((a, b) => a.localeCompare(b))
    entry.policyOverrides.sort((a, b) => a.job.localeCompare(b.job))
  }
  return result.sort((a, b) => a.key.localeCompare(b.key))
}
