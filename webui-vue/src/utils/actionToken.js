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

// Confirmation tokens for actions that write to a Director (delete, prune,
// purge, update). A token is created when the user confirms or opens the
// dialog and is bound to its target; the page keeps only the current one.
//
//  - Invalidation is permanent: a change of the route scope (volume, pool,
//    route Director) or of the active Director invalidates the token for good,
//    except an active-Director change TO the target's own Director (the page's
//    own switch). Returning to the old values never revives a token.
//  - A write needs the current, valid, unused token, an unchanged scope and an
//    active Director equal to the target's Director, checked right before the
//    call (after the last await).
import { markRaw } from 'vue'

export function createActionTokenGuard() {
  let current = null
  let sequence = 0
  const invalidate = token => { if (token) token.invalidated = true }

  const guard = {
    open(target, scope, activeDirector) {
      if (!target || typeof target.director !== 'string' || !target.director) {
        throw new TypeError('Action target with a Director required')
      }
      invalidate(current)
      // markRaw: a token kept in a ref must stay the same object the guard holds
      current = markRaw({
        id: ++sequence,
        target: Object.freeze({ ...target }),
        scope,
        active: activeDirector,
        invalidated: false,
        used: false,
      })
      return current
    },
    // Call from a synchronous watcher on every change of scope or active Director.
    observe(scope, activeDirector) {
      const token = current
      if (!token || token.invalidated) return
      if (scope !== token.scope) {
        invalidate(token)
      } else if (activeDirector !== token.active) {
        if (activeDirector === token.target.director) token.active = activeDirector
        else invalidate(token)
      }
    },
    // Still the page's token for this scope (used or not).
    isCurrent(token, scope) {
      return Boolean(token) && token === current && !token.invalidated && token.scope === scope
    },
    // The last check before a write.
    canWrite(token, scope, activeDirector) {
      return guard.isCurrent(token, scope) && !token.used && activeDirector === token.target.director
    },
    consume(token) { if (token) token.used = true },
    invalidateCurrent() { invalidate(current) },
  }
  return guard
}
