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

import { describe, expect, it } from 'vitest'
import statusFileClientDir from './fixtures/status-file-client-dir.txt?raw'
import statusStorageS3 from './fixtures/status-storage-S3.txt?raw'
import s3Poll02 from './fixtures/s3-poll-02.txt?raw'
import {
  parseDaemonHeader,
  parseDeviceBlocks,
  parseRunningJobs,
  trimHistory,
  pairRates,
  formatMBs,
  formatCount,
  mergeDaemonDevices,
  BACKEND_PARSERS,
} from '../../src/utils/deviceStatus.js'
import {
  chunkedDeviceParser,
  parseStorageStatus,
} from '../../src/utils/chunkedDeviceStatus.js'

const pollCaptures = import.meta.glob('./fixtures/s3-poll-*.txt', { query: '?raw', import: 'default', eager: true })

describe('BACKEND_PARSERS', () => {
  it('registers chunkedDeviceParser', () => {
    expect(BACKEND_PARSERS).toContain(chunkedDeviceParser)
  })
})

describe('parseDaemonHeader', () => {
  it('parses Storage Daemon header with comma after run', () => {
    const header = parseDaemonHeader(statusFileClientDir)
    expect(header).toEqual({
      name: 'lab-sd1',
      version: '25.1.1',
      started: '19-Oct-26 00:44',
      jobsRun: 0,
      jobsRunning: 0,
    })
  })

  it('parses File Daemon (client) header without comma after run', () => {
    const clientSection = statusFileClientDir.slice(statusFileClientDir.indexOf('status client='))
    const header = parseDaemonHeader(clientSection)
    expect(header).toEqual({
      name: 'fd1',
      version: '25.1.1',
      started: '19-Oct-26 00:44',
      jobsRun: 0,
      jobsRunning: 0,
    })
  })

  it('parses Director header', () => {
    const dirSection = statusFileClientDir.slice(statusFileClientDir.indexOf('status director'))
    const header = parseDaemonHeader(dirSection)
    expect(header).toEqual({
      name: 'lab-dir',
      version: '25.1.1',
      started: '19-Oct-26 00:43',
      jobsRun: 0,
      jobsRunning: 0,
    })
  })

  it('parses Storage Daemon header with running jobs from S3 poll fixture', () => {
    const header = parseDaemonHeader(s3Poll02)
    expect(header).toEqual({
      name: 'lab-sd3',
      version: '25.1.1',
      started: '18-Oct-26 22:33',
      jobsRun: 0,
      jobsRunning: 1,
    })
  })

  it('returns partial object when only version line is available', () => {
    const text = '1000 OK: lab-dir Version: 25.1.1 (14 September 2025)'
    expect(parseDaemonHeader(text)).toEqual({
      name: 'lab-dir',
      version: '25.1.1',
      started: null,
      jobsRun: null,
      jobsRunning: null,
    })
  })

  it('returns null for null, empty or invalid input', () => {
    expect(parseDaemonHeader(null)).toBeNull()
    expect(parseDaemonHeader('')).toBeNull()
    expect(parseDaemonHeader('Random text without bareos header')).toBeNull()
  })
})

