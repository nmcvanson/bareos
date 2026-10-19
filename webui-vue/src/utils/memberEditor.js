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

// Pure list operations for the ordered storage member editor.

export function moveMember(list, index, delta) {
  const target = index + delta
  if (!Number.isInteger(index) || index < 0 || index >= list.length) return [...list]
  if (target < 0 || target >= list.length) return [...list]
  const next = [...list]
  ;[next[index], next[target]] = [next[target], next[index]]
  return next
}

export function removeMember(list, index) {
  if (!Number.isInteger(index) || index < 0 || index >= list.length) return [...list]
  return list.filter((_, i) => i !== index)
}

export function addMember(list, name) {
  if (typeof name !== 'string' || !name || list.includes(name)) return [...list]
  return [...list, name]
}

export function membersChanged(before, after) {
  return before.length !== after.length || before.some((name, i) => name !== after[i])
}

// Storages that can still be added to the list.
export function addableMembers(all, list) {
  return all.filter(name => !list.includes(name))
}

// Group owner type as the lower-case configure resource keyword.
export function ownerResourceType(ownerType) {
  const type = String(ownerType).toLowerCase()
  if (!['job', 'jobdefs', 'pool'].includes(type)) throw new TypeError('Unsupported owner type')
  return type
}
