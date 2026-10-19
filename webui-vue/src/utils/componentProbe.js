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

async function executeRawCall(director, cmd) {
  if (director && typeof director.rawCall === 'function') {
    return await director.rawCall(cmd)
  }
  if (director && typeof director.call === 'function') {
    const res = await director.call(cmd)
    if (typeof res === 'string') return res
    if (res?.output && typeof res.output === 'string') return res.output
    if (res?.result && typeof res.result === 'string') return res.result
    return JSON.stringify(res)
  }
  throw new Error('Director does not support rawCall or call')
}

export async function probeComponents(director, { storages = [], clients = [], probeDirector = true } = {}) {
  const results = []

  // 1. Probe Director
  if (probeDirector) {
    try {
      const res = await executeRawCall(director, 'status director')
      const header = parseDaemonHeader(res)
      if (header) {
        results.push({
          id: 'director',
          kind: 'director',
          name: header.name || 'Director',
          reachable: true,
          header,
          error: null,
          probeTime: Date.now(),
        })
      } else {
        results.push({
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
      results.push({
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

  // 2. Probe Storage Daemons sequentially (one status call at a time)
  const daemonList = Array.isArray(storages) && storages[0]?.firstStorageName
    ? storages
    : groupStorageDaemons(storages)

  for (const sd of daemonList) {
    const target = sd.firstStorageName || sd.storages?.[0]?.name || sd.name
    if (!target) continue
    try {
      const res = await executeRawCall(director, 'status storage=' + quoteDirectorString(target))
      const header = parseDaemonHeader(res)
      if (header) {
        results.push({
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
        results.push({
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
      results.push({
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

  // 3. Probe Clients sequentially
  const clientList = Array.isArray(clients) && clients.length > 0 && typeof clients[0] === 'object' && clients[0]?.name && !clients[0]?.clients
    ? clients
    : extractClientList(clients)
  for (const c of clientList) {
    const cName = typeof c === 'string' ? c : c?.name
    if (!cName) continue
    try {
      const res = await executeRawCall(director, 'status client=' + quoteDirectorString(cName))
      const header = parseDaemonHeader(res)
      if (header) {
        results.push({
          id: cName,
          kind: 'client',
          name: cName,
          reachable: true,
          header,
          error: null,
          probeTime: Date.now(),
        })
      } else {
        results.push({
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
      results.push({
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

  return results
}
