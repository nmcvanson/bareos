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

import { describe, expect, it } from 'vitest'
import { nextTick, reactive, watch } from 'vue'
import { useJobScope } from '../../src/utils/jobScope.js'
import { createInflightGuard } from '../../src/utils/inflightGuard.js'
import { createLiveProgress } from '../../src/utils/liveProgress.js'

const deferred = () => {
  let resolve
  const promise = new Promise(r => { resolve = r })
  return { promise, resolve }
}

// Mirrors the wiring of JobDetailsPage: the watcher resets the live samples,
// the loader captures the scope before its await and drops a stale result.
function pageHarness(route, auth, settings) {
  const { scopeKey, jobDirector } = useJobScope({ route, auth, settings })
  const live = createLiveProgress()
  const guard = createInflightGuard()
  const shown = { director: null, loads: [] }
  const requests = []
  live.reset(scopeKey.value)
  watch(scopeKey, () => { live.reset(scopeKey.value); shown.director = null })

  const load = () => guard.run(scopeKey.value, async () => {
    const scope = scopeKey.value
    const directorName = jobDirector.value
    const gate = deferred()
    requests.push({ directorName, gate })
    await gate.promise
    if (scope !== scopeKey.value) return
    shown.director = directorName
    shown.loads.push(directorName)
  })
  return { scopeKey, live, shown, requests, load }
}

describe('job details scope with the resolved Director', () => {
  it('changes the scope when the active Director changes and the route has no Director query', async () => {
    const route = reactive({ params: { id: '7' }, query: {} })
    const auth = reactive({ user: { director: 'dirA' } })
    const settings = reactive({ directorName: '' })
    const page = pageHarness(route, auth, settings)

    const keyA = page.scopeKey.value
    const ticketA = page.live.ticket()
    expect(page.live.accept(ticketA, 'fd', { files: 5, bytes: 5 })).toBe(true)
    const loadA = page.load()

    auth.user.director = 'dirB'
    await nextTick()
    expect(page.scopeKey.value).not.toBe(keyA)
    expect(page.live.fd).toBeNull()                       // old sample dropped
    expect(page.live.accept(ticketA, 'sd', { files: 9, bytes: 9 })).toBe(false)

    page.requests[0].gate.resolve()                       // the Director A load ends late
    await loadA
    expect(page.shown.loads).toEqual([])                  // and is not shown for B

    const loadB = page.load()
    expect(page.requests).toHaveLength(2)
    expect(page.requests[1].directorName).toBe('dirB')
    page.requests[1].gate.resolve()
    await loadB
    expect(page.shown.loads).toEqual(['dirB'])
  })

  it('falls back to the configured Director and prefers the route query', async () => {
    const route = reactive({ params: { id: '7' }, query: {} })
    const auth = reactive({ user: null })
    const settings = reactive({ directorName: 'cfg' })
    const { scopeKey, jobDirector } = useJobScope({ route, auth, settings })
    expect(jobDirector.value).toBe('cfg')
    route.query = { director: 'viaQuery' }
    expect(jobDirector.value).toBe('viaQuery')
    expect(scopeKey.value).toBe('viaQuery\u00007')
    route.params = { id: '8' }
    expect(scopeKey.value).toBe('viaQuery\u00008')
  })
})
