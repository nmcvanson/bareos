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
    <!-- Header panel with Daemon status and poll interval -->
    <q-card flat bordered class="bareos-panel q-mb-md">
      <q-card-section class="panel-header row items-center justify-between">
        <div class="row items-center q-gutter-x-sm">
          <q-icon name="dns" size="sm" />
          <span class="text-h6 text-weight-medium" data-testid="sd-header-title">
            {{ t('Storage Daemon') }}: {{ header?.name || storageName }}
          </span>
        </div>
        <q-btn
          flat
          round
          dense
          icon="refresh"
          color="white"
          :loading="loading"
          data-testid="refresh-btn"
          @click="fetchStatus"
        />
      </q-card-section>

      <q-card-section>
        <q-banner v-if="pageError" dense class="bg-negative text-white rounded-borders q-mb-md">
          {{ pageError }}
        </q-banner>

        <!-- Daemon info -->
        <div v-if="header" class="row q-col-gutter-md items-center q-mb-md text-body2">
          <div class="col-12 col-sm-3">
            <span class="text-grey-7">{{ t('Version') }}: </span>
            <span class="text-weight-bold" data-testid="sd-version">{{ header.version }}</span>
          </div>
          <div class="col-12 col-sm-3">
            <span class="text-grey-7">{{ t('Started') }}: </span>
            <span class="text-weight-bold" data-testid="sd-started">{{ header.started }}</span>
          </div>
          <div class="col-6 col-sm-3">
            <span class="text-grey-7">{{ t('Jobs run') }}: </span>
            <span class="text-weight-bold" data-testid="sd-jobs-run">{{ header.jobsRun }}</span>
          </div>
          <div class="col-6 col-sm-3">
            <span class="text-grey-7">{{ t('Jobs running') }}: </span>
            <span class="text-weight-bold" data-testid="sd-jobs-running">{{ header.jobsRunning }}</span>
          </div>
        </div>

        <!-- Controls: Poll Interval -->
        <div class="row items-center q-col-gutter-md">
          <div class="col-12 col-sm-4">
            <q-select
              v-model="pollInterval"
              dense
              outlined
              emit-value
              map-options
              :options="intervalOptions"
              :label="t('Poll interval')"
              data-testid="poll-interval-select"
            />
          </div>
        </div>
      </q-card-section>
    </q-card>

    <!-- Running Jobs Table (if present) -->
    <q-card v-if="runningJobs.length > 0" flat bordered class="bareos-panel q-mb-md">
      <q-card-section class="panel-header row items-center justify-between">
        <span class="text-h6 text-weight-medium">{{ t('Running Jobs') }} ({{ runningJobs.length }})</span>
      </q-card-section>
      <q-card-section class="q-pa-none">
        <q-table
          dense
          flat
          :rows="runningJobs"
          :columns="runningJobColumns"
          row-key="jobId"
          :pagination="{ rowsPerPage: 10 }"
        >
          <template #body-cell-bytes="props">
            <q-td :props="props">
              {{ props.value != null ? formatBytes(props.value) : '—' }}
            </q-td>
          </template>
          <template #body-cell-bytesPerSec="props">
            <q-td :props="props">
              {{ props.value != null ? `${formatBytes(props.value)}/s` : '—' }}
            </q-td>
          </template>
        </q-table>
      </q-card-section>
    </q-card>

    <!-- Devices List -->
    <div class="text-subtitle1 text-weight-medium q-mb-sm">{{ t('Devices') }}</div>
    <div v-if="devices.length > 0">
      <DevicePanel
        v-for="device in devices"
        :key="device.name"
        :device="device"
        :rates="ratesFor(device)"
        :points="pointsFor(device)"
      />
    </div>
    <div
      v-else-if="!loading"
      class="text-grey-7 q-pa-lg text-center"
      data-testid="no-devices-msg"
    >
      {{ t('No devices found on this storage daemon.') }}
    </div>
  </q-page>
</template>

<script setup>
import { ref, computed, watch, onMounted, onBeforeUnmount } from 'vue'
import { useRoute } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { useDirectorStore } from '../stores/director.js'
import { quoteDirectorString } from '../utils/directorStrings.js'
import {
  parseDaemonHeader,
  parseDeviceBlocks,
  parseRunningJobs,
  mergeDaemonDevices,
  trimHistory,
  pairRates,
} from '../utils/deviceStatus.js'
import { computeRates } from '../utils/chunkedDeviceStatus.js'
import { createPoller } from '../utils/poller.js'
import { createInflightGuard } from '../utils/inflightGuard.js'
import { formatBytes } from '../mock/index.js'
import DevicePanel from '../components/DevicePanel.vue'

const { t } = useI18n()
const director = useDirectorStore()
const route = useRoute()

const storageName = computed(() => route.params.storage || '')
const rawStatus = ref('')
const statusList = ref([])
const loading = ref(false)
const pageError = ref(null)
const pollInterval = ref(0) // Default: Off
const history = ref([])
const latestRates = ref({})
let poller = null

