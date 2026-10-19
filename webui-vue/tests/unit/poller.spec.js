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

import { describe, expect, it, vi, beforeEach, afterEach } from 'vitest'
import { createPoller } from '../../src/utils/poller.js'

describe('createPoller', () => {
  let hidden
  const pollers = []
  const makePoller = fn => {
    const poller = createPoller(fn, 1000)
    pollers.push(poller)
    return poller
  }
  beforeEach(() => {
    vi.useFakeTimers()
    hidden = false
    vi.spyOn(document, 'hidden', 'get').mockImplementation(() => hidden)
  })
  afterEach(() => {
    for (const poller of pollers.splice(0)) poller.stop()
    vi.useRealTimers()
    vi.restoreAllMocks()
  })

  it('polls immediately, skips ticks during an in-flight request and stops cleanly', async () => {
    let finish
    const fn = vi.fn().mockImplementationOnce(() => new Promise(resolve => { finish = resolve })).mockResolvedValue(undefined)
    const poller = makePoller(fn)
    expect(fn).not.toHaveBeenCalled()
    poller.start()
    poller.start()
    expect(fn).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(3000)
    expect(fn).toHaveBeenCalledTimes(1)
    expect(poller.pending).toBe(true)
    finish()
    await vi.advanceTimersByTimeAsync(1000)
    expect(fn).toHaveBeenCalledTimes(2)
    poller.stop()
    await vi.advanceTimersByTimeAsync(3000)
    expect(fn).toHaveBeenCalledTimes(2)
    expect(vi.getTimerCount()).toBe(0)
  })

  it('pauses on document.hidden, resumes on visibility and removes the listener on stop', async () => {
    const fn = vi.fn().mockResolvedValue(undefined)
    const poller = makePoller(fn)
    poller.start()
    await vi.advanceTimersByTimeAsync(0)
    hidden = true
    document.dispatchEvent(new Event('visibilitychange'))
    expect(vi.getTimerCount()).toBe(0)
    await vi.advanceTimersByTimeAsync(3000)
    expect(fn).toHaveBeenCalledTimes(1)
    hidden = false
    document.dispatchEvent(new Event('visibilitychange'))
    await vi.advanceTimersByTimeAsync(1000)
    expect(fn).toHaveBeenCalledTimes(3)
    poller.stop()
    document.dispatchEvent(new Event('visibilitychange'))
    expect(vi.getTimerCount()).toBe(0)
  })

  it('does not start hidden or restart while a pre-hide call remains in flight', async () => {
    let finish
    hidden = true
    const fn = vi.fn(() => new Promise(resolve => { finish = resolve }))
    const poller = makePoller(fn)
    poller.start()
    await vi.advanceTimersByTimeAsync(1000)
    expect(fn).not.toHaveBeenCalled()
    hidden = false
    document.dispatchEvent(new Event('visibilitychange'))
    hidden = true
    document.dispatchEvent(new Event('visibilitychange'))
    hidden = false
    document.dispatchEvent(new Event('visibilitychange'))
    expect(fn).toHaveBeenCalledTimes(1)
    poller.stop()
    finish()
    await vi.advanceTimersByTimeAsync(2000)
    expect(fn).toHaveBeenCalledTimes(1)
  })

  it('records a failure, releases the busy flag and clears the error on success', async () => {
    const error = new Error('SD unavailable')
    const fn = vi.fn().mockRejectedValueOnce(error).mockResolvedValue(undefined)
    const poller = makePoller(fn)
    poller.start()
    await vi.advanceTimersByTimeAsync(0)
    expect(poller.error).toBe(error)
    expect(poller.pending).toBe(false)
    await vi.advanceTimersByTimeAsync(1000)
    expect(poller.error).toBeNull()
    expect(fn).toHaveBeenCalledTimes(2)
  })

  it('rejects invalid poll settings', () => {
    for (const interval of [0, -1, NaN, Infinity]) expect(() => createPoller(() => {}, interval)).toThrow()
    expect(() => createPoller(null, 1000)).toThrow()
  })
})
