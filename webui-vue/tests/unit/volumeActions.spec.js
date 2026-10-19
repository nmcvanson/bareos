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
import {
  VOLUME_STATUS_OPTIONS,
  buildDeleteVolumeCommand,
  buildPruneVolumeCommand,
  buildPurgeVolumeCommand,
  buildUpdatePoolFromConfigCommand,
  buildUpdateVolumeStatusCommand,
  formatDirectorReply,
  captureVolumeActionTarget,
  volumeActionTargetMatches,
  parseDirectorActionReply,
} from '../../src/utils/volumeActions.js'

describe('volumeActions utils', () => {
  it('captures an immutable volume and Director identity and rejects every different scope', () => {
    const target = captureVolumeActionTarget('volume-A', 'Director-A', 'Director-A')
    expect(Object.isFrozen(target)).toBe(true)
    expect(volumeActionTargetMatches(target, 'volume-A', 'Director-A', 'Director-A')).toBe(true)
    expect(volumeActionTargetMatches(target, 'volume-B', 'Director-A', 'Director-A')).toBe(false)
    expect(volumeActionTargetMatches(target, 'volume-A', 'Director-B', 'Director-A')).toBe(false)
    expect(volumeActionTargetMatches(target, 'volume-A', 'Director-A', 'Director-B')).toBe(false)
    expect(volumeActionTargetMatches(null, 'volume-A', 'Director-A', 'Director-A')).toBe(false)
    expect(() => captureVolumeActionTarget('', 'Director-A', 'Director-A')).toThrow()
  })
  it('exports the standard volume status options', () => {
    expect(VOLUME_STATUS_OPTIONS).toEqual([
      'Append',
      'Full',
      'Used',
      'Archive',
      'Read-Only',
      'Disabled',
    ])
  })

  describe('buildUpdateVolumeStatusCommand', () => {
    it('builds an update volume status command with canonical casing', () => {
      expect(buildUpdateVolumeStatusCommand('Vol001', 'Full')).toBe(
        'update volume="Vol001" volstatus=Full'
      )
      expect(buildUpdateVolumeStatusCommand('Vol001', 'append')).toBe(
        'update volume="Vol001" volstatus=Append'
      )
      expect(buildUpdateVolumeStatusCommand('Vol001', 'read-only')).toBe(
        'update volume="Vol001" volstatus=Read-Only'
      )
      expect(buildUpdateVolumeStatusCommand('Vol001', 'DISABLED')).toBe(
        'update volume="Vol001" volstatus=Disabled'
      )
    })

    it('quotes volume names containing spaces and special characters', () => {
      expect(buildUpdateVolumeStatusCommand('Volume Space', 'Used')).toBe(
        'update volume="Volume Space" volstatus=Used'
      )
      expect(buildUpdateVolumeStatusCommand('Vol"With"Quotes', 'Archive')).toBe(
        'update volume="Vol\\"With\\"Quotes" volstatus=Archive'
      )
    })

    it('refuses unknown volume statuses', () => {
      expect(() => buildUpdateVolumeStatusCommand('Vol001', 'Purged')).toThrow(
        /Unknown volume status/
      )
      expect(() => buildUpdateVolumeStatusCommand('Vol001', 'Recycle')).toThrow(
        /Unknown volume status/
      )
      expect(() => buildUpdateVolumeStatusCommand('Vol001', 'invalid')).toThrow(
        /Unknown volume status/
      )
      expect(() => buildUpdateVolumeStatusCommand('Vol001', '')).toThrow(
        /Volume status required/
      )
      expect(() => buildUpdateVolumeStatusCommand('Vol001', null)).toThrow(
        /Volume status required/
      )
    })

    it('requires a non-empty volume name', () => {
      expect(() => buildUpdateVolumeStatusCommand('', 'Full')).toThrow(
        /Volume name required/
      )
      expect(() => buildUpdateVolumeStatusCommand('   ', 'Full')).toThrow(
        /Volume name required/
      )
      expect(() => buildUpdateVolumeStatusCommand(null, 'Full')).toThrow(
        /Volume name required/
      )
    })
  })

  describe('buildPruneVolumeCommand', () => {
    it('builds a prune volume command with quotes', () => {
      expect(buildPruneVolumeCommand('Vol001')).toBe('prune volume="Vol001" yes')
      expect(buildPruneVolumeCommand('Vol Name With Space')).toBe(
        'prune volume="Vol Name With Space" yes'
      )
      expect(buildPruneVolumeCommand('Vol"Quotes')).toBe(
        'prune volume="Vol\\"Quotes" yes'
      )
    })

    it('requires a non-empty volume name', () => {
      expect(() => buildPruneVolumeCommand('')).toThrow(/Volume name required/)
      expect(() => buildPruneVolumeCommand('   ')).toThrow(/Volume name required/)
      expect(() => buildPruneVolumeCommand(undefined)).toThrow(/Volume name required/)
    })
  })

  describe('buildPurgeVolumeCommand', () => {
    it('builds a purge volume command with quotes', () => {
      expect(buildPurgeVolumeCommand('Vol001')).toBe('purge volume="Vol001" yes')
      expect(buildPurgeVolumeCommand('Volume Two')).toBe('purge volume="Volume Two" yes')
    })

    it('requires a non-empty volume name', () => {
      expect(() => buildPurgeVolumeCommand('')).toThrow(/Volume name required/)
      expect(() => buildPurgeVolumeCommand(null)).toThrow(/Volume name required/)
    })
  })

  describe('buildDeleteVolumeCommand', () => {
    it('builds a delete volume command with quotes', () => {
      expect(buildDeleteVolumeCommand('Vol001')).toBe('delete volume="Vol001" yes')
      expect(buildDeleteVolumeCommand('Volume Three')).toBe(
        'delete volume="Volume Three" yes'
      )
    })

    it('requires a non-empty volume name', () => {
      expect(() => buildDeleteVolumeCommand('')).toThrow(/Volume name required/)
      expect(() => buildDeleteVolumeCommand(null)).toThrow(/Volume name required/)
    })
  })

  describe('buildUpdatePoolFromConfigCommand', () => {
    it('builds an update pool from configuration command with quotes', () => {
      expect(buildUpdatePoolFromConfigCommand('Full')).toBe('update pool="Full"')
      expect(buildUpdatePoolFromConfigCommand('Daily Pool')).toBe(
        'update pool="Daily Pool"'
      )
      expect(buildUpdatePoolFromConfigCommand('Pool"Quote')).toBe(
        'update pool="Pool\\"Quote"'
      )
    })

    it('requires a non-empty pool name', () => {
      expect(() => buildUpdatePoolFromConfigCommand('')).toThrow(/Pool name required/)
      expect(() => buildUpdatePoolFromConfigCommand('  ')).toThrow(/Pool name required/)
      expect(() => buildUpdatePoolFromConfigCommand(null)).toThrow(/Pool name required/)
    })
  })

  describe('formatDirectorReply', () => {
    it('handles string replies', () => {
      expect(formatDirectorReply('Volume Vol001 marked Purged.')).toBe(
        'Volume Vol001 marked Purged.'
      )
    })

    it('handles error instances and objects', () => {
      expect(formatDirectorReply(new Error('Connection failed'))).toBe(
        'Connection failed'
      )
      expect(formatDirectorReply({ error: { message: 'Volume does not exist' } })).toBe(
        'Volume does not exist'
      )
      expect(formatDirectorReply({ error: 'Permission denied' })).toBe(
        'Permission denied'
      )
    })

    it('extracts messages from reply structures', () => {
      expect(
        formatDirectorReply({
          messages: {
            info: ['Volume Vol001 marked Purged.\n', 'Catalog records deleted.\n'],
          },
        })
      ).toBe('Volume Vol001 marked Purged.\nCatalog records deleted.\n')

      expect(
        formatDirectorReply({
          result: {
            messages: ['Pool updated from configuration.'],
          },
        })
      ).toBe('Pool updated from configuration.')
    })

    it('handles result strings and empty inputs', () => {
      expect(formatDirectorReply({ result: 'OK' })).toBe('OK')
      expect(formatDirectorReply(null)).toBe('')
      expect(formatDirectorReply(undefined)).toBe('')
    })
  })
  it('checks resolved failures and preserves detailed diagnostics before generic failed text', () => {
    const error = { code: 1, message: 'failed', data: { messages: { error: ['  Permission denied.\n', 'Command refused.\n'] } } }
    for (const reply of [{ error }, { jsonrpc: '2.0', id: null, error }, { result: { error } },
      { type: 'response', data: { error } }]) {
      expect(parseDirectorActionReply(reply)).toEqual({ ok: false, text: '  Permission denied.\nCommand refused.\n' })
    }
    for (const reply of [{ messages: { error: ['Denied\n'] } }, { result: { messages: { error: ['Denied\n'] } } },
      { success: false }, { result: { ok: false } }, null, undefined, new Error('Disconnected'), 'Denied']) {
      expect(parseDirectorActionReply(reply).ok).toBe(false)
    }
    expect(parseDirectorActionReply({ result: {}, messages: { info: ['Updated.\n'] } }))
      .toEqual({ ok: true, text: 'Updated.\n' })
    expect(parseDirectorActionReply({}).ok).toBe(true)
  })
})
