<!--
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
-->
<template>
  <q-page class="q-pa-md">
    <!-- Top toolbar with title, interval select, and Probe now button -->
    <div class="row items-center justify-between q-mb-md">
      <div class="row items-center q-gutter-x-sm">
        <q-icon name="dns" size="md" color="primary" />
        <span class="text-h5 text-weight-medium" data-testid="components-overview-title">
          {{ t('Components Overview') }}
        </span>
      </div>

      <div class="row items-center q-gutter-x-sm">
        <q-select
          v-model="pollInterval"
          :options="intervalOptions"
          emit-value
          map-options
          dense
          outlined
          options-dense
          style="min-width: 120px"
          :label="t('Poll interval')"
          data-testid="probe-interval-select"
        />
        <q-btn
          color="primary"
          icon="sensors"
          :label="t('Probe now')"
          :loading="probing"
          data-testid="probe-now-btn"
          @click="probeAll"
        />
        <q-btn
          flat
          round
          dense
          icon="refresh"
          :loading="loadingResources"
          data-testid="refresh-resources-btn"
          @click="fetchResources"
        >
          <q-tooltip>{{ t('Reload resource lists') }}</q-tooltip>
        </q-btn>
      </div>
    </div>

    <q-banner v-if="pageError" dense class="bg-negative text-white rounded-borders q-mb-md">
      {{ pageError }}
    </q-banner>

    <!-- SECTION 1: Director -->
    <div class="q-mb-lg">
      <div class="row items-center q-gutter-x-xs q-mb-sm text-subtitle1 text-weight-bold text-grey-8">
        <q-icon name="settings" size="sm" />
        <span>{{ t('Director') }}</span>
      </div>

      <div class="row q-col-gutter-md">
        <div class="col-12 col-md-6 col-lg-4">
          <q-card flat bordered class="bareos-panel" data-testid="director-component-card">
            <q-card-section class="panel-header row items-center justify-between">
              <div class="row items-center q-gutter-x-xs">
                <q-icon name="dns" size="xs" />
                <span class="text-weight-bold" data-testid="director-name">
                  {{ directorProbe?.name || directorName }}
                </span>
              </div>
              <div>
                <q-chip
                  v-if="!directorProbe"
                  size="sm"
                  color="grey-6"
                  text-color="white"
                  icon="help_outline"
                  data-testid="director-status-chip"
                >
                  {{ t('Not probed') }}
                </q-chip>
                <q-chip
                  v-else-if="directorProbe.reachable"
                  size="sm"
                  color="positive"
                  text-color="white"
                  icon="check_circle"
                  data-testid="director-status-chip"
                >
                  {{ t('Reachable') }}
                </q-chip>
                <q-chip
                  v-else
                  size="sm"
                  color="negative"
                  text-color="white"
                  icon="error"
                  data-testid="director-status-chip"
                >
                  {{ t('Down') }}
                </q-chip>
              </div>
            </q-card-section>

            <q-card-section class="q-pa-md text-body2">
              <template v-if="directorProbe?.reachable">
                <div class="row q-col-gutter-xs q-mb-xs">
                  <div class="col-4 text-grey-7">{{ t('Version') }}:</div>
                  <div class="col-8 text-weight-medium" data-testid="director-version">
                    {{ directorProbe.version || '—' }}
                  </div>
                </div>
                <div class="row q-col-gutter-xs q-mb-xs">
                  <div class="col-4 text-grey-7">{{ t('Started') }}:</div>
                  <div class="col-8" data-testid="director-started">
                    {{ directorProbe.started || '—' }}
                  </div>
                </div>
                <div class="row q-col-gutter-xs q-mb-xs">
                  <div class="col-4 text-grey-7">{{ t('Jobs run') }}:</div>
                  <div class="col-8" data-testid="director-jobs-run">
                    {{ directorProbe.jobsRun ?? '—' }}
                  </div>
                </div>
                <div class="row q-col-gutter-xs">
                  <div class="col-4 text-grey-7">{{ t('Jobs running') }}:</div>
                  <div class="col-8" data-testid="director-jobs-running">
                    {{ directorProbe.jobsRunning ?? '—' }}
                  </div>
                </div>
              </template>
              <template v-else-if="directorProbe && !directorProbe.reachable">
                <div class="text-negative" data-testid="director-error">
                  {{ directorProbe.error }}
                </div>
              </template>
              <template v-else>
                <div class="text-grey-6 italic">
                  {{ t('Click "Probe now" to test Director reachability and status.') }}
                </div>
              </template>
            </q-card-section>

            <q-separator />

            <q-card-actions align="right">
              <q-btn
                flat
                dense
                color="primary"
                :to="{ name: 'director' }"
                :label="t('Director details')"
                data-testid="director-details-link"
              />
            </q-card-actions>
          </q-card>
        </div>
      </div>
    </div>

    <!-- SECTION 2: Storage Daemons -->
    <div class="q-mb-lg">
      <div class="row items-center q-gutter-x-xs q-mb-sm text-subtitle1 text-weight-bold text-grey-8">
        <q-icon name="storage" size="sm" />
        <span>{{ t('Storage Daemons') }}</span>
      </div>

      <div v-if="loadingResources && storageDaemons.length === 0" class="text-grey-6 q-pa-sm">
        {{ t('Loading storage daemons…') }}
      </div>
      <div v-else-if="storageDaemons.length === 0" class="text-grey-6 q-pa-sm">
        {{ t('No storage daemons configured.') }}
      </div>
      <div v-else class="row q-col-gutter-md">
        <div
          v-for="sd in storageDaemons"
          :key="sd.key"
          class="col-12 col-md-6 col-lg-4"
        >
          <q-card flat bordered class="bareos-panel" :data-testid="'sd-card-' + sd.key">
            <q-card-section class="panel-header row items-center justify-between">
              <div class="row items-center q-gutter-x-xs ellipsis">
                <q-icon name="dns" size="xs" />
                <span class="text-weight-bold" :data-testid="'sd-name-' + sd.key">
                  {{ probeResults.storages[sd.key]?.name ? `${probeResults.storages[sd.key].name} (${sd.key})` : sd.key }}
                </span>
              </div>
              <div>
                <q-chip
                  v-if="!probeResults.storages[sd.key]"
                  size="sm"
                  color="grey-6"
                  text-color="white"
                  icon="help_outline"
                  :data-testid="'sd-status-chip-' + sd.key"
                >
                  {{ t('Not probed') }}
                </q-chip>
                <q-chip
                  v-else-if="probeResults.storages[sd.key].reachable"
                  size="sm"
                  color="positive"
                  text-color="white"
                  icon="check_circle"
                  :data-testid="'sd-status-chip-' + sd.key"
                >
                  {{ t('Reachable') }}
                </q-chip>
                <q-chip
                  v-else
                  size="sm"
                  color="negative"
                  text-color="white"
                  icon="error"
                  :data-testid="'sd-status-chip-' + sd.key"
                >
                  {{ t('Down') }}
                </q-chip>
              </div>
            </q-card-section>

            <q-card-section class="q-pa-md text-body2">
              <div class="row q-col-gutter-xs q-mb-xs">
                <div class="col-4 text-grey-7">{{ t('Endpoint') }}:</div>
                <div class="col-8 text-weight-medium">{{ sd.key }}</div>
              </div>

              <div class="row q-col-gutter-xs q-mb-xs">
                <div class="col-4 text-grey-7">{{ t('Storages') }}:</div>
                <div class="col-8">
                  <span
                    v-for="(st, idx) in sd.storages"
                    :key="st.name"
                  >
                    <span class="text-weight-medium">{{ st.name }}</span><span v-if="idx < sd.storages.length - 1">, </span>
                  </span>
                </div>
              </div>

              <div v-if="sd.deviceCount > 0" class="row q-col-gutter-xs q-mb-xs">
                <div class="col-4 text-grey-7">{{ t('Devices') }}:</div>
                <div class="col-8">{{ sd.deviceCount }}</div>
              </div>

              <template v-if="probeResults.storages[sd.key]?.reachable">
                <q-separator class="q-my-sm" />
                <div class="row q-col-gutter-xs q-mb-xs">
                  <div class="col-4 text-grey-7">{{ t('Version') }}:</div>
                  <div class="col-8 text-weight-medium" :data-testid="'sd-version-' + sd.key">
                    {{ probeResults.storages[sd.key].version || '—' }}
                  </div>
                </div>
                <div class="row q-col-gutter-xs q-mb-xs">
                  <div class="col-4 text-grey-7">{{ t('Started') }}:</div>
                  <div class="col-8">
                    {{ probeResults.storages[sd.key].started || '—' }}
                  </div>
                </div>
                <div class="row q-col-gutter-xs q-mb-xs">
                  <div class="col-4 text-grey-7">{{ t('Jobs run') }}:</div>
                  <div class="col-8">
                    {{ probeResults.storages[sd.key].jobsRun ?? '—' }}
                  </div>
                </div>
                <div class="row q-col-gutter-xs">
                  <div class="col-4 text-grey-7">{{ t('Jobs running') }}:</div>
                  <div class="col-8">
                    {{ probeResults.storages[sd.key].jobsRunning ?? '—' }}
                  </div>
                </div>
              </template>
              <template v-else-if="probeResults.storages[sd.key] && !probeResults.storages[sd.key].reachable">
                <q-separator class="q-my-sm" />
                <div class="text-negative" :data-testid="'sd-error-' + sd.key">
                  {{ probeResults.storages[sd.key].error }}
                </div>
              </template>
            </q-card-section>

            <q-separator />

            <q-card-actions align="right">
              <q-btn
                flat
                dense
                color="primary"
                :to="{ name: 'storage-daemon', params: { storage: sd.firstStorageName } }"
                :label="t('Storage Daemon details')"
                :data-testid="'sd-link-' + sd.firstStorageName"
              />
            </q-card-actions>
          </q-card>
        </div>
      </div>
    </div>

    <!-- SECTION 3: Clients -->
    <div class="q-mb-lg">
      <div class="row items-center q-gutter-x-xs q-mb-sm text-subtitle1 text-weight-bold text-grey-8">
        <q-icon name="devices" size="sm" />
        <span>{{ t('Clients') }}</span>
      </div>

      <div v-if="loadingResources && clients.length === 0" class="text-grey-6 q-pa-sm">
        {{ t('Loading clients…') }}
      </div>
      <div v-else-if="clients.length === 0" class="text-grey-6 q-pa-sm">
        {{ t('No clients configured.') }}
      </div>
      <div v-else class="row q-col-gutter-md">
        <div
          v-for="c in clients"
          :key="c.name"
          class="col-12 col-md-6 col-lg-4"
        >
          <q-card flat bordered class="bareos-panel" :data-testid="'client-card-' + c.name">
            <q-card-section class="panel-header row items-center justify-between">
              <div class="row items-center q-gutter-x-xs ellipsis">
                <q-icon name="computer" size="xs" />
                <span class="text-weight-bold" :data-testid="'client-name-' + c.name">
                  {{ c.name }}
                </span>
              </div>
              <div>
                <q-chip
                  v-if="!probeResults.clients[c.name]"
                  size="sm"
                  color="grey-6"
                  text-color="white"
                  icon="help_outline"
                  :data-testid="'client-status-chip-' + c.name"
                >
                  {{ t('Not probed') }}
                </q-chip>
                <q-chip
                  v-else-if="probeResults.clients[c.name].reachable"
                  size="sm"
                  color="positive"
                  text-color="white"
                  icon="check_circle"
                  :data-testid="'client-status-chip-' + c.name"
                >
                  {{ t('Reachable') }}
                </q-chip>
                <q-chip
                  v-else
                  size="sm"
                  color="negative"
                  text-color="white"
                  icon="error"
                  :data-testid="'client-status-chip-' + c.name"
                >
                  {{ t('Down') }}
                </q-chip>
              </div>
            </q-card-section>

            <q-card-section class="q-pa-md text-body2">
              <div class="row q-col-gutter-xs q-mb-xs">
                <div class="col-4 text-grey-7">{{ t('Address') }}:</div>
                <div class="col-8 text-weight-medium">{{ c.address || '—' }}</div>
              </div>

              <template v-if="probeResults.clients[c.name]?.reachable">
                <q-separator class="q-my-sm" />
                <div class="row q-col-gutter-xs q-mb-xs">
                  <div class="col-4 text-grey-7">{{ t('Version') }}:</div>
                  <div class="col-8 text-weight-medium" :data-testid="'client-version-' + c.name">
                    {{ probeResults.clients[c.name].version || '—' }}
                  </div>
                </div>
                <div class="row q-col-gutter-xs q-mb-xs">
                  <div class="col-4 text-grey-7">{{ t('Started') }}:</div>
                  <div class="col-8">
                    {{ probeResults.clients[c.name].started || '—' }}
                  </div>
                </div>
                <div class="row q-col-gutter-xs q-mb-xs">
                  <div class="col-4 text-grey-7">{{ t('Jobs run') }}:</div>
                  <div class="col-8">
                    {{ probeResults.clients[c.name].jobsRun ?? '—' }}
                  </div>
                </div>
                <div class="row q-col-gutter-xs">
                  <div class="col-4 text-grey-7">{{ t('Jobs running') }}:</div>
                  <div class="col-8">
                    {{ probeResults.clients[c.name].jobsRunning ?? '—' }}
                  </div>
                </div>
              </template>
              <template v-else-if="probeResults.clients[c.name] && !probeResults.clients[c.name].reachable">
                <q-separator class="q-my-sm" />
                <div class="text-negative" :data-testid="'client-error-' + c.name">
                  {{ probeResults.clients[c.name].error }}
                </div>
              </template>
            </q-card-section>

            <q-separator />

            <q-card-actions align="right">
              <q-btn
                flat
                dense
                color="primary"
                :to="'/clients/' + encodeURIComponent(c.name)"
                :label="t('Client details')"
                :data-testid="'client-link-' + c.name"
              />
            </q-card-actions>
          </q-card>
        </div>
      </div>
    </div>
  </q-page>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useDirectorStore } from '../stores/director.js'
