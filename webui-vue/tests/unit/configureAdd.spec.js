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

import { describe, expect, it, vi } from 'vitest'
import {
  ADD_RESOURCE_TYPES, validResourceName, buildConfigureAddCommand, buildConfigureAddPreview,
  redactConfigureText, parseNativeConfigureAddResult, parseNativeReloadResult, configureAddAndReload,
} from '../../src/utils/configureAdd.js'

const fields = [
  { key: 'Name', value: 'archive', quoted: true },
  { key: 'Description', value: 'Sao l\u01b0u \u65e5\u672c\u8a9e', quoted: true },
  { key: 'Enabled', value: 'yes', quoted: false },
]
const addReply = { result: { configure: { add: {
  resource: 'Client', name: 'archive', filename: '/config/client/archive.conf',
  content: 'Client {\n  Name = "archive"\n  Password = "synthetic-value"\n}\n',
} } } }
const reloadReply = { result: { reload: { success: true } } }

describe('native configure add commands and local previews', () => {
  it('formats a flat resource like config_add_directive', () => {
    expect(buildConfigureAddCommand('client', fields)).toBe('configure add client name="archive" description="Sao l\u01b0u \u65e5\u672c\u8a9e" enabled="yes"')
    expect(buildConfigureAddPreview('client', fields)).toBe('Client {\n  Name = "archive"\n  Description = "Sao l\u01b0u \u65e5\u672c\u8a9e"\n  Enabled = yes\n}\n')
    for (const type of Object.keys(ADD_RESOURCE_TYPES)) expect(buildConfigureAddCommand(type, fields)).toContain(`configure add ${type} `)
  })
  it('masks password directives in the preview', () => {
    const secret = { key: 'Password', value: 'synthetic-value', quoted: true }
    expect(buildConfigureAddPreview('client', [...fields, secret])).toContain('Password = "<redacted>"')
    expect(buildConfigureAddPreview('client', [...fields, secret])).not.toContain(secret.value)
  })
  it.each(['fileset', 'console', 'profile', 'director', 'catalog', '__proto__'])('refuses read-only type %s', type => {
    expect(() => buildConfigureAddCommand(type, fields)).toThrow()
    expect(() => buildConfigureAddPreview(type, fields)).toThrow()
  })
  it.each(['', '../archive', 'a/b', 'a\\b', 'a"b', 'a\nb', 'a;b'])('refuses invalid names %j', value => {
    expect(validResourceName(value)).toBe(false)
    expect(() => buildConfigureAddCommand('client', [{ ...fields[0], value }])).toThrow()
  })
  it('requires explicit quoting metadata and exactly one Name; rejects unsafe rows', () => {
    for (const rows of [null, [], [fields[0], fields[0]], [{ key: 'Name', value: 'archive' }],
      [fields[0], { key: 'Run Script', value: 'x', quoted: true }],
      [fields[0], { key: 'Enabled', value: 'yes # ignored', quoted: false }],
      [fields[0], { key: 'Address', value: '@include', quoted: true }],
      [fields[0], { key: 'Address', value: '', quoted: true }]]) {
      expect(() => buildConfigureAddCommand('client', rows)).toThrow()
    }
  })
})

describe('native configure replies', () => {
  it('reads add success in wrapped or unwrapped replies and masks returned passwords', () => {
    expect(parseNativeConfigureAddResult(addReply)).toMatchObject({ ok: true, name: 'archive', filename: '/config/client/archive.conf', error: null })
    expect(parseNativeConfigureAddResult(addReply.result).content).not.toContain('synthetic-value')
    expect(parseNativeReloadResult(reloadReply)).toMatchObject({ ok: true, success: true })
    expect(parseNativeReloadResult(reloadReply.result).ok).toBe(true)
  })
  it('preserves Director error whitespace and masks supplied secret values', () => {
    const error = 'Invalid value synthetic-value\nline 2\n'
    const json = { error: { message: 'failed', data: { messages: { error: [error] } } } }
    expect(parseNativeConfigureAddResult(json, { secrets: ['synthetic-value'] }).error).toBe('Invalid value <redacted>\nline 2\n')
    expect(redactConfigureText(null)).toBeNull()
  })
  it('does not accept missing or ambiguous success fields', () => {
    for (const input of [null, {}, { configure: { add: {} } }, { error: { message: 'Denied' }, ...addReply }]) {
      expect(parseNativeConfigureAddResult(input).ok).toBe(false)
    }
    for (const success of [false, 'true', 1, null, undefined]) {
      expect(parseNativeReloadResult({ reload: { success } }).ok).toBe(false)
    }
    expect(parseNativeReloadResult({ error: 'Denied', ...reloadReply }).error).toBe('Denied')
  })
})

describe('native add followed by reload', () => {
  it('sends only add then reload and keeps the two distinct replies', async () => {
    const call = vi.fn().mockResolvedValueOnce(addReply).mockResolvedValueOnce(reloadReply)
    const result = await configureAddAndReload(call, 'configure add client name="archive"')
    expect(call.mock.calls).toEqual([['configure add client name="archive"'], ['reload']])
    expect(result).toMatchObject({ ok: true, add: { ok: true }, reload: { ok: true }, reloaded: true })
  })
  it('stops after an add error without reload or any other call', async () => {
    const call = vi.fn().mockResolvedValue({ error: { message: 'Permission denied\n' } })
    expect(await configureAddAndReload(call, 'add')).toMatchObject({ ok: false, reload: null, error: 'Permission denied\n' })
    expect(call).toHaveBeenCalledTimes(1)
  })
  it('reports a created file alongside an export error but never proceeds to reload', async () => {
    const call = vi.fn().mockResolvedValue({ error: { message: 'failed', data: {
      result: addReply.result, messages: { error: ['Client export failed\n'] },
    } } })
    expect(await configureAddAndReload(call, 'add')).toMatchObject({
      ok: false, add: { ok: false, created: true }, reload: null, error: 'Client export failed\n',
    })
    expect(call).toHaveBeenCalledTimes(1)
  })
  it('retains successful creation if reload fails; never retries or claims rollback', async () => {
    const call = vi.fn().mockResolvedValueOnce(addReply).mockResolvedValueOnce({ reload: { success: false } })
    expect(await configureAddAndReload(call, 'add')).toMatchObject({ ok: false, add: { ok: true }, reload: { ok: false }, reloaded: false })
    expect(call).toHaveBeenCalledTimes(2)
  })
  it('stops after a transport error and redacts secrets in the exception text', async () => {
    const call = vi.fn().mockRejectedValue(new Error('Timed out password="synthetic-value"'))
    const result = await configureAddAndReload(call, 'add', { secrets: ['synthetic-value'] })
    expect(result).toMatchObject({ ok: false, add: null, reload: null })
    expect(result.error).not.toContain('synthetic-value')
    expect(call).toHaveBeenCalledTimes(1)
  })
  it('preserves the add reply when the reload transport fails', async () => {
    const call = vi.fn().mockResolvedValueOnce(addReply).mockRejectedValueOnce(new Error('Disconnected'))
    expect(await configureAddAndReload(call, 'add')).toMatchObject({ ok: false, add: { ok: true }, reload: null, error: 'Disconnected' })
    expect(call).toHaveBeenCalledTimes(2)
  })
})
