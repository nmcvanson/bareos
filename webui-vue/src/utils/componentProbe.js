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
import { parseDaemonHeader } from './deviceStatus.js'
import { directorCollection } from '../composables/useDirectorFetch.js'

export function extractStorageList(showStorages) {
  if (!showStorages) return []
  const container = showStorages?.storages ?? showStorages?.result?.storages ?? showStorages
  if (Array.isArray(container)) return container
  if (container && typeof container === 'object') {
    return Object.entries(container).map(([k, v]) => {
      if (v && typeof v === 'object' && !Array.isArray(v)) {
        return { name: k, ...v }
      }
      return { name: k }
    })
  }
  return []
}

export function extractClientList(showClients) {
  if (!showClients) return []
  const container = showClients?.clients ?? showClients?.result?.clients ?? showClients?.result ?? showClients
  if (Array.isArray(container)) {
    return container.map((c) => (typeof c === 'string' ? { name: c } : c)).filter(Boolean)
  }
  if (container && typeof container === 'object') {
    const inner = container?.clients ?? container
    if (Array.isArray(inner)) {
      return inner.map((c) => (typeof c === 'string' ? { name: c } : c)).filter(Boolean)
    }
    if (inner && typeof inner === 'object') {
      return Object.entries(inner).map(([k, v]) => {
        if (v && typeof v === 'object' && !Array.isArray(v)) {
          return { name: v.name || k, ...v }
        }
        return { name: k }
      })
    }
  }
  return []
}

export function groupStorageDaemons(showStorages) {
  const storageList = extractStorageList(showStorages)
  const groups = {}

  for (const s of storageList) {
    if (!s || !s.name) continue
    const address = (s.address || s.ip || 'localhost').trim()
    const port = s.sdport ?? s.SDport ?? s.port ?? 9103
    const key = `${address}:${port}`

    if (!groups[key]) {
      groups[key] = {
        key,
        address,
        port,
        storages: [],
        deviceNames: new Set(),
      }
    }

    groups[key].storages.push(s)
    const devs = Array.isArray(s.device) ? s.device : (s.device ? [s.device] : [])
    for (const d of devs) {
      if (typeof d === 'string') groups[key].deviceNames.add(d)
      else if (d?.name) groups[key].deviceNames.add(d.name)
    }
  }

  return Object.values(groups).map((g) => ({
    key: g.key,
    address: g.address,
    port: g.port,
    storages: g.storages,
    firstStorageName: g.storages[0]?.name || '',
    deviceCount: g.deviceNames.size,
  }))
}

const PROBE_LIMIT_MS = 20_000
const pendingByDirector = new WeakMap()

function startRawCall(director, cmd) {
  if (director && typeof director.rawCall === 'function') return director.rawCall(cmd)
  if (director && typeof director.call === 'function') {
    return Promise.resolve(director.call(cmd)).then(res => {
      if (typeof res === 'string') return res
      if (typeof res?.output === 'string') return res.output
      if (typeof res?.result === 'string') return res.result
      return JSON.stringify(res)
    })
  }
  throw new Error('Director does not support rawCall or call')
}

// At most one unanswered call per Director name and command; a later probe waits on it.
async function executeRawCall(director, cmd, directorName) {
  let timer
  const key = `${directorName ?? ''}\u0000${cmd}`
  const waiting = director !== null && typeof director === 'object' ? pendingByDirector.get(director)?.get(key) : undefined
  const deadline = new Promise((resolve, reject) => {
    timer = setTimeout(() => reject(new Error(waiting
      ? 'Previous probe is still waiting for an answer'
      : `No answer within ${PROBE_LIMIT_MS / 1000} s`)), PROBE_LIMIT_MS)
  })
  try {
    let request = waiting
    if (!request) {
      request = Promise.resolve(startRawCall(director, cmd))
      const pending = pendingByDirector.get(director) ?? new Map()
      pendingByDirector.set(director, pending)
      pending.set(key, request)
      const clear = () => { if (pending.get(key) === request) pending.delete(key) }
      request.then(clear, clear)
    }
    // The deadline bounds the UI wait; it cannot cancel a native Director call.
    return await Promise.race([request, deadline])
  } finally {
    clearTimeout(timer)
  }
}

