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

// Live FD/SD counters of the job shown on the details page. Every sample is
// bound to the scope (Director and JobId) and epoch it was requested for, so a
// late reply of an older job or route is dropped, and a reset clears both.

export function liveScope(jobId, directorName) {
  return `${directorName ?? ''}\u0000${jobId ?? ''}`
}

export function createLiveProgress() {
  let scope = null
  let epoch = 0
  const sample = { fd: null, sd: null }
  let last = null
  return {
    get fd() { return sample.fd },
    get sd() { return sample.sd },
    // Last non-empty live files and bytes; kept until the final totals arrive.
    get last() { return last },
    // New job, Director or completion: forget samples and invalidate requests.
    reset(newScope = null) {
      scope = newScope
      epoch += 1
      sample.fd = null
      sample.sd = null
      last = null
    },
    // Ticket taken before a status request starts.
    ticket() { return { scope, epoch } },
    // Stores the sample only if the ticket is still current.
    accept(ticket, kind, value) {
      if (!ticket || ticket.epoch !== epoch || ticket.scope !== scope) return false
      if (kind !== 'fd' && kind !== 'sd') return false
      sample[kind] = value
      // Counters only grow: a smaller sample from the other daemon does not lower the kept numbers.
      if (value && Number.isFinite(value.files) && Number.isFinite(value.bytes)
          && (!last || value.bytes >= last.bytes)) {
        last = { files: value.files, bytes: value.bytes }
      }
      return true
    },
  }
}

// Files and bytes to show: live counters only while that job is running.
export function displayProgress({ job, running, fd, sd, last = null }) {
  const live = running === true && job != null
  return {
    files: (live ? (fd?.files ?? last?.files ?? sd?.files) : undefined) ?? job?.files ?? 0,
    bytes: (live ? (fd?.bytes ?? last?.bytes ?? sd?.bytes) : undefined) ?? job?.bytes ?? 0,
  }
}
