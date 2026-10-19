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

export function chunkedDeviceParser(lines) {
  let chunks = null
  let upload = { chunks: null, bytes: null, failedTries: null, retriedTries: null }
  let download = { objects: null, bytes: null, failed: null, transportRetries: null }
  let lastUpload = { state: null, agoS: null, error: null }
  let refusesWrites = null

  for (const rawLine of lines ?? []) {
    const line = String(rawLine).trim()
    if (line === 'No upload since the daemon started.') {
      lastUpload = { state: 'none', agoS: null, error: null }
    }
    let match = line.match(/^Chunks: received (\d+), uploaded (\d+), pending (\d+) \(queued (\d+), uploading (\d+), kept (\d+)\), waiting for retry (\d+)(?:, oldest pending (\d+) s)?\.$/)
    if (match) {
      chunks = {
        received: Number(match[1]), uploaded: Number(match[2]),
        pending: Number(match[3]), queued: Number(match[4]),
        uploading: Number(match[5]), kept: Number(match[6]),
        waitingRetry: Number(match[7]),
        oldestPendingS: match[8] === undefined ? null : Number(match[8]),
      }
    }
    match = line.match(/^Upload: (\d+) chunks, (\d+) bytes; failed tries (\d+), retried tries (\d+)\.$/)
    if (match) {
      upload = {
        chunks: Number(match[1]), bytes: Number(match[2]),
        failedTries: Number(match[3]), retriedTries: Number(match[4]),
      }
    }
    match = line.match(/^Download: (\d+) objects, (\d+) bytes, failed (\d+); transport retries (\d+)\.$/)
    if (match) {
      download = {
        objects: Number(match[1]), bytes: Number(match[2]),
        failed: Number(match[3]), transportRetries: Number(match[4]),
      }
    }
    match = line.match(/^Last upload (succeeded|failed) (\d+) s ago(?:\.|: (.*))?$/)
    if (match) {
      lastUpload = {
        state: match[1] === 'succeeded' ? 'ok' : 'failed',
        agoS: Number(match[2]), error: match[3] ?? null,
      }
    }
    match = line.match(/^Device refuses writes: (.*)$/)
    if (match) refusesWrites = match[1]
  }

  if (chunks === null) return null
  return { chunks, upload, download, lastUpload, refusesWrites }
}
chunkedDeviceParser.kind = 'chunked'

// Counters use Number: integer values through 2^53 are exact; larger SD counters
// may lose precision. Rates use seconds/minutes and deduped ids (or device names).
export function parseStorageStatus(text) {
  const devices = []
  let device = null
  let currentLines = []

  const flush = () => {
    if (device && currentLines.length > 0) {
      const metrics = chunkedDeviceParser(currentLines)
      if (metrics !== null) {
        devices.push({ name: device.name, ...metrics })
      }
    }
  }

  for (const rawLine of String(text ?? '').split(/\r?\n/)) {
    const line = rawLine.trim()
    const match = line.match(/^Device "([^"]+)"/)
    if (match) {
      flush()
      device = { name: match[1] }
      currentLines = [rawLine]
    } else if (device) {
      currentLines.push(rawLine)
    }
  }
  flush()
  return { devices }
}

export const S3_TOTAL_FIELDS = Object.freeze([
  'upload.bytes', 'download.bytes', 'chunks.uploaded', 'chunks.received',
  'upload.chunks', 'upload.failedTries', 'upload.retriedTries',
  'download.objects', 'download.failed', 'download.transportRetries',
])
export const S3_GAUGE_FIELDS = Object.freeze([
  'chunks.pending', 'chunks.queued', 'chunks.uploading', 'chunks.kept',
  'chunks.waitingRetry', 'chunks.oldestPendingS',
])

// Only totals detect restarts; gauges describe the current queue, with no rates.
const counters = device => S3_TOTAL_FIELDS.map(path => (
  path.split('.').reduce((value, key) => value?.[key], device) ?? null
))

// First snapshot wins for aliases of one SD/device. Missing addresses stay
// separate by storage, avoiding accidental merging of unknown SD identities.
export function dedupeDevices(list) {
  const unique = new Map()
  for (const { storage, address, devices } of list) {
    const sd = String(address ?? '').trim().toLowerCase()
    for (const device of devices ?? []) {
      const id = JSON.stringify([sd || `storage:${storage}`, device.name])
      if (!unique.has(id)) {
        unique.set(id, { ...device, id, storage, address: address ?? null, storages: [] })
      }
      const aliases = unique.get(id).storages
      if (!aliases.includes(storage)) aliases.push(storage)
    }
  }
  return [...unique.values()]
}

export function computeRates(samples, windowS = 30) {
  if (!Number.isFinite(windowS) || windowS <= 0) {
    throw new RangeError('Rate window must be positive')
  }
  const groups = new Map()
  for (const sample of samples) {
    const name = sample.device.id ?? sample.device.name
    if (!groups.has(name)) groups.set(name, [])
    groups.get(name).push(sample)
  }
  return Object.fromEntries([...groups].map(([name, history]) => {
    const latest = history.at(-1)
    const cutoff = latest.t - windowS * 1000
    let baseline = history[0]
    let previous = counters(baseline.device)
    let reset = false
    for (let i = 1; i < history.length; i += 1) {
      const sample = history[i]
      const current = counters(sample.device)
      if (current.some((value, index) => Number.isFinite(value) && Number.isFinite(previous[index]) && value < previous[index])) {
        baseline = sample
        if (sample.t >= cutoff) reset = true
      } else if (sample.t <= cutoff) {
        baseline = sample
      } else if (baseline.t < cutoff) {
        // Interpolate a boundary between polls to keep a true sliding window.
        const fraction = (cutoff - baseline.t) / (sample.t - baseline.t)
        const start = counters(baseline.device)
        const boundary = start.map((value, index) => (
          Number.isFinite(value) && Number.isFinite(current[index])
            ? value + (current[index] - value) * fraction : null
        ))
        baseline = { t: cutoff, device: baseline.device, boundary }
      }
      previous = current
    }
    const elapsedS = (latest.t - baseline.t) / 1000
    const start = baseline.boundary ?? counters(baseline.device)
    const end = counters(latest.device)
    const rates = end.slice(0, 4).map((value, index) => (
      !Number.isFinite(value) || !Number.isFinite(start[index]) ? null
        : elapsedS > 0 ? Math.max(0, value - start[index]) / elapsedS : 0
    ))
    return [name, {
      uploadBytesPerS: rates[0], downloadBytesPerS: rates[1],
      uploadedChunksPerMin: rates[2] === null ? null : rates[2] * 60,
      receivedChunksPerMin: rates[3] === null ? null : rates[3] * 60, reset,
    }]
  }))
}
