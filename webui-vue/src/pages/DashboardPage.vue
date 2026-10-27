<!--
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
-->
<template>
  <q-page class="q-pa-md">
    <DirectorErrorsBanner :errors="directorErrors" />

    <!-- ── Step R4 New Cards ──────────────────────────────────────────────── -->
    <div class="row q-col-gutter-md q-mb-md">
      <!-- 1. Last 24 hours card -->
      <div class="col-12 col-md-4">
        <q-card flat bordered class="bareos-panel fit" data-testid="card-last-24h">
          <q-card-section class="panel-header row items-center">
            <q-icon name="history" size="xs" class="q-mr-xs" />
            <span>{{ t('Last 24 hours') }}</span>
          </q-card-section>
          <q-card-section>
            <div class="row q-col-gutter-sm text-center">
              <div class="col-3">
                <router-link
                  :to="{ name: 'jobs', query: withJobsStatusFilterQuery({}, 'T') }"
                  class="text-decoration-none"
                >
                  <StatNumber
                    :value="outcomeCounts24h.ok"
                    :label="t('OK')"
                    color="positive"
                    data-testid="last-24h-count-ok"
                  />
                </router-link>
              </div>
              <div class="col-3">
                <router-link
                  :to="{ name: 'jobs', query: withJobsStatusFilterQuery({}, 'W') }"
                  class="text-decoration-none"
                >
                  <StatNumber
                    :value="outcomeCounts24h.warning"
                    :label="t('Warning')"
                    color="warning"
                    data-testid="last-24h-count-warning"
                  />
                </router-link>
              </div>
              <div class="col-3">
                <router-link
                  :to="{ name: 'jobs', query: withJobsStatusFilterQuery({}, 'f') }"
                  class="text-decoration-none"
                >
                  <StatNumber
                    :value="outcomeCounts24h.failed"
                    :label="t('Failed')"
                    color="negative"
                    data-testid="last-24h-count-failed"
                  />
                </router-link>
              </div>
              <div class="col-3">
                <router-link
                  :to="{ name: 'jobs', query: withJobsStatusFilterQuery({}, 'A') }"
                  class="text-decoration-none"
                >
                  <StatNumber
                    :value="outcomeCounts24h.canceled"
                    :label="t('Canceled')"
                    color="grey-7"
                    data-testid="last-24h-count-canceled"
                  />
                </router-link>
              </div>
            </div>
          </q-card-section>
        </q-card>
      </div>

      <!-- 2. Failed jobs card -->
      <div class="col-12 col-md-4">
        <q-card flat bordered class="bareos-panel fit" data-testid="card-failed-jobs">
          <q-card-section class="panel-header row items-center">
            <q-icon name="error_outline" size="xs" class="q-mr-xs" />
            <span>{{ t('Failed jobs') }}</span>
          </q-card-section>
          <q-card-section class="q-pa-none" style="max-height: 220px; overflow-y: auto">
            <div v-if="failedJobs24h.length === 0" class="q-pa-md text-grey text-caption text-center">
              {{ t('No failed jobs in the last 24 hours.') }}
            </div>
            <q-list v-else separator dense>
              <q-item v-for="job in failedJobs24h" :key="jobReasonScopeKey(job)" class="q-py-xs">
                <q-item-section>
                  <q-item-label class="row items-center no-wrap">
                    <a href="#" class="text-primary text-weight-medium ellipsis" @click.prevent="openJobDetails(job)">
                      {{ job.name }}
                    </a>
                    <span class="text-grey-7 text-caption q-ml-xs">#{{ jobId(job) }}</span>
                    <q-space />
                    <JobStatusBadge :status="jobStatus(job)" size="xs" />
                  </q-item-label>
                  <q-item-label caption class="text-negative ellipsis" :title="failedJobReasons[jobReasonScopeKey(job)] || ''">
                    {{ failedJobReasons[jobReasonScopeKey(job)] || t('Loading reason…') }}
                  </q-item-label>
                </q-item-section>
              </q-item>
            </q-list>
          </q-card-section>
        </q-card>
      </div>

      <!-- 3. Components card -->
      <div class="col-12 col-md-4">
        <q-card flat bordered class="bareos-panel fit" data-testid="card-components">
          <q-card-section class="panel-header row items-center justify-between">
            <div class="row items-center">
              <q-icon name="dns" size="xs" class="q-mr-xs" />
              <span>{{ t('Components') }}</span>
            </div>
            <q-btn
              color="primary"
              dense
              size="sm"
              icon="sensors"
              :label="t('Probe now')"
              :loading="probingComponents"
              data-testid="dashboard-probe-now-btn"
              @click="runComponentsProbe"
            />
          </q-card-section>
          <q-card-section style="max-height: 220px; overflow-y: auto">
            <div v-if="allComponentCards.length === 0" class="text-grey text-caption text-center q-py-sm">
              {{ t('No components found.') }}
            </div>
            <div v-else class="row q-gutter-xs items-center">
              <q-chip
                v-for="c in allComponentCards"
                :key="c.id"
                dense
                size="sm"
                :color="c.color"
                :text-color="c.textColor"
                :icon="c.icon"
                :data-testid="'dashboard-component-chip-' + c.id"
              >
                {{ c.label }}
                <q-tooltip>{{ c.tooltip }}</q-tooltip>
              </q-chip>
            </div>
          </q-card-section>
        </q-card>
      </div>
    </div>

    <div class="row q-col-gutter-md">
      <!-- Left column: 8/12 -->
      <div class="col-12 col-md-8">
        <!-- Jobs Past 24h stat row -->
        <q-card flat bordered class="q-mb-md bareos-panel">
          <q-card-section class="panel-header row items-center">
            <span>{{ t('Jobs started during the past 24 hours') }}</span>
            <q-space />
            <span class="text-white text-caption q-mr-sm panel-refresh-countdown">
              <span aria-hidden="true">↻</span>
              <span class="panel-refresh-countdown__value">{{ countdown }}s</span>
            </span>
            <q-btn
              flat
              round
              dense
              icon="refresh"
              color="white"
              size="sm"
              :loading="loadingJobs"
              @click="manualRefresh"
            />
          </q-card-section>
          <q-card-section>
            <div class="row q-col-gutter-md text-center">
              <div class="col" v-for="s in summaryStats" :key="s.label">
                <router-link
                  :to="{ name: 'jobs', query: withJobsStatusFilterQuery({}, s.status) }"
                  class="text-decoration-none"
                >
                  <StatNumber :value="s.count" :label="s.label" :color="s.color" />
                </router-link>
              </div>
            </div>
          </q-card-section>
        </q-card>

        <!-- Jobs Last Status table -->
        <q-card flat bordered class="bareos-panel">
          <q-card-section class="panel-header row items-center">
            <span>{{ t('Most recent job status per job name') }}</span>
            <q-space />
            <q-btn
              flat
              round
              dense
              icon="refresh"
              color="white"
              size="sm"
              :loading="loadingJobs"
              @click="manualRefresh"
            />
          </q-card-section>
          <q-card-section class="q-pa-none">
            <q-table
              :rows="recentJobs"
              :columns="recentCols"
              row-key="scopeKey"
              dense flat
              :loading="loadingJobs"
              v-model:pagination="recentJobsPagination"
            >
              <template #body-cell-id="props">
                <q-td :props="props" class="text-right">
                  <a href="#" class="text-primary" @click.prevent="openJobDetails(props.row)">
                    {{ props.value }}
                  </a>
                </q-td>
              </template>
              <template #body-cell-director="props">
                <q-td :props="props">
                  <div class="row items-center q-gutter-sm no-wrap">
                    <span :style="directorSwatchStyle(props.row.director || props.value || '')" />
                    <span>{{ props.row.director || props.value || '—' }}</span>
                  </div>
                </q-td>
              </template>
              <template #body-cell-status="props">
                <q-td :props="props">
                  <span
                    v-if="isWaitingStatus(jobStatus(props.row))"
                    class="row items-center no-wrap q-gutter-x-xs"
                  >
                    <q-icon name="hourglass_empty" color="orange-7" size="16px" class="animated-spin" />
                    <span class="text-orange-7 text-caption">{{ jobStatus(props.row) }}</span>
                  </span>
                  <JobStatusBadge v-else :status="jobStatus(props.row)" />
                </q-td>
              </template>
              <template #body-cell-client="props">
                <q-td :props="props">
                  <a href="#" class="text-primary" @click.prevent="openClientDetails(props.row)">
                    {{ props.value }}
                  </a>
                </q-td>
              </template>
              <template #body-cell-starttime="props">
                <q-td :props="props">
                  <span :title="settings.relativeTime ? props.value : timeAgo(props.value, settings.locale)">
                    {{ settings.relativeTime ? timeAgo(props.value, settings.locale) : props.value }}
                  </span>
                </q-td>
              </template>
              <template #body-cell-name="props">
                <q-td :props="props">
                  <a href="#" class="text-primary" @click.prevent="openJobDetails(props.row)">
                    {{ props.value }}
                  </a>
                </q-td>
              </template>
              <template #body-cell-level="props">
                <q-td :props="props" class="text-center">
                  <JobLevelBadge v-if="props.value" :level="props.value" />
                  <span v-else>—</span>
                </q-td>
              </template>
              <template #body-cell-bytes="props">
                <q-td :props="props" class="text-right" style="min-width:90px">
                  <div>{{ jobBytes(props.row) }}</div>
                  <q-linear-progress
                    v-if="isRunningJob(props.row)"
                    indeterminate color="primary" track-color="grey-3"
                    size="4px" class="q-mt-xs" rounded
                  />
                  <q-linear-progress
                    v-else
                    :value="bytesGauge(props.row.bytes)"
                    color="primary" track-color="grey-3"
                    size="4px" class="q-mt-xs" rounded
                  />
                </q-td>
              </template>
              <template #body-cell-duration="props">
                <q-td :props="props" class="text-right" style="min-width:80px">
                  <div>{{ props.value || '—' }}</div>
                  <q-linear-progress
                    :value="durationGauge(props.value)"
                    color="orange" track-color="grey-3"
                    size="4px" class="q-mt-xs" rounded
                  />
                </q-td>
              </template>
              <template #body-cell-speed="props">
                <q-td :props="props" class="text-right" style="min-width:80px">
                  <span v-if="isRunningJob(props.row)" class="text-grey-5">—</span>
                  <template v-else>
                    <div>{{ fmtSpeed(props.row.bytes, props.row.duration) }}</div>
                    <q-linear-progress
                      :value="speedGauge(props.row)"
                      color="cyan-7" track-color="grey-3"
                      size="4px" class="q-mt-xs" rounded
                    />
                  </template>
                </q-td>
              </template>
            </q-table>
          </q-card-section>
        </q-card>
      </div>

      <!-- Right column: 4/12 -->
      <div class="col-12 col-md-4">
        <!-- Job Totals -->
        <q-card flat bordered class="q-mb-md bareos-panel">
          <q-card-section class="panel-header">{{ t('Job Totals') }}</q-card-section>
          <q-card-section class="q-pa-sm">
            <div class="row q-gutter-sm">
              <div v-for="stat in totalStats" :key="stat.label" class="col-auto">
                <div class="text-caption text-grey-6" style="white-space:nowrap">{{ stat.label }}</div>
                <div class="text-weight-bold" style="font-size:1rem; line-height:1.2">{{ stat.value }}</div>
              </div>
            </div>
          </q-card-section>
        </q-card>

        <!-- Running Jobs -->
        <q-card flat bordered class="bareos-panel">
          <q-card-section class="panel-header row items-center">
            <span>{{ t('Running') }} {{ t('Jobs') }}</span>
            <q-space />
            <q-btn
              flat
              round
              dense
              icon="refresh"
              color="white"
              size="sm"
              :loading="loadingJobs"
              @click="manualRefresh"
            />
          </q-card-section>
          <q-card-section style="max-height:320px; overflow-y:auto; padding:0">
            <q-list separator>
              <q-item v-for="job in runningJobs" :key="job.scopeKey" class="q-py-sm">
                <q-item-section>
                  <q-item-label>
                    <a href="#" class="text-primary text-weight-medium" @click.prevent="openJobDetails(job)">
                      {{ job.name }}
                    </a>
                    <DirectorBadge
                      v-if="showDirectorColumn"
                      :director="job.director"
                      size="sm"
                      class="q-ml-xs"
                    />
                    <span class="text-grey-6 text-caption q-ml-xs">({{ job.client }})</span>
                  </q-item-label>
                  <q-item-label caption>
                      {{ formatNumber(job.files ?? 0, settings.locale) }} {{ t('Files') }} &middot; {{ jobBytes(job) }} &middot; {{ formatDuration(elapsedSecs(job)) }}
                  </q-item-label>
                  <q-item-label
                    v-if="jobStatus(job)"
                    caption
                    :class="isWaitingStatus(jobStatus(job)) ? 'text-orange-7' : 'text-grey-6'"
                  >
                    <q-icon
                      v-if="isWaitingStatus(jobStatus(job))"
                      name="hourglass_empty"
                      size="14px"
                      class="q-mr-xs"
                    />
                    {{ jobStatus(job) }}
                  </q-item-label>
                  <q-linear-progress indeterminate color="positive" class="q-mt-xs" style="height:6px; border-radius:3px" />
                </q-item-section>
                <q-item-section side>
                  <q-btn
                    flat
                    round
                    dense
                    icon="cancel"
                    color="negative"
                    size="sm"
                    :title="t('Cancel Job')"
                    @click="confirmCancel(job)"
                  />
                </q-item-section>
              </q-item>
              <q-item v-if="!runningJobs.length">
                <q-item-section class="text-grey text-caption text-center q-py-md">{{ t('No running jobs') }}</q-item-section>
              </q-item>
            </q-list>
          </q-card-section>
        </q-card>
      </div>
    </div>
  </q-page>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, watch } from 'vue'
