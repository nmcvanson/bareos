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

import { quoteDirectorString } from './directorStrings.js'

export function effectiveGroup({ job, pool } = {}) {
  // Verbose Job data already includes JobDefs; resolve each Pool override alone.
  const resources = [['Pool', pool], ['Job', job]]
  const members = resources.find(([, resource]) => Array.isArray(resource?.storage) && resource.storage.length)
  const policy = resources.find(([, resource]) => resource?.storagegrouppolicy)
  return {
    members: [...(members?.[1].storage ?? [])], membersSource: members?.[0] ?? 'default',
    policy: policy?.[1].storagegrouppolicy ?? 'ListedOrder',
    policySource: policy?.[0] ?? 'default',
  }
}

// configure writes these values into resource files without escaping them.
export function checkedDirectorValue(value) {
  if (typeof value !== 'string' || /[@;'"\r\n\\{}\x00-\x1f\x7f]/.test(value)) {
    throw new TypeError('Forbidden character in Director argument')
  }
  return value
}

export function buildConfigureSetCommand({ resourceType, name, directive, value, preview = false }) {
  if (typeof preview !== 'boolean') throw new TypeError('Preview must be boolean')
  const allowed = {
    job: ['storage', 'storagegrouppolicy'],
    jobdefs: ['storage', 'storagegrouppolicy'],
    pool: ['storage', 'storagegrouppolicy'],
    storage: ['enabled'],
  }
  if (!Object.hasOwn(allowed, resourceType) || !allowed[resourceType].includes(directive)) {
    throw new TypeError('Unsupported resource or directive')
  }
  if (!checkedDirectorValue(name).trim()) throw new TypeError('Resource name required')
  let argument = value
  if (directive === 'storage') {
    if (value === '') argument = ''
    else if (Array.isArray(value)) {
      argument = value.map(member => {
        if (!checkedDirectorValue(member) || /[,\s#]/.test(member)) {
          throw new TypeError('Invalid storage member')
        }
        return member
      }).join(',')
    } else throw new TypeError('Storage members must be a list')
  }
  if (!checkedDirectorValue(argument).trim()) throw new TypeError('Directive value required')
  if (directive === 'storagegrouppolicy' && !['ListedOrder', 'LeastUsed'].includes(argument)) {
    throw new TypeError('Unsupported storage group policy')
  }
  if (directive === 'enabled' && !['yes', 'no', 'true', 'false'].includes(argument)) {
    throw new TypeError('Enabled must be yes, no, true or false')
  }
  return `configure set ${resourceType}=${quoteDirectorString(name)} ${directive}=${quoteDirectorString(argument)}${preview ? ' preview' : ''}`
}

export function buildStorageEnabledCommand(name, enabled) {
  if (!checkedDirectorValue(name).trim()) throw new TypeError('Storage name required')
  if (typeof enabled !== 'boolean') throw new TypeError('Enabled must be boolean')
  return `${enabled ? 'enable' : 'disable'} storage=${quoteDirectorString(name)} yes`
}

export function buildReloadCommand() {
  return 'reload'
}

function parseConfigureReply(json, operation, { preview = false } = {}) {
  // On failure the Director's JSON-RPC envelope holds the object in error.data.result
  // and its messages in error.data.messages.
  const envelope = json?.error?.data
  const result = json?.result ?? envelope?.result ?? json
  const data = result?.configure?.[operation]
  const failure = json?.error ?? result?.error ?? data?.error
  const messages = Array.isArray(envelope?.messages?.error)
    ? envelope.messages.error.map(m => String(m).trim()).filter(Boolean).join(' ')
    : ''
  const error = messages || (typeof failure === 'string' ? failure : failure?.message)
  const reloaded = data?.reloaded === true
  const rollback = ['none', 'restored', 'removed', 'failed'].includes(data?.rollback) ? data.rollback : null
  const isPreview = preview === true || data?.preview === true
  const content = data?.content ?? null
  const filename = data?.file ?? data?.filename ?? null
  const completePreview = isPreview && !reloaded && filename !== null
    && data?.new !== undefined
  const ok = Boolean(data) && !failure && data.ok !== false && rollback === 'none'
    && (isPreview ? completePreview : reloaded)
  return {
    ok,
    old: data?.old ?? null, new: data?.new ?? null,
    filename, content, preview: isPreview, reloaded, rollback,
    error: error ?? (ok ? null : rollback === 'failed' ? 'ROLLBACK FAILED'
      : rollback === 'restored' ? 'Reload failed; original file restored'
        : rollback === 'removed' ? 'Reload failed; created file removed'
          : isPreview ? 'Configuration preview failed' : 'Configuration was not reloaded'),
  }
}

export function parseConfigureSetResult(json, options) {
  return parseConfigureReply(json, 'set', options)
}
