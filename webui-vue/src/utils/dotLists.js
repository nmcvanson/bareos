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

// The dot commands that list resource names, with the shape the Director
// returns in `.api 2` (captured on a real Director: .team/webui-demo/captures/
// dot-lists-api2.txt). Each reply is { result: { <key>: [ { <field>: name } ] } };
// .msgs uses the key "messages" and the field "text", the others "name".
export const DOT_LISTS = Object.freeze({
  jobs: { command: '.jobs', key: 'jobs', field: 'name' },
  jobdefs: { command: '.jobdefs', key: 'jobdefs', field: 'name' },
  clients: { command: '.clients', key: 'clients', field: 'name' },
  filesets: { command: '.filesets', key: 'filesets', field: 'name' },
  schedules: { command: '.schedule', key: 'schedules', field: 'name' },
  pools: { command: '.pools', key: 'pools', field: 'name' },
  storages: { command: '.storages', key: 'storages', field: 'name' },
  messages: { command: '.msgs', key: 'messages', field: 'text' },
})

// Names from one reply; strings, the expected field, then name/text/value are accepted.
export function dotListNames(listName, reply) {
  const spec = DOT_LISTS[listName]
  if (!spec) throw new TypeError(`Unknown list ${listName}`)
  const rows = reply?.result?.[spec.key] ?? reply?.[spec.key]
  if (!Array.isArray(rows)) return []
  return rows
    .map(row => (typeof row === 'string' ? row : row?.[spec.field] ?? row?.name ?? row?.text ?? row?.value))
    .filter(name => typeof name === 'string' && name.length > 0)
}
