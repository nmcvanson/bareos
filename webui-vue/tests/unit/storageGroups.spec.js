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

import capture from './fixtures/api2-show.txt?raw'
import verboseCapture from './fixtures/api2-verbose-defaults.txt?raw'
import { describe, expect, it } from 'vitest'
import {
  effectiveGroup, checkedDirectorValue, buildConfigureSetCommand,
  buildStorageEnabledCommand, buildReloadCommand, parseConfigureSetResult,
} from '../../src/utils/storageGroups.js'

const shown = (command, kind, name, text = capture) => {
  const after = text.replaceAll('\r\n', '\n').split(command + '\n')[1]
  return JSON.parse(after.slice(0, after.indexOf('\n}') + 2)).result[kind][name]
}
const job = shown('show job=backup-group', 'jobs', 'backup-group')
const failover = shown('show job=backup-s3-failover', 'jobs', 'backup-s3-failover')
const pool = shown('show pool=GroupPool', 'pools', 'GroupPool')

describe('effectiveGroup', () => {
  it('reads both lab job groups and the Pool group', () => {
    expect(effectiveGroup({ job })).toEqual({ members: ['File1a', 'File1b'], membersSource: 'Job', policy: 'ListedOrder', policySource: 'Job' })
    expect(effectiveGroup({ job: failover }).members).toEqual(['File2', 'S3'])
    expect(effectiveGroup({ job, pool })).toEqual({ members: ['File2', 'File1a'], membersSource: 'Pool', policy: 'ListedOrder', policySource: 'Pool' })
  })

  it('resolves Pool members and policy independently per verifier Q4', () => {
    expect(effectiveGroup({ job: { ...job, storagegrouppolicy: 'LeastUsed' }, pool: { storage: ['S3'] } })).toEqual({ members: ['S3'], membersSource: 'Pool', policy: 'LeastUsed', policySource: 'Job' })
    expect(effectiveGroup({ job, pool: { storage: [], storagegrouppolicy: 'LeastUsed' } })).toEqual({ members: ['File1a', 'File1b'], membersSource: 'Job', policy: 'LeastUsed', policySource: 'Pool' })
    expect(effectiveGroup({ job, pool: { storagegrouppolicy: '' } }).policySource).toBe('Job')
  })

  it('uses inherited values from verbose Job data and reports explicit defaults', () => {
    const inheritedJob = { jobdefs: 'DefaultJob', storage: ['File2', 'File1a'], storagegrouppolicy: 'LeastUsed' }
    expect(effectiveGroup({ job: inheritedJob })).toEqual({ members: ['File2', 'File1a'], membersSource: 'Job', policy: 'LeastUsed', policySource: 'Job' })
    expect(effectiveGroup({ job: { storage: ['S3'], storagegrouppolicy: '' } })).toEqual({ members: ['S3'], membersSource: 'Job', policy: 'ListedOrder', policySource: 'default' })
    expect(effectiveGroup()).toEqual({ members: [], membersSource: 'default', policy: 'ListedOrder', policySource: 'default' })
    const group = effectiveGroup({ job })
    group.members.reverse()
    expect(job.storage).toEqual(['File1a', 'File1b'])
  })

  it('reads the verbose lab Job without inventing a member or declared policy', () => {
    const verboseJob = shown('show job=demo-local-docs verbose', 'jobs', 'demo-local-docs', verboseCapture)
    expect(verboseJob).toMatchObject({ jobdefs: 'DefaultJob', pool: 'DemoLocalPool', storagegrouppolicy: '' })
    expect(effectiveGroup({ job: verboseJob })).toEqual({ members: [], membersSource: 'default', policy: 'ListedOrder', policySource: 'default' })
    expect(effectiveGroup({ job: verboseJob, pool }).membersSource).toBe('Pool')
  })
})