describe('parseDeviceBlocks', () => {
  it('parses real file device from status-file-client-dir capture', () => {
    const devices = parseDeviceBlocks(statusFileClientDir)
    expect(devices).toHaveLength(1)
    expect(devices[0]).toEqual({
      name: 'FileStorage1a',
      archive: '/srv/bareos/a',
      state: 'not open',
      volume: null,
      pool: null,
      mediaType: null,
      totalBytes: null,
      blocks: null,
      totalBytesRead: null,
      blocksRead: null,
      positionFile: null,
      positionBlock: null,
      slot: null,
      lines: [
        'Device "FileStorage1a" (/srv/bareos/a) is not open.',
        '==',
      ],
      backend: null,
    })
  })

  it('parses real idle S3 device from status-storage-S3 capture with backend.kind: "chunked"', () => {
    const devices = parseDeviceBlocks(statusStorageS3)
    expect(devices).toHaveLength(1)
    expect(devices[0]).toMatchObject({
      name: 'S3Storage',
      archive: 'Viettel Cloud Object Storage',
      state: 'not open',
      backend: {
        kind: 'chunked',
        metrics: {
          chunks: { received: 0, uploaded: 0, pending: 0, queued: 0, uploading: 0, kept: 0, waitingRetry: 0, oldestPendingS: null },
          upload: { chunks: 0, bytes: 0, failedTries: 0, retriedTries: 0 },
          download: { objects: 0, bytes: 0, failed: 0, transportRetries: 0 },
          lastUpload: { state: 'none', agoS: null, error: null },
          refusesWrites: null,
        },
      },
    })
  })

  it('parses real busy mounted S3 device from s3-poll-02 capture', () => {
    const devices = parseDeviceBlocks(s3Poll02)
    expect(devices).toHaveLength(1)
    expect(devices[0]).toEqual({
      name: 'S3Demo',
      archive: 'bucket-bareos',
      state: 'mounted',
      volume: 'demo-s3-0307',
      pool: 'DemoS3Pool',
      mediaType: 'S3Demo3',
      totalBytes: 51380429,
      blocks: 50,
      totalBytesRead: null,
      blocksRead: null,
      positionFile: 0,
      positionBlock: 51380428,
      slot: null,
      lines: expect.any(Array),
      backend: {
        kind: 'chunked',
        metrics: {
          chunks: { received: 6, uploaded: 1, pending: 4, queued: 2, uploading: 2, kept: 0, waitingRetry: 0, oldestPendingS: 1 },
          upload: { chunks: 1, bytes: 205, failedTries: 0, retriedTries: 0 },
          download: { objects: 1, bytes: 205, failed: 0, transportRetries: 0 },
          lastUpload: { state: 'ok', agoS: 1, error: null },
          refusesWrites: null,
        },
      },
    })
  })

  it('parses synthetic mounted file device', () => {
    const text = `
Device "FileStorage1" (/var/bareos/storage) is mounted with:
    Volume:      Vol-0001
    Pool:        FilePool
    Media type:  File
    Total Bytes=1,234,567 Blocks=12 Bytes/block=102,880
    Positioned at File=0 Block=1,234,567
==
====
`
    const devices = parseDeviceBlocks(text)
    expect(devices).toHaveLength(1)
    expect(devices[0]).toEqual({
      name: 'FileStorage1',
      archive: '/var/bareos/storage',
      state: 'mounted',
      volume: 'Vol-0001',
      pool: 'FilePool',
      mediaType: 'File',
      totalBytes: 1234567,
      blocks: 12,
      totalBytesRead: null,
      blocksRead: null,
      positionFile: 0,
      positionBlock: 1234567,
      slot: null,
      lines: expect.any(Array),
      backend: null,
    })
  })

  it('parses synthetic tape device with reading counters and autochanger slot', () => {
    const text = `
Device "TapeDrive1" (/dev/nst0) is mounted with:
    Volume:      Tape-0042
    Pool:        TapePool
    Media type:  LTO-8
    Slot 5 is loaded in drive 0.
    Total Bytes Read=9,876,543 Blocks Read=98 Bytes/block=100,781
    Positioned at File=2 Block=1,000
==
====
`
    const devices = parseDeviceBlocks(text)
    expect(devices).toHaveLength(1)
    expect(devices[0]).toEqual({
      name: 'TapeDrive1',
      archive: '/dev/nst0',
      state: 'mounted',
      volume: 'Tape-0042',
      pool: 'TapePool',
      mediaType: 'LTO-8',
      totalBytes: null,
      blocks: null,
      totalBytesRead: 9876543,
      blocksRead: 98,
      positionFile: 2,
      positionBlock: 1000,
      slot: 5,
      lines: expect.any(Array),
      backend: null,
    })
  })

  it('parses synthetic unmounted device with slot was last loaded', () => {
    const text = `
Device "TapeDrive2" (/dev/nst1) is not open.
    Slot 12 was last loaded in drive 1.
==
====
`
    const devices = parseDeviceBlocks(text)
    expect(devices).toHaveLength(1)
    expect(devices[0]).toMatchObject({
      name: 'TapeDrive2',
      archive: '/dev/nst1',
      state: 'not open',
      slot: 12,
      backend: null,
    })
  })

  it('parses device in open but unmounted state', () => {
    const text = `
Device "FileStorage2" (/var/bareos/storage2) open but no Bareos volume is currently mounted.
==
====
`
    const devices = parseDeviceBlocks(text)
    expect(devices).toHaveLength(1)
    expect(devices[0].state).toBe('open but no Bareos volume is currently mounted.')
    expect(devices[0].volume).toBeNull()
  })

  it('handles empty or null text cleanly', () => {
    expect(parseDeviceBlocks(null)).toEqual([])
    expect(parseDeviceBlocks('')).toEqual([])
  })
})

