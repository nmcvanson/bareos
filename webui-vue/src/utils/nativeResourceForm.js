/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026 Bareos GmbH & Co. KG

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

import { NATIVE_DIRECTIVES } from './configureAddDirectives.js'

function nativeDirective(type, input) {
  if (!Object.hasOwn(NATIVE_DIRECTIVES, type)) throw new TypeError('Unsupported resource type')
  const metadata = NATIVE_DIRECTIVES[type]
  const normalized = String(input ?? '').replace(/[ \t]/g, '').toLowerCase()
  for (const [group, names] of Object.entries(metadata)) {
    const key = names.find(name => name.toLowerCase() === normalized)
    if (key) return { key, quoted: group === 'quoted' }
  }
  throw new TypeError('Unsupported flat directive')
}

export function nativeResourceFields(type, name, rows) {
  const fields = [{ ...nativeDirective(type, 'Name'), value: name }]
  for (const row of rows) {
    if (!row.key && !row.value) continue
    const directive = nativeDirective(type, row.key)
    if (directive.key === 'Name') throw new TypeError('Use the Name field above')
    fields.push({ ...directive, value: row.value })
  }
  return fields
}

export function nativeDirectiveOptions(type, resources = []) {
  if (!Object.hasOwn(NATIVE_DIRECTIVES, type)) return []
  const existing = resources.flatMap(resource => Object.keys(resource.directives ?? {}))
  const options = []
  for (const input of [...existing, ...Object.values(NATIVE_DIRECTIVES[type]).flat()]) {
    try {
      const { key } = nativeDirective(type, input)
      if (key !== 'Name' && !options.includes(key)) options.push(key)
    } catch { /* Some show fields are status data, rather than directives. */ }
  }
  return options
}

export function newResourceRows(type) {
  const defaults = {
    jobdefs: [['Type', 'Backup']], client: [['Address', ''], ['Password', '']],
    pool: [['PoolType', 'Backup']], storage: [['Address', ''], ['Password', ''], ['Device', ''], ['MediaType', '']],
    schedule: [['Run', '']], messages: [['Catalog', 'all']],
  }
  if (!Object.hasOwn(defaults, type)) throw new TypeError('Use the job form or configuration files for this type')
  return defaults[type].map(([key, value]) => ({ key, value }))
}