describe('storage commands', () => {
  it.each(['job', 'jobdefs', 'pool'])('builds policy, ordered members and previews for %s', resourceType => {
    const target = { resourceType, name: 'demo', directive: 'storagegrouppolicy', value: 'LeastUsed' }
    expect(buildConfigureSetCommand(target)).toBe(`configure set ${resourceType}="demo" storagegrouppolicy="LeastUsed"`)
    expect(buildConfigureSetCommand({ ...target, directive: 'storage', value: ['File2', 'S3'] })).toBe(`configure set ${resourceType}="demo" storage="File2,S3"`)
    expect(buildConfigureSetCommand({ ...target, preview: true })).toBe(`configure set ${resourceType}="demo" storagegrouppolicy="LeastUsed" preview`)
    expect(() => buildConfigureSetCommand({ ...target, directive: 'storage', value: [] })).toThrow()
    expect(() => buildConfigureSetCommand({ ...target, value: '' })).toThrow()
    expect(() => buildConfigureSetCommand({ ...target, directive: 'storage', value: '' })).toThrow()
  })

  it('builds persisted and runtime storage changes and reload', () => {
    expect(buildConfigureSetCommand({ resourceType: 'storage', name: 'File2', directive: 'enabled', value: 'no' })).toBe('configure set storage="File2" enabled="no"')
    expect(buildStorageEnabledCommand('File2', true)).toBe('enable storage="File2" yes')
    expect(buildStorageEnabledCommand('File2', false)).toBe('disable storage="File2" yes')
    expect(buildReloadCommand()).toBe('reload')
  })

  it.each(['yes', 'no', 'true', 'false'])('builds an enabled=%s preview', value => {
    expect(buildConfigureSetCommand({ resourceType: 'storage', name: 'File2', directive: 'enabled', value, preview: true })).toBe(`configure set storage="File2" enabled="${value}" preview`)
  })

  it.each(['@x', 'x;y', 'x"y', "x'y", 'x\ny', 'x\ry', 'x\\y', 'x}y', 'x\0y'])('rejects unsafe names and values: %j', value => {
    expect(() => checkedDirectorValue(value)).toThrow()
    expect(() => buildStorageEnabledCommand(value, true)).toThrow()
    expect(() => buildConfigureSetCommand({ resourceType: 'job', name: value, directive: 'storage', value: [] })).toThrow()
    expect(() => buildConfigureSetCommand({ resourceType: 'job', name: 'demo', directive: 'storage', value: [value] })).toThrow()
    expect(() => buildConfigureSetCommand({ resourceType: 'job', name: 'demo', directive: 'storagegrouppolicy', value })).toThrow()
  })

  it('rejects unsupported forms and malformed storage lists', () => {
    const target = { resourceType: 'job', name: 'demo', directive: 'storage', value: ['S3'] }
    for (const change of [
      { resourceType: 'client' }, { resourceType: '__proto__' }, { directive: 'password' },
      { name: '' }, { value: 'S3' }, { value: [''] }, { value: ['File2,S3'] }, { value: ['has space'] }, { preview: 'true' },
      { directive: 'storagegrouppolicy', value: 'Random' },
      { resourceType: 'storage', directive: 'enabled', value: true },
    ]) expect(() => buildConfigureSetCommand({ ...target, ...change })).toThrow()
    expect(() => buildStorageEnabledCommand('', true)).toThrow()
    expect(() => buildStorageEnabledCommand('S3', 'no')).toThrow()
  })
})