const intervalOptions = [
  { label: t('Off'), value: 0 },
  { label: '5 s', value: 5 },
  { label: '10 s', value: 10 },
  { label: '30 s', value: 30 },
]

const runningJobColumns = [
  { name: 'jobId', label: t('Job ID'), field: 'jobId', align: 'left', sortable: true },
  { name: 'name', label: t('Name'), field: 'name', align: 'left', sortable: true },
  { name: 'level', label: t('Level'), field: 'level', align: 'left' },
  { name: 'type', label: t('Type'), field: 'type', align: 'left' },
  { name: 'status', label: t('Status'), field: 'status', align: 'left' },
  { name: 'volume', label: t('Volume'), field: 'volume', align: 'left' },
  { name: 'device', label: t('Device'), field: 'device', align: 'left' },
  { name: 'files', label: t('Files'), field: 'files', align: 'right' },
  { name: 'bytes', label: t('Bytes'), field: 'bytes', align: 'right' },
  { name: 'bytesPerSec', label: t('Rate'), field: 'aveBytesPerSec', align: 'right' },
]

const header = computed(() => parseDaemonHeader(rawStatus.value))
const devices = computed(() => {
  if (statusList.value.length > 0) {
    return mergeDaemonDevices(statusList.value)
  }
  return parseDeviceBlocks(rawStatus.value)
})
const runningJobs = computed(() => parseRunningJobs(rawStatus.value))

function ratesFor(device) {
  return latestRates.value[device.name] || null
}

function pointsFor(device) {
  return pairRates(history.value, device.name) || []
}

const fetchGuard = createInflightGuard()

// Timer, manual refresh, route watcher and interval changes share one
// request per storage.
function fetchStatus() {
  if (!storageName.value) return Promise.resolve()
  return fetchGuard.run(storageName.value, fetchStatusNow)
}

async function fetchStatusNow() {
  const scope = storageName.value
  loading.value = true
  pageError.value = null
  try {
    const storagesRes = await director.call('show storages').catch(() => ({}))
    const storagesMap = storagesRes?.storages ?? storagesRes?.result?.storages ?? storagesRes ?? {}
    const selectedStorage = storagesMap[storageName.value] ??
      Object.values(storagesMap).find(s => s && typeof s === 'object' && s.name === storageName.value)

    let targetStorages = [storageName.value]
    if (selectedStorage && typeof selectedStorage === 'object') {
      const targetAddress = String(selectedStorage.address || selectedStorage.ip || '').trim().toLowerCase()
      const targetPort = Number(selectedStorage.sdport ?? selectedStorage.SDport ?? selectedStorage.port ?? 9103)

      const matching = Object.values(storagesMap)
        .filter(s => s && typeof s === 'object' && s.name)
        .filter(s => {
          const addr = String(s.address || s.ip || '').trim().toLowerCase()
          const p = Number(s.sdport ?? s.SDport ?? s.port ?? 9103)
          return addr === targetAddress && p === targetPort
        })
        .map(s => s.name)

      targetStorages = [
        storageName.value,
        ...matching.filter(name => name !== storageName.value),
      ]
    }

    const parsedStatuses = []
    let primaryStatusText = ''

    for (const name of targetStorages) {
      try {
        const text = await director.rawCall('status storage=' + quoteDirectorString(name))
        if (!primaryStatusText) {
          primaryStatusText = text ?? ''
        }
        parsedStatuses.push({
          storage: name,
          devices: parseDeviceBlocks(text ?? ''),
        })
      } catch (err) {
        if (!primaryStatusText) {
          throw err
        }
      }
    }

    if (scope !== storageName.value) return
    rawStatus.value = primaryStatusText
    statusList.value = parsedStatuses

    // Record samples for devices with backend metrics
    const now = Date.now()
    for (const dev of devices.value) {
      if (dev.backend?.metrics) {
        history.value.push({
          t: now,
          device: {
            name: dev.name,
            ...dev.backend.metrics,
          },
        })
      }
    }
    trimHistory(history.value, now)
    latestRates.value = computeRates(history.value)
  } catch (err) {
    pageError.value = err?.message ?? String(err)
  } finally {
    loading.value = false
  }
}

function restartPoller() {
  if (poller) {
    poller.stop()
    poller = null
  }
  if (!pollInterval.value || pollInterval.value <= 0) return
  poller = createPoller(fetchStatus, pollInterval.value * 1000)
  poller.start()
}

watch(pollInterval, restartPoller)
watch(storageName, () => {
  history.value = []
  latestRates.value = {}
  statusList.value = []
  rawStatus.value = ''
  fetchStatus()
})

onMounted(() => {
  fetchStatus()
  restartPoller()
})

onBeforeUnmount(() => {
  if (poller) {
    poller.stop()
    poller = null
  }
})
</script>
