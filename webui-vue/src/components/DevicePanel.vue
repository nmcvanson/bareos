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
  <q-card flat bordered class="device-panel bareos-panel q-mb-md">
    <q-card-section class="panel-header row items-center justify-between">
      <div class="row items-center q-gutter-x-sm">
        <span class="text-h6 text-weight-medium" data-testid="device-name">{{ device.name }}</span>
        <q-chip
          v-if="device.state"
          dense
          square
          :color="stateColor"
          text-color="white"
          class="text-weight-bold"
          data-testid="device-state-chip"
        >
          {{ device.state }}
        </q-chip>
      </div>
      <div v-if="device.archive" class="text-caption text-white opacity-80" data-testid="device-archive">
        {{ device.archive }}
      </div>
    </q-card-section>

    <q-card-section class="q-pa-md">
      <!-- Error banner if present -->
      <q-banner v-if="error" dense class="bg-negative text-white rounded-borders q-mb-md">
        {{ error }}
      </q-banner>

      <!-- Standard Device Attributes (shown only when not null) -->
      <div class="row q-col-gutter-sm text-body2 q-mb-md">
        <div v-if="device.volume !== null && device.volume !== undefined" class="col-12 col-sm-4">
          <span class="text-grey-7">{{ t('Volume') }}: </span>
          <span class="text-weight-bold">{{ device.volume }}</span>
        </div>
        <div v-if="device.pool !== null && device.pool !== undefined" class="col-12 col-sm-4">
          <span class="text-grey-7">{{ t('Pool') }}: </span>
          <span class="text-weight-bold">{{ device.pool }}</span>
        </div>
        <div v-if="device.mediaType !== null && device.mediaType !== undefined" class="col-12 col-sm-4">
          <span class="text-grey-7">{{ t('Media type') }}: </span>
          <span class="text-weight-bold">{{ device.mediaType }}</span>
        </div>

        <div v-if="device.totalBytes !== null && device.totalBytes !== undefined" class="col-12 col-sm-4">
          <span class="text-grey-7">{{ t('Bytes written') }}: </span>
          <span class="text-weight-bold">{{ formatBytes(device.totalBytes) }}</span>
          <span v-if="device.blocks !== null && device.blocks !== undefined" class="text-caption text-grey-6 q-ml-xs">
            ({{ formatCount(device.blocks) }} {{ t('blocks') }})
          </span>
        </div>

        <div v-if="device.totalBytesRead !== null && device.totalBytesRead !== undefined" class="col-12 col-sm-4">
          <span class="text-grey-7">{{ t('Bytes read') }}: </span>
          <span class="text-weight-bold">{{ formatBytes(device.totalBytesRead) }}</span>
          <span v-if="device.blocksRead !== null && device.blocksRead !== undefined" class="text-caption text-grey-6 q-ml-xs">
            ({{ formatCount(device.blocksRead) }} {{ t('blocks') }})
          </span>
        </div>

        <div v-if="effectiveWriteRate !== null && effectiveWriteRate > 0" class="col-12 col-sm-4">
          <span class="text-grey-7">{{ t('Write rate') }}: </span>
          <span class="text-weight-bold text-primary">{{ formatMBs(effectiveWriteRate / 1e6) }}</span>
        </div>

        <div v-if="effectiveReadRate !== null && effectiveReadRate > 0" class="col-12 col-sm-4">
          <span class="text-grey-7">{{ t('Read rate') }}: </span>
          <span class="text-weight-bold text-secondary">{{ formatMBs(effectiveReadRate / 1e6) }}</span>
        </div>

        <div v-if="device.positionFile !== null && device.positionFile !== undefined" class="col-12 col-sm-4">
          <span class="text-grey-7">{{ t('Position') }}: </span>
          <span class="text-weight-bold">
            {{ t('File {file}, Block {block}', { file: device.positionFile, block: device.positionBlock ?? 0 }) }}
          </span>
        </div>

        <div v-if="device.slot !== null && device.slot !== undefined" class="col-12 col-sm-4">
          <span class="text-grey-7">{{ t('Slot') }}: </span>
          <span class="text-weight-bold">{{ device.slot }}</span>
        </div>
      </div>

      <!-- Backend Metrics Section (shown only when backend is set) -->
      <div v-if="backendMetrics" class="backend-metrics-section" data-testid="backend-metrics-section">
        <q-separator class="q-my-md" />
        <div class="text-subtitle1 text-weight-medium q-mb-sm">{{ t('Backend metrics') }}</div>

        <!-- Refuses writes banner -->
        <q-banner v-if="backendMetrics.refusesWrites" dense class="bg-negative text-white rounded-borders q-mb-md">
          {{ backendMetrics.refusesWrites }}
        </q-banner>

        <!-- Last upload status -->
        <div class="row items-center q-mb-md text-body2">
          <span class="text-weight-medium q-mr-xs">{{ t('Last upload') }}:</span>
          <span v-if="backendMetrics.lastUpload?.state === 'ok'" class="text-positive text-weight-bold">
            {{ t('OK, {s} s ago', { s: backendMetrics.lastUpload.agoS ?? 0 }) }}
          </span>
          <span v-else-if="backendMetrics.lastUpload?.state === 'failed'" class="text-negative text-weight-bold">
            {{ backendMetrics.lastUpload.error
              ? t('Failed, {s} s ago: {error}', { s: backendMetrics.lastUpload.agoS ?? 0, error: backendMetrics.lastUpload.error })
              : t('Failed, {s} s ago', { s: backendMetrics.lastUpload.agoS ?? 0 }) }}
          </span>
          <span v-else class="text-grey-7">
            {{ t('No upload since the daemon started') }}
          </span>
        </div>

        <!-- Rate tiles -->
        <div class="text-subtitle2 text-grey-8 q-mb-xs">{{ t('Rates (30s window)') }}</div>
        <div class="row q-col-gutter-sm q-mb-md">
          <div class="col-6 col-sm-3">
            <q-card flat bordered class="q-pa-sm text-center bg-grey-1">
              <div class="text-h6 text-primary text-weight-bold">{{ uploadMBsFormatted }}</div>
              <div class="text-caption text-grey-7">{{ t('Upload') }}</div>
            </q-card>
          </div>
          <div class="col-6 col-sm-3">
            <q-card flat bordered class="q-pa-sm text-center bg-grey-1">
              <div class="text-h6 text-secondary text-weight-bold">{{ downloadMBsFormatted }}</div>
              <div class="text-caption text-grey-7">{{ t('Download') }}</div>
            </q-card>
          </div>
          <div class="col-6 col-sm-3">
            <q-card flat bordered class="q-pa-sm text-center bg-grey-1">
              <div class="text-h6 text-weight-bold">{{ uploadedChunksPerMinFormatted }}</div>
              <div class="text-caption text-grey-7">{{ t('Uploaded chunks/min') }}</div>
            </q-card>
          </div>
          <div class="col-6 col-sm-3">
            <q-card flat bordered class="q-pa-sm text-center bg-grey-1">
              <div class="text-h6 text-weight-bold">{{ receivedChunksPerMinFormatted }}</div>
              <div class="text-caption text-grey-7">{{ t('Received chunks/min') }}</div>
            </q-card>
          </div>
        </div>

        <!-- Gauges -->
        <div class="text-subtitle2 text-grey-8 q-mb-xs">{{ t('Gauges') }}</div>
        <div class="row q-col-gutter-sm q-mb-md">
          <div class="col-12 col-sm-4">
            <q-card flat bordered class="q-pa-sm text-center bg-grey-1">
              <div class="text-h6 text-weight-bold">{{ formatCount(backendMetrics.chunks?.pending) }}</div>
              <div class="text-caption text-grey-7">{{ t('Pending') }}</div>
              <div class="text-caption text-grey-6 q-mt-xs">
                {{ t('queued') }}: {{ formatCount(backendMetrics.chunks?.queued) }} /
                {{ t('uploading') }}: {{ formatCount(backendMetrics.chunks?.uploading) }} /
                {{ t('kept') }}: {{ formatCount(backendMetrics.chunks?.kept) }}
              </div>
            </q-card>
          </div>
          <div class="col-6 col-sm-4">
            <q-card flat bordered class="q-pa-sm text-center bg-grey-1">
              <div class="text-h6 text-weight-bold">{{ formatCount(backendMetrics.chunks?.waitingRetry) }}</div>
              <div class="text-caption text-grey-7">{{ t('Waiting for retry') }}</div>
            </q-card>
          </div>
          <div class="col-6 col-sm-4">
            <q-card flat bordered class="q-pa-sm text-center bg-grey-1">
              <div class="text-h6 text-weight-bold">
                {{ backendMetrics.chunks?.oldestPendingS != null ? formatCount(backendMetrics.chunks.oldestPendingS) : '—' }}
              </div>
              <div class="text-caption text-grey-7">{{ t('Oldest pending (s)') }}</div>
            </q-card>
          </div>
        </div>

        <!-- Chart -->
        <div class="row items-center justify-between q-mb-xs">
          <div class="text-subtitle2 text-grey-8">{{ t('Transfer history (last 10 minutes)') }}</div>
          <div class="row items-center q-gutter-x-md text-caption">
            <div class="row items-center q-gutter-x-xs">
              <span class="chart-legend-color bg-primary"></span>
              <span>{{ t('Upload') }}</span>
            </div>
            <div class="row items-center q-gutter-x-xs">
              <span class="chart-legend-color bg-secondary"></span>
              <span>{{ t('Download') }}</span>
            </div>
            <div class="text-grey-7">
              {{ t('Max') }}: {{ formatMBs(svgData.maxVal) }}
            </div>
          </div>
        </div>
        <div class="chart-container q-mb-md">
          <svg
            viewBox="0 0 600 120"
            preserveAspectRatio="none"
            class="metrics-svg"
          >
            <!-- Grid lines -->
            <line x1="0" y1="0" x2="600" y2="0" stroke="#e0e0e0" stroke-width="1" />
            <line x1="0" y1="60" x2="600" y2="60" stroke="#f0f0f0" stroke-width="1" stroke-dasharray="4" />
            <line x1="0" y1="120" x2="600" y2="120" stroke="#e0e0e0" stroke-width="1" />

            <!-- Rate polylines -->
            <polyline
              v-if="svgData.uploadCoords"
              :points="svgData.uploadCoords"
              fill="none"
              stroke="#1976d2"
              stroke-width="2"
            />
            <polyline
              v-if="svgData.downloadCoords"
              :points="svgData.downloadCoords"
              fill="none"
              stroke="#26a69a"
              stroke-width="2"
            />
          </svg>
        </div>

        <!-- Cumulative Totals -->
        <div class="text-subtitle2 text-grey-8 q-mb-xs">{{ t('Totals since daemon start') }}</div>
        <div class="row q-col-gutter-sm text-caption">
          <div class="col-6 col-sm-3">
            <div class="text-grey-7">{{ t('Chunks uploaded') }}:</div>
            <div class="text-weight-bold">{{ formatCount(backendMetrics.upload?.chunks) }}</div>
          </div>
          <div class="col-6 col-sm-3">
            <div class="text-grey-7">{{ t('Bytes uploaded') }}:</div>
            <div class="text-weight-bold">{{ formatCount(backendMetrics.upload?.bytes) }}</div>
          </div>
          <div class="col-6 col-sm-3">
            <div class="text-grey-7">{{ t('Failed upload tries') }}:</div>
            <div class="text-weight-bold">{{ formatCount(backendMetrics.upload?.failedTries) }}</div>
          </div>
          <div class="col-6 col-sm-3">
            <div class="text-grey-7">{{ t('Retried upload tries') }}:</div>
            <div class="text-weight-bold">{{ formatCount(backendMetrics.upload?.retriedTries) }}</div>
          </div>
          <div class="col-6 col-sm-3">
            <div class="text-grey-7">{{ t('Objects downloaded') }}:</div>
            <div class="text-weight-bold">{{ formatCount(backendMetrics.download?.objects) }}</div>
          </div>
          <div class="col-6 col-sm-3">
            <div class="text-grey-7">{{ t('Bytes downloaded') }}:</div>
            <div class="text-weight-bold">{{ formatCount(backendMetrics.download?.bytes) }}</div>
          </div>
          <div class="col-6 col-sm-3">
            <div class="text-grey-7">{{ t('Download failures') }}:</div>
            <div class="text-weight-bold">{{ formatCount(backendMetrics.download?.failed) }}</div>
          </div>
          <div class="col-6 col-sm-3">
            <div class="text-grey-7">{{ t('Transport retries') }}:</div>
            <div class="text-weight-bold">{{ formatCount(backendMetrics.download?.transportRetries) }}</div>
          </div>
        </div>
      </div>
    </q-card-section>
  </q-card>
