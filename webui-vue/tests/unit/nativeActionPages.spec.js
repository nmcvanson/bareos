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

import { describe, expect, it, vi } from 'vitest'
import { ref, reactive, computed, watch } from 'vue'
import { parse } from '@babel/parser'
import volumeSource from '../../src/pages/VolumeDetailsPage.vue?raw'
import poolSource from '../../src/pages/PoolDetailsPage.vue?raw'
import storagesSource from '../../src/pages/StoragesPage.vue?raw'
import * as actions from '../../src/utils/volumeActions.js'
import { createActionTokenGuard } from '../../src/utils/actionToken.js'
import { quoteDirectorString } from '../../src/utils/directorStrings.js'
import { buildStorageEnabledCommand } from '../../src/utils/storageGroups.js'
import { actionAllowed } from '../../src/utils/acl.js'

function handler(source, name, environment) {
  const script = source.split('<script setup>')[1].split('</script>')[0]
  const node = parse(script, { sourceType: 'module' }).program.body.find(node => node.type === 'FunctionDeclaration' && node.id.name === name)
  const FunctionType = node.async ? Object.getPrototypeOf(async function () {}).constructor : Function
  return new FunctionType(...Object.keys(environment), 'argument',
    `return (${script.slice(node.start, node.end)})(argument)`)
    .bind(null, ...Object.values(environment))
}

function volumeEnvironment() {
  const volumeName = ref('volume-A')
  const auth = reactive({ user: { director: 'Director-A' } })
  const currentVolumeDirector = computed(() => auth.user.director)
  const environment = {
    ...actions, volumeName, auth, currentVolumeDirector,
    confirmGuard: createActionTokenGuard(), statusGuard: createActionTokenGuard(),
    activeDirectorName: () => auth.user.director,
    settings: { directorName: 'Director-A' },
    vol: ref({ volumename: 'volume-A' }), confirmDlg: ref({ open: false }),
    replyDlg: ref({ open: false }), actionBusy: ref(false),
    canDelete: ref(true), canPrune: ref(true), canPurge: ref(true),
    ensureVolumeDirector: vi.fn(async () => {}),
    director: { call: vi.fn(async () => ({ result: {}, messages: { info: ['Done\n'] } })) },
    router: { push: vi.fn() }, backLocation: ref({ name: 'storages' }), loadVolume: vi.fn(),
    t: text => text,
  }
  // read the scope parts lazily so a test can replace currentVolumeDirector
  environment.volumeScope = () => `${environment.volumeName.value}\u0000${environment.currentVolumeDirector.value}`
  return environment
}

function bindScopeInvalidation(environment) {
  const script = volumeSource.split('<script setup>')[1].split('</script>')[0]
  const node = parse(script, { sourceType: 'module' }).program.body.find(node =>
    node.type === 'ExpressionStatement' && node.expression.type === 'CallExpression'
      && node.expression.callee.name === 'watch' && script.slice(node.start, node.end).includes('confirmDlg.value.target = null'))
  return new Function(...Object.keys(environment), 'watch', `return ${script.slice(node.start, node.end)}`)
    (...Object.values(environment), watch)
}

