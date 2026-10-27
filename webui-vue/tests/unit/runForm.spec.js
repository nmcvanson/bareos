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
import { describe, expect, it, vi } from 'vitest'
import { parse } from '@babel/parser'
import pageSource from '../../src/pages/JobsPage.vue?raw'
import { effectiveGroup } from '../../src/utils/storageGroups.js'
import { runStorageDefault, runFormSubmission } from '../../src/utils/runForm.js'
import { buildRunJobCommand, buildJobDefaultsCommand, resolvePermittedRunJobDefault } from '../../src/utils/jobs.js'
import { quoteDirectorString } from '../../src/utils/directorStrings.js'

const parseFixture = (command, kind, name) => {
  const normalized = capture.replaceAll('\r\n', '\n')
  const after = normalized.split(command + '\n')[1]
  return JSON.parse(after.slice(0, after.indexOf('\n}') + 2)).result[kind][name]
}

describe('runStorageDefault', () => {
  it('clears storage and provides hint for a group job (backup-group fixture)', () => {
    const job = parseFixture('show job=backup-group', 'jobs', 'backup-group')
    const group = effectiveGroup({ job })
    expect(group.members).toEqual(['File1a', 'File1b'])

    const result = runStorageDefault('File1a', group)
    expect(result).toEqual({
      storage: '',
      hint: group,
    })
  })

  it('keeps defaultsStorage and gives null hint for a single-storage job', () => {
    const job = { name: 'single-storage-job', storage: ['File1a'] }
    const group = effectiveGroup({ job })
    expect(group.members).toEqual(['File1a'])

    const result = runStorageDefault('File1a', group)
    expect(result).toEqual({
      storage: 'File1a',
      hint: null,
    })
  })

  it('clears storage and provides hint for a Pool group', () => {
    const pool = parseFixture('show pool=GroupPool', 'pools', 'GroupPool')
    const job = { name: 'pool-backed-job', pool: 'GroupPool' }
    const group = effectiveGroup({ job, pool })
    expect(group.members).toEqual(['File2', 'File1a'])
    expect(group.membersSource).toBe('Pool')

    const result = runStorageDefault('File2', group)
    expect(result).toEqual({
      storage: '',
      hint: group,
    })
  })

  it('handles null defaultsStorage and null group gracefully', () => {
    expect(runStorageDefault(null, null)).toEqual({
      storage: '',
      hint: null,
    })
    expect(runStorageDefault('File1a', { members: [] })).toEqual({
      storage: 'File1a',
      hint: null,
    })
  })
})

describe('explicit Run overrides', () => {
  const form = { job: 'archive', client: 'client-default', fileset: 'files-default', pool: 'pool-default',
    storage: 'first-default-member', level: 'Full', priority: 20, when: '' }

  it('never submits untouched displayed defaults, including Pool and Storage for a group', () => {
    expect(buildRunJobCommand(runFormSubmission(form, {}))).toBe('run job="archive" yes')
  })

  it('does not pin Storage when the show lookup fails or is denied', () => {
    const display = runStorageDefault(form.storage, null)
    expect(buildRunJobCommand(runFormSubmission({ ...form, storage: display.storage }, {})))
      .toBe('run job="archive" yes')
  })

  it('sends an explicitly picked Storage so the UI can show the override warning', () => {
    const explicit = { ...form, storage: 'chosen-member' }
    expect(runFormSubmission(explicit, { storage: true }).storage).toBe('chosen-member')
    expect(buildRunJobCommand(runFormSubmission(explicit, { storage: true })))
      .toBe('run job="archive" storage="chosen-member" yes')
  })

  it('sends only the fields changed by the user plus an entered run time', () => {
    expect(buildRunJobCommand(runFormSubmission({ ...form, when: '2026-10-20 12:00:00' }, {
      client: true, fileset: true, pool: true, level: true, priority: true,
    }))).toBe('run job="archive" client="client-default" fileset="files-default" pool="pool-default" level="Full" when="2026-10-20 12:00:00" priority=20 yes')
    expect(buildRunJobCommand(runFormSubmission({ ...form, storage: null }, { storage: true })))
      .toBe('run job="archive" yes')
  })
})

describe('Jobs page Run flow', () => {
  const script = pageSource.split('<script setup>')[1].split('</script>')[0]
  const nodes = parse(script, { sourceType: 'module' }).program.body
  const handlers = ['onJobSelected', 'runJob'].map(name => {
    const node = nodes.find(node => node.type === 'FunctionDeclaration' && node.id.name === name)
    return script.slice(node.start, node.end)
  }).join('\n')

  it.each([false, true])('does not send default overrides when show is denied=%s', async denied => {
    const call = vi.fn(async command => {
      if (command.startsWith('.defaults')) return { defaults: { client: 'client', fileset: 'files', pool: 'pool', storage: 'one', level: 'Full', priority: 20 } }
      if (command.startsWith('show job=')) {
        if (denied) throw new Error('Permission denied')
        return { jobs: { archive: { storage: ['one', 'two'] } } }
      }
      if (command.startsWith('show pool=')) return { pools: { pool: { storage: ['one', 'two'] } } }
      return { run: { jobid: 42 } }
    })
    const environment = {
      runForm: { value: { job: 'archive', when: '' } }, runOverrides: { value: {} },
      runGroupHint: { value: null }, runGroupLoading: { value: false }, runLoading: { value: false },
      dotClients: { value: ['client'] }, dotFilesets: { value: ['files'] },
      dotPools: { value: ['pool'] }, dotStorages: { value: ['one', 'two'] },
      director: { call }, ensureSingletonTabDirector: async () => {},
      buildJobDefaultsCommand, resolvePermittedRunJobDefault, quoteDirectorString,
      effectiveGroup, runStorageDefault, runFormSubmission, buildRunJobCommand,
      $q: { notify: vi.fn() }, tab: { value: 'run' }, refresh: vi.fn(),
    }
    const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor
    const execute = new AsyncFunction(...Object.keys(environment),
      `let runDefaultsRequest = 0;\n${handlers}\nawait onJobSelected('archive'); await runJob();`)
    await execute(...Object.values(environment))
    expect(call.mock.calls.at(-1)[0]).toBe('run job="archive" yes')
    expect(environment.runOverrides.value).toEqual({})
  })
})
