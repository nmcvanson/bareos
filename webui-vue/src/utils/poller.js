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

// start() polls immediately; hidden tabs pause and resume on visibility change.
// stop() removes the timer/listener; an in-flight request may finish, without overlap.
export function createPoller(fn, intervalMs) {
  if (typeof fn !== 'function') throw new TypeError('Poll function required')
  if (!Number.isFinite(intervalMs) || intervalMs <= 0) {
    throw new RangeError('Poll interval must be positive')
  }
  const page = typeof document === 'undefined' ? null : document
  let active = false
  let pending = false
  let timer = null
  let error = null
  const tick = async () => {
    if (!active || pending || page?.hidden) return
    pending = true
    try {
      await fn()
      error = null
    } catch (failure) {
      error = failure
    } finally {
      pending = false
    }
  }
  const visibilityChanged = () => {
    if (timer !== null) clearInterval(timer)
    timer = null
    if (active && !page?.hidden) {
      void tick()
      timer = setInterval(tick, intervalMs)
    }
  }
  return {
    start() {
      if (active) return
      active = true
      page?.addEventListener('visibilitychange', visibilityChanged)
      visibilityChanged()
    },
    stop() {
      active = false
      if (timer !== null) clearInterval(timer)
      timer = null
      page?.removeEventListener('visibilitychange', visibilityChanged)
    },
    get pending() { return pending },
    get error() { return error },
  }
}
