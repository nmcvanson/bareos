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

import { checkedDirectorValue } from './storageGroups.js'
import { quoteDirectorString } from './directorStrings.js'

export const ADD_RESOURCE_TYPES = Object.freeze({
  job: 'Job', jobdefs: 'JobDefs', client: 'Client', pool: 'Pool',
  storage: 'Storage', schedule: 'Schedule', messages: 'Messages',
})

export function validResourceName(name) {
  return typeof name === 'string' && /^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(name)
}

function checkedFields(type, fields) {
  if (!Object.hasOwn(ADD_RESOURCE_TYPES, type)) throw new TypeError('Unsupported resource type')
  if (!Array.isArray(fields)) throw new TypeError('Directive rows required')
  if (fields.filter(field => field.key?.toLowerCase() === 'name').length !== 1) {
    throw new TypeError('Exactly one Name directive required')
  }
  return fields.map(field => {
    if (!/^[A-Za-z]+$/.test(field.key)) throw new TypeError('Invalid directive name')
    const value = checkedDirectorValue(field.value)
    if (!value.trim()) throw new TypeError('Directive value required')
    if (field.key.toLowerCase() === 'name' && !validResourceName(value)) throw new TypeError('Invalid resource name')
    if (typeof field.quoted !== 'boolean') throw new TypeError('Directive quoting metadata required')
    if (!field.quoted && value.includes('#')) throw new TypeError('Comment in unquoted directive value')
    return { ...field, value }
  })
}

export function buildConfigureAddCommand(type, fields) {
  return `configure add ${type} ${checkedFields(type, fields)
    .map(({ key, value }) => `${key.toLowerCase()}=${quoteDirectorString(value)}`).join(' ')}`
}

export function buildConfigureAddPreview(type, fields) {
  const lines = checkedFields(type, fields).map(({ key, value, quoted }) => {
    const shown = /password/i.test(key) ? '<redacted>' : value
    return `  ${key} = ${quoted ? `"${shown}"` : shown}\n`
  })
  return `${ADD_RESOURCE_TYPES[type]} {\n${lines.join('')}}\n`
}

export function redactConfigureText(text, secrets = []) {
  if (typeof text !== 'string') return null
  let result = text
  for (const secret of [...secrets].filter(Boolean).sort((a, b) => b.length - a.length)) {
    result = result.split(secret).join('<redacted>')
  }
  return result.replace(/(\b\w*password\w*\s*=\s*)("[^"\n]*"|[^\s\n]+)/gi, '$1"<redacted>"')
}

function nativeReply(json, secrets) {
  const envelope = json?.error?.data
  const result = json?.result ?? envelope?.result ?? json
  const messages = envelope?.messages ?? json?.messages ?? result?.messages ?? {}
  const errors = Array.isArray(messages.error) ? messages.error.join('') : messages.error
  const failure = json?.error ?? result?.error
  const error = redactConfigureText(typeof errors === 'string' && errors ? errors
    : typeof failure === 'string' ? failure : failure?.message, secrets)
  return { result, failed: Boolean(failure || error), error,
    messages: Object.values(messages).flat().filter(value => typeof value === 'string')
      .map(value => redactConfigureText(value, secrets)) }
}

export function parseNativeConfigureAddResult(json, { secrets = [] } = {}) {
  const { result, failed, error, messages } = nativeReply(json, secrets)
  const data = result?.configure?.add
  const created = Boolean(data) && ['resource', 'name', 'filename', 'content']
    .every(key => typeof data[key] === 'string' && data[key].length > 0)
  const ok = !failed && created
  return {
    ok, created, resource: data?.resource ?? null, name: data?.name ?? null,
    filename: redactConfigureText(data?.filename, secrets),
    content: redactConfigureText(data?.content, secrets), messages,
    error: error ?? (ok ? null : 'Configuration add did not confirm creation'),
  }
}

export function parseNativeReloadResult(json, { secrets = [] } = {}) {
  const { result, failed, error, messages } = nativeReply(json, secrets)
  const ok = !failed && result?.reload?.success === true
  return { ok, success: result?.reload?.success ?? null, messages,
    error: error ?? (ok ? null : 'Configuration reload did not succeed') }
}

export async function configureAddAndReload(call, command, { secrets = [] } = {}) {
  let add = null
  let reload = null
  try {
    add = parseNativeConfigureAddResult(await call(command), { secrets })
    if (!add.ok) return { ok: false, add, reload, reloaded: false, error: add.error }
    reload = parseNativeReloadResult(await call('reload'), { secrets })
    return { ok: reload.ok, add, reload, reloaded: reload.ok, error: reload.error }
  } catch (error) {
    return { ok: false, add, reload, reloaded: false,
      error: redactConfigureText(error?.message ?? String(error), secrets) }
  }
}