</template>

<script setup>
import { computed, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { formatMBs, formatCount } from '../utils/deviceStatus.js'
import { formatBytes } from '../mock/index.js'

const { t } = useI18n()

const props = defineProps({
  device: {
    type: Object,
    required: true,
  },
  rates: {
    type: Object,
    default: null,
  },
  points: {
    type: Array,
    default: () => [],
  },
  error: {
    type: String,
    default: null,
  },
})

const stateColor = computed(() => {
  const s = props.device?.state
  if (s === 'mounted') return 'positive'
  if (s === 'not open') return 'grey-7'
  return 'primary'
})

const backendMetrics = computed(() => (
  props.device?.backend?.metrics ?? (props.device?.chunks ? props.device : null)
))

// Track successive polls of totalBytes and totalBytesRead to compute rates when they grow
const writeRate = ref(null)
const readRate = ref(null)
const lastSample = ref(null)

watch(
  () => [props.device?.totalBytes, props.device?.totalBytesRead],
  ([bytes, readBytes]) => {
    const now = Date.now()
    if (lastSample.value) {
      const dt = (now - lastSample.value.t) / 1000
      if (dt > 0) {
        if (bytes != null && lastSample.value.bytes != null && bytes > lastSample.value.bytes) {
          writeRate.value = (bytes - lastSample.value.bytes) / dt
        } else if (bytes != null && lastSample.value.bytes != null && bytes < lastSample.value.bytes) {
          writeRate.value = 0
        }
        if (readBytes != null && lastSample.value.readBytes != null && readBytes > lastSample.value.readBytes) {
          readRate.value = (readBytes - lastSample.value.readBytes) / dt
        } else if (readBytes != null && lastSample.value.readBytes != null && readBytes < lastSample.value.readBytes) {
          readRate.value = 0
        }
      }
    }
    lastSample.value = { t: now, bytes, readBytes }
  },
  { immediate: true }
)

const effectiveWriteRate = computed(() => props.rates?.writeBytesPerS ?? writeRate.value)
const effectiveReadRate = computed(() => props.rates?.readBytesPerS ?? readRate.value)

const uploadMBsFormatted = computed(() => {
  const bytesPerS = props.rates?.uploadBytesPerS
  return formatMBs(bytesPerS != null ? bytesPerS / 1e6 : null)
})

const downloadMBsFormatted = computed(() => {
  const bytesPerS = props.rates?.downloadBytesPerS
  return formatMBs(bytesPerS != null ? bytesPerS / 1e6 : null)
})

const uploadedChunksPerMinFormatted = computed(() => {
  const val = props.rates?.uploadedChunksPerMin
  return val != null ? Number(val).toFixed(1) : '—'
})

const receivedChunksPerMinFormatted = computed(() => {
  const val = props.rates?.receivedChunksPerMin
  return val != null ? Number(val).toFixed(1) : '—'
})

const svgData = computed(() => {
  const pts = props.points || []
  if (pts.length === 0) {
    return { uploadCoords: '', downloadCoords: '', maxVal: 1 }
  }

  const validUploads = pts
    .map(p => p.uploadMBs)
    .filter(v => v !== null && Number.isFinite(v))
  const validDownloads = pts
    .map(p => p.downloadMBs)
    .filter(v => v !== null && Number.isFinite(v))

  const maxVal = Math.max(1, ...validUploads, ...validDownloads)
  const latestT = pts[pts.length - 1].t
  const startT = latestT - 600000 // 10 minutes

  const uploadPoints = []
  const downloadPoints = []

  for (const p of pts) {
    if (typeof p.t !== 'number') continue
    const x = Math.max(0, Math.min(600, ((p.t - startT) / 600000) * 600))

    if (p.uploadMBs !== null && Number.isFinite(p.uploadMBs)) {
      const y = 120 - Math.max(0, Math.min(120, (p.uploadMBs / maxVal) * 120))
      uploadPoints.push(`${x.toFixed(1)},${y.toFixed(1)}`)
    }

    if (p.downloadMBs !== null && Number.isFinite(p.downloadMBs)) {
      const y = 120 - Math.max(0, Math.min(120, (p.downloadMBs / maxVal) * 120))
      downloadPoints.push(`${x.toFixed(1)},${y.toFixed(1)}`)
    }
  }

  return {
    uploadCoords: uploadPoints.join(' '),
    downloadCoords: downloadPoints.join(' '),
    maxVal,
  }
})
</script>

<style scoped>
.chart-container {
  width: 100%;
  height: 120px;
  background-color: #fafafa;
  border: 1px solid #e0e0e0;
  border-radius: 4px;
  overflow: hidden;
}

.metrics-svg {
  width: 100%;
  height: 100%;
  display: block;
}

.chart-legend-color {
  display: inline-block;
  width: 12px;
  height: 12px;
  border-radius: 2px;
}
</style>