import { useQuasar } from 'quasar'
import { useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { formatBytes, formatSpeed, parseDurationSecs, timeAgo, formatDuration } from '../mock/index.js'
import {
  aggregateDirectorDashboardSnapshots,
  fetchDirectorDashboardSnapshot,
} from '../composables/directorAggregate.js'
import { useDirectorScope } from '../composables/useDirectorScope.js'
import { switchActiveDirector } from '../composables/useDirectorSession.js'
import { useDirectorStore } from '../stores/director.js'
import { useAuthStore } from '../stores/auth.js'
import { useSettingsStore } from '../stores/settings.js'
import { buildClientDetailsQuery } from '../utils/clients.js'
import { resolveDirectorColors } from '../utils/directorColors.js'
import {
  buildCancelJobCommand,
  buildJobDetailsQuery,
  withJobsStatusFilterQuery,
} from '../utils/jobs.js'
import { formatNumber } from '../utils/locales.js'
import { usePersistedTablePagination } from '../composables/usePersistedTablePagination.js'
import DirectorBadge from '../components/DirectorBadge.vue'
import DirectorErrorsBanner from '../components/DirectorErrorsBanner.vue'
import JobStatusBadge from '../components/JobStatusBadge.vue'
import JobLevelBadge from '../components/JobLevelBadge.vue'
import StatNumber from '../components/StatNumber.vue'
import { jobOutcome } from '../utils/jobOutcome.js'
import { createJobReasonCache, jobReasonScopeKey } from '../utils/jobReasonCache.js'
import {
  extractStorageList,
  extractClientList,
  groupStorageDaemons,
  probeComponents,
} from '../utils/componentProbe.js'
import { directorCollection } from '../composables/useDirectorFetch.js'

const auth = useAuthStore()
const director = useDirectorStore()
const settings = useSettingsStore()
const $q = useQuasar()
const router = useRouter()
const { t } = useI18n()
const recentJobsPagination = usePersistedTablePagination('dashboard.recentJobs', {
  rowsPerPage: 10,
  sortBy: 'id',
  descending: true,
})
const fmtBytes = formatBytes
const fmtSpeed = formatSpeed

// ── data ─────────────────────────────────────────────────────────────────────
const dashboardSnapshots = ref([])
const directorErrors = ref([])
const loadingJobs = ref(false)

const {
  directorOptions,
  selectedDirectorsModel,
  activeDirectors,
  isCommonScope: isCommonDashboard,
  scopeLabel: dashboardScopeLabel,
  syncSelectedDirectors,
} = useDirectorScope({
  t,
  syncEmptySelection: 'all',
})

const showDirectorColumn = computed(() => directorOptions.value.length > 1)

async function fetchDashboard() {
  const credentials = auth.getCredentials()
  if (!credentials || activeDirectors.value.length === 0) {
    dashboardSnapshots.value = []
    directorErrors.value = []
    return
  }

  loadingJobs.value = true
  try {
    const results = await Promise.allSettled(activeDirectors.value.map(currentDirector => (
      fetchDirectorDashboardSnapshot({
        ...credentials,
        director: currentDirector,
      })
    )))

    dashboardSnapshots.value = results
      .filter(result => result.status === 'fulfilled')
      .map(result => result.value)
    directorErrors.value = results.flatMap((result, index) => (
      result.status === 'rejected'
        ? [{
          director: activeDirectors.value[index],
          message: result.reason?.message ?? t('Failed to load dashboard data.'),
        }]
        : []
    ))
  } finally {
    loadingJobs.value = false
  }
}

async function loadAvailableDirectors() {
  try {
    await director.fetchAvailableDirectors()
  } catch {
    // Keep the selector usable with the active director when the proxy list is
    // unavailable.
  }
}

let refreshPromise = null
let refreshQueued = false

async function runRefreshBatch() {
  await fetchDashboard()
}

function refresh() {
  if (refreshPromise) {
    refreshQueued = true
    return refreshPromise
  }

  refreshPromise = (async () => {
    do {
      refreshQueued = false
      await runRefreshBatch()
    } while (refreshQueued)
  })().finally(() => {
    refreshPromise = null
  })

  return refreshPromise
}

function confirmCancel(job) {
  $q.dialog({
    title: 'Cancel Job',
    message: t('Cancel job {name} (ID {id})?', { name: job.name, id: jobId(job) }),
    ok: { label: t('Cancel Job'), color: 'negative', flat: true },
    cancel: { label: t('Keep Running'), flat: true },
  }).onOk(() => doCancel(job))
}

async function openJobDetails(row) {
  try {
    await switchActiveDirector(row.director)
    await router.push({
      name: 'job-details',
      params: { id: jobId(row) },
      query: buildJobDetailsQuery({
        director: row.director,
        dashboardOrigin: true,
      }),
    })
  } catch (error) {
    $q.notify({
      type: 'negative',
      message: t('Could not switch to director {director}: {message}', {
        director: row.director ?? t('unknown'),
        message: error.message,
      }),
    })
  }
}

async function openClientDetails(row) {
  try {
    await switchActiveDirector(row.director)
    await router.push({
      name: 'client-details',
      params: { name: row.client },
      query: buildClientDetailsQuery({
        director: row.director,
        dashboardOrigin: true,
      }),
    })
  } catch (error) {
    $q.notify({
      type: 'negative',
      message: t('Could not switch to director {director}: {message}', {
        director: row.director ?? t('unknown'),
        message: error.message,
      }),
    })
  }
}

async function doCancel(job) {
  try {
    await switchActiveDirector(job.director)
    await director.call(buildCancelJobCommand(jobId(job)))
    $q.notify({ type: 'positive', message: t('Job {id} cancelled.', { id: jobId(job) }) })
    refresh()
  } catch (e) {
    $q.notify({ type: 'negative', message: t('Cancel failed: {message}', { message: e.message }) })
  }
}

const now = ref(Date.now())

onMounted(async () => {
  await loadAvailableDirectors()
  syncSelectedDirectors()
  refresh()
})

// ── auto-refresh ──────────────────────────────────────────────────────────────
const countdown = ref(settings.refreshInterval)
let _countdownTimer = null
let _refreshTimer = null

function startAutoRefresh() {
  stopAutoRefresh()
  countdown.value = settings.refreshInterval
  _countdownTimer = setInterval(() => {
    now.value = Date.now()
    countdown.value -= 1
    if (countdown.value <= 0) {
      refresh()
      countdown.value = settings.refreshInterval
    }
  }, 1000)
}

function stopAutoRefresh() {
  clearInterval(_countdownTimer)
  clearInterval(_refreshTimer)
  _countdownTimer = null
  _refreshTimer = null
}

function manualRefresh() {
  refresh()
  countdown.value = settings.refreshInterval
}

onMounted(startAutoRefresh)
onUnmounted(stopAutoRefresh)
watch(() => directorOptions.value, () => {
  syncSelectedDirectors()
}, { deep: true })
watch(() => activeDirectors.value.join('\u0000'), () => {
  countdown.value = settings.refreshInterval
  refresh()
})

// ── computed views ────────────────────────────────────────────────────────────
const aggregate = computed(() => (
  aggregateDirectorDashboardSnapshots(dashboardSnapshots.value)
))
const past24hJobs = computed(() => aggregate.value.jobsPast24h)
const lastJobs = computed(() => aggregate.value.recentJobs)

const recentCols = computed(() => {
  const columns = [
    { name: 'id', label: 'ID', field: 'id', align: 'right', sortable: true },
    { name: 'name', label: t('Job Name'), field: 'name', align: 'left', sortable: true },
    { name: 'client', label: t('Client'), field: 'client', align: 'left', sortable: true },
    { name: 'level', label: t('Level'), field: 'level', align: 'center', sortable: true },
    { name: 'status', label: t('Status'), field: 'status', align: 'center', sortable: true },
    { name: 'starttime', label: t('Start'), field: 'starttime', align: 'left', sortable: true },
    {
      name: 'duration',
      label: t('Duration'),
      field: 'duration',
      align: 'right',
      sortable: true,
      sort: (a, b) => parseDurationSecs(a) - parseDurationSecs(b),
    },
    { name: 'bytes', label: t('Bytes'), field: 'bytes', align: 'right', sortable: true },
    { name: 'speed', label: t('Speed'), field: 'speed', align: 'right', sortable: true,
      sort: (_a, _b, rowA, rowB) => jobSpeedBps(rowA) - jobSpeedBps(rowB) },
  ]

  if (showDirectorColumn.value) {
    columns.splice(1, 0, {
      name: 'director',
      label: t('Director'),
      field: 'director',
      align: 'left',
      sortable: true,
    })
  }

  return columns
})

const recentJobs = computed(() => lastJobs.value)
const runningJobs = computed(() => aggregate.value.runningJobs)

function directorSwatchStyle(name) {
  const colors = resolveDirectorColors(
    name,
    directorOptions.value.map(option => option.value)
  )
  return {
    display: 'inline-block',
    width: '10px',
    height: '10px',
    borderRadius: '999px',
    backgroundColor: colors.background,
    border: `1px solid ${colors.border}`,
    flexShrink: 0,
  }
}

function elapsedSecs(job) {
  if (!job.starttime) return 0
  const start = new Date(job.starttime.replace(' ', 'T')).getTime()
  if (isNaN(start)) return 0
  return Math.max(0, Math.floor((now.value - start) / 1000))
}

const maxBytes = computed(() => Math.max(1, ...recentJobs.value.map(j => j.bytes)))
const maxDurationSecs = computed(() => Math.max(1, ...recentJobs.value.map(j => parseDurationSecs(j.duration))))
function jobSpeedBps(row) {
  const secs = parseDurationSecs(row.duration)
  if (!secs) return 0
  const bytes = typeof row.bytes === 'string' ? parseFloat(row.bytes) : (row.bytes || 0)
  return bytes / secs
}
const maxSpeedBps = computed(() => Math.max(1, ...recentJobs.value
  .filter(j => !isRunningJob(j))
  .map(j => jobSpeedBps(j))))
function bytesGauge(val) { return val / maxBytes.value }
function durationGauge(str) { return parseDurationSecs(str) / maxDurationSecs.value }
function speedGauge(row) { return jobSpeedBps(row) / maxSpeedBps.value }

const summaryStats = computed(() => {
  const countByStatus = (code) => past24hJobs.value.filter(j => j.status === code).length
  return [
    { label: t('Running'), status: 'R', color: 'info', count: countByStatus('R') },
    { label: t('Waiting'), status: 'C', color: 'grey', count: countByStatus('C') },
    { label: t('Successful'), status: 'T', color: 'positive', count: countByStatus('T') },
    { label: t('Warning'), status: 'W', color: 'warning', count: countByStatus('W') },
    { label: t('Failed'), status: 'f', color: 'negative', count: countByStatus('f') },
  ]
})

const totals = computed(() => ({
  jobs: aggregate.value.jobTotals.jobs,
  files: aggregate.value.jobTotals.files,
  bytes: aggregate.value.jobTotals.bytes,
  clients: aggregate.value.clientCount,
  storages: aggregate.value.storageCount,
}))

const totalStats = computed(() => [
  { label: t('Total Jobs'), value: totals.value.jobs },
  { label: t('Total Files'), value: formatNumber(totals.value.files ?? 0, settings.locale) },
  { label: t('Total Bytes'), value: fmtBytes(totals.value.bytes ?? 0) },
  { label: t('Clients'), value: totals.value.clients },
  { label: t('Storages'), value: totals.value.storages },
])

// helper: keep status/bytes getters for template compatibility
function jobStatus(row) { return row.runtimeStatus ?? row.status ?? '?' }
function isWaitingStatus(status) {
  return typeof status === 'string' && status.toLowerCase().includes('is waiting')
}
function isRunningJob(row) {
  return row?.status === 'R' || row?.runtimeStatus != null
}
function jobBytes(row) { return fmtBytes(row.bytes ?? 0) }
function jobId(row) {
  return row.id ?? row.jobid
}

// ── Step R4 logic ────────────────────────────────────────────────────────────
const directorName = computed(() => {
  return auth.user?.director || settings.directorName || director.currentDirector || 'Director'
})

const outcomeCounts24h = computed(() => {
  const counts = {
    ok: 0,
    warning: 0,
    failed: 0,
    canceled: 0,
    running: 0,
    waiting: 0,
  }
  for (const j of past24hJobs.value) {
    const { state } = jobOutcome(j)
    if (counts[state] !== undefined) {
      counts[state]++
    }
  }
  return counts
})

const failedJobs24h = computed(() => {
  return past24hJobs.value
    .filter((j) => {
      const { state } = jobOutcome(j)
      return state === 'failed' || state === 'canceled'
    })
    .sort((a, b) => {
      const timeA = a.starttime ? new Date(a.starttime.replace(' ', 'T')).getTime() : 0
      const timeB = b.starttime ? new Date(b.starttime.replace(' ', 'T')).getTime() : 0
      if (timeA !== timeB) return timeB - timeA
      return (Number(b.id || b.jobid) || 0) - (Number(a.id || a.jobid) || 0)
    })
    .slice(0, 5)
})

const failedJobReasons = ref({})
const loadingJobLogs = ref(false)
const reasonCache = createJobReasonCache({
  switchDirector: row => switchActiveDirector(row.director),
  call: command => director.call(command),
  fallbackDirector: () => auth.user?.director || '',
  onUpdate: (key, value) => {
    if (!value.loading) failedJobReasons.value = {
      ...failedJobReasons.value,
      [key]: value.error || value.reasons[0] || value.termination || t('No specific error reason in log'),
    }
  },
})
onUnmounted(() => reasonCache.dispose())

async function fetchFailedJobReasons() {
  const jobsToFetch = failedJobs24h.value.filter(job => jobId(job))
  if (jobsToFetch.length === 0) return

  loadingJobLogs.value = true
  try {
    await Promise.allSettled(jobsToFetch.map(job => reasonCache.load(job)))
  } finally {
    loadingJobLogs.value = false
  }
}

watch(failedJobs24h, () => {
  fetchFailedJobReasons()
}, { immediate: true })

// Components probe & card
const componentStorages = ref([])
const componentClients = ref([])
const componentProbeResults = ref([])
const probingComponents = ref(false)

async function loadComponentResources() {
  try {
    const [storagesRes, clientsRes] = await Promise.all([
      director.call('show storages').catch(() => ({})),
      director.call('show clients').catch(() => ({})),
    ])

    let sList = extractStorageList(storagesRes)
    if (sList.length === 0) {
      try {
        const dotStorages = await director.call('.storages')
        sList = directorCollection(dotStorages?.storages ?? dotStorages)
      } catch {}
    }
    componentStorages.value = sList

    let cList = extractClientList(clientsRes)
    if (cList.length === 0) {
      try {
        const dotClients = await director.call('.clients')
        cList = extractClientList(dotClients)
      } catch {}
    }
    componentClients.value = cList
  } catch {}
}

async function runComponentsProbe() {
  if (probingComponents.value) return
  probingComponents.value = true
  try {
    if (componentStorages.value.length === 0 || componentClients.value.length === 0) {
      await loadComponentResources()
    }
    componentProbeResults.value = []
    await probeComponents(director, {
      storages: componentStorages.value,
      clients: componentClients.value,
      probeDirector: true,
      directorName: auth.user?.director || '',
      onResult: result => { componentProbeResults.value.push(result) },
    })
  } catch (err) {
    $q.notify({ type: 'negative', message: err?.message ?? String(err) })
  } finally {
    probingComponents.value = false
  }
}

onMounted(() => {
  loadComponentResources().catch(() => {})
})

const allComponentCards = computed(() => {
  const items = []

  // Director
  const dirProbe = componentProbeResults.value.find(p => p.kind === 'director')
  const dirLabel = `${t('Director')}: ${dirProbe?.name || directorName.value}`
  if (!dirProbe) {
    items.push({
      id: 'director',
      label: dirLabel,
      color: 'grey-6',
      textColor: 'white',
      icon: 'help_outline',
      tooltip: t('Not probed'),
    })
  } else if (dirProbe.reachable) {
    items.push({
      id: 'director',
      label: dirLabel,
      color: 'positive',
      textColor: 'white',
      icon: 'check_circle',
      tooltip: dirProbe.header?.version
        ? t('Reachable (v{version})', { version: dirProbe.header.version })
        : t('Reachable'),
    })
  } else {
    items.push({
      id: 'director',
      label: dirLabel,
      color: 'negative',
      textColor: 'white',
      icon: 'error',
      tooltip: dirProbe.error || t('Down'),
    })
  }

  // Storage Daemons
  const groupedSDs = groupStorageDaemons(componentStorages.value)
  for (const sd of groupedSDs) {
    const p = componentProbeResults.value.find(pr => pr.kind === 'storage' && (pr.key === sd.key || pr.id === sd.key))
    const sdLabel = `SD: ${p?.name || sd.key}`
    if (!p) {
      items.push({
        id: sd.key,
        label: sdLabel,
        color: 'grey-6',
        textColor: 'white',
        icon: 'help_outline',
        tooltip: t('Not probed'),
      })
    } else if (p.reachable) {
      items.push({
        id: sd.key,
        label: sdLabel,
        color: 'positive',
        textColor: 'white',
        icon: 'check_circle',
        tooltip: p.header?.version
          ? t('Reachable (v{version})', { version: p.header.version })
          : t('Reachable'),
      })
    } else {
      items.push({
        id: sd.key,
        label: sdLabel,
        color: 'negative',
        textColor: 'white',
        icon: 'error',
        tooltip: p.error || t('Down'),
      })
    }
  }

  // Clients
  const clientList = Array.isArray(componentClients.value)
    ? componentClients.value
    : directorCollection(componentClients.value)
  for (const cl of clientList) {
    const clName = typeof cl === 'string' ? cl : cl?.name
    if (!clName) continue
    const p = componentProbeResults.value.find(pr => pr.kind === 'client' && pr.id === clName)
    const clLabel = `Client: ${clName}`
    if (!p) {
      items.push({
        id: clName,
        label: clLabel,
        color: 'grey-6',
        textColor: 'white',
        icon: 'help_outline',
        tooltip: t('Not probed'),
      })
    } else if (p.reachable) {
      items.push({
        id: clName,
        label: clLabel,
        color: 'positive',
        textColor: 'white',
        icon: 'check_circle',
        tooltip: p.header?.version
          ? t('Reachable (v{version})', { version: p.header.version })
          : t('Reachable'),
      })
    } else {
      items.push({
        id: clName,
        label: clLabel,
        color: 'negative',
        textColor: 'white',
        icon: 'error',
        tooltip: p.error || t('Down'),
      })
    }
  }

  return items
})
</script>

<style scoped>
.animated-spin {
  animation: hourglass-spin 1.4s ease-in-out infinite;
}

@keyframes hourglass-spin {
  0%   { transform: rotate(0deg); }
  45%  { transform: rotate(0deg); }
  55%  { transform: rotate(180deg); }
  100% { transform: rotate(180deg); }
}
</style>
