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
  moveMember, removeMember, addMember, membersChanged, addableMembers, ownerResourceType,
} from '../../src/utils/memberEditor.js'
import { buildConfigureSetCommand } from '../../src/utils/storageGroups.js'

describe('member editor', () => {
  it('moves members without mutating the input and ignores out-of-range moves', () => {
    const list = ['A', 'B', 'C']
    expect(moveMember(list, 1, -1)).toEqual(['B', 'A', 'C'])
    expect(moveMember(list, 1, 1)).toEqual(['A', 'C', 'B'])
    expect(moveMember(list, 0, -1)).toEqual(list)
    expect(moveMember(list, 2, 1)).toEqual(list)
    expect(moveMember(list, 5, 1)).toEqual(list)
    expect(list).toEqual(['A', 'B', 'C'])
  })

  it('removes and adds members, refusing duplicates and empty names', () => {
    expect(removeMember(['A', 'B'], 0)).toEqual(['B'])
    expect(removeMember(['A', 'B'], 7)).toEqual(['A', 'B'])
    expect(addMember(['A'], 'B')).toEqual(['A', 'B'])
    expect(addMember(['A'], 'A')).toEqual(['A'])
    expect(addMember(['A'], '')).toEqual(['A'])
  })

  it('detects order changes and lists addable storages', () => {
    expect(membersChanged(['A', 'B'], ['A', 'B'])).toBe(false)
    expect(membersChanged(['A', 'B'], ['B', 'A'])).toBe(true)
    expect(membersChanged(['A', 'B'], ['A'])).toBe(true)
    expect(addableMembers(['A', 'B', 'C'], ['B'])).toEqual(['A', 'C'])
  })

  it('maps the group owner type to the configure keyword used by the command builder', () => {
    expect(ownerResourceType('Pool')).toBe('pool')
    expect(ownerResourceType('Job')).toBe('job')
    expect(() => ownerResourceType('Client')).toThrow()
    expect(buildConfigureSetCommand({
      resourceType: ownerResourceType('Pool'), name: 'DemoLocalPool', directive: 'storage', value: ['File1a', 'File2'], preview: true,
    })).toBe('configure set pool="DemoLocalPool" storage="File1a,File2" preview')
  })
})
