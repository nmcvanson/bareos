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
import {
  clientConfigurationUnknown,
  hasUnconfiguredClients,
  markConfiguredClients,
  visibleClients,
} from '../../src/utils/clients.js'

const catalog = [{ name: 'lab-fd1' }, { name: 'lab-fd2' }, { name: 's3b-old' }]
const dot = { clients: [{ name: 'lab-fd1', enabled: true }, { name: 'lab-fd2', enabled: false }] }

describe('configured clients of the Clients page', () => {
  it('lists only the clients the Director has configured by default', () => {
    const rows = markConfiguredClients(catalog, dot)
    expect(visibleClients(rows, false).map(row => row.name)).toEqual(['lab-fd1', 'lab-fd2'])
    expect(visibleClients(rows, false).map(row => row.enabled)).toEqual([true, false])
    expect(hasUnconfiguredClients(rows)).toBe(true)
    expect(clientConfigurationUnknown(rows)).toBe(false)
  })

  it('adds the catalog-only clients on request and marks them not configured', () => {
    const rows = visibleClients(markConfiguredClients(catalog, dot), true)
    expect(rows.map(row => row.name)).toEqual(['lab-fd1', 'lab-fd2', 's3b-old'])
    const old = rows.find(row => row.name === 's3b-old')
    expect(old.configured).toBe(false)
    expect(old.enabled).toBeUndefined()
  })

  it('shows the full catalog list unfiltered when .clients failed', () => {
    for (const failed of [null, undefined, {}, 'error']) {
      const rows = markConfiguredClients(catalog, failed)
      expect(visibleClients(rows, false)).toHaveLength(3)
      expect(rows.every(row => row.configured === null && row.enabled === true)).toBe(true)
      expect(clientConfigurationUnknown(rows)).toBe(true)
    }
  })

  it('treats an empty configured list as success, so every catalog client is catalog-only', () => {
    const rows = markConfiguredClients(catalog, { clients: [] })
    expect(visibleClients(rows, false)).toEqual([])
    expect(visibleClients(rows, true)).toHaveLength(3)
  })

  it('works for a console that sees only some clients', () => {
    const rows = markConfiguredClients([{ name: 'lab-fd1' }, { name: 'other-fd' }], { clients: { clients: [{ name: 'lab-fd1' }] } })
    expect(visibleClients(rows, false).map(row => row.name)).toEqual(['lab-fd1'])
    expect(rows.find(row => row.name === 'lab-fd1').enabled).toBe(true)
    expect(visibleClients(markConfiguredClients([], dot), false)).toEqual([])
  })
})
