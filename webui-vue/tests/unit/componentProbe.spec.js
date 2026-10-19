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

import { afterEach, describe, expect, it, vi } from 'vitest'
import { parse } from '@babel/parser'
import componentsPage from '../../src/pages/ComponentsPage.vue?raw'
import dashboardPage from '../../src/pages/DashboardPage.vue?raw'
import clientFixture from './fixtures/show-clients.json'
import {
  groupStorageDaemons,
  probeComponents,
  extractStorageList,
  extractClientList,
} from '../../src/utils/componentProbe.js'

describe('extractClientList', () => {
  it('extracts client list from real fixture shape { clients: { fd1: { name: "fd1", ... } } }', () => {
    const list = extractClientList(clientFixture)
    expect(list).toHaveLength(1)
    expect(list[0].name).toBe('fd1')
    expect(list[0].address).toBe('127.0.0.1')
    expect(list[0].version).toBe('25.1.1')
  })

  it('handles result.clients and arrays', () => {
    const arr = [{ name: 'ClientA', address: 'client1' }]
    expect(extractClientList({ result: { clients: arr } })).toEqual(arr)
    expect(extractClientList(arr)).toEqual(arr)
    expect(extractClientList(null)).toEqual([])
  })
})

describe('extractStorageList', () => {
  it('handles object dictionary of storages', () => {
    const raw = {
      StorageA: { address: 'sd1.example', sdport: 9103 },
      StorageB: { address: 'sd2.example', sdport: 9103 },
    }
    const list = extractStorageList(raw)
    expect(list).toEqual([
      { name: 'StorageA', address: 'sd1.example', sdport: 9103 },
      { name: 'StorageB', address: 'sd2.example', sdport: 9103 },
    ])
  })

  it('handles result.storages and arrays', () => {
    const arr = [{ name: 'S1', address: 'sd1' }]
    expect(extractStorageList({ result: { storages: arr } })).toEqual(arr)
    expect(extractStorageList(arr)).toEqual(arr)
    expect(extractStorageList(null)).toEqual([])
  })
})

describe('groupStorageDaemons', () => {
  it('groups multiple storages pointing to the same address:port', () => {
    const storages = {
      Storage1a: {
        name: 'Storage1a',
        address: 'sd-host-1',
        sdport: 9103,
        device: ['Device1a', 'Device1b'],
      },
      Storage1b: {
        name: 'Storage1b',
        address: 'sd-host-1',
        sdport: 9103,
        device: ['Device1a', 'Device1c'],
      },
      Storage2: {
        name: 'Storage2',
        address: 'sd-host-2',
        port: 9105,
        device: 'Device2',
      },
    }

    const daemons = groupStorageDaemons(storages)
    expect(daemons).toHaveLength(2)

    const d1 = daemons.find((d) => d.key === 'sd-host-1:9103')
    expect(d1).toBeDefined()
    expect(d1.address).toBe('sd-host-1')
    expect(d1.port).toBe(9103)
    expect(d1.storages).toHaveLength(2)
    expect(d1.firstStorageName).toBe('Storage1a')
    expect(d1.deviceCount).toBe(3) // Device1a, Device1b, Device1c

    const d2 = daemons.find((d) => d.key === 'sd-host-2:9105')
    expect(d2).toBeDefined()
    expect(d2.address).toBe('sd-host-2')
    expect(d2.port).toBe(9105)
    expect(d2.storages).toHaveLength(1)
    expect(d2.firstStorageName).toBe('Storage2')
    expect(d2.deviceCount).toBe(1)
  })

  it('handles empty or invalid storages input cleanly', () => {
    expect(groupStorageDaemons(null)).toEqual([])
    expect(groupStorageDaemons({})).toEqual([])
    expect(groupStorageDaemons([])).toEqual([])
  })
})