describe('volume confirmation scope', () => {
  it.each(['delete', 'prune', 'purge'])('does not send %s after volume A becomes B', async action => {
    const environment = volumeEnvironment()
    handler(volumeSource, 'openConfirmDialog', environment)(action)
    environment.volumeName.value = 'volume-B'
    await handler(volumeSource, 'executeConfirmedAction', environment)()
    expect(environment.director.call).not.toHaveBeenCalled()
  })
  it('does not send after the active Director changes during the pre-command await', async () => {
    const environment = volumeEnvironment()
    let release
    environment.ensureVolumeDirector = vi.fn(() => new Promise(resolve => { release = resolve }))
    handler(volumeSource, 'openConfirmDialog', environment)('delete')
    const pending = handler(volumeSource, 'executeConfirmedAction', environment)()
    environment.auth.user.director = 'Director-B'
    release()
    await pending
    expect(environment.director.call).not.toHaveBeenCalled()
  })
  it('closes the dialog on route changes and does not reuse an old confirmation after A-B-A', async () => {
    const environment = volumeEnvironment()
    const stop = bindScopeInvalidation(environment)
    handler(volumeSource, 'openConfirmDialog', environment)('delete')
    environment.volumeName.value = 'volume-B'
    expect(environment.confirmDlg.value.open).toBe(false)
    expect(environment.confirmDlg.value.target).toBeNull()
    environment.volumeName.value = 'volume-A'
    await handler(volumeSource, 'executeConfirmedAction', environment)()
    expect(environment.director.call).not.toHaveBeenCalled()
    stop()
  })
  it('sends only the captured volume when the confirmation remains in the same scope', async () => {
    const environment = volumeEnvironment()
    handler(volumeSource, 'openConfirmDialog', environment)('prune')
    environment.vol.value.volumename = 'stale-loaded-record'
    await handler(volumeSource, 'executeConfirmedAction', environment)()
    expect(environment.director.call).toHaveBeenCalledWith('prune volume="volume-A" yes')
  })
})

const diagnostic = 'Permission denied for this action.\n'
const poolTarget = environment => environment.pruneGuard.open(
  actions.capturePoolPruneTarget('pool-A', ['volume-A'], environment.currentPoolDirector.value,
    environment.activeDirectorName()),
  environment.poolScope(), environment.activeDirectorName())
const rpcError = { code: 1, message: 'failed', data: { result: {}, messages: { error: [diagnostic] } } }
const refusals = [
  { jsonrpc: '2.0', id: null, error: rpcError },
  { error: rpcError },
  { result: { error: rpcError } },
  { messages: { error: [diagnostic] } },
]

function poolEnvironment(reply) {
  const auth = reactive({ user: { director: 'Director-A' } })
  const requested = ref('Director-A')
  const environment = {
    ...actions, quoteDirectorString, canUpdatePool: ref(true), canPrune: ref(true),
    auth, requested, settings: { locale: 'en', directorName: 'Director-A' },
    currentPoolDirector: computed(() => requested.value),
    pruneGuard: createActionTokenGuard(), updateGuard: createActionTokenGuard(),
    activeDirectorName: () => auth.user.director, pruneDialog: null, pruneToken: null,
    updatePoolLoading: ref(false), pruneSelectedLoading: ref(false), pruneSelectedDisabled: ref(false),
    selectedPrunableVolumes: ref([{ name: 'volume-A', prunablejobs: 1 }]),
    poolName: ref('pool-A'), ensurePoolDirector: vi.fn(async () => {}),
    director: { call: vi.fn(async () => reply) }, replyDlg: ref({ open: false }),
    loadPool: vi.fn(), formatNumber: value => String(value),
    $q: { notify: vi.fn(), dialog: vi.fn(() => ({ onOk: vi.fn(), hide: vi.fn() })) },
    formatBytes: value => String(value), t: text => text,
  }
  environment.poolScope = () => `${environment.poolName.value}\u0000${environment.currentPoolDirector.value}`
  return environment
}

