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

import capture from './fixtures/joblog-group.txt?raw'
import disabledCapture from './fixtures/joblog-disabled-member.txt?raw'
import { describe, expect, it } from 'vitest'
import { parseJobStorageEvents } from '../../src/utils/jobStorageEvents.js'

const logEntries = id => [{
  logtext: capture.replaceAll('\r\n', '\n').split(`list joblog jobid=${id}\n`)[1].split('list joblog jobid=')[0],
}]

describe('parseJobStorageEvents', () => {
  it('parses lab job 828 with no failover', () => {
    expect(parseJobStorageEvents(logEntries(828))).toEqual({
      candidates: 2, policy: 'ListedOrder', storageSource: 'Job resource',
      selected: 'File1a', attempts: [{ storage: 'File1a', outcome: 'used' }],
      failedOverTo: null, finalStorage: 'File1a',
      allDisabled: false, dispatchedTo: null,
    })
  })

  it('parses lab job 283 with a refusal and successful failover', () => {
    expect(parseJobStorageEvents(logEntries(283))).toEqual({
      candidates: 2, policy: 'ListedOrder', storageSource: 'Job resource', selected: 'File2',
      attempts: [{ storage: 'File2', outcome: 'refused' }, { storage: 'File1a', outcome: 'used' }],
      failedOverTo: 'File1a', finalStorage: 'File1a',
      allDisabled: false, dispatchedTo: null,
    })
  })

  it('ignores unknown lines and does not invent success from a next candidate', () => {
    expect(parseJobStorageEvents([
      { logtext: 'Storage group: 3 candidates, using "A" (Pool resource, policy LeastUsed).' },
      { logtext: 'Storage group: "A" could not take the job, trying "B".' },
      { logtext: 'Storage group: "B" could not take the job, trying "C".' },
      { logtext: 'Job failed. Storage group: unknown future message.' },
    ])).toEqual({
      candidates: 3, policy: 'LeastUsed', storageSource: 'Pool resource', selected: 'A',
      attempts: [{ storage: 'A', outcome: 'refused' }, { storage: 'B', outcome: 'refused' }],
      failedOverTo: null, finalStorage: null,
      allDisabled: false, dispatchedTo: null,
    })
  })

  it('reports a known used member before the final summary arrives', () => {
    expect(parseJobStorageEvents([{ logtext: 'Storage group: failed over to "B" after 2 attempts.' }]).attempts).toEqual([{ storage: 'B', outcome: 'used' }])
    expect(parseJobStorageEvents([{ logtext: 'Storage group: 2 candidates, using "A" (Job resource, policy ListedOrder).' }]).attempts).toEqual([])
  })

  it('accepts empty and unrelated entries', () => {
    expect(parseJobStorageEvents([{}, { logtext: 'unrelated' }])).toEqual(parseJobStorageEvents([]))
    expect(parseJobStorageEvents(null).finalStorage).toBeNull()
  })

  it('parses real job 1102 as a disabled skip followed by one candidate, without failover', () => {
    const entries = [{ logtext: disabledCapture.replaceAll('\r\n', '\n').split('list joblog jobid=1102\n')[1] }]
    expect(parseJobStorageEvents(entries)).toEqual({
      candidates: 1, policy: 'ListedOrder', storageSource: 'Pool resource', selected: 'File1a',
      attempts: [{ storage: 'File2', outcome: 'skipped-disabled' }, { storage: 'File1a', outcome: 'used' }],
      failedOverTo: null, finalStorage: 'File1a', allDisabled: false, dispatchedTo: null,
    })
  })

  it('retains all-disabled fallback and dispatch separately from initial selection and final storage', () => {
    expect(parseJobStorageEvents([{ logtext: `Storage group: every member is disabled, using the configured list unchanged.
Storage group: 2 candidates, using "A" (Job resource, policy LeastUsed).
Storage group: dispatching to "B".
  Storage: "B" (From Job resource)` }])).toEqual({
      candidates: 2, policy: 'LeastUsed', storageSource: 'Job resource', selected: 'A',
      attempts: [{ storage: 'B', outcome: 'used' }], failedOverTo: null, finalStorage: 'B',
      allDisabled: true, dispatchedTo: 'B',
    })
    const partial = parseJobStorageEvents([{ logtext: 'Storage group: dispatching to "B".' }])
    expect(partial.dispatchedTo).toBe('B')
    expect(partial.finalStorage).toBeNull()
    expect(partial.attempts).toEqual([])
  })
})