describe('configure set/create reply parsers', () => {
  const reply = (operation, data) => ({ result: { configure: { [operation]: {
    old: 'ListedOrder', new: 'LeastUsed', file: '/etc/bareos/job/demo.conf', reloaded: true, rollback: 'none', ...data,
  } } } })

  it('preserves returned values, file and rollback after a successful set', () => {
    expect(parseConfigureSetResult(reply('set'))).toEqual({ ok: true, old: 'ListedOrder', new: 'LeastUsed', filename: '/etc/bareos/job/demo.conf', content: null, preview: false, reloaded: true, rollback: 'none', error: null })
    expect(parseConfigureSetResult(reply('set', { old: ['File2'], new: ['S3'] })).new).toEqual(['S3'])
    expect(parseConfigureSetResult(reply('set', { old: 'no', new: 'yes' }).result).ok).toBe(true)
  })

  it('accepts checked previews without claiming a reload or synthesizing resource text', () => {
    expect(parseConfigureSetResult(reply('set', { reloaded: false }), { preview: true })).toMatchObject({ ok: true, preview: true, reloaded: false, old: 'ListedOrder', new: 'LeastUsed' })
  })

  it('returns Director errors and never treats an absent reload as success', () => {
    expect(parseConfigureSetResult({ error: { message: 'Permission denied' } }).error).toBe('Permission denied')
    expect(parseConfigureSetResult(reply('set', { error: 'reload failed, file restored', reloaded: false, rollback: 'restored' })).error).toBe('reload failed, file restored')
    for (const input of [null, {}, { reloaded: true }, reply('set', { reloaded: 'false' }), reply('set', { reloaded: false }), reply('set', { ok: false }), reply('set', { rollback: null }), reply('create')]) {
      expect(parseConfigureSetResult(input).ok).toBe(false)
    }
  })

  it.each(['restored', 'removed', 'failed'])('retains the %s rollback outcome and rejects success', rollback => {
    for (const [operation, parser] of [['set', parseConfigureSetResult]]) {
      const result = parser(reply(operation, { rollback, reloaded: false }))
      expect(result).toMatchObject({ ok: false, rollback, reloaded: false })
      expect(result.error).toBeTruthy()
      expect(parser(reply(operation, { rollback, reloaded: true })).ok).toBe(false)
      expect(parser(reply(operation, { rollback, reloaded: false }), { preview: true }).ok).toBe(false)
    }
  })

  // Shapes printed by the Director on a failed command (JSON-RPC error with the
  // configure object in error.data.result and the messages in error.data.messages).
  // set-restored and create-removed were captured from the WSL gtest Director
  // (core/src/tests/configure_set.cc); the failed variants are built from the source.
  const envelope = (operation, data, message) => ({
    jsonrpc: '2.0', id: null,
    error: { code: 1, message: 'failed', data: { result: { configure: { [operation]: {
      resource: 'Job', name: 'web-1', filename: '/etc/bareos/bareos-dir.d/job/web-1.conf', preview: false, reloaded: false, ...data,
    } } }, messages: { error: [message + '\n'] } } },
  })

  it.each([
    ['set', 'restored', 'reload failed, the file was restored', { directive: 'StorageGroupPolicy', old: '', new: 'LeastUsed' }],
    ['set', 'failed', 'ROLLBACK FAILED: reload failed, check "/etc/bareos/bareos-dir.d/job/web-1.conf"', { directive: 'StorageGroupPolicy', old: 'ListedOrder', new: 'LeastUsed' }],
  ])('reads the JSON-RPC error envelope of a failed \n (\n)', (operation, rollback, message, extra) => {
    const parser = parseConfigureSetResult
    const result = parser(envelope(operation, { rollback, ...extra }, message))
    expect(result).toMatchObject({ ok: false, reloaded: false, rollback, filename: '/etc/bareos/bareos-dir.d/job/web-1.conf', error: message })
    expect(result).toMatchObject({ old: extra.old, new: extra.new })
    if (rollback === 'failed') expect(result.error.startsWith('ROLLBACK FAILED')).toBe(true)
  })

  it('uses redacted passwords and checks the capture contains no other credential fields', () => {
    const values = [...capture.matchAll(/"password": "([^"]*)"/g)].map(match => match[1])
    expect(values).toEqual(['[md5]redacted', '[md5]redacted'])
    expect(capture).not.toMatch(/"(?:secret|token|credential|apikey)"\s*:/i)
  })
})
