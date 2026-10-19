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

import { describe, expect, it, vi } from 'vitest'
import { parse } from '@babel/parser'
import jobsSource from '../../src/pages/JobsPage.vue?raw'
import dashboardSource from '../../src/pages/DashboardPage.vue?raw'
import { jobOutcome } from '../../src/utils/jobOutcome.js'
import { createJobReasonCache, jobReasonScopeKey, toggleJobReasonExpansion } from '../../src/utils/jobReasonCache.js'

const row = director => ({ director, scopeKey: `${director}:7`, id: 7, status: 'E' })
function handler(source, name, environment) {
  const script = source.split('<script setup>')[1].split('</script>')[0]
  const node = parse(script, { sourceType: 'module' }).program.body.find(node => node.type === 'FunctionDeclaration' && node.id.name === name)
  const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor
  return new AsyncFunction(...Object.keys(environment), 'props', script.slice(node.body.start + 1, node.body.end - 1))
    .bind(null, ...Object.values(environment))
}

describe('Director-scoped reason page regressions', () => {
  it('keeps Jobs A/7 and B/7 reasons separate and fetches both Directors', async () => {
    let active = 'A'
    const rowReasons = { value: {} }
    const switchToJobDirector = vi.fn(async job => { active = job.director })
    const director = { call: vi.fn(async () => ({ joblog: [{ logtext: `Fatal error: reason from ${active}` }] })) }
    const expandedReasonRows = { value: {} }
    const reasonCache = createJobReasonCache({ switchDirector: switchToJobDirector, call: director.call,
      onUpdate: (key, value) => { rowReasons.value = { ...rowReasons.value, [key]: value } } })
    const toggle = handler(jobsSource, 'toggleRowReasons', { rowReasons, switchToJobDirector, director, jobOutcome,
      expandedReasonRows, reasonCache, jobReasonScopeKey, toggleJobReasonExpansion, auth: { user: { director: 'A' } }, t: value => value })
    await toggle({ row: row('A'), expand: false })
    await toggle({ row: row('B'), expand: false })
    expect(director.call).toHaveBeenCalledTimes(2)
    expect(rowReasons.value['A:7'].reasons.join(' ')).toContain('reason from A')
    expect(rowReasons.value['B:7'].reasons.join(' ')).toContain('reason from B')
    expect(expandedReasonRows.value).toEqual({ 'A:7': true, 'B:7': true })
    await toggle({ row: row('A'), expand: true })
    expect(expandedReasonRows.value).toEqual({ 'A:7': false, 'B:7': true })
    expect(director.call).toHaveBeenCalledTimes(2)
  })
  it('fetches Dashboard A/7 and B/7 logs through each rows Director', async () => {
    let active = 'A'
    const failedJobReasons = { value: {} }
    const director = { call: vi.fn(async () => ({ joblog: [{ logtext: `Fatal error: reason from ${active}` }] })) }
    const switchActiveDirector = vi.fn(async target => { active = target })
    const reasonCache = createJobReasonCache({ switchDirector: job => switchActiveDirector(job.director), call: director.call,
      onUpdate: (key, value) => { if (!value.loading) failedJobReasons.value[key] = value.error || value.reasons[0] || value.termination } })
    const fetchReasons = handler(dashboardSource, 'fetchFailedJobReasons', {
      failedJobs24h: { value: [row('A'), row('B')] }, failedJobReasons,
      loadingJobLogs: { value: false }, director, switchActiveDirector,
      jobId: job => job.id, jobOutcome, reasonCache, jobReasonScopeKey, t: value => value,
    })
    await fetchReasons()
    expect(failedJobReasons.value['A:7']).toContain('reason from A')
    expect(failedJobReasons.value['B:7']).toContain('reason from B')
  })
})

