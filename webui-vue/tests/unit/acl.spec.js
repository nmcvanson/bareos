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
import { actionAllowed } from '../../src/utils/acl.js'

describe('acl utils', () => {
  describe('actionAllowed', () => {
    it('returns true when commands is null or undefined (not loaded yet)', () => {
      expect(actionAllowed(null, 'run')).toBe(true)
      expect(actionAllowed(undefined, 'run')).toBe(true)
    })

    it('returns true when command is allowed (permission is true)', () => {
      const commands = {
        run: { permission: true },
        enable: { permission: true },
      }
      expect(actionAllowed(commands, 'run')).toBe(true)
      expect(actionAllowed(commands, 'enable')).toBe(true)
    })

    it('returns false when command is denied (permission is false)', () => {
      const commands = {
        run: { permission: false },
        disable: { permission: false },
      }
      expect(actionAllowed(commands, 'run')).toBe(false)
      expect(actionAllowed(commands, 'disable')).toBe(false)
    })

    it('returns false when command is unknown or missing from commands', () => {
      const commands = {
        status: { permission: true },
      }
      expect(actionAllowed(commands, 'run')).toBe(false)
      expect(actionAllowed(commands, 'enable')).toBe(false)
      expect(actionAllowed(commands, 'nonexistent')).toBe(false)
    })
  })
})
