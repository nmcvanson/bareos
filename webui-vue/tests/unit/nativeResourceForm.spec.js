/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026 Bareos GmbH & Co. KG

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
import { nativeResourceFields, nativeDirectiveOptions, newResourceRows } from '../../src/utils/nativeResourceForm.js'
import { buildConfigureAddCommand, buildConfigureAddPreview } from '../../src/utils/configureAdd.js'
import { NATIVE_DIRECTIVES } from '../../src/utils/configureAddDirectives.js'
import { readFileSync } from 'node:fs'
import { fileURLToPath, URL } from 'node:url'

const source = path => readFileSync(fileURLToPath(new URL(path, import.meta.url)), 'utf8')
const directorSource = source('../../../core/src/dird/dird_conf.cc')
const tlsSource = source('../../../core/src/lib/tls_resource_items.h')
const messagesSource = source('../../../core/src/lib/messages_resource_items.h')
const configureSource = source('../../../core/src/dird/ua_configure.cc')

const examples = {
  jobdefs: [{ key: 'Type', value: 'Backup' }, { key: 'Client', value: 'archive-client' }],
  client: [{ key: 'Address', value: 'backup.example.invalid' }, { key: 'Password', value: 'synthetic-value' }],
  storage: [{ key: 'Address', value: 'backup.example.invalid' }, { key: 'Password', value: 'synthetic-value' }, { key: 'Device', value: 'ArchiveDevice' }, { key: 'MediaType', value: 'File' }],
  pool: [{ key: 'PoolType', value: 'Backup' }, { key: 'VolumeRetention', value: '30 days' }],
  schedule: [{ key: 'Run', value: 'Level=Full daily at 23:00' }, { key: 'Run', value: 'Level=Incremental hourly' }],
  messages: [{ key: 'Catalog', value: 'all' }, { key: 'Console', value: 'all,!skipped' }],
}

describe('flat native resource forms', () => {
  it.each(Object.keys(examples))('builds %s with canonical directives and native quoting', type => {
    const fields = nativeResourceFields(type, 'archive', examples[type])
    expect(buildConfigureAddCommand(type, fields)).toMatch(new RegExp(`^configure add ${type} name="archive" `))
    expect(buildConfigureAddPreview(type, fields)).toContain('  Name = "archive"\n')
  })
  it('matches native scalar, resource, list and password formatting', () => {
    const storage = buildConfigureAddPreview('storage', nativeResourceFields('storage', 'archive', examples.storage))
    expect(storage).toContain('  Address = "backup.example.invalid"\n')
    expect(storage).toContain('  Device = ArchiveDevice\n')
    expect(storage).toContain('  MediaType = "File"\n')
    expect(storage).not.toContain('synthetic-value')
    const schedule = buildConfigureAddPreview('schedule', nativeResourceFields('schedule', 'archive', examples.schedule))
    expect(schedule).toContain('  Run = Level=Full daily at 23:00\n  Run = Level=Incremental hourly\n')
    expect(buildConfigureAddPreview('pool', nativeResourceFields('pool', 'archive', examples.pool))).toContain('  VolumeRetention = 30 days\n')
  })
  it('puts existing resource directive suggestions first without copying any values', () => {
    const suggestions = nativeDirectiveOptions('client', [{ directives: { address: 'never-copy-host', password: 'never-copy-secret', status: 'online', 'maximum concurrent jobs': 3 } }])
    expect(suggestions.slice(0, 3)).toEqual(['Address', 'Password', 'MaximumConcurrentJobs'])
    expect(suggestions).not.toContain('Name')
    expect(suggestions).not.toContain('status')
    expect(suggestions.join(',')).not.toMatch(/never-copy/)
    expect(nativeDirectiveOptions('fileset')).toEqual([])
  })
  it('uses the Director tables for quoting and excludes nested RunScript', () => {
    const quotedBlock = configureSource.match(/quotable_types\{([\s\S]*?)\};/)[1]
    const quoted = new Set([...quotedBlock.matchAll(/CFG_TYPE_(\w+)/g)].map(match => match[1]))
    const rows = source => [...source.matchAll(/\{\s*"([^"]+)"\s*,\s*CFG_TYPE_(\w+)/g)].map(match => [match[1], match[2]])
    const arrays = { job: 'job_items', jobdefs: 'job_items', client: 'client_items', storage: 'store_items', pool: 'pool_items', schedule: 'sch_items' }
    for (const [type, metadata] of Object.entries(NATIVE_DIRECTIVES)) {
      const block = type === 'messages' ? messagesSource : directorSource.match(new RegExp(`(?:static )?const ResourceItem ${arrays[type]}\\[\\] = \\{([\\s\\S]*?)\\n\\};`))[1]
      const sourceRows = [...rows(block), ...(/TLS_COMMON_CONFIG/.test(block) ? rows(tlsSource) : [])]
      for (const [key, kind] of sourceRows) {
        if (kind === 'RUNSCRIPT') {
          expect([...metadata.quoted, ...metadata.bare]).not.toContain(key)
        } else expect(metadata[quoted.has(kind) ? 'quoted' : 'bare']).toContain(key)
      }
    }
  })
  it('rejects unsupported types, nested directives and extra Name rows', () => {
    for (const type of ['fileset', 'console', 'profile', 'director', '__proto__']) {
      expect(() => nativeResourceFields(type, 'archive', [])).toThrow()
      expect(() => newResourceRows(type)).toThrow()
    }
    for (const row of [{ key: 'Name', value: 'other' }, { key: 'RunScript', value: 'x' }, { key: 'Unknown', value: 'x' }]) {
      expect(() => nativeResourceFields('jobdefs', 'archive', [row])).toThrow()
    }
    expect(nativeResourceFields('pool', 'archive', [{ key: '', value: '' }])).toHaveLength(1)
    expect(() => nativeResourceFields('pool', 'archive', [{ key: '', value: 'not-empty' }])).toThrow()
  })
  it('creates fresh default rows with no existing passwords and supports empty installations', () => {
    for (const type of Object.keys(examples)) {
      const first = newResourceRows(type)
      const original = structuredClone(first)
      first[0].value = 'changed'
      expect(newResourceRows(type)).toEqual(original)
      expect(nativeDirectiveOptions(type).length).toBeGreaterThan(0)
    }
    expect(newResourceRows('client')).toEqual([{ key: 'Address', value: '' }, { key: 'Password', value: '' }])
    expect(() => newResourceRows('job')).toThrow()
  })
})