import { useAuthStore } from '../stores/auth.js'
import { useSettingsStore } from '../stores/settings.js'
import { quoteDirectorString } from '../utils/directorStrings.js'
import { parseDaemonHeader } from '../utils/deviceStatus.js'
import { directorCollection } from '../composables/useDirectorFetch.js'
import { createPoller } from '../utils/poller.js'
import { groupStorageDaemons, probeComponents, extractClientList } from '../utils/componentProbe.js'

const { t } = useI18n()
const director = useDirectorStore()
const auth = useAuthStore()
const settings = useSettingsStore()

const loadingResources = ref(false)
const probing = ref(false)
const pageError = ref(null)

const pollInterval = ref(0)
const intervalOptions = [
  { label: t('Off'), value: 0 },
  { label: '10 s', value: 10 },
  { label: '30 s', value: 30 },
  { label: '60 s', value: 60 },
]

let poller = null

const rawStorages = ref([])
const rawClients = ref([])

const probeResults = ref({
  director: null,
  storages: {},
  clients: {},
})

const directorName = computed(() => {
  return auth.user?.director || settings.directorName || director.currentDirector || 'Director'
})

const directorProbe = computed(() => probeResults.value.director)

function extractCollection(res, key) {
  const container = res?.[key] ?? res?.result?.[key] ?? res ?? {}
  if (Array.isArray(container)) return container
  if (container && typeof container === 'object') {
    return Object.entries(container).map(([k, v]) => {
      if (v && typeof v === 'object' && !Array.isArray(v)) {
        return { name: k, ...v }
      }
      return { name: k }
    })
  }
  return []
}

