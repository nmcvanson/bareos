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

import { describe, expect, it, vi } from 'vitest'
import { parse } from '@babel/parser'
import pageSource from '../../src/pages/JobDetailsPage.vue?raw'
import { createLiveProgress, displayProgress, liveScope } from '../../src/utils/liveProgress.js'
import { parseFdRunning, parseSdRunning, sdStatusCandidates, storageConnected } from '../../src/utils/jobProgress.js'
import { buildJobDefaultsCommand } from '../../src/utils/jobs.js'
import { quoteDirectorString } from '../../src/utils/directorStrings.js'

describe('live job progress', () => {
  it('shows live counters while running and the catalog record once finished', () => {
    const live = createLiveProgress()
    live.reset(liveScope(7, 'dir'))
    live.accept(live.ticket(), 'fd', { files: 5, bytes: 500 })
    const running = { files: 0, bytes: 0 }
    expect(displayProgress({ job: running, running: true, fd: live.fd, sd: live.sd })).toEqual({ files: 5, bytes: 500 })
    // completion clears the samples; the catalog record (50 files) wins even if a sample is passed
    live.reset(liveScope(7, 'dir'))
    const done = { files: 50, bytes: 5000 }
    expect(displayProgress({ job: done, running: false, fd: { files: 5, bytes: 500 }, sd: null })).toEqual({ files: 50, bytes: 5000 })
    expect(displayProgress({ job: done, running: false, fd: live.fd, sd: live.sd })).toEqual({ files: 50, bytes: 5000 })
  })

  it('drops a late reply that belongs to an older job, route or Director', () => {
    const live = createLiveProgress()
    live.reset(liveScope(7, 'dirA'))
    const old = live.ticket()
    live.reset(liveScope(8, 'dirA'))
    expect(live.accept(old, 'fd', { files: 5, bytes: 500 })).toBe(false)
    expect(live.fd).toBeNull()
    const otherDirector = live.ticket()
    live.reset(liveScope(8, 'dirB'))
    expect(live.accept(otherDirector, 'sd', { files: 9, bytes: 9 })).toBe(false)
    expect(live.sd).toBeNull()
    expect(live.accept(live.ticket(), 'sd', { files: 3, bytes: 30 })).toBe(true)
    expect(live.sd).toEqual({ files: 3, bytes: 30 })
  })

  it('clears samples on reset and ignores unknown kinds', () => {
    const live = createLiveProgress()
    live.reset(liveScope(1, 'd'))
    live.accept(live.ticket(), 'fd', { files: 1, bytes: 1 })
    live.reset(liveScope(2, 'd'))
    expect(live.fd).toBeNull()
    expect(live.accept(live.ticket(), 'x', {})).toBe(false)
    expect(displayProgress({ job: null, running: true, fd: null, sd: null })).toEqual({ files: 0, bytes: 0 })
  })
})

describe('live numbers kept until the final totals', () => {
  it('shows the last live numbers while the running job record still has zero', () => {
    const live = createLiveProgress()
    live.reset(liveScope(9, 'dir'))
    live.accept(live.ticket(), 'fd', { files: 347, bytes: 232870000 })
    live.accept(live.ticket(), 'fd', null)
    live.accept(live.ticket(), 'sd', null)
    const running = { files: 0, bytes: 0 }
    expect(live.fd).toBeNull()
    expect(displayProgress({ job: running, running: true, fd: live.fd, sd: live.sd, last: live.last }))
      .toEqual({ files: 347, bytes: 232870000 })
    const done = { files: 347, bytes: 232870912 }
    expect(displayProgress({ job: done, running: false, fd: live.fd, sd: live.sd, last: live.last }))
      .toEqual({ files: 347, bytes: 232870912 })
  })

  it('forgets the last numbers on reset', () => {
    const live = createLiveProgress()
    live.reset(liveScope(9, 'dir'))
    live.accept(live.ticket(), 'sd', { files: 5, bytes: 50 })
    live.reset(liveScope(10, 'dir'))
    expect(live.last).toBeNull()
    expect(displayProgress({ job: { files: 0, bytes: 0 }, running: true, fd: null, sd: null, last: live.last }))
      .toEqual({ files: 0, bytes: 0 })
  })
})

