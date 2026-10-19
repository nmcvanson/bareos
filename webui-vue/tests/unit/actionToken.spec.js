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
import { ref } from 'vue'
import { createActionTokenGuard } from '../../src/utils/actionToken.js'

const open = (guard, director = 'B', active = 'A') =>
  guard.open({ volume: 'v', director }, `v\u0000${director}`, active)

describe('action tokens', () => {
  it('allows a write only for the current token while the active Director is the target Director', () => {
    const guard = createActionTokenGuard()
    const token = open(guard)
    expect(guard.isCurrent(token, 'v\u0000B')).toBe(true)
    expect(guard.canWrite(token, 'v\u0000B', 'A')).toBe(false)   // not switched yet
    guard.observe('v\u0000B', 'B')                                // the page switches to B
    expect(guard.canWrite(token, 'v\u0000B', 'B')).toBe(true)
    expect(guard.canWrite(token, 'v\u0000other', 'B')).toBe(false)
  })

  it('invalidates for good: back to the old Director or route never revives the token', () => {
    const guard = createActionTokenGuard()
    const token = open(guard)
    guard.observe('v\u0000B', 'B')
    guard.observe('v\u0000B', 'A')                                // the original Director returns
    guard.observe('v\u0000B', 'B')
    expect(guard.canWrite(token, 'v\u0000B', 'B')).toBe(false)
    const second = open(guard, 'A', 'A')
    guard.observe('w\u0000A', 'A')                                // route changes
    guard.observe('v\u0000A', 'A')                                // and returns
    expect(guard.isCurrent(second, 'v\u0000A')).toBe(false)
  })

  it('a new token replaces the old one, is single use and survives being stored in a ref', () => {
    const guard = createActionTokenGuard()
    const first = open(guard, 'A', 'A')
    const second = open(guard, 'A', 'A')
    expect(guard.canWrite(first, 'v\u0000A', 'A')).toBe(false)
    const stored = ref({ token: second })
    expect(guard.canWrite(stored.value.token, 'v\u0000A', 'A')).toBe(true)
    guard.consume(stored.value.token)
    expect(guard.canWrite(second, 'v\u0000A', 'A')).toBe(false)
    expect(guard.isCurrent(second, 'v\u0000A')).toBe(true)        // still current for navigation
    expect(() => guard.open({ volume: 'v' }, 's', 'A')).toThrow()
  })
})
