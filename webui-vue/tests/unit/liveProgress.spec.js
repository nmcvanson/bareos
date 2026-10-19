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

import { describe, expect, it } from 'vitest'
import { createLiveProgress, displayProgress, liveScope } from '../../src/utils/liveProgress.js'

describe('live job progress', () => {
  it('shows live counters while running and the catalog record once finished', () => {
    const live = createLiveProgress()
    live.reset(liveScope(7, 'dir'))
    live.accept(live.ticket(), 'fd', { files: 5, bytes: 500 })
    const running = { files: 0, bytes: 0 }
    expect(displayProgress({ job: running, running: true, fd: live.fd, sd: live.sd })).toEqual({ files: 5, bytes: 500 })
    // completion clears the samples; the catalog record (50 files) wins even if a sample is passed
    live.reset(liveScope(7, 'dir'))
    const done = { files: 50, bytes: 5000 }
    expect(displayProgress({ job: done, running: false, fd: { files: 5, bytes: 500 }, sd: null })).toEqual({ files: 50, bytes: 5000 })
    expect(displayProgress({ job: done, running: false, fd: live.fd, sd: live.sd })).toEqual({ files: 50, bytes: 5000 })
  })

  it('drops a late reply that belongs to an older job, route or Director', () => {
    const live = createLiveProgress()
    live.reset(liveScope(7, 'dirA'))
    const old = live.ticket()
    live.reset(liveScope(8, 'dirA'))
    expect(live.accept(old, 'fd', { files: 5, bytes: 500 })).toBe(false)
    expect(live.fd).toBeNull()
    const otherDirector = live.ticket()
    live.reset(liveScope(8, 'dirB'))
    expect(live.accept(otherDirector, 'sd', { files: 9, bytes: 9 })).toBe(false)
    expect(live.sd).toBeNull()
    expect(live.accept(live.ticket(), 'sd', { files: 3, bytes: 30 })).toBe(true)
    expect(live.sd).toEqual({ files: 3, bytes: 30 })
  })

  it('clears samples on reset and ignores unknown kinds', () => {
    const live = createLiveProgress()
    live.reset(liveScope(1, 'd'))
    live.accept(live.ticket(), 'fd', { files: 1, bytes: 1 })
    live.reset(liveScope(2, 'd'))
    expect(live.fd).toBeNull()
    expect(live.accept(live.ticket(), 'x', {})).toBe(false)
    expect(displayProgress({ job: null, running: true, fd: null, sd: null })).toEqual({ files: 0, bytes: 0 })
  })
})
