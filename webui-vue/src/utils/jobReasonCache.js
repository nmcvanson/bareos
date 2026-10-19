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

import { jobOutcome } from './jobOutcome.js'

// All reason-cache instances share the singleton Director connection. Keep a
// switch and its log request together, including requests from another page.
let requestQueue = Promise.resolve()

export function jobReasonScopeKey(row, fallbackDirector = '') {
  const id = row?.id ?? row?.jobid
  const director = row?.director || fallbackDirector
  if (!director || !/^[1-9]\d*$/.test(String(id))) throw new TypeError('Director and valid JobId required')
  return `${director}:${id}`
}

export function toggleJobReasonExpansion(expanded, row, fallbackDirector = '') {
  const key = jobReasonScopeKey(row, fallbackDirector)
  return { ...expanded, [key]: !expanded[key] }
}

export function createJobReasonCache({ switchDirector, call, fallbackDirector = () => '', onUpdate = () => {} }) {
  const cache = new Map()
  const pending = new Map()
  let disposed = false

  function publish(key, value) {
    if (!disposed) {
      cache.set(key, value)
      onUpdate(key, value)
    }
    return value
  }

  function load(row) {
    // Capture the row's scope before any await or active-Director change.
    const scoped = { ...row, director: row.director || fallbackDirector() }
    const key = jobReasonScopeKey(scoped)
    if (pending.has(key)) return pending.get(key)
    const cached = cache.get(key)
    if (cached && !cached.error && !cached.loading) return Promise.resolve(cached)
    if (disposed) return Promise.resolve(null)
    publish(key, { loading: true, error: null, reasons: [], termination: null })
    const request = requestQueue.then(async () => {
      if (disposed) return null
      try {
        await switchDirector(scoped)
        if (disposed) return null
        const reply = await call(`list joblog jobid=${scoped.id ?? scoped.jobid}`)
        const errors = reply?.error?.data?.messages?.error
        const failure = reply?.error ?? reply?.result?.error
        if (failure) {
          throw new Error(Array.isArray(errors) ? errors.join('')
            : typeof failure === 'string' ? failure : failure.message || 'Failed to load job log')
        }
        const log = reply?.joblog ?? reply?.result?.joblog ?? (Array.isArray(reply) ? reply : [])
        const outcome = jobOutcome(scoped, log)
        return publish(key, { loading: false, error: null,
          reasons: outcome.reasonLines ?? [], termination: outcome.termination ?? null })
      } catch (error) {
        return publish(key, { loading: false, error: error?.message ?? String(error), reasons: [], termination: null })
      }
    }).finally(() => pending.delete(key))
    pending.set(key, request)
    requestQueue = request.catch(() => {})
    return request
  }

  return { load, dispose() { disposed = true } }
}
