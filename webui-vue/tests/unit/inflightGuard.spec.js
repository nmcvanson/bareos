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

import { afterEach, describe, expect, it, vi } from 'vitest'
import { createInflightGuard } from '../../src/utils/inflightGuard.js'
import { createPoller } from '../../src/utils/poller.js'

const deferred = () => {
  let resolve
  const promise = new Promise(r => { resolve = r })
  return { promise, resolve }
}

describe('in-flight guard', () => {
  afterEach(() => { vi.useRealTimers() })

  it('shares one request between two timer ticks, a manual refresh and a restarted poller', async () => {
    vi.useFakeTimers()
    const slow = deferred()
    let started = 0
    const guard = createInflightGuard()
    const refresh = () => guard.run('sd1', () => { started += 1; return slow.promise })

    const first = createPoller(refresh, 1000)
    first.start()                       // immediate tick starts the request
    await vi.advanceTimersByTimeAsync(2500)  // two more ticks while it is pending
    const manual = refresh()            // manual refresh
    first.stop()
    const second = createPoller(refresh, 5000)  // interval changed: new poller
    second.start()
    await vi.advanceTimersByTimeAsync(0)
    expect(started).toBe(1)

    slow.resolve('done')
    await expect(manual).resolves.toBe('done')
    expect(guard.busy).toBe(false)
    second.stop()
    await refresh()
    expect(started).toBe(2)
  })

  it('starts a new request for a different scope without waiting for the old one', async () => {
    const slow = deferred()
    const guard = createInflightGuard()
    let started = 0
    const a = guard.run('job7', () => { started += 1; return slow.promise })
    const b = guard.run('job8', () => { started += 1; return Promise.resolve('b') })
    expect(started).toBe(2)
    await expect(b).resolves.toBe('b')
    slow.resolve('a')
    await expect(a).resolves.toBe('a')
    expect(guard.busy).toBe(false)
  })

  it('reuses the first request when the scope returns to A while A and B are still pending', async () => {
    const slowA = deferred()
    const slowB = deferred()
    const guard = createInflightGuard()
    let started = 0
    const first = guard.run('A', () => { started += 1; return slowA.promise })
    guard.run('B', () => { started += 1; return slowB.promise })
    const again = guard.run('A', () => { started += 1; return Promise.resolve('second') })
    expect(started).toBe(2)
    expect(again).toBe(first)
    slowA.resolve('a')
    slowB.resolve('b')
    await expect(again).resolves.toBe('a')
    expect(guard.busy).toBe(false)
  })

  it('reuses A after B already finished and A is still pending', async () => {
    const slowA = deferred()
    const guard = createInflightGuard()
    let started = 0
    const first = guard.run('A', () => { started += 1; return slowA.promise })
    await guard.run('B', () => { started += 1; return Promise.resolve('b') })
    expect(guard.busy).toBe(true)                       // A is still pending
    const again = guard.run('A', () => { started += 1; return Promise.resolve('x') })
    expect(again).toBe(first)
    expect(started).toBe(2)
    slowA.resolve('a')
    await again
    expect(guard.busy).toBe(false)
  })

  it('releases the scope after a failure', async () => {
    const guard = createInflightGuard()
    await expect(guard.run('x', () => Promise.reject(new Error('boom')))).rejects.toThrow('boom')
    expect(guard.busy).toBe(false)
    await expect(guard.run('x', () => 'ok')).resolves.toBe('ok')
  })
})