describe('parseRunningJobs', () => {
  it('parses running job from s3-poll-02 capture', () => {
    const jobs = parseRunningJobs(s3Poll02)
    expect(jobs).toHaveLength(1)
    expect(jobs[0]).toEqual({
      jobId: 1101,
      level: 'Full',
      type: 'Backup',
      name: 'demo-s3-backup',
      status: 'Running',
      volume: 'demo-s3-0307',
      pool: 'DemoS3Pool',
      device: 'S3Demo',
      files: 19,
      bytes: 52168971,
      aveBytesPerSec: 52168971,
      lastBytesPerSec: 52168971,
    })
  })

  it('returns empty array when no jobs running in status-file-client-dir', () => {
    const jobs = parseRunningJobs(statusFileClientDir)
    expect(jobs).toEqual([])
  })

  it('handles null, empty or invalid text', () => {
    expect(parseRunningJobs(null)).toEqual([])
    expect(parseRunningJobs('')).toEqual([])
    expect(parseRunningJobs('No job inventory section')).toEqual([])
  })
})

describe('trimHistory', () => {
  it('drops samples older than nowMs - keepMs', () => {
    const samples = [
      { t: 1000, val: 1 },
      { t: 3000, val: 2 },
      { t: 6000, val: 3 },
      { t: 9000, val: 4 },
    ]
    const trimmed = trimHistory(samples, 9000, 5000)
    expect(trimmed).toEqual([
      { t: 6000, val: 3 },
      { t: 9000, val: 4 },
    ])
    expect(samples).toBe(trimmed)
  })

  it('defaults keepMs to 600000 ms (10 minutes)', () => {
    const now = 1000000
    const samples = [
      { t: now - 700000 },
      { t: now - 600000 },
      { t: now - 100000 },
    ]
    trimHistory(samples, now)
    expect(samples).toHaveLength(2)
    expect(samples[0].t).toBe(now - 600000)
    expect(samples[1].t).toBe(now - 100000)
  })

  it('handles empty or non-array input', () => {
    expect(trimHistory([], 1000)).toEqual([])
    expect(trimHistory(null, 1000)).toEqual([])
  })
})

describe('pairRates', () => {
  it('computes upload and download rates for real consecutive poll fixtures', () => {
    const sortedEntries = Object.entries(pollCaptures).sort(([a], [b]) => a.localeCompare(b))
    const history = sortedEntries.map(([, text], index) => {
      const parsed = parseStorageStatus(text)
      return {
        t: index * 3000,
        device: {
          ...parsed.devices[0],
          id: JSON.stringify(['lab-sd3', parsed.devices[0].name]),
        },
      }
    })

    const deviceId = JSON.stringify(['lab-sd3', 'S3Demo'])
    const points = pairRates(history, deviceId)

    expect(points).toHaveLength(9)
    expect(points[0].t).toBe(3000)
    expect(points[0].uploadMBs).toBeCloseTo(205 / 3 / 1e6)
    expect(points[0].downloadMBs).toBeCloseTo(205 / 3 / 1e6)

    const lastPoint = points.at(-1)
    expect(lastPoint.t).toBe(27000)
    expect(typeof lastPoint.uploadMBs).toBe('number')
  })

  it('handles SD restart case with negative delta by yielding null', () => {
    const restartSamples = [
      {
        t: 0,
        device: {
          id: 'dev1',
          upload: { bytes: 50000000 },
          download: { bytes: 20000000 },
        },
      },
      {
        t: 5000,
        device: {
          id: 'dev1',
          upload: { bytes: 1000 }, // dropped after restart
          download: { bytes: 30000000 },
        },
      },
      {
        t: 10000,
        device: {
          id: 'dev1',
          upload: { bytes: 11000 },
          download: { bytes: 35000000 },
        },
      },
    ]

    const points = pairRates(restartSamples, 'dev1')
    expect(points).toHaveLength(2)
    expect(points[0]).toEqual({
      t: 5000,
      uploadMBs: null,
      downloadMBs: (10000000 / 5) / 1e6,
    })
    expect(points[1]).toEqual({
      t: 10000,
      uploadMBs: (10000 / 5) / 1e6,
      downloadMBs: (5000000 / 5) / 1e6,
    })
  })

  it('handles null bytes and single/zero sample devices', () => {
    const nullSamples = [
      {
        t: 0,
        device: { id: 'dev2', upload: { bytes: null }, download: { bytes: null } },
      },
      {
        t: 5000,
        device: { id: 'dev2', upload: { bytes: 1000 }, download: { bytes: null } },
      },
    ]
    expect(pairRates(nullSamples, 'dev2')).toEqual([
      { t: 5000, uploadMBs: null, downloadMBs: null },
    ])
    expect(pairRates([], 'dev2')).toEqual([])
    expect(pairRates([nullSamples[0]], 'dev2')).toEqual([])
  })
})