describe('probeComponents', () => {
  afterEach(() => vi.useRealTimers())
  const dirBanner = `1000 OK: my-dir Version: 25.1.1 (14 September 2025)
Daemon started 19-Oct-26 00:43. Jobs: run=12, running=2
`
  const sdBanner = `1000 OK: my-sd Version: 25.1.1 (14 September 2025)
Daemon started 19-Oct-26 00:44. Jobs: run=5, running=1
Device "StorageDevice" (/var/bareos/storage) is not open.
`
  const fdBanner = `1000 OK: my-fd Version: 25.1.1 (14 September 2025)
Daemon started 19-Oct-26 00:44. Jobs: run=3 running=0
`

  it.each([
    ['Components', componentsPage, 'probeAll'],
    ['Dashboard', dashboardPage, 'runComponentsProbe'],
  ])('%s applies cards before the pending storage call ends', async (name, source, handlerName) => {
    vi.useFakeTimers()
    const script = source.split('<script setup>')[1].split('</script>')[0]
    const node = parse(script, { sourceType: 'module' }).program.body.find(node =>
      node.type === 'FunctionDeclaration' && node.id.name === handlerName)
    const environment = {
      probing: { value: false }, probingComponents: { value: false },
      rawStorages: { value: [{ name: 'Stopped' }] }, rawClients: { value: [{ name: 'client' }] },
      componentStorages: { value: [{ name: 'Stopped' }] }, componentClients: { value: [{ name: 'client' }] },
      probeResults: { value: { director: null, storages: {}, clients: {} } },
      componentProbeResults: { value: [] }, pageError: { value: null },
      auth: { user: { director: 'dirA' } }, $q: { notify: vi.fn() }, loadComponentResources: vi.fn(), probeComponents,
      director: { rawCall: command => command.startsWith('status storage=') ? new Promise(() => {})
        : Promise.resolve(command === 'status director' ? dirBanner : fdBanner) },
    }
    const handler = new Function(...Object.keys(environment), script.slice(node.start, node.end) + `; return ${handlerName}`)(...Object.values(environment))
    const pending = handler()
    await vi.advanceTimersByTimeAsync(0)
    if (name === 'Components') {
      expect(environment.probeResults.value.director.reachable).toBe(true)
      expect(environment.probeResults.value.clients.client.reachable).toBe(true)
      expect(environment.probeResults.value.storages).toEqual({})
    } else {
      expect(environment.componentProbeResults.value.map(result => result.kind)).toEqual(['director', 'client'])
    }
    await vi.advanceTimersByTimeAsync(20_000)
    await pending
    const storage = name === 'Components'
      ? Object.values(environment.probeResults.value.storages)[0]
      : environment.componentProbeResults.value.find(result => result.kind === 'storage')
    expect(storage).toMatchObject({ reachable: false, error: 'No answer within 20 s' })
    expect(environment.probing.value).toBe(false)
    expect(environment.probingComponents.value).toBe(false)
    expect(environment.pageError.value).toBe(null)
    expect(environment.$q.notify).not.toHaveBeenCalled()
  })

  it('publishes each result before a stopped SD times out, then moves to the next SD and ends', async () => {
    vi.useFakeTimers()
    let resolveStopped
    const stopped = new Promise(resolve => { resolveStopped = resolve })
    const onResult = vi.fn()
    const director = { rawCall: vi.fn(command => {
      if (command === 'status director') return Promise.resolve(dirBanner)
      if (command.startsWith('status client=')) return Promise.resolve(fdBanner)
      if (command === 'status storage="Stopped"') return stopped
      return Promise.resolve(sdBanner)
    }) }
    const pending = probeComponents(director, {
      clients: [{ name: 'client' }],
      storages: [{ name: 'Stopped', address: 'stopped' }, { name: 'Healthy', address: 'healthy' }],
      onResult,
    })
    await vi.advanceTimersByTimeAsync(0)
    expect(onResult.mock.calls.map(([result]) => result.kind)).toEqual(['director', 'client'])
    expect(director.rawCall.mock.calls.map(([command]) => command)).toEqual([
      'status director', 'status client="client"', 'status storage="Stopped"',
    ])
    await vi.advanceTimersByTimeAsync(19_999)
    expect(onResult).toHaveBeenCalledTimes(2)
    await vi.advanceTimersByTimeAsync(1)
    const results = await pending
    expect(results).toHaveLength(4)
    expect(results[2]).toMatchObject({ storageName: 'Stopped', reachable: false, error: 'No answer within 20 s' })
    expect(results[3]).toMatchObject({ storageName: 'Healthy', reachable: true })
    expect(onResult.mock.calls.map(([result]) => result)).toEqual(results)
    expect(vi.getTimerCount()).toBe(0)
    resolveStopped(sdBanner)
    await vi.advanceTimersByTimeAsync(0)
    expect(onResult).toHaveBeenCalledTimes(4)
    expect(results[2].reachable).toBe(false)
  })

  it.each(['status director', 'status client="client"'])('limits a pending %s before moving on', async blockedCommand => {
    vi.useFakeTimers()
    const pending = probeComponents({ rawCall: command => command === blockedCommand
      ? new Promise(() => {}) : Promise.resolve(fdBanner) }, {
      clients: [{ name: 'client' }], storages: [{ name: 'storage' }],
    })
    await vi.advanceTimersByTimeAsync(20_000)
    const results = await pending
    expect(results).toHaveLength(3)
    expect(results.find(result => result.error === 'No answer within 20 s')).toBeDefined()
    expect(vi.getTimerCount()).toBe(0)
  })

  it('sends one status storage for repeated probes of a silent SD and sends again after it settles', async () => {
    vi.useFakeTimers()
    let settle
    const silent = new Promise(resolve => { settle = resolve })
    const director = { rawCall: vi.fn(() => silent) }
    const options = { probeDirector: false, storages: [{ name: 'Stopped', address: 'stopped' }] }
    const first = probeComponents(director, options)
    await vi.advanceTimersByTimeAsync(20_000)
    expect((await first)[0].error).toBe('No answer within 20 s')
    const later = []
    for (let i = 0; i < 5; i++) {
      const run = probeComponents(director, options)
      await vi.advanceTimersByTimeAsync(20_000)
      later.push((await run)[0])
    }
    expect(director.rawCall).toHaveBeenCalledTimes(1)
    expect(later.every(r => r.reachable === false && r.error === 'Previous probe is still waiting for an answer')).toBe(true)
    settle(sdBanner)
    await vi.advanceTimersByTimeAsync(0)
    const after = probeComponents(director, options)
    await vi.advanceTimersByTimeAsync(0)
    expect(director.rawCall).toHaveBeenCalledTimes(2)
    await after
  })

  it('lets a new probe take the answer of the pending call within its own limit', async () => {
    vi.useFakeTimers()
    let settle
    const silent = new Promise(resolve => { settle = resolve })
    const director = { rawCall: vi.fn(() => silent) }
    const options = { probeDirector: false, storages: [{ name: 'Slow', address: 'slow' }] }
    const first = probeComponents(director, options)
    await vi.advanceTimersByTimeAsync(20_000)
    await first
    const second = probeComponents(director, options)
    await vi.advanceTimersByTimeAsync(5_000)
    settle(sdBanner)
    const results = await second
    expect(director.rawCall).toHaveBeenCalledTimes(1)
    expect(results[0]).toMatchObject({ reachable: true })
  })

  it('does not reuse a pending call made under another Director name on the same store object', async () => {
    vi.useFakeTimers()
    let settleA
    const answerA = new Promise(resolve => { settleA = resolve })
    const store = { rawCall: vi.fn(() => (store.current === 'dirA' ? answerA : Promise.resolve(sdBanner))) }
    const base = { probeDirector: false, storages: [{ name: 'File', address: 'sd' }] }
    store.current = 'dirA'
    const first = probeComponents(store, { ...base, directorName: 'dirA' })
    await vi.advanceTimersByTimeAsync(20_000)
    await first
    store.current = 'dirB'
    const second = await probeComponents(store, { ...base, directorName: 'dirB' })
    expect(store.rawCall).toHaveBeenCalledTimes(2)
    expect(second[0]).toMatchObject({ reachable: true })
    store.current = 'dirA'
    const again = probeComponents(store, { ...base, directorName: 'dirA' })
    await vi.advanceTimersByTimeAsync(20_000)
    expect((await again)[0].error).toBe('Previous probe is still waiting for an answer')
    expect(store.rawCall).toHaveBeenCalledTimes(2)
    settleA(sdBanner)
  })

  it('shows a Director failure reply that arrives just after 15 s verbatim', async () => {
    vi.useFakeTimers()
    const reply = 'Failed to connect to Client client.'
    const director = { rawCall: vi.fn(() => new Promise(resolve => setTimeout(() => resolve(reply), 15_050))) }
    const run = probeComponents(director, { probeDirector: false, clients: [{ name: 'client' }] })
    await vi.advanceTimersByTimeAsync(15_050)
    const results = await run
    expect(results[0]).toMatchObject({ kind: 'client', reachable: false, error: reply })
  })

  it('probes Director, Clients, then Storage Daemons sequentially', async () => {
    const callLog = []
    const fakeDirector = {
      rawCall: vi.fn(async (cmd) => {
        callLog.push(cmd)
        if (cmd === 'status director') return dirBanner
        if (cmd.startsWith('status storage=')) return sdBanner
        if (cmd.startsWith('status client=')) return fdBanner
        return ''
      }),
    }

    const storages = [
      {
        key: 'sd1.internal:9103',
        address: 'sd1.internal',
        port: 9103,
        firstStorageName: 'StorageAlpha',
        storages: [{ name: 'StorageAlpha' }],
      },
    ]

    const clients = [
      { name: 'client-node1', address: 'client1.internal' },
    ]

    const results = await probeComponents(fakeDirector, {
      storages,
      clients,
      probeDirector: true,
    })

    expect(results).toHaveLength(3)

    // Verify call sequence
    expect(callLog).toEqual([
      'status director',
      'status client="client-node1"',
      'status storage="StorageAlpha"',
    ])

    // Verify Director result
    expect(results[0]).toMatchObject({
      id: 'director',
      kind: 'director',
      name: 'my-dir',
      reachable: true,
      error: null,
    })
    expect(results[0].header).toEqual({
      name: 'my-dir',
      version: '25.1.1',
      started: '19-Oct-26 00:43',
      jobsRun: 12,
      jobsRunning: 2,
    })

    // Verify Storage result
    expect(results[2]).toMatchObject({
      id: 'sd1.internal:9103',
      kind: 'storage',
      key: 'sd1.internal:9103',
      storageName: 'StorageAlpha',
      reachable: true,
      error: null,
    })
    expect(results[2].header).toEqual({
      name: 'my-sd',
      version: '25.1.1',
      started: '19-Oct-26 00:44',
      jobsRun: 5,
      jobsRunning: 1,
    })

    // Verify Client result
    expect(results[1]).toMatchObject({
      id: 'client-node1',
      kind: 'client',
      name: 'client-node1',
      reachable: true,
      error: null,
    })
    expect(results[1].header).toEqual({
      name: 'my-fd',
      version: '25.1.1',
      started: '19-Oct-26 00:44',
      jobsRun: 3,
      jobsRunning: 0,
    })
  })

  it('marks component as unreachable when rawCall throws an error or fails to parse', async () => {
    const fakeDirector = {
      rawCall: vi.fn(async (cmd) => {
        if (cmd === 'status director') {
          throw new Error('Connection refused by Director')
        }
        if (cmd.startsWith('status storage=')) {
          return 'Failed to connect to Storage daemon.'
        }
        if (cmd.startsWith('status client=')) {
          throw new Error('Client connection timed out')
        }
        return ''
      }),
    }

    const storages = [{ name: 'FailedStorage', address: 'sd.down', sdport: 9103 }]
    const clients = [{ name: 'FailedClient', address: 'client.down' }]

    const results = await probeComponents(fakeDirector, {
      storages,
      clients,
      probeDirector: true,
    })

    expect(results).toHaveLength(3)

    expect(results[0]).toMatchObject({
      id: 'director',
      kind: 'director',
      reachable: false,
      header: null,
      error: 'Connection refused by Director',
    })

    expect(results[2]).toMatchObject({
      id: 'sd.down:9103',
      kind: 'storage',
      reachable: false,
      header: null,
      error: 'Failed to connect to Storage daemon.',
    })

    expect(results[1]).toMatchObject({
      id: 'FailedClient',
      kind: 'client',
      reachable: false,
      header: null,
      error: 'Client connection timed out',
    })
  })

  it('allows probeDirector=false to omit Director probing', async () => {
    const fakeDirector = {
      rawCall: vi.fn().mockResolvedValue(fdBanner),
    }

    const results = await probeComponents(fakeDirector, {
      storages: [],
      clients: [{ name: 'client-only' }],
      probeDirector: false,
    })

    expect(results).toHaveLength(1)
    expect(results[0].kind).toBe('client')
    expect(fakeDirector.rawCall).toHaveBeenCalledTimes(1)
    expect(fakeDirector.rawCall).toHaveBeenCalledWith('status client="client-only"')
  })
})
