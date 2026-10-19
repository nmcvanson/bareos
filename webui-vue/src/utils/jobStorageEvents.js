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

const disabledMemberMatcher = /Storage group: skipping "([^"]+)", it is disabled\./

export function parseJobStorageEvents(logEntries) {
  const result = {
    candidates: null, policy: null, storageSource: null, selected: null,
    attempts: [], failedOverTo: null, finalStorage: null,
    allDisabled: false, dispatchedTo: null,
  }
  for (const entry of logEntries ?? []) {
    for (const line of String(entry?.logtext ?? '').split(/\r?\n/)) {
      let match = line.match(/Storage group: (\d+) candidates, using "([^"]+)" \((.+?), policy ([^)]+)\)\./)
      if (match) {
        result.candidates = Number(match[1])
        result.selected = match[2]
        result.storageSource = match[3]
        result.policy = match[4]
      }
      match = line.match(/Storage group: "([^"]+)" could not take the job, trying "([^"]+)"\./)
      if (match) result.attempts.push({ storage: match[1], outcome: 'refused' })
      match = line.match(disabledMemberMatcher)
      if (match) result.attempts.push({ storage: match[1], outcome: 'skipped-disabled' })
      if (line.includes('Storage group: every member is disabled, using the configured list unchanged.')) {
        result.allDisabled = true
      }
      match = line.match(/Storage group: (?:LeastUsed )?dispatching to "([^"]+)"\./)
      if (match) result.dispatchedTo = match[1]
      match = line.match(/Storage group: failed over to "([^"]+)" after \d+ attempts\./)
      if (match) result.failedOverTo = match[1]
      match = line.match(/^\s*Storage:\s*"([^"]+)" \(From [^)]+\)/)
      if (match) result.finalStorage = match[1]
    }
  }
  const used = result.finalStorage ?? result.failedOverTo
  if (used) result.attempts.push({ storage: used, outcome: 'used' })
  return result
}