describe('resolved Director refusals through native action page handlers', () => {
  it.each(refusals)('keeps a refused delete result visible without navigation or reload', async reply => {
    const environment = volumeEnvironment()
    environment.director.call.mockResolvedValue(reply)
    handler(volumeSource, 'openConfirmDialog', environment)('delete')
    await handler(volumeSource, 'executeConfirmedAction', environment)()
    expect(environment.replyDlg.value.open).toBe(true)
    expect(environment.replyDlg.value.text).toContain(diagnostic)
    expect(environment.router.push).not.toHaveBeenCalled()
    expect(environment.loadVolume).not.toHaveBeenCalled()
  })
  it.each(refusals)('does not refresh Volume status after a refused update', async reply => {
    const environment = { ...volumeEnvironment(), statusDlg: ref({ open: true, status: 'Full' }), canUpdate: ref(true) }
    environment.director.call.mockResolvedValue(reply)
    await handler(volumeSource, 'executeUpdateStatus', environment)()
    expect(environment.replyDlg.value.text).toContain(diagnostic)
    expect(environment.loadVolume).not.toHaveBeenCalled()
  })
  it.each(refusals)('does not announce or refresh Pool prune on refusal', async reply => {
    const environment = poolEnvironment(reply)
    await handler(poolSource, 'pruneSelectedVolumes', environment)(poolTarget(environment))
    expect(environment.$q.notify).not.toHaveBeenCalledWith(expect.objectContaining({ type: 'positive' }))
    expect(environment.loadPool).not.toHaveBeenCalled()
    expect(environment.replyDlg.value.text).toContain(diagnostic)
  })
  it.each(refusals)('does not refresh Pool after a refused configuration update', async reply => {
    const environment = poolEnvironment(reply)
    await handler(poolSource, 'updatePoolFromConfig', environment)()
    expect(environment.loadPool).not.toHaveBeenCalled()
    expect(environment.replyDlg.value.text).toContain(diagnostic)
  })
  it.each(refusals)('does not refresh runtime Storage after a refused enable', async reply => {
    const environment = {
      ...actions, buildStorageEnabledCommand, canDisable: ref(true), canEnable: ref(true),
      storageActionBusy: ref(false), switchToRowDirector: vi.fn(async () => {}),
      director: { call: vi.fn(async () => reply) }, replyDlg: ref({ open: false }), refresh: vi.fn(), t: text => text,
    }
    await handler(storagesSource, 'toggleStorageEnabled', environment)({ name: 'archive-storage', enabled: false })
    expect(environment.refresh).not.toHaveBeenCalled()
    expect(environment.replyDlg.value.text).toContain(diagnostic)
  })
  it('retains a successful delete reply until acknowledged, then navigates once', async () => {
    const environment = volumeEnvironment()
    handler(volumeSource, 'openConfirmDialog', environment)('delete')
    await handler(volumeSource, 'executeConfirmedAction', environment)()
    expect(environment.replyDlg.value.open).toBe(true)
    expect(environment.router.push).not.toHaveBeenCalled()
    await handler(volumeSource, 'closeReplyDialog', environment)()
    expect(environment.replyDlg.value.open).toBe(false)
    expect(environment.router.push).toHaveBeenCalledTimes(1)
    await handler(volumeSource, 'closeReplyDialog', environment)()
    expect(environment.router.push).toHaveBeenCalledTimes(1)
  })
  it('acknowledges a refused delete without navigating and does not navigate a successful stale result', async () => {
    for (const reply of [refusals[0], { result: {} }]) {
      const environment = volumeEnvironment()
      environment.director.call.mockResolvedValue(reply)
      handler(volumeSource, 'openConfirmDialog', environment)('delete')
      await handler(volumeSource, 'executeConfirmedAction', environment)()
      if (!reply.error) environment.volumeName.value = 'volume-B'
      await handler(volumeSource, 'closeReplyDialog', environment)()
      expect(environment.router.push).not.toHaveBeenCalled()
    }
  })
  it('refreshes and announces only a checked successful Pool prune', async () => {
    const environment = poolEnvironment({ result: {}, messages: { info: ['Pruned.\n'] } })
    await handler(poolSource, 'pruneSelectedVolumes', environment)(poolTarget(environment))
    expect(environment.$q.notify).toHaveBeenCalledWith(expect.objectContaining({ type: 'positive' }))
    expect(environment.loadPool).toHaveBeenCalledTimes(1)
    expect(environment.replyDlg.value.text).toBe('Pruned.\n')
  })
})

