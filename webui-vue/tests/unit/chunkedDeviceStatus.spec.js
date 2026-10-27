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

import capture from './fixtures/status-storage-S3.txt?raw'
import { describe, expect, it } from 'vitest'
import {
  computeRates, parseStorageStatus, dedupeDevices, chunkedDeviceParser,
  S3_TOTAL_FIELDS, S3_GAUGE_FIELDS,
} from '../../src/utils/chunkedDeviceStatus.js'

const pollCaptures = import.meta.glob('./fixtures/s3-poll-*.txt', { query: '?raw', import: 'default', eager: true })

const busy = `Device "Busy" (S3) is open.
Last upload failed 12 s ago: endpoint unreachable.
Device refuses writes: upload retry limit reached
Chunks: received 15, uploaded 9, pending 6 (queued 2, uploading 1, kept 3), waiting for retry 3, oldest pending 42 s.
Upload: 9 chunks, 603979776 bytes; failed tries 7, retried tries 4.
Download: 2 objects, 134217728 bytes, failed 1; transport retries 5.`
const sample = (t, received, uploaded, bytes, download = 0, name = 'S3') => ({
  t,
  device: {
    name,
    chunks: { received, uploaded, pending: received - uploaded },
    upload: { chunks: uploaded, bytes, failedTries: 0, retriedTries: 0 },
    download: { objects: 0, bytes: download, failed: 0, transportRetries: 0 },
  },
})

describe('chunkedDeviceParser', () => {
  it('exposes kind: "chunked"', () => {
    expect(chunkedDeviceParser.kind).toBe('chunked')
  })

  it('returns null for non-chunked or empty lines', () => {
    expect(chunkedDeviceParser([])).toBeNull()
    expect(chunkedDeviceParser(null)).toBeNull()
    expect(chunkedDeviceParser(['Device "File" is not open.', '=='])).toBeNull()
  })

  it('parses idle chunked lines', () => {
    const lines = [
      'No upload since the daemon started.',
      'No pending IO flush requests.',
      'Chunks: received 0, uploaded 0, pending 0 (queued 0, uploading 0, kept 0), waiting for retry 0.',
      'Upload: 0 chunks, 0 bytes; failed tries 0, retried tries 0.',
      'Download: 0 objects, 0 bytes, failed 0; transport retries 0.',
    ]
    expect(chunkedDeviceParser(lines)).toEqual({
      chunks: { received: 0, uploaded: 0, pending: 0, queued: 0, uploading: 0, kept: 0, waitingRetry: 0, oldestPendingS: null },
      upload: { chunks: 0, bytes: 0, failedTries: 0, retriedTries: 0 },
      download: { objects: 0, bytes: 0, failed: 0, transportRetries: 0 },
      lastUpload: { state: 'none', agoS: null, error: null },
      refusesWrites: null,
    })
  })

  it('parses busy chunked lines with error and refusal', () => {
    const lines = busy.split('\n')
    expect(chunkedDeviceParser(lines)).toEqual({
      chunks: { received: 15, uploaded: 9, pending: 6, queued: 2, uploading: 1, kept: 3, waitingRetry: 3, oldestPendingS: 42 },
      upload: { chunks: 9, bytes: 603979776, failedTries: 7, retriedTries: 4 },
      download: { objects: 2, bytes: 134217728, failed: 1, transportRetries: 5 },
      lastUpload: { state: 'failed', agoS: 12, error: 'endpoint unreachable.' },
      refusesWrites: 'upload retry limit reached',
    })
  })
})

