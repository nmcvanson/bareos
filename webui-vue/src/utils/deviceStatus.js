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

import { chunkedDeviceParser } from './chunkedDeviceStatus.js'

export const BACKEND_PARSERS = [chunkedDeviceParser]

export function parseDaemonHeader(text) {
  if (!text || typeof text !== 'string') return null
  let currentName = null
  let currentVersion = null

  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim()
    const vMatch = line.match(/^(?:1000 OK:\s+)?(\S+)\s+Version:\s+([^\s(]+)/)
    if (vMatch) {
      currentName = vMatch[1]
      currentVersion = vMatch[2]
    }
    const sMatch = line.match(/^Daemon started (.*?)\.\s+Jobs:\s+run=(\d+)[,]?\s+running=(\d+)/)
    if (sMatch && currentName) {
      return {
        name: currentName,
        version: currentVersion,
        started: sMatch[1],
        jobsRun: Number(sMatch[2]),
        jobsRunning: Number(sMatch[3]),
      }
    }
  }

  if (currentName) {
    return {
      name: currentName,
      version: currentVersion,
      started: null,
      jobsRun: null,
      jobsRunning: null,
    }
  }

  return null
}

function buildDeviceEntry({ name, archive, rest, lines }) {
  let state = rest.trim()
  if (state.endsWith('is not open.')) {
    state = 'not open'
  } else if (state.includes('is mounted with:')) {
    state = 'mounted'
  }

  let volume = null
  let pool = null
  let mediaType = null
  let totalBytes = null
  let blocks = null
  let totalBytesRead = null
  let blocksRead = null
  let positionFile = null
  let positionBlock = null
  let slot = null

  for (const rawLine of lines) {
    const line = rawLine.trim()

    let m = line.match(/^Volume:\s*(.*)$/)
    if (m) volume = m[1].trim() || null

    m = line.match(/^Pool:\s*(.*)$/)
    if (m) pool = m[1].trim() || null

    m = line.match(/^Media type:\s*(.*)$/)
    if (m) mediaType = m[1].trim() || null

    m = line.match(/^Total Bytes=([\d,]+)\s+Blocks=([\d,]+)/)
    if (m) {
      totalBytes = Number(m[1].replaceAll(',', ''))
      blocks = Number(m[2].replaceAll(',', ''))
    }

    m = line.match(/^Total Bytes Read=([\d,]+)\s+Blocks Read=([\d,]+)/)
    if (m) {
      totalBytesRead = Number(m[1].replaceAll(',', ''))
      blocksRead = Number(m[2].replaceAll(',', ''))
    }

    m = line.match(/^Positioned at File=([\d,]+)\s+Block=([\d,]+)/)
    if (m) {
      positionFile = Number(m[1].replaceAll(',', ''))
      positionBlock = Number(m[2].replaceAll(',', ''))
    }

    m = line.match(/^Slot\s+(\d+)\s+(?:is|was last)\s+loaded/)
    if (m) {
      slot = Number(m[1])
    }
  }

  let backend = null
  for (const parser of BACKEND_PARSERS) {
    const metrics = parser(lines)
    if (metrics !== null) {
      backend = { kind: parser.kind ?? 'chunked', metrics }
      break
    }
  }

  return {
    name,
    archive,
    state,
    volume,
    pool,
    mediaType,
    totalBytes,
    blocks,
    totalBytesRead,
    blocksRead,
    positionFile,
    positionBlock,
    slot,
    lines,
    backend,
  }
}

export function parseDeviceBlocks(text) {
  if (!text || typeof text !== 'string') return []
  const blocks = []
  let currentBlock = null

  for (const rawLine of text.split(/\r?\n/)) {
    const trimmed = rawLine.trim()

    if (trimmed === 'Used Volume status:' || trimmed === '====') {
      if (currentBlock) {
        blocks.push(currentBlock)
        currentBlock = null
      }
      continue
    }

    const deviceMatch = rawLine.match(/^\s*Device\s+"([^"]+)"(?:\s+\((.*?)\))?\s*(.*)$/)
    if (deviceMatch) {
      if (currentBlock) {
        blocks.push(currentBlock)
      }
      currentBlock = {
        name: deviceMatch[1],
        archive: deviceMatch[2] ?? null,
        rest: deviceMatch[3] ?? '',
        lines: [rawLine],
      }
      continue
    }

    if (currentBlock) {
      currentBlock.lines.push(rawLine)
    }
  }

  if (currentBlock) {
    blocks.push(currentBlock)
  }

  return blocks.map(block => buildDeviceEntry(block))
}