describe('Pool prune command ACL', () => {
  it('disables the real button expression for a console with visible candidates but no prune permission', () => {
    const environment = poolEnvironment({ result: {} })
    const acl = { commands: {} }
    environment.canPrune = computed(() => actionAllowed(acl.commands, 'prune'))
    const script = poolSource.split('<script setup>')[1].split('</script>')[0]
    const nodes = parse(script, { sourceType: 'module' }).program.body
    const declaration = nodes.filter(node => node.type === 'VariableDeclaration')
      .flatMap(node => node.declarations).find(node => node.id.name === 'pruneSelectedDisabled')
    const body = declaration.init.arguments[0].body
    const disabled = new Function(...Object.keys(environment), `return (${script.slice(body.start, body.end)})`)
      (...Object.values(environment))
    expect(environment.selectedPrunableVolumes.value).toHaveLength(1)
    expect(disabled).toBe(true)
  })
  it('refuses both dialog opening and execution even if a stale disabled ref says enabled', async () => {
    const environment = poolEnvironment({ result: {} })
    environment.canPrune.value = false
    environment.pruneSelectedDisabled.value = false
    handler(poolSource, 'confirmPruneSelectedVolumes', environment)()
    await handler(poolSource, 'pruneSelectedVolumes', environment)(poolTarget(environment))
    expect(environment.$q.dialog).not.toHaveBeenCalled()
    expect(environment.director.call).not.toHaveBeenCalled()
    expect(environment.$q.notify).not.toHaveBeenCalled()
  })
  it('rechecks permission after the Director preflight await', async () => {
    const environment = poolEnvironment({ result: {} })
    let release
    environment.ensurePoolDirector = vi.fn(() => new Promise(resolve => { release = resolve }))
    const pending = handler(poolSource, 'pruneSelectedVolumes', environment)(poolTarget(environment))
    environment.canPrune.value = false
    release()
    await pending
    expect(environment.director.call).not.toHaveBeenCalled()
  })
})

describe('volume actions survive the page switching to the target Director', () => {
  const crossDirector = () => {
    const environment = volumeEnvironment()
    environment.currentVolumeDirector = ref('Director-B')      // the route names Director B
    environment.ensureVolumeDirector = vi.fn(async () => { environment.auth.user.director = 'Director-B' })
    return environment
  }
  const deferred = () => {
    let resolve
    const promise = new Promise(done => { resolve = done })
    return { promise, resolve }
  }
  it('sends one delete to Director B while B is active after the page itself switches, then navigates', async () => {
    const environment = crossDirector()
    const stop = bindScopeInvalidation(environment)
    handler(volumeSource, 'openConfirmDialog', environment)('delete')
    await handler(volumeSource, 'executeConfirmedAction', environment)()
    expect(environment.director.call).toHaveBeenCalledTimes(1)
    expect(environment.director.call).toHaveBeenCalledWith('delete volume="volume-A" yes')
    expect(environment.auth.user.director).toBe('Director-B')
    await handler(volumeSource, 'closeReplyDialog', environment)()
    expect(environment.router.push).toHaveBeenCalledTimes(1)
    stop()
  })
  it('sends nothing when the active Director returns to A while the ACL refresh is pending, also after B again', async () => {
    for (const comeBack of [false, true]) {
      const environment = crossDirector()
      const stop = bindScopeInvalidation(environment)
      const refresh = deferred()
      environment.ensureVolumeDirector = vi.fn(async () => {
        environment.auth.user.director = 'Director-B'
        await refresh.promise
      })
      handler(volumeSource, 'openConfirmDialog', environment)('delete')
      const pending = handler(volumeSource, 'executeConfirmedAction', environment)()
      environment.auth.user.director = 'Director-A'
      if (comeBack) environment.auth.user.director = 'Director-B'
      refresh.resolve()
      await pending
      expect(environment.director.call).not.toHaveBeenCalled()
      stop()
    }
  })
  it('invalidates for good on a third Director: zero calls, also after returning to A or B', async () => {
    const environment = crossDirector()
    const stop = bindScopeInvalidation(environment)
    handler(volumeSource, 'openConfirmDialog', environment)('purge')
    environment.auth.user.director = 'Director-C'
    expect(environment.confirmDlg.value.open).toBe(false)
    for (const back of ['Director-A', 'Director-B']) {
      environment.auth.user.director = back
      await handler(volumeSource, 'executeConfirmedAction', environment)()
      expect(environment.director.call).not.toHaveBeenCalled()
    }
    // a late callback that still holds the old token sends nothing either
    environment.confirmDlg.value = { ...environment.confirmDlg.value, open: true }
    await handler(volumeSource, 'executeConfirmedAction', environment)()
    expect(environment.director.call).not.toHaveBeenCalled()
    stop()
  })
  it('a confirmation can be used once', async () => {
    const environment = volumeEnvironment()
    handler(volumeSource, 'openConfirmDialog', environment)('prune')
    await handler(volumeSource, 'executeConfirmedAction', environment)()
    environment.confirmDlg.value.open = true
    await handler(volumeSource, 'executeConfirmedAction', environment)()
    expect(environment.director.call).toHaveBeenCalledTimes(1)
  })
  it('update status follows the same rule: route A-B-A during the await sends nothing, a clean run sends one', async () => {
    const environment = { ...volumeEnvironment(), statusDlg: ref({ open: true, status: 'Disabled' }), canUpdate: ref(true) }
    const stop = bindScopeInvalidation(environment)
    const gate = deferred()
    environment.ensureVolumeDirector = vi.fn(() => gate.promise)
    const pending = handler(volumeSource, 'executeUpdateStatus', environment)()
    environment.volumeName.value = 'volume-B'
    environment.volumeName.value = 'volume-A'
    gate.resolve()
    await pending
    expect(environment.director.call).not.toHaveBeenCalled()
    stop()

    const denied = { ...volumeEnvironment(), statusDlg: ref({ open: true, status: 'Disabled' }), canUpdate: ref(false) }
    await handler(volumeSource, 'executeUpdateStatus', denied)()
    expect(denied.director.call).not.toHaveBeenCalled()

    const clean = { ...volumeEnvironment(), statusDlg: ref({ open: true, status: 'Full' }), canUpdate: ref(true) }
    await handler(volumeSource, 'executeUpdateStatus', clean)()
    expect(clean.director.call).toHaveBeenCalledTimes(1)
    expect(clean.director.call).toHaveBeenCalledWith('update volume="volume-A" volstatus=Full')
  })
})

