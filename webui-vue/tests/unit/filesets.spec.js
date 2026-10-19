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

import capture from './fixtures/api2-fileset-pools-lists.txt?raw'
import { describe, expect, it } from 'vitest'
import { filesetIncludePaths } from '../../src/utils/filesets.js'

function parseShowFilesetFromCapture(captureText, filesetName) {
  const normalized = captureText.replaceAll('\r\n', '\n')
  const marker = `show fileset=${filesetName}\n`
  const after = normalized.split(marker)[1]
  const jsonText = after.slice(0, after.indexOf('\n}show pools') + 2)
  return JSON.parse(jsonText)
}

describe('filesets utils', () => {
  describe('filesetIncludePaths', () => {
    it('extracts include file paths from real show fileset=Demo-Projects fixture', () => {
      const showJson = parseShowFilesetFromCapture(capture, 'Demo-Projects')
      const paths = filesetIncludePaths(showJson, 'Demo-Projects')
      expect(paths).toEqual(['/srv/demo/Projects'])
    })

    it('handles multiple include blocks and multiple files', () => {
      const complexJson = {
        result: {
          filesets: {
            'Multi-Include': {
              name: 'Multi-Include',
              include: [
                {
                  options: [{ signature: 'MD5' }],
                  file: ['/path/one', '/path/two'],
                },
                {
                  file: ['/path/three'],
                },
              ],
            },
          },
        },
      }
      expect(filesetIncludePaths(complexJson, 'Multi-Include')).toEqual([
        '/path/one',
        '/path/two',
        '/path/three',
      ])
    })

    it('returns empty array when fileset is not found or has no include blocks', () => {
      const showJson = parseShowFilesetFromCapture(capture, 'Demo-Projects')
      expect(filesetIncludePaths(showJson, 'NonExistent')).toEqual([])
      expect(filesetIncludePaths({ result: { filesets: {} } }, 'Foo')).toEqual([])
      expect(filesetIncludePaths(null, 'Foo')).toEqual([])
      expect(filesetIncludePaths({}, '')).toEqual([])
    })

    it('handles direct fileset object without result wrapper', () => {
      const filesetObj = {
        name: 'Simple',
        include: [{ file: ['/srv/data'] }],
      }
      expect(filesetIncludePaths(filesetObj, 'Simple')).toEqual(['/srv/data'])
      expect(filesetIncludePaths(filesetObj)).toEqual(['/srv/data'])
    })
  })
})
