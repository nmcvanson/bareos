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

// One request in flight per scope. A second caller with the same scope (timer
// tick, manual refresh, route watcher, or a return to an earlier scope) gets
// the pending promise instead of starting another request; every scope keeps
// its own pending entry, so A -> B -> A reuses the first A while it runs.
export function createInflightGuard() {
  const pending = new Map()
  return {
    run(scope, fn) {
      const existing = pending.get(scope)
      if (existing) return existing
      const promise = (async () => fn())().finally(() => {
        if (pending.get(scope) === promise) pending.delete(scope)
      })
      pending.set(scope, promise)
      return promise
    },
    get busy() { return pending.size > 0 },
  }
}