const storageDaemons = computed(() => groupStorageDaemons(rawStorages.value))

const clients = computed(() => {
  return rawClients.value.map((c) => ({
    name: c.name,
    address: c.address || c.ip || '',
    ...c,
  }))
})

async function fetchResources() {
  loadingResources.value = true
  pageError.value = null
  try {
    const [storagesRes, clientsRes] = await Promise.all([
      director.call('show storages').catch(() => ({})),
      director.call('show clients').catch(() => ({})),
    ])

    let sList = extractCollection(storagesRes, 'storages')
    if (sList.length === 0) {
      try {
        const dotStorages = await director.call('.storages')
        sList = directorCollection(dotStorages?.storages ?? dotStorages)
      } catch {
        // ignore
      }
    }
    rawStorages.value = sList

    let cList = extractClientList(clientsRes)
    if (cList.length === 0) {
      try {
        const dotClients = await director.call('.clients')
        cList = extractClientList(dotClients)
      } catch {
        // ignore
      }
    }
    rawClients.value = cList
  } catch (err) {
    pageError.value = err?.message ?? String(err)
  } finally {
    loadingResources.value = false
  }
}

async function probeAll() {
  if (probing.value) return
  probing.value = true

  try {
    const probed = await probeComponents(director, {
      storages: rawStorages.value,
      clients: rawClients.value,
      probeDirector: true,
    })

    for (const p of probed) {
      if (p.kind === 'director') {
        probeResults.value.director = p.reachable
          ? { reachable: true, ...p.header }
          : { reachable: false, error: p.error }
      } else if (p.kind === 'storage') {
        probeResults.value.storages[p.key || p.id] = p.reachable
          ? { reachable: true, ...p.header }
          : { reachable: false, error: p.error }
      } else if (p.kind === 'client') {
        probeResults.value.clients[p.id] = p.reachable
          ? { reachable: true, ...p.header }
          : { reachable: false, error: p.error }
      }
    }
  } catch (err) {
    pageError.value = err?.message ?? String(err)
  } finally {
    probing.value = false
  }
}

function restartPoller() {
  if (poller) {
    poller.stop()
    poller = null
  }
  if (!pollInterval.value || pollInterval.value <= 0) return
  poller = createPoller(probeAll, pollInterval.value * 1000)
  poller.start()
}

watch(pollInterval, restartPoller)

onMounted(async () => {
  await fetchResources()
})

onUnmounted(() => {
  if (poller) {
    poller.stop()
    poller = null
  }
})
</script>