describe('scoped reason cache and request sequencing', () => {
  it('scopes keys and expansion by Director, with a captured fallback for local rows', () => {
    expect(jobReasonScopeKey(row('A'))).toBe('A:7')
    expect(jobReasonScopeKey(row('B'))).toBe('B:7')
    expect(jobReasonScopeKey({ jobid: 7 }, 'C')).toBe('C:7')
    expect(() => jobReasonScopeKey({ id: '7;bad', director: 'A' })).toThrow()
    expect(() => jobReasonScopeKey({ id: 7 })).toThrow()
    const state = toggleJobReasonExpansion({}, row('A'))
    expect(toggleJobReasonExpansion(state, row('B'))).toEqual({ 'A:7': true, 'B:7': true })
    expect(state).toEqual({ 'A:7': true })
  })
  it('serializes switches and log calls across cache instances and deduplicates the same row', async () => {
    let active = 'A'
    let release
    const gate = new Promise(resolve => { release = resolve })
    const calls = []
    const switchDirector = vi.fn(async job => { active = job.director })
    const call = vi.fn(async command => {
      const captured = active
      calls.push([captured, command])
      if (captured === 'A') await gate
      return { result: { joblog: [{ logtext: `Fatal error: ${captured}` }] } }
    })
    const firstCache = createJobReasonCache({ switchDirector, call })
    const secondCache = createJobReasonCache({ switchDirector, call })
    const first = firstCache.load(row('A'))
    expect(firstCache.load(row('A'))).toBe(first)
    const second = secondCache.load(row('B'))
    await vi.waitFor(() => expect(call).toHaveBeenCalledTimes(1))
    expect(active).toBe('A')
    release()
    const [a, b] = await Promise.all([first, second])
    expect(calls).toEqual([['A', 'list joblog jobid=7'], ['B', 'list joblog jobid=7']])
    expect(a.reasons).toContain('Fatal error: A')
    expect(b.reasons).toContain('Fatal error: B')
    await firstCache.load(row('A'))
    expect(call).toHaveBeenCalledTimes(2)
  })
  it('captures the selected Director before a queued request can switch it', async () => {
    let selected = 'A'
    const targets = []
    const cache = createJobReasonCache({ fallbackDirector: () => selected,
      switchDirector: async job => { targets.push(job.director) },
      call: async () => ({ joblog: [{ logtext: 'Fatal error: scoped' }] }) })
    const first = cache.load({ id: 7, status: 'E' })
    selected = 'B'
    const second = cache.load({ id: 7, status: 'E' })
    await Promise.all([first, second])
    expect(targets).toEqual(['A', 'B'])
  })
  it('keeps a refusal in its own scope and permits an explicit retry without poisoning the queue', async () => {
    const call = vi.fn().mockResolvedValueOnce({ error: { message: 'failed', data: { messages: { error: ['Permission denied\n'] } } } })
      .mockResolvedValue({ joblog: [{ logtext: 'Fatal error: retried' }] })
    const updates = {}
    const cache = createJobReasonCache({ switchDirector: async () => {}, call,
      onUpdate: (key, value) => { updates[key] = value } })
    expect((await cache.load(row('A'))).error).toBe('Permission denied\n')
    expect((await cache.load(row('B'))).error).toBeNull()
    expect((await cache.load(row('A'))).error).toBeNull()
    expect(Object.keys(updates).sort()).toEqual(['A:7', 'B:7'])
  })
  it('disposes queued work and suppresses late UI updates on unmount', async () => {
    let release
    const gate = new Promise(resolve => { release = resolve })
    const switchDirector = vi.fn(async () => {})
    const onUpdate = vi.fn()
    const call = vi.fn(async () => { await gate; return { joblog: [] } })
    const first = createJobReasonCache({ switchDirector, call, onUpdate })
    const next = createJobReasonCache({ switchDirector, call })
    const pending = first.load(row('A'))
    await vi.waitFor(() => expect(call).toHaveBeenCalledTimes(1))
    const queued = next.load(row('B'))
    first.dispose()
    next.dispose()
    release()
    await Promise.all([pending, queued])
    expect(switchDirector).toHaveBeenCalledTimes(1)
    expect(onUpdate).toHaveBeenCalledTimes(1)
    expect(await next.load(row('B'))).toBeNull()
  })
})
