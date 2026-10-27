/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026 Bareos GmbH & Co. KG

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

import { quoteDirectorString } from './directorStrings.js'

export const VOLUME_STATUS_OPTIONS = [
  'Append',
  'Full',
  'Used',
  'Archive',
  'Read-Only',
  'Disabled',
]

function checkedName(name, label) {
  if (typeof name !== 'string' || !name.trim()) {
    throw new TypeError(`${label} required`)
  }
  return name.trim()
}

export function buildUpdateVolumeStatusCommand(volumeName, status) {
  const name = checkedName(volumeName, 'Volume name')
  if (typeof status !== 'string' || !status.trim()) {
    throw new TypeError('Volume status required')
  }
  const canonical = VOLUME_STATUS_OPTIONS.find(
    opt => opt.toLowerCase() === status.trim().toLowerCase()
  )
  if (!canonical) {
    throw new TypeError(`Unknown volume status: ${status}`)
  }
  return `update volume=${quoteDirectorString(name)} volstatus=${canonical}`
}

export function buildPruneVolumeCommand(volumeName) {
  const name = checkedName(volumeName, 'Volume name')
  return `prune volume=${quoteDirectorString(name)} yes`
}

export function buildPurgeVolumeCommand(volumeName) {
  const name = checkedName(volumeName, 'Volume name')
  return `purge volume=${quoteDirectorString(name)} yes`
}

export function buildDeleteVolumeCommand(volumeName) {
  const name = checkedName(volumeName, 'Volume name')
  return `delete volume=${quoteDirectorString(name)} yes`
}

export function buildUpdatePoolFromConfigCommand(poolName) {
  const name = checkedName(poolName, 'Pool name')
  return `update pool=${quoteDirectorString(name)}`
}

export function captureVolumeActionTarget(volume, director, activeDirector) {
  return Object.freeze({ volume: checkedName(volume, 'Volume name'),
    director: checkedName(director, 'Director'), activeDirector })
}

// Stateless check of a captured target against the current scope: same volume
// and route Director, and the active Director is the target's own Director.
// Pages use actionToken.js for the permanent invalidation rule.
export function volumeActionTargetMatches(target, volume, director, activeDirector) {
  return Boolean(target) && target.volume === volume && target.director === director
    && activeDirector === target.director
}

// Pool prune: the pool, its Director and the volumes chosen when the dialog opened.
export function capturePoolPruneTarget(pool, names, director, activeDirector) {
  const base = captureVolumeActionTarget(pool, director, activeDirector)
  if (!Array.isArray(names) || names.length === 0) throw new TypeError('Volumes required')
  return Object.freeze({ ...base, names: Object.freeze(names.map(name => checkedName(name, 'Volume name'))) })
}

export function buildPrunePoolVolumesCommand(target) {
  return [
    `prune volume pool=${quoteDirectorString(target.volume)}`,
    ...target.names.map(name => `volume=${quoteDirectorString(name)}`),
    'yes',
  ].join(' ')
}

function extractMessages(obj) {
  if (!obj) return []
  if (typeof obj === 'string') return [obj]
  if (Array.isArray(obj)) return obj.flatMap(extractMessages)
  if (typeof obj === 'object') {
    return Object.values(obj).flatMap(extractMessages)
  }
  return [String(obj)]
}

function actionPayload(reply) {
  return reply?.type === 'response' ? reply.data : reply
}

function actionMessageGroups(reply) {
  return [reply, reply?.result].flatMap(layer => [
    layer?.error?.data?.messages, layer?.error?.messages, layer?.messages,
  ]).filter(Boolean)
}

export function formatDirectorReply(reply) {
  reply = actionPayload(reply)
  if (reply === null || reply === undefined) return ''
  if (typeof reply === 'string') return reply
  if (reply instanceof Error) return reply.message
  const lines = actionMessageGroups(reply).flatMap(extractMessages).filter(line => line.length > 0)
  if (lines.length) {
    return lines.reduce((text, line) => text + (text && !text.endsWith('\n') ? '\n' : '') + line, '')
  }
  const err = reply?.error ?? reply?.result?.error
  if (typeof err === 'string') return err
  if (typeof err?.message === 'string') return err.message

  const result = reply?.result
  if (typeof result === 'string') return result

  try {
    return JSON.stringify(reply, null, 2)
  } catch {
    return String(reply)
  }
}

export function parseDirectorActionReply(reply) {
  const value = actionPayload(reply)
  const layers = [value, value?.result]
  const failed = value instanceof Error || layers.some(layer => Boolean(layer?.error)
    || layer?.success === false || layer?.ok === false)
    || actionMessageGroups(value).some(group => extractMessages([group?.error, group?.fatal])
      .some(line => line.trim().length > 0))
  const received = value !== null && typeof value === 'object' && !Array.isArray(value)
    && value.result !== false && value.result !== null
  return { ok: received && !failed, text: formatDirectorReply(reply) }
}
