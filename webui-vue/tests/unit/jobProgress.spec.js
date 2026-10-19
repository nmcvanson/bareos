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

import { describe, expect, it } from 'vitest'
import {
  parseFdRunning,
  parseSdRunning,
  finalRate,
  parseDurationSeconds,
  parseNumberWithCommas,
} from '../../src/utils/jobProgress.js'

describe('parseFdRunning', () => {
  const sampleFdStatus = `
Connecting to Client: fd1 at lab-fd1:9102
1000 OK: fd1 Version: 25.1.1 (14 September 2025) Ubuntu 24.04.5 LTS

Running Jobs:
JobId 827 Job other-job.2026-10-08 is running.
    Incremental Backup Job started: 08-Oct-2026 08:40:00
    Files=10 Bytes=10,000 Bytes/sec=1,000 Errors=0
    Bwlimit=0
    Files Examined=10
    Processing file: /var/log/syslog
    SDReadSeqNo=1 fd=3
JobId 828 Job backup-group.2026-10-08_08.44.51_18 is running.
    Full Backup Job started: 08-Oct-2026 08:44:54
    Files=2,036 Bytes=314,613,788 Bytes/sec=314,613 Errors=0
    Bwlimit=0
    Files Examined=2,036
    Processing file: /etc/hosts
    SDReadSeqNo=123 fd=5
JobId 829 Job zero-job.2026-10-08 is running.
    Differential Backup Job started: 08-Oct-2026 08:50:00
    Files=0 Bytes=0 Bytes/sec=0 Errors=2
    Bwlimit=0
    Files Examined=0
    SDSocket closed.
====
`

  it('parses running job with files and processing file', () => {
    const result = parseFdRunning(sampleFdStatus, 828)
    expect(result).toEqual({
      files: 2036,
      bytes: 314613788,
      bytesPerSec: 314613,
      errors: 0,
      processingFile: '/etc/hosts',
    })
  })

  it('parses running job with string jobId', () => {
    const result = parseFdRunning(sampleFdStatus, '827')
    expect(result).toEqual({
      files: 10,
      bytes: 10000,
      bytesPerSec: 1000,
      errors: 0,
      processingFile: '/var/log/syslog',
    })
  })

  it('handles job with 0 files and no processing file line', () => {
    const result = parseFdRunning(sampleFdStatus, 829)
    expect(result).toEqual({
      files: 0,
      bytes: 0,
      bytesPerSec: 0,
      errors: 2,
      processingFile: null,
    })
  })

  it('returns null if job is not listed in status text', () => {
    expect(parseFdRunning(sampleFdStatus, 999)).toBeNull()
  })

  it('returns null on invalid inputs', () => {
    expect(parseFdRunning('', 828)).toBeNull()
    expect(parseFdRunning(null, 828)).toBeNull()
    expect(parseFdRunning(sampleFdStatus, null)).toBeNull()
  })
})

describe('parseSdRunning', () => {
  const sampleSdStatus = `
Connecting to Storage daemon at lab-sd1:9103
1000 OK: lab-sd1 Version: 25.1.1 (14 September 2025) Ubuntu 24.04.5 LTS

Job inventory:

JobId=828 Level=Full Type=Backup Name=backup-group Status=R
Writing: Volume="Full-0290"
    pool="Full" device=/srv/bareos/a
    spooling=0 despooling=0 despool_wait=0
    Files=2,036 Bytes=314,840,759 AveBytes/sec=157,420 LastBytes/sec=157,420
    FDReadSeqNo=456 in_msg=10 out_msg=20 fd=7

JobId=900 Level=Incremental Type=Backup Name=other-job Status=R
Reading: Volume="Incr-0010"
    pool="Incremental" device=/srv/bareos/b
    Files=50 Bytes=5,000,000 AveBytes/sec=500,000 LastBytes/sec=450,000
    FDSocket closed

====
`

  it('parses SD running job stats for target JobId', () => {
    const result = parseSdRunning(sampleSdStatus, 828)
    expect(result).toEqual({
      files: 2036,
      bytes: 314840759,
      aveBytesPerSec: 157420,
      lastBytesPerSec: 157420,
    })
  })

  it('parses second running job in status text', () => {
    const result = parseSdRunning(sampleSdStatus, '900')
    expect(result).toEqual({
      files: 50,
      bytes: 5000000,
      aveBytesPerSec: 500000,
      lastBytesPerSec: 450000,
    })
  })

  it('returns null when JobId is absent', () => {
    expect(parseSdRunning(sampleSdStatus, 999)).toBeNull()
  })

  it('returns null on invalid inputs', () => {
    expect(parseSdRunning('', 828)).toBeNull()
    expect(parseSdRunning(null, 828)).toBeNull()
    expect(parseSdRunning(sampleSdStatus, null)).toBeNull()
  })
})

describe('finalRate', () => {
  it('computes bytes divided by duration in seconds', () => {
    const job = { bytes: 314613788, duration: '00:00:01' }
    expect(finalRate(job)).toBe(314613788)
  })

  it('handles 2 seconds duration and formatted string bytes', () => {
    const job = { bytes: '314,613,788', duration: '00:00:02' }
    expect(finalRate(job)).toBe(157306894)
  })

  it('handles jobbytes property and HH:MM:SS duration', () => {
    const job = { jobbytes: 104857600, duration: '0:04:12' }
    // 4 mins 12 secs = 252 seconds
    expect(finalRate(job)).toBeCloseTo(104857600 / 252, 4)
  })

  it('returns 0 if bytes is 0 and duration > 0', () => {
    const job = { bytes: 0, duration: '00:00:10' }
    expect(finalRate(job)).toBe(0)
  })

  it('returns null when duration is 0 or zero seconds', () => {
    expect(finalRate({ bytes: 1000, duration: '00:00:00' })).toBeNull()
    expect(finalRate({ bytes: 1000, duration: '0' })).toBeNull()
    expect(finalRate({ bytes: 1000, duration: 0 })).toBeNull()
  })

  it('returns null on missing job or duration or bytes', () => {
    expect(finalRate(null)).toBeNull()
    expect(finalRate({})).toBeNull()
    expect(finalRate({ bytes: 1000 })).toBeNull()
    expect(finalRate({ duration: '00:01:00' })).toBeNull()
  })
})

describe('parseDurationSeconds', () => {
  it('parses HH:MM:SS correctly', () => {
    expect(parseDurationSeconds('01:02:03')).toBe(3723)
  })

  it('parses MM:SS correctly', () => {
    expect(parseDurationSeconds('05:30')).toBe(330)
  })

  it('parses single number or string seconds', () => {
    expect(parseDurationSeconds('45')).toBe(45)
    expect(parseDurationSeconds(60)).toBe(60)
  })

  it('returns 0 for invalid or empty duration', () => {
    expect(parseDurationSeconds('')).toBe(0)
    expect(parseDurationSeconds(null)).toBe(0)
    expect(parseDurationSeconds('invalid')).toBe(0)
  })
})

describe('parseNumberWithCommas', () => {
  it('parses numbers with commas', () => {
    expect(parseNumberWithCommas('2,036')).toBe(2036)
    expect(parseNumberWithCommas('314,613,788')).toBe(314613788)
  })

  it('returns numeric input directly', () => {
    expect(parseNumberWithCommas(42)).toBe(42)
  })

  it('returns 0 for non-numeric or empty string', () => {
    expect(parseNumberWithCommas('')).toBe(0)
    expect(parseNumberWithCommas(null)).toBe(0)
  })
})
