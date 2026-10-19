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

import capture from './fixtures/dot-lists-api2.txt?raw'
import { describe, expect, it } from 'vitest'
import { DOT_LISTS, dotListNames } from '../../src/utils/dotLists.js'

// Real `.api 2` replies of a Director (one section per command).
function reply(command) {
  const section = capture.split(/^##### /m).find(part => part.startsWith(`api2: ${command}\n`))
  const body = section.slice(section.indexOf(`}${command}\n`) + command.length + 1)
  return JSON.parse(body.slice(body.indexOf('{'), body.lastIndexOf('}') + 1))
}

describe('dot list replies of a real Director', () => {
  it.each(Object.entries(DOT_LISTS))('reads %s from the captured %s reply', (name, spec) => {
    const names = dotListNames(name, reply(spec.command))
    expect(names.length).toBeGreaterThan(0)
    expect(names.every(item => typeof item === 'string' && item.length > 0)).toBe(true)
  })

  it('reads the message resources from .msgs, which uses the key messages and the field text', () => {
    expect(dotListNames('messages', reply('.msgs'))).toEqual(['Standard', 'Daemon'])
    expect(dotListNames('jobdefs', reply('.jobdefs'))).toEqual(['s3b-defs', 'DefaultJob'])
    expect(dotListNames('schedules', reply('.schedule')).length).toBe(2)
  })

  it('tolerates unwrapped replies, plain strings and an unexpected shape', () => {
    expect(dotListNames('messages', { messages: [{ text: 'A' }, 'B'] })).toEqual(['A', 'B'])
    expect(dotListNames('clients', { result: {} })).toEqual([])
    expect(dotListNames('clients', null)).toEqual([])
    expect(() => dotListNames('nothing', {})).toThrow()
  })
})
