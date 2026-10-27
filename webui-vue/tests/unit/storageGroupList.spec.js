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

import capture from './fixtures/api2-show.txt?raw'
import { describe, expect, it } from 'vitest'
import { listGroups } from '../../src/utils/storageGroupList.js'

const parseFixture = (command, kind, name) => {
  const normalized = capture.replaceAll('\r\n', '\n')
  const after = normalized.split(command + '\n')[1]
  return JSON.parse(after.slice(0, after.indexOf('\n}') + 2)).result[kind][name]
}

describe('storageGroupList utils', () => {
  describe('listGroups with synthetic input', () => {
    it('correctly aggregates pool-owned, jobdefs-owned, and job-owned groups and sorts by key', () => {
      const jobdefs = {
        GroupDef: {
          name: 'GroupDef',
          storage: ['Store1', 'Store2'],
          storagegrouppolicy: 'ListedOrder',
        },
        UnusedDef: {
          name: 'UnusedDef',
          storage: ['Store3', 'Store4'],
        },
        SingleStoreDef: {
          name: 'SingleStoreDef',
          storage: ['SingleStore'],
        },
      }

      const pools = {
        GroupPool: {
          name: 'GroupPool',
          storage: ['StoreA', 'StoreB'],
          storagegrouppolicy: 'ListedOrder',
        },
        SinglePool: {
          name: 'SinglePool',
        },
      }

      const jobs = {
        jobInheritsDef: {
          name: 'jobInheritsDef',
          jobdefs: 'GroupDef',
          pool: 'SinglePool',
        },
        jobPoolWinsOverDef: {
          name: 'jobPoolWinsOverDef',
          jobdefs: 'GroupDef',
          pool: 'GroupPool',
        },
        jobOwnGroupWins: {
          name: 'jobOwnGroupWins',
          jobdefs: 'GroupDef',
          pool: 'SinglePool',
          storage: ['StoreX', 'StoreY'],
          storagegrouppolicy: 'LeastUsed',
        },
        jobSingleStore: {
          name: 'jobSingleStore',
          pool: 'SinglePool',
          storage: ['SingleOnly'],
        },
      }

      const groups = listGroups(jobs, pools, jobdefs)

      expect(groups).toHaveLength(4)

      expect(groups[0]).toEqual({
        key: 'Job:jobOwnGroupWins',
        ownerType: 'Job',
        ownerName: 'jobOwnGroupWins',
        members: ['StoreX', 'StoreY'],
        policy: 'LeastUsed',
        policySource: 'Job',
        policyOverrides: [],
        jobs: ['jobOwnGroupWins'],
      })

      expect(groups[1]).toEqual({
        key: 'JobDefs:GroupDef',
        ownerType: 'JobDefs',
        ownerName: 'GroupDef',
        members: ['Store1', 'Store2'],
        policy: 'ListedOrder',
        policySource: 'JobDefs',
        policyOverrides: [],
        jobs: ['jobInheritsDef'],
      })

      expect(groups[2]).toEqual({
        key: 'JobDefs:UnusedDef',
        ownerType: 'JobDefs',
        ownerName: 'UnusedDef',
        members: ['Store3', 'Store4'],
        policy: 'ListedOrder',
        policySource: 'default',
        policyOverrides: [],
        jobs: [],
      })

      expect(groups[3]).toEqual({
        key: 'Pool:GroupPool',
        ownerType: 'Pool',
        ownerName: 'GroupPool',
        members: ['StoreA', 'StoreB'],
        policy: 'ListedOrder',
        policySource: 'Pool',
        policyOverrides: [],
        jobs: ['jobPoolWinsOverDef'],
      })
    })
  })

  describe('members and policy are resolved independently, as the Director does', () => {
    it('shows the Pool policy fallback to the Job policy as an override, not as the Pool policy', () => {
      const pools = { P: { name: 'P', storage: ['a', 'b'] } }
      const jobs = { J: { name: 'J', pool: 'P', storagegrouppolicy: 'LeastUsed' } }
      const [group] = listGroups(jobs, pools, {})
      expect(group).toMatchObject({ key: 'Pool:P', members: ['a', 'b'], policy: 'ListedOrder', policySource: 'default', jobs: ['J'] })
      expect(group.policyOverrides).toEqual([{ job: 'J', policy: 'LeastUsed', policySource: 'Job' }])
    })

    it('lets a Pool list of one member win over the Job group', () => {
      const pools = { P: { name: 'P', storage: ['only'] } }
      const jobs = { J: { name: 'J', pool: 'P', storage: ['three', 'four'] } }
      expect(listGroups(jobs, pools, {}).map(g => g.key)).toEqual([])
    })

    it('registers an unused Pool list and keeps the Pool policy over the Job policy', () => {
      const pools = {
        Unused: { name: 'Unused', storage: ['x', 'y'], storagegrouppolicy: 'LeastUsed' },
        Used: { name: 'Used', storage: ['m', 'n'], storagegrouppolicy: 'ListedOrder' },
      }
      const jobs = { J: { name: 'J', pool: 'Used', storagegrouppolicy: 'LeastUsed' } }
      const groups = listGroups(jobs, pools, {})
      expect(groups.map(g => g.key)).toEqual(['Pool:Unused', 'Pool:Used'])
      expect(groups[0]).toMatchObject({ jobs: [], policy: 'LeastUsed', policySource: 'Pool', policyOverrides: [] })
      expect(groups[1].policyOverrides).toEqual([])
    })

    it('uses the Pool policy for a job-owned group when the Pool has no list', () => {
      const pools = { P: { name: 'P', storagegrouppolicy: 'LeastUsed' } }
      const jobs = { J: { name: 'J', pool: 'P', storage: ['a', 'b'] } }
      const [group] = listGroups(jobs, pools, {})
      expect(group).toMatchObject({ key: 'Job:J', policy: 'ListedOrder', policySource: 'default' })
      expect(group.policyOverrides).toEqual([{ job: 'J', policy: 'LeastUsed', policySource: 'Pool' }])
    })
  })

  describe('listGroups with fixture jobs and pools', () => {
    it('extracts groups from fixture objects', () => {
      const job1 = parseFixture('show job=backup-group', 'jobs', 'backup-group')
      const job2 = parseFixture('show job=backup-s3-failover', 'jobs', 'backup-s3-failover')
      const pool = parseFixture('show pool=GroupPool', 'pools', 'GroupPool')

      const jobs = {
        'backup-group': job1,
        'backup-s3-failover': job2,
        'pool-job': {
          name: 'pool-job',
          pool: 'GroupPool',
        },
      }
      const pools = {
        GroupPool: pool,
      }

      const groups = listGroups(jobs, pools)
      expect(groups).toHaveLength(3)

      expect(groups.map((g) => g.key)).toEqual([
        'Job:backup-group',
        'Job:backup-s3-failover',
        'Pool:GroupPool',
      ])

      const backupGroup = groups.find((g) => g.key === 'Job:backup-group')
      expect(backupGroup.members).toEqual(['File1a', 'File1b'])
      expect(backupGroup.policy).toBe('ListedOrder')
      expect(backupGroup.ownerType).toBe('Job')
      expect(backupGroup.jobs).toEqual(['backup-group'])

      const poolGroup = groups.find((g) => g.key === 'Pool:GroupPool')
      expect(poolGroup.members).toEqual(['File2', 'File1a'])
      expect(poolGroup.ownerType).toBe('Pool')
      expect(poolGroup.jobs).toEqual(['pool-job'])
    })

    it('returns empty array when no groups have 2+ members or empty input', () => {
      expect(listGroups({}, {})).toEqual([])
      expect(listGroups(null, null)).toEqual([])
      expect(listGroups(null, null, null)).toEqual([])
      expect(
        listGroups(
          { j1: { name: 'j1', storage: ['File1'] } },
          { p1: { name: 'p1', storage: ['File2'] } },
          { d1: { name: 'd1', storage: ['File3'] } },
        ),
      ).toEqual([])
    })
  })

  describe('storage group policy fallback', () => {
    it('treats empty storagegrouppolicy: "" as unset and defaults to ListedOrder (default)', () => {
      const jobs = {
        'backup-job': {
          name: 'backup-job',
          storage: ['Store1', 'Store2'],
          storagegrouppolicy: '',
        },
      }
      const groups = listGroups(jobs, {})
      expect(groups).toHaveLength(1)
      expect(groups[0].policy).toBe('ListedOrder')
      expect(groups[0].policySource).toBe('default')
    })
  })
})
