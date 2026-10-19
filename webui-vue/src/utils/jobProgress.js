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

export function parseNumberWithCommas(val) {
  if (typeof val === 'number') return Number.isFinite(val) ? val : 0
  if (typeof val !== 'string') return 0
  const clean = val.replace(/,/g, '').trim()
  const num = Number(clean)
  return Number.isFinite(num) ? num : 0
}

export function parseDurationSeconds(duration) {
  if (typeof duration === 'number') {
    return Number.isFinite(duration) && duration >= 0 ? duration : 0
  }
  if (typeof duration !== 'string') return 0
  const trimmed = duration.trim()
  if (!trimmed) return 0
  const parts = trimmed.split(':').map(Number)
  if (parts.some(p => isNaN(p) || p < 0)) return 0
  if (parts.length === 3) {
    return parts[0] * 3600 + parts[1] * 60 + parts[2]
  }
  if (parts.length === 2) {
    return parts[0] * 60 + parts[1]
  }
  if (parts.length === 1) {
    return parts[0]
  }
  return 0
}

export function parseFdRunning(text, jobId) {
  if (typeof text !== 'string' || !text || jobId == null) {
    return null
  }

  const escapedId = String(jobId).trim().replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
  if (!escapedId) return null

  const startRegex = new RegExp(`(?:^|\\n)\\s*JobId\\s*=?\\s*${escapedId}\\b[^\\n]*`, 'i')
  const startMatch = text.match(startRegex)
  if (!startMatch || startMatch.index == null) {
    return null
  }

  const startIndex = startMatch.index + startMatch[0].length
  const rest = text.slice(startIndex)
  const nextBlockMatch = rest.match(/(?:^|\n)\s*(?:JobId\b|Unknown connection\b|====|No Jobs running\b)/i)
  const blockText = nextBlockMatch && nextBlockMatch.index != null ? rest.slice(0, nextBlockMatch.index) : rest

  const statsMatch = blockText.match(
    /Files=([\d,]+)\s+Bytes=([\d,]+)\s+Bytes\/sec=([\d,]+)\s+Errors=([\d,]+)/i
  )
  if (!statsMatch) {
    return null
  }

  const fileMatch = blockText.match(/Processing file:\s*(.*)$/im)
  const processingFile = fileMatch ? fileMatch[1].trim() : null

  return {
    files: parseNumberWithCommas(statsMatch[1]),
    bytes: parseNumberWithCommas(statsMatch[2]),
    bytesPerSec: parseNumberWithCommas(statsMatch[3]),
    errors: parseNumberWithCommas(statsMatch[4]),
    processingFile,
  }
}

export function parseSdRunning(text, jobId) {
  if (typeof text !== 'string' || !text || jobId == null) {
    return null
  }

  const escapedId = String(jobId).trim().replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
  if (!escapedId) return null

  const startRegex = new RegExp(`(?:^|\\n)\\s*JobId\\s*=?\\s*${escapedId}\\b[^\\n]*`, 'i')
  const startMatch = text.match(startRegex)
  if (!startMatch || startMatch.index == null) {
    return null
  }

  const startIndex = startMatch.index + startMatch[0].length
  const rest = text.slice(startIndex)
  const nextBlockMatch = rest.match(
    /(?:^|\n)\s*(?:JobId\b|====|Device status:|Used Volume status:|No Jobs running\b)/i
  )
  const blockText = nextBlockMatch && nextBlockMatch.index != null ? rest.slice(0, nextBlockMatch.index) : rest

  const statsMatch = blockText.match(
    /Files=([\d,]+)\s+Bytes=([\d,]+)\s+AveBytes\/sec=([\d,]+)\s+LastBytes\/sec=([\d,]+)/i
  )
  if (!statsMatch) {
    return null
  }

  return {
    files: parseNumberWithCommas(statsMatch[1]),
    bytes: parseNumberWithCommas(statsMatch[2]),
    aveBytesPerSec: parseNumberWithCommas(statsMatch[3]),
    lastBytesPerSec: parseNumberWithCommas(statsMatch[4]),
  }
}

export function finalRate(job) {
  if (!job) return null

  const duration = job.duration ?? job.Duration
  if (duration == null) return null

  const seconds = parseDurationSeconds(duration)
  if (seconds <= 0) return null

  const rawBytes = job.jobbytes ?? job.bytes ?? job.JobBytes
  if (rawBytes == null) return null

  const bytes = parseNumberWithCommas(rawBytes)
  if (!Number.isFinite(bytes) || bytes < 0) return null

  return bytes / seconds
}

// Storages to ask for a running job's SD counters, only those that took the
// job: the final and dispatched member, the failover target, then the
// configured storage. The first member tried counts only when none refused.
export function sdStatusCandidates(job, events, configured = []) {
  const skipped = new Set((events?.attempts ?? [])
    .filter(attempt => attempt?.outcome === 'refused' || attempt?.outcome === 'skipped-disabled')
    .map(attempt => attempt.storage))
  const names = [
    events?.finalStorage,
    events?.dispatchedTo,
    events?.failedOverTo,
    job?.storage,
    ...(Array.isArray(configured) ? configured : [configured]),
    skipped.size === 0 ? events?.selected : null,
  ]
  const seen = new Set()
  const out = []
  for (const name of names) {
    if (typeof name !== 'string' || !name.trim() || seen.has(name) || skipped.has(name)) continue
    seen.add(name)
    out.push(name)
  }
  return out
}

// True once the job log shows the Director connected to a Storage Daemon, so a
// member the Director is still trying is not asked for status.
export function storageConnected(logEntries) {
  const entries = Array.isArray(logEntries) ? logEntries : []
  return entries.some(entry => /Connected Storage daemon at /.test(String(entry?.logtext ?? entry ?? '')))
}
