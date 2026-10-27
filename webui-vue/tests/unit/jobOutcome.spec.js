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

import capture from './fixtures/joblog-group.txt?raw'
import { describe, expect, it } from 'vitest'
import {
  jobOutcome,
  resolveJobOutcomeState,
  stripJobPrefix,
  WAITING_JOB_CODES,
  FAILED_JOB_CODES,
  JOB_OUTCOME_LABELS,
} from '../../src/utils/jobOutcome.js'

const logEntriesFromCapture = id => [{
  logtext: capture.replaceAll('\r\n', '\n').split(`list joblog jobid=${id}\n`)[1].split('list joblog jobid=')[0],
}]

describe('jobOutcome', () => {
  it('parses an OK job (job 828 in fixture)', () => {
    const job = { jobid: 828, jobstatus: 'T' }
    const entries = logEntriesFromCapture(828)
    const result = jobOutcome(job, entries)

    expect(result).toEqual({
      state: 'ok',
      label: 'OK',
      reasonLines: [],
      termination: 'Backup OK',
    })
  })

  it('parses a failover job with warnings (job 283 in fixture)', () => {
    const job = { jobid: 283, jobstatus: 'W' }
    const entries = logEntriesFromCapture(283)
    const result = jobOutcome(job, entries)

    expect(result.state).toBe('warning')
    expect(result.label).toBe('Warning')
    expect(result.termination).toBe('Backup OK')
    expect(result.reasonLines).toEqual([
      'Warning: lib/bsock_tcp.cc:171 Could not connect to Storage daemon on lab-sd2:9103. ERR=Connection refused',
      'Warning: lib/bsock_tcp.cc:179 Unable to connect to Storage daemon on lab-sd2:9103. ERR=Connection refused',
      'Warning: Storage group: "File2" could not take the job, trying "File1a".',
    ])
  })

  it('parses a synthetic failed job from real failure text (job 1101 on lab)', () => {
    const job = { jobid: 1101, jobstatus: 'f' }
    const entries = [
      {
        logtext:
          'lab-dir JobId 1101: Fatal error: cats/sql_create.cc:801 Fill Path table Query failed: ... ERR=ERROR:  relation "batch" does not exist\n' +
          'lab-dir JobId 1101: Error: Bareos lab-dir 25.1.1 (14Sep25):\n' +
          'Termination:            *** Backup Error ***\n',
      },
    ]
    const result = jobOutcome(job, entries)

    expect(result).toEqual({
      state: 'failed',
      label: 'Failed',
      reasonLines: [
        'Fatal error: cats/sql_create.cc:801 Fill Path table Query failed: ... ERR=ERROR:  relation "batch" does not exist',
      ],
      termination: '*** Backup Error ***',
    })
    expect(result.reasonLines).not.toContain('Error: Bareos lab-dir 25.1.1 (14Sep25):')
  })

  it('skips job summary headers from reasonLines for warning and error lines', () => {
    const job = { jobstatus: 'W' }
    const entries = [
      {
        logtext:
          'lab-dir JobId 123: Warning: Bareos lab-dir 25.1.1 (14Sep25):\n' +
          'lab-dir JobId 123: Warning: real warning line\n' +
          'Termination: Backup OK -- with warnings\n',
      },
    ]
    const result = jobOutcome(job, entries)
    expect(result.reasonLines).toEqual(['Warning: real warning line'])
  })

  it('maps all failed job codes (E, e, f) to failed', () => {
    for (const code of FAILED_JOB_CODES) {
      const res = jobOutcome({ jobstatus: code }, [])
      expect(res.state).toBe('failed')
      expect(res.label).toBe('Failed')
    }
  })

  it('maps canceled code (A) to canceled', () => {
    const res = jobOutcome({ jobstatus: 'A' }, [{ logtext: 'Termination: Canceled by user' }])
    expect(res.state).toBe('canceled')
    expect(res.label).toBe('Canceled')
    expect(res.termination).toBe('Canceled by user')
  })

  it('maps running code (R) to running and leaves reasonLines empty', () => {
    const res = jobOutcome({ jobstatus: 'R' }, [{ logtext: 'Warning: something\n' }])
    expect(res.state).toBe('running')
    expect(res.label).toBe('Running')
    expect(res.reasonLines).toEqual([])
  })

  it('maps all waiting codes to waiting', () => {
    for (const code of WAITING_JOB_CODES) {
      const res = jobOutcome({ jobstatus: code }, [])
      expect(res.state).toBe('waiting')
      expect(res.label).toBe('Waiting')
    }
  })

  it('maps unrecognised or empty codes to unknown', () => {
    expect(jobOutcome(null)).toEqual({
      state: 'unknown',
      label: 'Unknown',
      reasonLines: [],
      termination: null,
    })
    expect(jobOutcome({ jobstatus: 'XYZ' })).toEqual({
      state: 'unknown',
      label: 'Unknown',
      reasonLines: [],
      termination: null,
    })
  })

  it('caps error and warning lines at 5, in order, plus non-OK FD and SD termination lines', () => {
    const job = { jobstatus: 'f' }
    const logEntries = [
      {
        logtext: [
          '2026-10-19 01:00:00 dir JobId 999: Error: error 1',
          '2026-10-19 01:00:01 dir JobId 999: Warning: warning 2',
          '2026-10-19 01:00:02 dir JobId 999: Fatal error: fatal 3',
          '2026-10-19 01:00:03 dir JobId 999: Error: error 4',
          '2026-10-19 01:00:04 dir JobId 999: Warning: warning 5',
          '2026-10-19 01:00:05 dir JobId 999: Error: error 6 (should be skipped by 5 cap)',
          '  FD termination status:  Fatal Error',
          '  SD termination status:  Error',
          '  Termination:            *** Backup Error ***',
        ].join('\n'),
      },
    ]

    const result = jobOutcome(job, logEntries)
    expect(result.reasonLines).toEqual([
      'Error: error 1',
      'Warning: warning 2',
      'Fatal error: fatal 3',
      'Error: error 4',
      'Warning: warning 5',
      'FD termination status:  Fatal Error',
      'SD termination status:  Error',
    ])
    expect(result.termination).toBe('*** Backup Error ***')
  })

  it('does not include FD or SD termination status if they are OK', () => {
    const job = { jobstatus: 'f' }
    const logEntries = [
      {
        logtext: [
          'dir JobId 999: Error: simple failure',
          '  FD termination status:  OK',
          '  SD termination status:  OK',
          '  Termination:            Error',
        ].join('\n'),
      },
    ]

    const result = jobOutcome(job, logEntries)
    expect(result.reasonLines).toEqual(['Error: simple failure'])
    expect(result.termination).toBe('Error')
  })
})

describe('stripJobPrefix', () => {
  it('strips date, daemon, and JobId prefix', () => {
    expect(
      stripJobPrefix(
        ' 2026-10-02 10:30:39 lab-dir JobId 283: Warning: connection failed'
      )
    ).toBe('Warning: connection failed')
  })

  it('strips daemon and JobId prefix without date', () => {
    expect(
      stripJobPrefix(
        'lab-dir JobId 1101: Fatal error: relation "batch" does not exist'
      )
    ).toBe('Fatal error: relation "batch" does not exist')
  })

  it('strips JobId prefix alone', () => {
    expect(stripJobPrefix('JobId 42: Error: timeout')).toBe('Error: timeout')
  })

  it('preserves line without prefix after trimming', () => {
    expect(stripJobPrefix('  FD termination status: Error  ')).toBe(
      'FD termination status: Error'
    )
  })

  it('handles non-string inputs safely', () => {
    expect(stripJobPrefix(null)).toBe('')
    expect(stripJobPrefix(undefined)).toBe('')
  })
})
