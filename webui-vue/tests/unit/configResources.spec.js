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
import {
  CONFIG_TYPES,
  stripPasswords,
  normaliseResources,
  referencesTo,
  isInherited,
  withInherited,
} from '../../src/utils/configResources.js'

const parseFixture = (command) => {
  const normalized = capture.replaceAll('\r\n', '\n')
  const after = normalized.split(command + '\n')[1]
  return JSON.parse(after.slice(0, after.indexOf('\n}') + 2))
}

describe('configResources utils', () => {
  describe('CONFIG_TYPES', () => {
    it('defines the 10 configuration resource types with correct writable flags', () => {
      const keys = CONFIG_TYPES.map((t) => t.key)
      expect(keys).toEqual([
        'job',
        'jobdefs',
        'client',
        'fileset',
        'schedule',
        'pool',
        'storage',
        'messages',
        'console',
        'profile',
      ])

      const writableKeys = CONFIG_TYPES.filter((t) => t.writable).map((t) => t.key)
      expect(writableKeys).toEqual([
        'job',
        'jobdefs',
        'client',
        'schedule',
        'pool',
        'storage',
        'messages',
      ])

      const readOnlyKeys = CONFIG_TYPES.filter((t) => !t.writable).map((t) => t.key)
      expect(readOnlyKeys).toEqual(['fileset', 'console', 'profile'])
    })
  })

  describe('stripPasswords', () => {
    it('removes password fields case-insensitively at all depths', () => {
      const input = {
        name: 'test',
        password: 'plain',
        Password: 'title',
        nested: {
          clientPassword: 'deep',
          allowed: true,
          deepArray: [
            { password: 'in-array', port: 9102 },
            { name: 'clean', secretpassword: '123' },
          ],
        },
      }

      const stripped = stripPasswords(input)
      expect(stripped).toEqual({
        name: 'test',
        nested: {
          allowed: true,
          deepArray: [{ port: 9102 }, { name: 'clean' }],
        },
      })
    })
  })

  describe('normaliseResources with real capture', () => {
    it('normalises storage fixture and strips password', () => {
      const storageJson = parseFixture('show storage=File2')
      const normalised = normaliseResources('storage', storageJson)

      expect(normalised).toHaveLength(1)
      expect(normalised[0].name).toBe('File2')
      expect('password' in normalised[0].directives).toBe(false)
      expect(normalised[0].directives.password).toBeUndefined()
      expect(normalised[0].directives.address).toBe('lab-sd2')
      expect(normalised[0].directives.mediatype).toBe('File')
      expect(normalised[0].directives.device).toEqual(['FileStorage2'])
    })

    it('normalises jobs fixture and sorts by name', () => {
      const job1Json = parseFixture('show job=backup-group')
      const job2Json = parseFixture('show job=backup-s3-failover')

      const combined = {
        result: {
          jobs: {
            'backup-s3-failover': job2Json.result.jobs['backup-s3-failover'],
            'backup-group': job1Json.result.jobs['backup-group'],
          },
        },
      }

      const normalised = normaliseResources('job', combined)
      expect(normalised).toHaveLength(2)
      expect(normalised[0].name).toBe('backup-group')
      expect(normalised[1].name).toBe('backup-s3-failover')
      expect(normalised[0].directives.storage).toEqual(['File1a', 'File1b'])
    })

    it('normalises synthetic nested FileSet and strips nested passwords', () => {
      const filesetJson = {
        filesets: {
          LinuxFull: {
            name: 'LinuxFull',
            include: {
              file: ['/etc', '/var'],
              options: {
                compression: 'GZIP',
                password: 'TopSecretPassword',
                signature: 'SHA1',
              },
            },
          },
        },
      }

      const normalised = normaliseResources('fileset', filesetJson)
      expect(normalised).toHaveLength(1)
      expect(normalised[0].name).toBe('LinuxFull')
      expect(normalised[0].directives.include.file).toEqual(['/etc', '/var'])
      expect(normalised[0].directives.include.options.compression).toBe('GZIP')
      expect(normalised[0].directives.include.options.signature).toBe('SHA1')
      expect(normalised[0].directives.include.options.password).toBeUndefined()
    })

    it('returns empty array on empty or invalid input', () => {
      expect(normaliseResources('job', null)).toEqual([])
      expect(normaliseResources('job', {})).toEqual([])
    })
  })

  describe('referencesTo', () => {
    it('detects resources of other types that reference a resource', () => {
      const allResources = {
        job: [
          {
            name: 'backup-group',
            directives: {
              storage: ['File1a', 'File1b'],
              jobdefs: 'DefaultJob',
            },
          },
          {
            name: 'backup-s3-failover',
            directives: {
              storage: ['File2', 'S3'],
              pool: 'HybridPool',
              jobdefs: 'DefaultJob',
            },
          },
        ],
        pool: [
          {
            name: 'GroupPool',
            directives: {
              storage: ['File2', 'File1a'],
              storagegrouppolicy: 'ListedOrder',
            },
          },
          {
            name: 'HybridPool',
            directives: {
              storage: ['File2'],
            },
          },
        ],
      }

      // References to storage 'File2' from jobs and pools
      const file2Refs = referencesTo('File2', 'storage', allResources)
      expect(file2Refs).toEqual([
        { type: 'job', name: 'backup-s3-failover', directive: 'storage' },
        { type: 'pool', name: 'GroupPool', directive: 'storage' },
        { type: 'pool', name: 'HybridPool', directive: 'storage' },
      ])

      // References to pool 'HybridPool'
      const poolRefs = referencesTo('HybridPool', 'pool', allResources)
      expect(poolRefs).toEqual([
        { type: 'job', name: 'backup-s3-failover', directive: 'pool' },
      ])

      // References to JobDefs 'DefaultJob'
      const defRefs = referencesTo('DefaultJob', 'jobdefs', allResources)
      expect(defRefs).toEqual([
        { type: 'job', name: 'backup-group', directive: 'jobdefs' },
        { type: 'job', name: 'backup-s3-failover', directive: 'jobdefs' },
      ])
    })

    it('ignores same-type resources and does not match the resource own name', () => {
      const allResources = {
        pool: [
          { name: 'PoolA', directives: { name: 'PoolA', storage: ['Store1'] } },
          { name: 'PoolB', directives: { name: 'PoolB', nextpool: 'PoolA' } },
        ],
      }

      // Looking up 'PoolA' with type 'pool': same type is ignored
      const refs = referencesTo('PoolA', 'pool', allResources)
      expect(refs).toEqual([])
    })
  })

  describe('withInherited and isInherited', () => {
    const jobdefsMap = {
      DefaultJob: {
        name: 'DefaultJob',
        client: 'default-client',
        fileset: 'StandardSet',
        pool: 'DefaultPool',
        storage: 'DefaultStore',
        messages: 'Standard',
      },
    }

    it('inherits missing client, fileset, and pool when job specifies jobdefs', () => {
      const job = {
        name: 'backup-app',
        jobdefs: 'DefaultJob',
      }

      const merged = withInherited(job, jobdefsMap)

      expect(merged.client).toBe('default-client')
      expect(merged.fileset).toBe('StandardSet')
      expect(merged.pool).toBe('DefaultPool')
      expect(merged.storage).toBe('DefaultStore')
      expect(merged.directives.client).toBe('default-client')
      expect(merged.directives.fileset).toBe('StandardSet')
      expect(merged.inherited).toEqual({
        client: 'default-client',
        fileset: 'StandardSet',
        pool: 'DefaultPool',
        storage: 'DefaultStore',
        messages: 'Standard',
      })
      expect(merged.inheritedKeys).toEqual([
        'client',
        'fileset',
        'pool',
        'storage',
        'messages',
      ])

      expect(isInherited(merged, 'client')).toBe(true)
      expect(isInherited(merged, 'fileset')).toBe(true)
      expect(isInherited(merged, 'pool')).toBe(true)
      expect(isInherited(merged, 'jobdefs')).toBe(false)
      expect(isInherited(merged, 'name')).toBe(false)
    })

    it('preserves job explicit directives and does not mark them inherited', () => {
      const job = {
        name: 'backup-custom',
        jobdefs: 'DefaultJob',
        client: 'custom-client',
        pool: 'SpecialPool',
      }

      const merged = withInherited(job, jobdefsMap)

      expect(merged.client).toBe('custom-client')
      expect(merged.pool).toBe('SpecialPool')
      expect(merged.fileset).toBe('StandardSet')
      expect(isInherited(merged, 'client')).toBe(false)
      expect(isInherited(merged, 'pool')).toBe(false)
      expect(isInherited(merged, 'fileset')).toBe(true)
      expect(merged.inherited.client).toBeUndefined()
      expect(merged.inherited.fileset).toBe('StandardSet')
    })

    it('treats empty string directive in job as unset and inherits from JobDefs', () => {
      const job = {
        name: 'backup-empty-pool',
        jobdefs: 'DefaultJob',
        pool: '',
      }

      const merged = withInherited(job, jobdefsMap)

      expect(merged.pool).toBe('DefaultPool')
      expect(isInherited(merged, 'pool')).toBe(true)
    })

    it('handles normalised resource objects with directives property', () => {
      const jobResource = {
        name: 'backup-normalised',
        directives: {
          name: 'backup-normalised',
          jobdefs: 'DefaultJob',
          pool: 'CustomPool',
        },
      }

      const jobdefsList = [
        {
          name: 'DefaultJob',
          directives: {
            client: 'default-client',
            fileset: 'StandardSet',
            pool: 'DefaultPool',
          },
        },
      ]

      const merged = withInherited(jobResource, jobdefsList)

      expect(merged.name).toBe('backup-normalised')
      expect(merged.directives.client).toBe('default-client')
      expect(merged.directives.fileset).toBe('StandardSet')
      expect(merged.directives.pool).toBe('CustomPool')
      expect(isInherited(merged, 'client')).toBe(true)
      expect(isInherited(merged, 'fileset')).toBe(true)
      expect(isInherited(merged, 'pool')).toBe(false)
    })

    it('returns job unmodified when jobdefs is missing or not found', () => {
      const jobWithoutDef = { name: 'independent-job', client: 'standalone' }
      const res1 = withInherited(jobWithoutDef, jobdefsMap)
      expect(res1.inheritedKeys).toEqual([])
      expect(isInherited(res1, 'client')).toBe(false)

      const jobWithUnknownDef = { name: 'job-unknown', jobdefs: 'NoSuchDef' }
      const res2 = withInherited(jobWithUnknownDef, jobdefsMap)
      expect(res2.inheritedKeys).toEqual([])
      expect(isInherited(res2, 'jobdefs')).toBe(false)

      expect(withInherited(null, jobdefsMap)).toBeNull()
    })
  })
})