describe('Pool actions are one-way tokens bound to the pool and Director they were opened for', () => {
  const deferred = () => {
    let resolve
    const promise = new Promise(done => { resolve = done })
    return { promise, resolve }
  }
  // Runs the page's own opener and returns the OK callback, with the real watcher bound.
  const open = environment => {
    let ok
    environment.$q.dialog = vi.fn(() => ({ onOk: callback => { ok = callback }, hide: vi.fn() }))
    environment.pruneSelectedVolumes = argument => handler(poolSource, 'pruneSelectedVolumes', environment)(argument)
    environment.selectedPrunableVolumes.value = [{ name: 'volume-A', prunablejobs: 1 }]
    handler(poolSource, 'confirmPruneSelectedVolumes', environment)()
    expect(environment.$q.dialog).toHaveBeenCalledTimes(1)
    return ok
  }
  const bindWatcher = environment => {
    const script = poolSource.split('<script setup>')[1].split('</script>')[0]
    const nodes = parse(script, { sourceType: 'module' }).program.body
    const start = nodes.find(node => node.type === 'VariableDeclaration'
      && script.slice(node.start, node.end).startsWith('let pruneDialog'))
    const watcher = nodes.find(node => node.type === 'ExpressionStatement'
      && script.slice(node.start, node.end).includes('pruneDialog?.hide?.()'))
    const scope = Object.fromEntries(Object.entries(environment)
      .filter(([key]) => !['pruneDialog', 'pruneToken'].includes(key)))
    return new Function(...Object.keys(scope), 'watch',
      `${script.slice(start.start, watcher.end)}\nreturn { set(d, t) { pruneDialog = d; pruneToken = t } }`)
      (...Object.values(scope), watch)
  }
  it('sends the captured names once when the page switches to the pool Director', async () => {
    const environment = poolEnvironment({ result: {}, messages: { info: ['Pruned.\n'] } })
    environment.requested.value = 'Director-B'
    environment.ensurePoolDirector = vi.fn(async () => { environment.auth.user.director = 'Director-B' })
    const ok = open(environment)
    environment.selectedPrunableVolumes.value = [{ name: 'other-volume', prunablejobs: 1 }]  // changed after opening
    await ok()
    expect(environment.director.call).toHaveBeenCalledTimes(1)
    expect(environment.director.call).toHaveBeenCalledWith('prune volume pool="pool-A" volume="volume-A" yes')
  })
  it('sends nothing after Pool A, B, A or Director A, C, A while the preflight is pending', async () => {
    for (const change of [
      environment => { environment.poolName.value = 'pool-B'; environment.poolName.value = 'pool-A' },
      environment => { environment.auth.user.director = 'Director-C'; environment.auth.user.director = 'Director-A' },
      environment => { environment.requested.value = 'Director-C'; environment.requested.value = 'Director-A' },
    ]) {
      const environment = poolEnvironment({ result: {} })
      bindWatcher(environment)
      const gate = deferred()
      environment.ensurePoolDirector = vi.fn(() => gate.promise)
      const ok = open(environment)
      const pending = ok()
      change(environment)
      gate.resolve()
      await pending
      expect(environment.director.call).not.toHaveBeenCalled()
    }
  })
  it('a cancelled prune does not resume when the old values return, and a late OK sends nothing', async () => {
    const environment = poolEnvironment({ result: {} })
    bindWatcher(environment)
    const ok = open(environment)
    environment.auth.user.director = 'Director-C'
    environment.auth.user.director = 'Director-A'
    await ok()
    expect(environment.director.call).not.toHaveBeenCalled()
  })
  it('closes the pending prune dialog on a pool or third-Director change but not on the self-switch', () => {
    for (const [change, closed] of [
      [environment => { environment.requested.value = 'Director-B'; environment.auth.user.director = 'Director-B' }, false],
      [environment => { environment.auth.user.director = 'Director-C' }, true],
      [environment => { environment.poolName.value = 'pool-B' }, true],
    ]) {
      const environment = poolEnvironment({ result: {} })
      environment.requested.value = 'Director-B'
      const scope = bindWatcher(environment)
      const dialog = { hide: vi.fn() }
      const token = environment.pruneGuard.open(
        actions.capturePoolPruneTarget('pool-A', ['volume-A'], 'Director-B', 'Director-A'),
        environment.poolScope(), environment.activeDirectorName())
      scope.set(dialog, token)
      change(environment)
      expect(dialog.hide).toHaveBeenCalledTimes(closed ? 1 : 0)
    }
  })
  it('update pool follows the same rule: Director A, C, A during the await sends nothing, a self-switch sends one', async () => {
    const moved = poolEnvironment({ result: {} })
    bindWatcher(moved)
    const gate = deferred()
    moved.ensurePoolDirector = vi.fn(() => gate.promise)
    const pending = handler(poolSource, 'updatePoolFromConfig', moved)()
    moved.auth.user.director = 'Director-C'
    moved.auth.user.director = 'Director-A'
    gate.resolve()
    await pending
    expect(moved.director.call).not.toHaveBeenCalled()

    const own = poolEnvironment({ result: {} })
    bindWatcher(own)
    own.requested.value = 'Director-B'
    own.ensurePoolDirector = vi.fn(async () => { own.auth.user.director = 'Director-B' })
    await handler(poolSource, 'updatePoolFromConfig', own)()
    expect(own.director.call).toHaveBeenCalledTimes(1)
    expect(own.director.call).toHaveBeenCalledWith('update pool="pool-A"')
  })
})

describe('command permissions follow a Director switch made by the page', () => {
  const environment = switched => ({
    requestedDirector: ref('Director-B'), auth: { user: { director: 'Director-A' } },
    director: { isConnected: true }, switchActiveDirector: vi.fn(async () => switched),
    acl: { refresh: vi.fn(async () => {}) },
  })
  it.each([['pool', poolSource, 'ensurePoolDirector'], ['volume', volumeSource, 'ensureVolumeDirector']])(
    'reloads the %s page command list only after a real switch', async (_name, source, name) => {
      const switched = environment(true)
      await handler(source, name, switched)()
      expect(switched.acl.refresh).toHaveBeenCalledTimes(1)
      const unchanged = environment(false)
      await handler(source, name, unchanged)()
      expect(unchanged.acl.refresh).not.toHaveBeenCalled()
    })
})