describe('parseStorageStatus', () => {
  it('parses the idle lab capture', () => {
    expect(parseStorageStatus(capture)).toEqual({ devices: [{
      name: 'S3Storage',
      chunks: { received: 0, uploaded: 0, pending: 0, queued: 0, uploading: 0, kept: 0, waitingRetry: 0, oldestPendingS: null },
      upload: { chunks: 0, bytes: 0, failedTries: 0, retriedTries: 0 },
      download: { objects: 0, bytes: 0, failed: 0, transportRetries: 0 },
      lastUpload: { state: 'none', agoS: null, error: null }, refusesWrites: null,
    }] })
  })

  it('keeps busy counters, ages and errors within their own device', () => {
    const text = `${busy}\nDevice "File" is not open.\nLast upload succeeded 1 s ago.\n${busy.replace('"Busy"', '"Other"').replace('failed 12 s ago: endpoint unreachable.', 'succeeded 2 s ago.').replace('Device refuses writes: upload retry limit reached\n', '')}`
    const { devices } = parseStorageStatus(text.replaceAll('\n', '\r\n'))
    expect(devices).toHaveLength(2)
    expect(devices[0]).toEqual({
      name: 'Busy',
      chunks: { received: 15, uploaded: 9, pending: 6, queued: 2, uploading: 1, kept: 3, waitingRetry: 3, oldestPendingS: 42 },
      upload: { chunks: 9, bytes: 603979776, failedTries: 7, retriedTries: 4 },
      download: { objects: 2, bytes: 134217728, failed: 1, transportRetries: 5 },
      lastUpload: { state: 'failed', agoS: 12, error: 'endpoint unreachable.' },
      refusesWrites: 'upload retry limit reached',
    })
    expect(devices[1].lastUpload).toEqual({ state: 'ok', agoS: 2, error: null })
    expect(devices[1].refusesWrites).toBeNull()
  })

  it('ignores non-S3 devices, empty input and unattached counters', () => {
    expect(parseStorageStatus('Chunks: received 1\nDevice "File" is open.')).toEqual({ devices: [] })
    expect(parseStorageStatus(null)).toEqual({ devices: [] })
  })

  it('preserves large exact integers through 2^53 and defaults missing lines', () => {
    const { devices } = parseStorageStatus(`Device "Large"\nChunks: received 9007199254740992, uploaded 9007199254740991, pending 1 (queued 1, uploading 0, kept 0), waiting for retry 0.`)
    expect(devices[0].chunks.received).toBe(2 ** 53)
    expect(devices[0].chunks.uploaded).toBe(Number.MAX_SAFE_INTEGER)
    expect(devices[0].upload.bytes).toBeNull()
    expect(devices[0].lastUpload.state).toBeNull()
    expect(devices[0].download).toEqual({ objects: null, bytes: null, failed: null, transportRetries: null })
  })

  it('distinguishes a missing upload outcome from an explicit no-upload line', () => {
    const truncated = capture.replace('No upload since the daemon started.', '')
    expect(parseStorageStatus(truncated).devices[0].lastUpload).toEqual({ state: null, agoS: null, error: null })
    expect(parseStorageStatus(capture).devices[0].lastUpload.state).toBe('none')
  })

  it('parses all ten real 3-second S3 polls and the completed 100 MB upload', () => {
    const history = Object.entries(pollCaptures).sort(([a], [b]) => a.localeCompare(b)).map(([, text], index) => ({
      t: index * 3000, device: parseStorageStatus(text).devices[0],
    }))
    expect(history).toHaveLength(10)
    expect(history[0].device.upload.bytes).toBe(0)
    expect(history[4].device.chunks).toMatchObject({ received: 11, uploaded: 6, pending: 4, queued: 2, uploading: 2, waitingRetry: 0, oldestPendingS: 8 })
    expect(history[9].device.upload.bytes).toBe(104872553)
    expect(history[9].device.lastUpload).toEqual({ state: 'ok', agoS: 4, error: null })
  })
})

describe('dedupeDevices', () => {
  it('merges identical address and device names, keeping distinct aliases', () => {
    const first = { name: 'Dev', chunks: { received: 1 } }
    const list = [
      { storage: 'StorageA', address: 'sd1.lab:9103', devices: [first] },
      { storage: 'StorageB', address: 'SD1.LAB:9103', devices: [first] },
      { storage: 'StorageC', address: 'sd2.lab:9103', devices: [first] },
    ]
    const deduped = dedupeDevices(list)
    expect(deduped).toHaveLength(2)
    expect(deduped[0]).toMatchObject({
      name: 'Dev', storage: 'StorageA', storages: ['StorageA', 'StorageB'],
    })
    expect(deduped[1]).toMatchObject({
      name: 'Dev', storage: 'StorageC', storages: ['StorageC'],
    })
  })

  it('keeps storages without address separate and safely handles missing arrays', () => {
    const list = [
      { storage: 'A', devices: [{ name: 'Dev' }] },
      { storage: 'B', devices: [{ name: 'Dev' }] },
      { storage: 'Empty' },
    ]
    expect(dedupeDevices(list).map(d => d.storage)).toEqual(['A', 'B'])
  })
})

