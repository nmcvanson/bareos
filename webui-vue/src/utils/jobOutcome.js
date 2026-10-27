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

export const WAITING_JOB_CODES = new Set([
  'C', 'F', 'S', 'm', 'M', 's', 'j', 'c', 'd', 't', 'p', 'q', 'i', 'a', 'l', 'L', 'B',
])

export const FAILED_JOB_CODES = new Set(['E', 'e', 'f'])

export const JOB_SUMMARY_HEADER_RE = /^(Error|Warning): Bareos \S+ \S+ \(/

export const JOB_OUTCOME_LABELS = {
  ok: 'OK',
  warning: 'Warning',
  failed: 'Failed',
  canceled: 'Canceled',
  running: 'Running',
  waiting: 'Waiting',
  unknown: 'Unknown',
}

export function resolveJobOutcomeState(code) {
  if (typeof code !== 'string' || !code) {
    return 'unknown'
  }
  const status = code.trim()
  if (status === 'T') return 'ok'
  if (status === 'W') return 'warning'
  if (FAILED_JOB_CODES.has(status)) return 'failed'
  if (status === 'A') return 'canceled'
  if (status === 'R') return 'running'
  if (WAITING_JOB_CODES.has(status)) return 'waiting'
  return 'unknown'
}

export function stripJobPrefix(line) {
  if (typeof line !== 'string') return ''
  return line
    .replace(/^\s*(?:\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\s+)?(?:\S+\s+)?JobId\s*=?\s*\d+:\s*/i, '')
    .trim()
}

export function normalizeLogLines(logEntries) {
  if (!logEntries) return []
  const entries = Array.isArray(logEntries) ? logEntries : [logEntries]
  const lines = []
  for (const entry of entries) {
    let text = ''
    if (typeof entry === 'string') {
      text = entry
    } else if (entry && typeof entry === 'object') {
      text = entry.logtext ?? entry.text ?? entry.msg ?? ''
    }
    if (text) {
      for (const line of text.split(/\r?\n/)) {
        const trimmed = line.trim()
        if (trimmed) {
          lines.push(trimmed)
        }
      }
    }
  }
  return lines
}

export function jobOutcome(job, logEntries) {
  const code =
    typeof job === 'string'
      ? job
      : (job?.jobstatus ?? job?.status ?? job?.runtimeStatus ?? '')
  const state = resolveJobOutcomeState(code)
  const label = JOB_OUTCOME_LABELS[state] ?? 'Unknown'

  const lines = normalizeLogLines(logEntries)

  let termination = null
  for (const line of lines) {
    const termMatch = line.match(/(?:^|\s)Termination:\s*(.*)$/i)
    if (termMatch) {
      const val = termMatch[1].trim()
      if (val) {
        termination = val
      }
    }
  }

  if (state !== 'failed' && state !== 'canceled' && state !== 'warning') {
    return {
      state,
      label,
      reasonLines: [],
      termination,
    }
  }

  const errorOrWarningLines = []
  let fdTermLine = null
  let sdTermLine = null

  for (const line of lines) {
    const fdMatch = line.match(/FD termination status:\s*(.+)$/i)
    if (fdMatch) {
      if (fdMatch[1].trim() !== 'OK') {
        fdTermLine = stripJobPrefix(line)
      }
      continue
    }

    const sdMatch = line.match(/SD termination status:\s*(.+)$/i)
    if (sdMatch) {
      if (sdMatch[1].trim() !== 'OK') {
        sdTermLine = stripJobPrefix(line)
      }
      continue
    }

    if (
      line.includes('Fatal error:') ||
      line.includes('Error:') ||
      line.includes('Warning:')
    ) {
      const stripped = stripJobPrefix(line)
      if (JOB_SUMMARY_HEADER_RE.test(stripped)) {
        continue
      }
      if (errorOrWarningLines.length < 5) {
        errorOrWarningLines.push(stripped)
      }
    }
  }

  const reasonLines = [...errorOrWarningLines]
  if (fdTermLine) reasonLines.push(fdTermLine)
  if (sdTermLine) reasonLines.push(sdTermLine)

  return {
    state,
    label,
    reasonLines,
    termination,
  }
}