export function parseRunningJobs(text) {
  if (!text || typeof text !== 'string') return []
  const jobs = []

  const invMatch = text.match(/Job inventory:\s*\r?\n([\s\S]*?)(?:\r?\n====|\r?\nJobs waiting|\r?\nTerminated Jobs)/i)
  const section = invMatch ? invMatch[1] : ''
  if (!section || section.includes('No Jobs running.')) return []

  const jobBlocks = section.split(/(?=(?:JobId=\d+|\b\S+\s+Job\s+\S+\s+waiting for Client))/i)
  for (const block of jobBlocks) {
    const trimmed = block.trim()
    if (!trimmed) continue
    const head = trimmed.match(/JobId=(\d+)\s+Level=(\S+)\s+Type=(\S+)\s+Name=(\S+)\s+Status=(\S+)/i)
    if (head) {
      const volMatch = trimmed.match(/(?:Writing|Reading):\s*Volume="([^"]*)"/i)
      const poolDevMatch = trimmed.match(/pool="([^"]*)"\s+device="?([^"\s()]+)/i)
      const statsMatch = trimmed.match(/Files=([\d,]+)\s+Bytes=([\d,]+)\s+AveBytes\/sec=([\d,]+)\s+LastBytes\/sec=([\d,]+)/i)
      jobs.push({
        jobId: Number(head[1]),
        level: head[2],
        type: head[3],
        name: head[4],
        status: head[5],
        volume: volMatch ? volMatch[1] : null,
        pool: poolDevMatch ? poolDevMatch[1] : null,
        device: poolDevMatch ? poolDevMatch[2] : null,
        files: statsMatch ? Number(statsMatch[1].replaceAll(',', '')) : null,
        bytes: statsMatch ? Number(statsMatch[2].replaceAll(',', '')) : null,
        aveBytesPerSec: statsMatch ? Number(statsMatch[3].replaceAll(',', '')) : null,
        lastBytesPerSec: statsMatch ? Number(statsMatch[4].replaceAll(',', '')) : null,
      })
    } else {
      const wait = trimmed.match(/^(\S+)\s+Job\s+(\S+)\s+waiting for Client connection/i)
      if (wait) {
        jobs.push({
          jobId: null,
          level: null,
          type: wait[1],
          name: wait[2],
          status: 'Waiting for client',
          volume: null,
          pool: null,
          device: null,
          files: null,
          bytes: null,
          aveBytesPerSec: null,
          lastBytesPerSec: null,
        })
      }
    }
  }

  return jobs
}

export function trimHistory(samples, nowMs, keepMs = 600000) {
  if (!Array.isArray(samples)) return []
  const cutoff = nowMs - keepMs
  const kept = samples.filter(s => s && typeof s.t === 'number' && s.t >= cutoff)
  try {
    samples.length = 0
    samples.push(...kept)
    return samples
  } catch {
    return kept
  }
}

export function pairRates(samples, deviceId) {
  if (!Array.isArray(samples)) return []
  const deviceSamples = samples
    .filter(s => (s?.device?.id === deviceId) || (s?.device?.name === deviceId))
    .sort((a, b) => a.t - b.t)

  const points = []
  for (let i = 1; i < deviceSamples.length; i++) {
    const prev = deviceSamples[i - 1]
    const curr = deviceSamples[i]
    const dt = (curr.t - prev.t) / 1000

    let uploadMBs = null
    let downloadMBs = null

    if (dt > 0) {
      const prevUp = prev?.device?.upload?.bytes
      const currUp = curr?.device?.upload?.bytes
      if (Number.isFinite(prevUp) && Number.isFinite(currUp)) {
        const deltaUp = currUp - prevUp
        if (deltaUp >= 0) {
          uploadMBs = deltaUp / dt / 1e6
        }
      }

      const prevDown = prev?.device?.download?.bytes
      const currDown = curr?.device?.download?.bytes
      if (Number.isFinite(prevDown) && Number.isFinite(currDown)) {
        const deltaDown = currDown - prevDown
        if (deltaDown >= 0) {
          downloadMBs = deltaDown / dt / 1e6
        }
      }
    }

    points.push({
      t: curr.t,
      uploadMBs,
      downloadMBs,
    })
  }
  return points
}

export function formatMBs(value) {
  if (value === null || value === undefined || !Number.isFinite(Number(value))) {
    return '—'
  }
  return `${Number(value).toFixed(2)} MB/s`
}

export function formatCount(value) {
  if (value === null || value === undefined || !Number.isFinite(Number(value))) {
    return '—'
  }
  return Number(value).toLocaleString()
}

export function mergeDaemonDevices(listOfParsedStatuses) {
  if (!Array.isArray(listOfParsedStatuses)) {
    return []
  }

  const merged = new Map()

  for (const entry of listOfParsedStatuses) {
    if (!entry) continue
    const storageName = entry.storage ?? entry.storageName ?? entry.name ?? null
    let devices = []
    if (Array.isArray(entry.devices)) {
      devices = entry.devices
    } else if (typeof entry.text === 'string') {
      devices = parseDeviceBlocks(entry.text)
    } else if (Array.isArray(entry)) {
      devices = entry
    }

    for (const dev of devices) {
      if (!dev || !dev.name) continue
      const name = dev.name
      if (!merged.has(name)) {
        const initialStorages = []
        if (Array.isArray(dev.storages)) {
          initialStorages.push(...dev.storages)
        } else if (dev.storage) {
          initialStorages.push(dev.storage)
        }
        if (storageName && !initialStorages.includes(storageName)) {
          initialStorages.push(storageName)
        }
        merged.set(name, {
          ...dev,
          storages: initialStorages,
        })
      } else {
        const existing = merged.get(name)
        if (storageName && !existing.storages.includes(storageName)) {
          existing.storages.push(storageName)
        }
        if (Array.isArray(dev.storages)) {
          for (const s of dev.storages) {
            if (s && !existing.storages.includes(s)) {
              existing.storages.push(s)
            }
          }
        }
      }
    }
  }

  return [...merged.values()]
}