export async function probeComponents(director, { storages = [], clients = [], probeDirector = true, onResult = null, directorName = '' } = {}) {
  const results = []
  const publish = result => {
    results.push(result)
    onResult?.(result)
  }

  // 1. Probe Director
  if (probeDirector) {
    try {
      const res = await executeRawCall(director, 'status director', directorName)
      const header = parseDaemonHeader(res)
      if (header) {
        publish({
          id: 'director',
          kind: 'director',
          name: header.name || 'Director',
          reachable: true,
          header,
          error: null,
          probeTime: Date.now(),
        })
      } else {
        publish({
          id: 'director',
          kind: 'director',
          name: 'Director',
          reachable: false,
          header: null,
          error: (typeof res === 'string' ? res.trim() : null) || 'Failed to parse Director status',
          probeTime: Date.now(),
        })
      }
    } catch (err) {
      publish({
        id: 'director',
        kind: 'director',
        name: 'Director',
        reachable: false,
        header: null,
        error: err?.message ?? String(err),
        probeTime: Date.now(),
      })
    }
  }

  // 2. Probe Clients before Storage Daemons
  const clientList = Array.isArray(clients) && clients.length > 0 && typeof clients[0] === 'object' && clients[0]?.name && !clients[0]?.clients
    ? clients
    : extractClientList(clients)
  for (const c of clientList) {
    const cName = typeof c === 'string' ? c : c?.name
    if (!cName) continue
    try {
      const res = await executeRawCall(director, 'status client=' + quoteDirectorString(cName), directorName)
      const header = parseDaemonHeader(res)
      if (header) {
        publish({
          id: cName,
          kind: 'client',
          name: cName,
          reachable: true,
          header,
          error: null,
          probeTime: Date.now(),
        })
      } else {
        publish({
          id: cName,
          kind: 'client',
          name: cName,
          reachable: false,
          header: null,
          error: (typeof res === 'string' ? res.trim() : null) || 'Failed to parse Client status',
          probeTime: Date.now(),
        })
      }
    } catch (err) {
      publish({
        id: cName,
        kind: 'client',
        name: cName,
        reachable: false,
        header: null,
        error: err?.message ?? String(err),
        probeTime: Date.now(),
      })
    }
  }

  // 3. Probe Storage Daemons last, sequentially
  const daemonList = Array.isArray(storages) && storages[0]?.firstStorageName
    ? storages
    : groupStorageDaemons(storages)

  for (const sd of daemonList) {
    const target = sd.firstStorageName || sd.storages?.[0]?.name || sd.name
    if (!target) continue
    try {
      const res = await executeRawCall(director, 'status storage=' + quoteDirectorString(target), directorName)
      const header = parseDaemonHeader(res)
      if (header) {
        publish({
          id: sd.key || target,
          kind: 'storage',
          key: sd.key,
          name: sd.key || header.name || target,
          storageName: target,
          reachable: true,
          header,
          error: null,
          probeTime: Date.now(),
        })
      } else {
        publish({
          id: sd.key || target,
          kind: 'storage',
          key: sd.key,
          name: sd.key || target,
          storageName: target,
          reachable: false,
          header: null,
          error: (typeof res === 'string' ? res.trim() : null) || 'Failed to parse Storage daemon status',
          probeTime: Date.now(),
        })
      }
    } catch (err) {
      publish({
        id: sd.key || target,
        kind: 'storage',
        key: sd.key,
        name: sd.key || target,
        storageName: target,
        reachable: false,
        header: null,
        error: err?.message ?? String(err),
        probeTime: Date.now(),
      })
    }
  }

  return results
}