describe('pollRunningProgress of the job page', () => {
  const fdText = (files, bytes) => `Running Jobs:
JobId 1130 Job some-job.2026-10-19 is running.
    Full Backup Job started: 19-Oct-2026 08:40:00
    Files=${files} Bytes=${bytes} Bytes/sec=1,000 Errors=0
    Processing file: /etc/hosts
`
  const sdText = (files, bytes) => `Job inventory:

JobId=1130 Level=Full Type=Backup Name=some-job Status=Running
Writing: Volume="vol-0347"
    pool="SomePool" device="Dev1" (bucket)
    spooling=0 despooling=0 despool_wait=0
    Files=${files} Bytes=${bytes} AveBytes/sec=16,395,033 LastBytes/sec=16,395,033

====
`
  const noJobSd = 'Job inventory:\n\n====\n'
  const noJobFd = 'Running Jobs:\nNo Jobs running.\n'

  const connectedLog = [{ logtext: ' 2026-10-19 08:40:02 dir JobId 1130: Connected Storage daemon at sd:9103, encryption: TLS\n' }]

  function makePage({ job, events = { attempts: [] }, rawCall, log = connectedLog, defaults = { defaults: { storage: 'Cfg-sd' } } }) {
    const script = pageSource.split('<script setup>')[1].split('</script>')[0]
    const wanted = ['loadConfiguredStorage', 'pollRunningProgress']
    const body = parse(script, { sourceType: 'module' }).program.body
      .filter(node => node.type === 'FunctionDeclaration' && wanted.includes(node.id.name))
      .map(node => script.slice(node.start, node.end)).join('\n')
    const live = createLiveProgress()
    live.reset(liveScope(1130, 'dir'))
    const env = {
      job: { value: job }, isRunning: { value: true }, liveProgress: live,
      fdRunning: { value: null }, sdRunning: { value: null },
      storageEvents: { value: events }, rawJobLog: { value: log }, quoteDirectorString, parseFdRunning, parseSdRunning,
      sdStatusCandidates, storageConnected, buildJobDefaultsCommand,
      director: { rawCall: vi.fn(rawCall), call: vi.fn(async () => defaults) },
    }
    const run = new Function(...Object.keys(env), `let configuredStorage = null\n${body}\nreturn pollRunningProgress`)(...Object.values(env))
    return { env, live, run }
  }

  it('asks the configured storage of a single-storage job and shows both SD rates', async () => {
    const { env, run } = makePage({
      job: { id: 1130, name: 'some-job', client: 'c1' },
      rawCall: async command => (command.startsWith('status client=') ? fdText(303, '49,000,000') : sdText(303, '49,185,099')),
    })
    await run()
    expect(env.director.rawCall).toHaveBeenCalledWith('status storage="Cfg-sd"')
    expect(env.sdRunning.value).toMatchObject({ aveBytesPerSec: 16395033, lastBytesPerSec: 16395033 })
  })

  it('clears the SD sample when the daemon no longer lists the job and keeps the last numbers', async () => {
    let phase = 1
    const { env, live, run } = makePage({
      job: { id: 1130, name: 'some-job', client: 'c1' },
      rawCall: async command => {
        if (command.startsWith('status client=')) return phase === 1 ? fdText(347, '232,870,000') : noJobFd
        return phase === 1 ? sdText(300, '200,000,000') : noJobSd
      },
    })
    await run()
    expect(env.sdRunning.value).not.toBeNull()
    phase = 2
    await run()
    expect(env.sdRunning.value).toBeNull()
    expect(env.fdRunning.value).toBeNull()
    expect(displayProgress({ job: { files: 0, bytes: 0 }, running: true, fd: env.fdRunning.value, sd: env.sdRunning.value, last: live.last }))
      .toEqual({ files: 347, bytes: 232870000 })
  })

  it('asks no Storage Daemon before the job log shows a connect, then asks the configured one', async () => {
    const { env, run } = makePage({
      job: { id: 1130, name: 'some-job', client: 'c1' },
      log: [{ logtext: ' 2026-10-19 08:40:00 dir JobId 1130: Storage group: 2 candidates, using "SD1" (Job, policy ListedOrder).\n' }],
      defaults: { defaults: { storage: 'SD1' } },
      rawCall: async command => (command.startsWith('status client=') ? fdText(5, '50') : sdText(5, '50')),
    })
    await run()
    expect(env.director.rawCall.mock.calls.map(([command]) => command).some(command => command.startsWith('status storage='))).toBe(false)
    env.rawJobLog.value = connectedLog
    await run()
    expect(env.director.rawCall).toHaveBeenCalledWith('status storage="SD1"')
  })

  it('shows the last live numbers, not the lagging SD counts, when the FD sample is gone', async () => {
    let phase = 1
    const { env, live, run } = makePage({
      job: { id: 1130, name: 'some-job', client: 'c1' },
      rawCall: async command => {
        if (command.startsWith('status client=')) return phase === 1 ? fdText(347, '232,870,000') : noJobFd
        return sdText(320, '80,000,000')
      },
    })
    await run()
    phase = 2
    await run()
    expect(env.fdRunning.value).toBeNull()
    expect(env.sdRunning.value).toMatchObject({ files: 320 })
    expect(displayProgress({ job: { files: 0, bytes: 0 }, running: true, fd: env.fdRunning.value, sd: env.sdRunning.value, last: live.last }))
      .toEqual({ files: 347, bytes: 232870000 })
  })

  it('does not ask a member that refused the job after a failover', async () => {
    const { env, run } = makePage({
      job: { id: 1130, name: 'some-job', client: 'c1' },
      events: { selected: 'SD1', failedOverTo: 'SD2', attempts: [{ storage: 'SD1', outcome: 'refused' }, { storage: 'SD2', outcome: 'used' }] },
      defaults: { defaults: { storage: 'SD1' } },
      rawCall: async command => (command.startsWith('status client=') ? fdText(5, '50') : sdText(5, '50')),
    })
    await run()
    const asked = env.director.rawCall.mock.calls.map(([command]) => command).filter(command => command.startsWith('status storage='))
    expect(asked).toEqual(['status storage="SD2"'])
  })
})