describe('computeRates', () => {
  it('smooths a single 64 MiB chunk step across a 30-second window', () => {
    const samples = [0, 5, 10, 15, 20, 25, 30].map(t => sample(t * 1000, 1, t >= 15 ? 1 : 0, t >= 15 ? 67108864 : 0, t >= 20 ? 1024 : 0))
    expect(computeRates(samples).S3).toEqual({
      uploadBytesPerS: 67108864 / 30, downloadBytesPerS: 1024 / 30,
      uploadedChunksPerMin: 2, receivedChunksPerMin: 0, reset: false,
    })
  })

  it('starts a fresh baseline at a restart and recovers without negative rates', () => {
    const samples = [sample(0, 100, 90, 900), sample(5000, 2, 1, 10), sample(10000, 4, 3, 30)]
    expect(computeRates(samples).S3).toEqual({
      uploadBytesPerS: 4, downloadBytesPerS: 0,
      uploadedChunksPerMin: 24, receivedChunksPerMin: 24, reset: true,
    })
    expect(computeRates(samples.slice(0, 2)).S3.uploadBytesPerS).toBe(0)
  })

  it('detects a reset of any cumulative counter, but ignores falling gauges', () => {
    const first = sample(0, 10, 5, 500)
    first.device.upload.failedTries = 3
    const second = sample(5000, 10, 6, 600)
    second.device.upload.failedTries = 2
    expect(computeRates([first, second]).S3.reset).toBe(true)
    first.device.upload.failedTries = 2
    expect(computeRates([first, second]).S3.reset).toBe(false)
    expect(computeRates([first, second]).S3.uploadBytesPerS).toBe(20)
  })

  it('uses only the requested window and interpolates between boundary polls', () => {
    expect(computeRates([sample(0, 0, 0, 0), sample(20000, 20, 20, 200), sample(40000, 60, 60, 600)], 30).S3.uploadBytesPerS).toBeCloseTo(500 / 30)
    const history = [sample(0, 100, 90, 900), sample(5000, 0, 0, 0), sample(40000, 35, 35, 350), sample(45000, 40, 40, 400)]
    expect(computeRates(history).S3.reset).toBe(false)
    expect(computeRates(history).S3.uploadBytesPerS).toBe(10)
  })

  it('separates device names, and handles empty, singleton and equal-time samples', () => {
    expect(computeRates([])).toEqual({})
    const samples = [sample(0, 0, 0, 0), sample(0, 0, 0, 0, 0, 'Other'), sample(5000, 1, 1, 100)]
    expect(computeRates(samples).Other.uploadBytesPerS).toBe(0)
    expect(computeRates(samples).S3.uploadBytesPerS).toBe(20)
    expect(computeRates([sample(0, 0, 0, 0), sample(0, 1, 1, 100)]).S3.uploadBytesPerS).toBe(0)
    expect(() => computeRates(samples, 0)).toThrow()
    expect(() => computeRates(samples, NaN)).toThrow()
  })

  it('keeps unknown byte counters unknown instead of reporting zero traffic', () => {
    const device = parseStorageStatus('Device "S3"\nChunks: received 1, uploaded 0, pending 1 (queued 1, uploading 0, kept 0), waiting for retry 0.').devices[0]
    expect(computeRates([{ t: 0, device }, { t: 5000, device }]).S3).toEqual({
      uploadBytesPerS: null, downloadBytesPerS: null,
      uploadedChunksPerMin: 0, receivedChunksPerMin: 0, reset: false,
    })
  })
})
