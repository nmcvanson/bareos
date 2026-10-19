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

import { describe, expect, it } from 'vitest'
import { validateJobForm, buildConfigureAddJobCommand, buildJobResourcePreview } from '../../src/utils/jobConfig.js'

// Native config_add_directive formatting (ua_configure.cc) and job_items types
// (dird_conf.cc); the preview writes nothing and sends no Director command.
const lists = {
  jobs: ['web-existing'], clients: ['fd1'], filesets: ['Demo-Documents'],
  jobdefs: ['DefaultJob'], schedules: ['WeeklyCycle'], pools: ['Full'],
  storages: ['File2', 'S3'], messages: ['Standard'],
}
const form = {
  name: 'web-demo', jobdefs: 'DefaultJob', client: 'fd1',
  fileset: 'Demo-Documents', schedule: 'WeeklyCycle', pool: 'Full',
  storage: ['File2', 'S3'], storagegrouppolicy: 'LeastUsed',
  level: 'Full', priority: 10, messages: 'Standard',
}

describe('validateJobForm', () => {
  it('accepts a complete form, all backup levels and both policies', () => {
    for (const level of ['Full', 'Incremental', 'Differential']) {
      for (const policy of ['', 'ListedOrder', 'LeastUsed']) {
        expect(validateJobForm({ ...form, level, storagegrouppolicy: policy }, lists)).toEqual({})
      }
    }
    expect(validateJobForm({ ...form, name: 'web-demo_2.1-X', priority: '100', storage: [], jobdefs: '', schedule: '' }, lists)).toEqual({})
    const namedLists = Object.fromEntries(Object.entries(lists).map(([key, names]) => [key, names.map(name => ({ name }))]))
    expect(validateJobForm({ ...form, priority: 1 }, namedLists)).toEqual({})
  })

  it.each(['', '../job', 'web-space name', 'web-@x', 'web-x;y', 'web-x"y', 'web-x\ny', 'web-é', 'web-existing'])('rejects invalid or existing name %j', name => {
    expect(validateJobForm({ ...form, name }, lists)).toHaveProperty('name')
  })

  it.each(['client', 'fileset', 'pool', 'messages'])('requires an available %s', field => {
    expect(validateJobForm({ ...form, [field]: '' }, lists)).toHaveProperty(field)
    expect(validateJobForm({ ...form, [field]: 'unknown' }, lists)).toHaveProperty(field)
  })

  it.each(['jobdefs', 'schedule'])('validates optional %s against its resource list', field => {
    expect(validateJobForm({ ...form, [field]: 'unknown' }, lists)).toHaveProperty(field)
  })

  it.each([['unknown'], ['File2', 'File2'], 'File2'])('rejects invalid storage members %j', storage => {
    expect(validateJobForm({ ...form, storage }, lists)).toHaveProperty('storage')
  })

  it.each([0, 101, -1, 1.5, '', null, true, false, 'ten', Infinity])('rejects invalid priority %j', priority => {
    expect(validateJobForm({ ...form, priority }, lists)).toHaveProperty('priority')
  })

  it('rejects unknown policy and level and validates a missing form against empty lists', () => {
    expect(validateJobForm({ ...form, storagegrouppolicy: 'RoundRobin', level: 'Base' }, lists)).toMatchObject({ storagegrouppolicy: expect.any(String), level: expect.any(String) })
    expect(validateJobForm({})).toHaveProperty('name')
  })
})

describe('native configure add job', () => {
  it.each(['archive', '1job', 'Archive_2026.10', 'archive..job'])('allows the native generic name %s', name => {
    expect(validateJobForm({ ...form, name }, lists)).toEqual({})
    expect(buildConfigureAddJobCommand({ ...form, name })).toContain(`configure add job name="${name}"`)
    expect(buildJobResourcePreview({ ...form, name })).toContain(`Name = "${name}"`)
  })
  it('quotes each command argument and keeps ordered members in one argument', () => {
    expect(buildConfigureAddJobCommand(form)).toBe('configure add job name="web-demo" type="Backup" jobdefs="DefaultJob" client="fd1" fileset="Demo-Documents" schedule="WeeklyCycle" pool="Full" storage="File2,S3" storagegrouppolicy="LeastUsed" level="Full" priority="10" messages="Standard"')
  })

  it('previews exactly the native resource text with no marker or preview command', () => {
    expect(buildJobResourcePreview(form)).toBe('Job {\n  Name = "web-demo"\n  Type = Backup\n  JobDefs = "DefaultJob"\n  Client = "fd1"\n  FileSet = "Demo-Documents"\n  Schedule = "WeeklyCycle"\n  Pool = "Full"\n  Storage = File2,S3\n  StorageGroupPolicy = "LeastUsed"\n  Level = Full\n  Priority = 10\n  Messages = "Standard"\n}\n')
    expect(buildConfigureAddJobCommand(form)).not.toContain(' preview')
  })

  it('omits empty fields, preserves resource spaces and fixes the job type to backup', () => {
    const minimal = { name: 'web-min', type: 'Restore', storage: [], client: '', schedule: null }
    expect(buildConfigureAddJobCommand(minimal)).toBe('configure add job name="web-min" type="Backup"')
    expect(buildConfigureAddJobCommand({ name: 'archive' })).toBe('configure add job name="archive" type="Backup"')
    expect(validateJobForm({ ...form, name: 'archive' }, lists)).toEqual({})
    expect(buildConfigureAddJobCommand({ name: 'web-space', fileset: 'Demo Documents' })).toContain(' fileset="Demo Documents"')
  })

  it('accepts Description and excludes every non-whitelisted directive', () => {
    const input = { ...form, description: 'Executive demo backup', password: 'ignored', runscript: 'ignored', preview: true }
    const command = buildConfigureAddJobCommand(input)
    expect(command).toContain(' description="Executive demo backup"')
    expect(command).not.toMatch(/password=|runscript=|preview/)
    expect(validateJobForm(input, lists)).toEqual({})
    expect(validateJobForm({ ...form, description: 'unsafe;value' }, lists)).toHaveProperty('description')
  })

  it.each(['@x', 'x;y', 'x"y', "x'y", 'x\ny', 'x\ry', 'x\\y', 'x}y'])('refuses dangerous resource values %j', value => {
    const unsafe = { ...form, client: value }
    expect(validateJobForm(unsafe, { ...lists, clients: [value] })).toHaveProperty('client')
    expect(() => buildConfigureAddJobCommand(unsafe)).toThrow()
    expect(() => buildConfigureAddJobCommand({ ...form, description: value }, { preview: true })).toThrow()
  })

  it.each([
    { name: '' }, { name: '../job' }, { level: 'Full\nMessages = evil' },
    { priority: '10 # comment' }, { storage: ['File2,S3'] },
    { storage: ['has space'] }, { storage: 'S3' }, { storagegrouppolicy: 'Random' },
  ])('refuses invalid command form %j', change => {
    expect(() => buildConfigureAddJobCommand({ ...form, ...change })).toThrow()
    expect(() => buildJobResourcePreview({ ...form, ...change })).toThrow()
  })
})