describe('formatMBs', () => {
  it('formats null, undefined or non-finite values to em-dash', () => {
    expect(formatMBs(null)).toBe('—')
    expect(formatMBs(undefined)).toBe('—')
    expect(formatMBs(NaN)).toBe('—')
  })

  it('formats numbers with 2 decimal places and MB/s', () => {
    expect(formatMBs(0)).toBe('0.00 MB/s')
    expect(formatMBs(3.5)).toBe('3.50 MB/s')
    expect(formatMBs(12.3456)).toBe('12.35 MB/s')
  })
})

describe('formatCount', () => {
  it('formats null, undefined or non-finite values to em-dash', () => {
    expect(formatCount(null)).toBe('—')
    expect(formatCount(undefined)).toBe('—')
    expect(formatCount(NaN)).toBe('—')
  })

  it('formats numbers with toLocaleString', () => {
    expect(formatCount(0)).toBe((0).toLocaleString())
    expect(formatCount(1234567)).toBe((1234567).toLocaleString())
  })
})

describe('mergeDaemonDevices', () => {
  it('merges devices from multiple storage resources by device name (first block wins, keeps storage names)', () => {
    const list = [
      {
        storage: 'File1',
        devices: [
          { name: 'FileStorage1a', state: 'mounted', volume: 'Vol-001' },
          { name: 'FileStorage1b', state: 'not open', volume: null },
        ],
      },
      {
        storage: 'File1a',
        devices: [
          { name: 'FileStorage1a', state: 'not open', volume: 'DifferentVol' }, // duplicate; first block wins
          { name: 'FileStorageShared', state: 'mounted', volume: 'Vol-Shared' },
        ],
      },
    ]

    const merged = mergeDaemonDevices(list)
    expect(merged).toHaveLength(3)

    // FileStorage1a: first block wins (mounted, Vol-001) and records both storages
    expect(merged[0]).toEqual({
      name: 'FileStorage1a',
      state: 'mounted',
      volume: 'Vol-001',
      storages: ['File1', 'File1a'],
    })

    // FileStorage1b: only from File1
    expect(merged[1]).toEqual({
      name: 'FileStorage1b',
      state: 'not open',
      volume: null,
      storages: ['File1'],
    })

    // FileStorageShared: from File1a
    expect(merged[2]).toEqual({
      name: 'FileStorageShared',
      state: 'mounted',
      volume: 'Vol-Shared',
      storages: ['File1a'],
    })
  })

  it('handles status text or empty devices', () => {
    const rawA = 'Device "DevA" (/srv/a) is not open.\n====\n'
    const list = [
      { storage: 'StorageA', text: rawA },
      { storage: 'StorageB', devices: [] },
    ]
    const merged = mergeDaemonDevices(list)
    expect(merged).toHaveLength(1)
    expect(merged[0].name).toBe('DevA')
    expect(merged[0].storages).toEqual(['StorageA'])
  })

  it('handles empty or non-array inputs gracefully', () => {
    expect(mergeDaemonDevices([])).toEqual([])
    expect(mergeDaemonDevices(null)).toEqual([])
    expect(mergeDaemonDevices(undefined)).toEqual([])
  })
})

