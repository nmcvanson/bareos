<template>
  <q-page class="q-pa-md">
    <q-inner-loading :showing="loading" :label="t('Loading job…')" />
    <div v-if="error" class="text-negative q-pa-md">{{ error }}</div>

    <div v-else-if="!loading">
      <!-- Header row -->
      <div class="row items-center q-mb-md">
        <q-btn
          flat
          icon="arrow_back"
          :label="backLabel"
          :to="backLocation"
          no-caps
          class="q-mr-md"
        />
        <div class="row items-center q-gutter-sm">
          <div class="text-h6" data-testid="job-details-title">
            <template v-if="hasValidJob">
              Job #{{ job.id }} — {{ job.name }}
            </template>
            <template v-else>
              Job {{ currentJobId }}
            </template>
          </div>
          <q-spinner v-if="isRunning" color="primary" size="18px" :title="t('Auto-refreshing…')" />
        </div>
      </div>

      <div v-if="!hasValidJob" class="text-grey q-pa-md" data-testid="job-details-not-found">
        {{ t('This job does not exist or this console may not see it.') }}
      </div>

      <div v-else class="row q-col-gutter-md">

        <!-- Outcome -->
        <div class="col-12">
          <q-card flat bordered class="bareos-panel">
            <q-card-section class="panel-header">{{ t('Outcome') }}</q-card-section>
            <q-card-section>
              <div class="row items-center q-gutter-sm">
                <q-chip
                  dense
                  :color="outcomeChipProps.color"
                  :text-color="outcomeChipProps.textColor"
                  :icon="outcomeChipProps.icon"
                  :label="t(outcome.label)"
                />
                <span v-if="outcome.termination" class="text-body2 text-weight-medium">
                  {{ outcome.termination }}
                </span>
              </div>
              <div
                v-if="showReasonLines"
                class="q-mt-sm"
              >
                <div class="text-caption text-weight-bold q-mb-xs">{{ t('Reasons') }}:</div>
                <q-list dense bordered separator class="rounded-borders">
                  <q-item v-for="(line, idx) in outcome.reasonLines" :key="idx">
                    <q-item-section class="text-body2 font-monospace">{{ line }}</q-item-section>
                  </q-item>
                </q-list>
              </div>
            </q-card-section>
          </q-card>
        </div>

        <!-- Summary -->
        <div class="col-12 col-md-6">
          <q-card flat bordered class="bareos-panel">
            <q-card-section class="panel-header">{{ t('Job Summary') }}</q-card-section>
            <q-card-section>
              <q-list dense separator>
                <q-item v-for="row in summaryRows" :key="row.label">
                  <q-item-section class="text-weight-medium" style="max-width:140px">{{ row.label }}</q-item-section>
                  <q-item-section>
                    <span v-if="row.status">
                      <span
                        v-if="row.waiting"
                        class="row items-center no-wrap q-gutter-x-xs"
                      >
                        <q-icon name="hourglass_empty" color="orange-7" size="16px" class="animated-spin" />
                        <span class="text-orange-7 text-caption">{{ row.status }}</span>
                      </span>
                      <JobStatusBadge v-else :status="row.status" />
                    </span>
                    <span v-else-if="row.level">
                      <JobLevelBadge :level="row.level" />
                      <span class="q-ml-xs text-grey-7">{{ row.value }}</span>
                    </span>
                    <span v-else-if="row.type">
                      <JobTypeBadge :type="row.type" />
                      <span class="q-ml-xs text-grey-7">{{ row.value }}</span>
                    </span>
                    <span v-else>{{ row.value }}</span>
                  </q-item-section>
                </q-item>
              </q-list>
            </q-card-section>
          </q-card>
        </div>

        <!-- Volumes + Actions -->
        <div class="col-12 col-md-6 column q-gutter-md">

          <!-- Throughput -->
          <q-card flat bordered class="bareos-panel">
            <q-card-section class="panel-header">{{ t('Throughput') }}</q-card-section>
            <q-card-section>
              <q-list dense separator>
                <q-item>
                  <q-item-section class="text-weight-medium" style="max-width:160px">{{ t('Files') }}</q-item-section>
                  <q-item-section>
                    {{ formatNumber(displayThroughputFiles, settings.locale) }}
                  </q-item-section>
                </q-item>
                <q-item>
                  <q-item-section class="text-weight-medium" style="max-width:160px">{{ t('Bytes') }}</q-item-section>
                  <q-item-section>
                    {{ formatBytes(displayThroughputBytes) }}
                  </q-item-section>
                </q-item>
                <q-item>
                  <q-item-section class="text-weight-medium" style="max-width:160px">{{ t('Duration') }}</q-item-section>
                  <q-item-section>
                    {{ job?.duration || '—' }}
                  </q-item-section>
                </q-item>
                <q-item v-if="!isRunning">
                  <q-item-section class="text-weight-medium" style="max-width:160px">{{ t('Rate') }}</q-item-section>
                  <q-item-section>
                    {{ displayThroughputRate }}
                  </q-item-section>
                </q-item>
                <template v-if="isRunning">
                  <q-item v-if="fdRunning">
                    <q-item-section class="text-weight-medium" style="max-width:160px">{{ t('FD Rate') }}</q-item-section>
                    <q-item-section>
                      {{ fdRunning.bytesPerSec != null ? `${formatBytes(fdRunning.bytesPerSec)}/s` : '—' }}
                    </q-item-section>
                  </q-item>
                  <q-item v-if="sdRunning">
                    <q-item-section class="text-weight-medium" style="max-width:160px">{{ t('SD Average Rate') }}</q-item-section>
                    <q-item-section>
                      {{ sdRunning.aveBytesPerSec != null ? `${formatBytes(sdRunning.aveBytesPerSec)}/s` : '—' }}
                    </q-item-section>
                  </q-item>
                  <q-item v-if="sdRunning">
                    <q-item-section class="text-weight-medium" style="max-width:160px">{{ t('SD Last Rate') }}</q-item-section>
                    <q-item-section>
                      {{ sdRunning.lastBytesPerSec != null ? `${formatBytes(sdRunning.lastBytesPerSec)}/s` : '—' }}
                    </q-item-section>
                  </q-item>
                  <q-item v-if="fdRunning?.processingFile">
                    <q-item-section class="text-weight-medium" style="max-width:160px">{{ t('Processing file') }}</q-item-section>
                    <q-item-section class="font-monospace text-caption ellipsis" :title="fdRunning.processingFile">
                      {{ fdRunning.processingFile }}
                    </q-item-section>
                  </q-item>
                </template>
              </q-list>
              <div class="q-mt-sm text-caption text-grey-7">
                {{ t('CPU and memory per job are not reported by Bareos.') }}
              </div>
            </q-card-section>
          </q-card>

          <!-- Actions -->
          <q-card flat bordered class="bareos-panel">
            <q-card-section class="panel-header">{{ t('Actions') }}</q-card-section>
            <q-card-section class="q-gutter-sm">
              <q-btn
                v-if="canRestoreFromThisJob"
                icon="settings_backup_restore"
                :label="t('Restore from this job')"
                color="secondary"
                no-caps
                :to="{ path: '/restore', query: restoreFromJobQuery }"
                data-testid="restore-from-job-btn"
              />
              <q-btn icon="restart_alt" :label="t('Rerun Job')" color="primary" no-caps
                     :loading="rerunLoading" @click="confirmRerun" />
              <q-btn v-if="isRunning" icon="cancel" :label="t('Cancel Job')" color="negative" no-caps
                     :loading="cancelLoading" @click="confirmCancel" />
            </q-card-section>
          </q-card>

          <!-- Storage -->
          <q-card v-if="showStorageCard" flat bordered class="bareos-panel">
            <q-card-section class="panel-header">{{ t('Storage') }}</q-card-section>
            <q-card-section>
              <div v-if="storageEvents.attempts.length > 0" class="row items-center q-gutter-xs q-mb-sm">
                <template v-for="(attempt, i) in storageEvents.attempts" :key="i">
                  <q-chip
                    v-if="attempt.outcome === 'skipped-disabled'"
                    dense
                    color="grey"
                    text-color="white"
                    icon="block"
                    :label="`${attempt.storage} (${t('disabled')})`"
                  />
                  <q-chip
                    v-else-if="attempt.outcome === 'refused'"
                    dense
                    color="orange"
                    text-color="white"
                    icon="error"
                    :label="`${attempt.storage} (${t('could not take the job')})`"
                  />
                  <q-chip
                    v-else-if="attempt.outcome === 'used'"
                    dense
                    color="positive"
                    text-color="white"
                    icon="check_circle"
                    :label="attempt.storage"
                  />
                </template>
              </div>

              <div class="q-gutter-y-xs text-body2">
                <div v-if="storageEvents.policy">
                  <span class="text-weight-bold">{{ t('Policy') }}:</span> {{ storageEvents.policy }}
                </div>
                <div v-if="storageEvents.storageSource">
                  <span class="text-weight-bold">{{ t('Storage list from') }}:</span> {{ storageEvents.storageSource }}
                </div>
                <div v-if="storageEvents.failedOverTo">
                  {{ t('Failed over to {storage}', { storage: storageEvents.failedOverTo }) }}
                </div>
                <div v-if="storageEvents.allDisabled" class="text-negative text-weight-bold">
                  {{ t('All members are disabled') }}
                </div>
                <div v-if="storageEvents.dispatchedTo">
                  {{ t('Dispatched to {storage}', { storage: storageEvents.dispatchedTo }) }}
                </div>
              </div>
            </q-card-section>
          </q-card>

          <!-- Volumes used -->
          <q-card v-if="showVolumesCard" flat bordered class="bareos-panel">
            <q-card-section class="panel-header">{{ t('Volumes Used') }}</q-card-section>
            <q-card-section class="q-pa-none">
              <q-table
                :rows="volumes"
                :columns="volumeCols"
                row-key="volumename"
                dense flat
                v-model:pagination="volumesPagination"
                :no-data-label="t('No volumes recorded.')"
              >
                <template #body-cell-volumename="props">
                  <q-td :props="props">
                    <q-icon name="album" color="primary" size="xs" class="q-mr-xs" />
                    <VolumeNameLink
                      :name="props.value"
                      :volume="props.row"
                      :query="{
                        ...currentJobDetailsQuery,
                        ...buildVolumeDetailsQuery({
                          director: currentJobDirector,
                          jobId: currentJobId,
                        }),
                      }"
                    />
                  </q-td>
                </template>
              </q-table>
            </q-card-section>
          </q-card>

        </div>

        <!-- Job Log -->
        <div class="col-12">
          <q-card flat bordered class="bareos-panel">
            <q-card-section class="panel-header row items-center">
                <span>{{ t('Job Log') }}</span>
              <q-space />
              <q-btn flat round dense icon="content_copy" size="sm" color="white"
                     :title="t('Copy log')" @click="copyLog" />
            </q-card-section>
            <q-card-section class="q-pa-none">
              <div v-if="highlightedLines.length" class="job-log q-pa-md" ref="logContainer">
                <div v-for="(line, i) in highlightedLines" :key="i"
                     :class="['log-line', `log-line--${line.type}`]">{{ line.text }}</div>
              </div>
               <div v-else class="text-grey text-caption q-pa-md">{{ t('No log entries found.') }}</div>
            </q-card-section>
          </q-card>
        </div>

      </div>
    </div>
  </q-page>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted, watch, nextTick } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { useQuasar } from 'quasar'
import { useI18n } from 'vue-i18n'
import { jobLevelMap, formatBytes, formatSpeed } from '../mock/index.js'
import {
  displayJobStatus,
  isRunningJobStatus,
  isWaitingJobStatus,
  normaliseJob,
  overlayRuntimeStatuses,
} from '../composables/useDirectorFetch.js'
import { switchActiveDirector } from '../composables/useDirectorSession.js'
import { usePersistedTablePagination } from '../composables/usePersistedTablePagination.js'
import { useAuthStore } from '../stores/auth.js'
import { useDirectorStore } from '../stores/director.js'
import { useDirectorAclStore } from '../stores/directorAcl.js'
import { useSettingsStore } from '../stores/settings.js'
import { buildClientDetailsQuery } from '../utils/clients.js'
import { quoteDirectorString } from '../utils/directorStrings.js'
import { buildDirectorPageQuery } from '../utils/director.js'
import { formatNumber } from '../utils/locales.js'
import { actionAllowed } from '../utils/acl.js'
import {
  buildCancelJobCommand,
  buildJobDetailsQuery,
  buildListJobCommand,
  buildRerunJobCommand,
  buildRestoreFromJobQuery,
  canRestoreFromJob,
  isValidJobRecord,
  resolveJobDetailsClientOrigin,
  resolveJobDetailsQuery,
  resolveJobDetailsDashboardOrigin,
  resolveJobDetailsDirectorOrigin,
  resolveJobDetailsRestoreOrigin,
  resolveJobDetailsVolumeOrigin,
  resolveJobsListQuery,
  mergeJobMediaByVolume,
} from '../utils/jobs.js'
import { resolveJobTypeCode } from '../utils/jobTypes.js'
import { buildVolumeDetailsQuery } from '../utils/volumes.js'
import JobStatusBadge from '../components/JobStatusBadge.vue'
import JobLevelBadge from '../components/JobLevelBadge.vue'
import JobTypeBadge from '../components/JobTypeBadge.vue'
import VolumeNameLink from '../components/VolumeNameLink.vue'
import { parseJobStorageEvents } from '../utils/jobStorageEvents.js'
import { jobOutcome } from '../utils/jobOutcome.js'
import { parseFdRunning, parseSdRunning, finalRate } from '../utils/jobProgress.js'
import { createPoller } from '../utils/poller.js'
import { createInflightGuard } from '../utils/inflightGuard.js'
import { useJobScope } from '../utils/jobScope.js'
import { createLiveProgress, displayProgress } from '../utils/liveProgress.js'

const route    = useRoute()
const router   = useRouter()
const $q       = useQuasar()
const auth = useAuthStore()
const director = useDirectorStore()
const acl = useDirectorAclStore()
const settings = useSettingsStore()
const { t } = useI18n()

onMounted(() => {
  acl.ensureLoaded().catch(() => {})
})

const {
  jobId: currentJobId,
  requestedDirector,
  jobDirector: currentJobDirector,
  scopeKey,
} = useJobScope({ route, auth, settings })
const backToJobsQuery = computed(() => resolveJobsListQuery(route.query))
const dashboardOrigin = computed(() => resolveJobDetailsDashboardOrigin(route.query))
const clientOrigin = computed(() => resolveJobDetailsClientOrigin(route.query))
const directorOrigin = computed(() => resolveJobDetailsDirectorOrigin(route.query))
const restoreOrigin = computed(() => resolveJobDetailsRestoreOrigin(route.query))
const volumeOrigin = computed(() => resolveJobDetailsVolumeOrigin(route.query))
const currentJobDetailsQuery = computed(() => resolveJobDetailsQuery(route.query))
const backLabel = computed(() => (
  clientOrigin.value
    ? t('Back to Client')
    : (dashboardOrigin.value
      ? t('Back to Dashboard')
      : (
        directorOrigin.value
          ? t('Back to Director')
          : (
            restoreOrigin.value
              ? t('Back to Restore')
              : (volumeOrigin.value ? t('Back to Volume') : t('Back to Jobs'))
          )
      ))
))
const backLocation = computed(() => {
  if (clientOrigin.value) {
    return {
      name: 'client-details',
      params: { name: clientOrigin.value.name },
      query: buildClientDetailsQuery({
        director: clientOrigin.value.director,
        clientsTab: clientOrigin.value.clientsTab,
        clientsScopeDirector: clientOrigin.value.scopeDirector,
        jobsAction: clientOrigin.value.jobsAction,
        jobsStatus: clientOrigin.value.jobsStatus,
        jobsLevel: clientOrigin.value.jobsLevel,
        jobsType: clientOrigin.value.jobsType,
        jobsJob: clientOrigin.value.jobsJob,
        jobsClient: clientOrigin.value.jobsClient,
        jobsSearch: clientOrigin.value.jobsSearch,
        dashboardOrigin: clientOrigin.value.dashboardOrigin,
      }),
    }
  }

  if (dashboardOrigin.value) {
    return { name: 'dashboard' }
  }

  if (directorOrigin.value) {
    return {
      name: 'director',
      query: buildDirectorPageQuery({}, {
        tab: directorOrigin.value.tab,
        targetDirector: directorOrigin.value.targetDirector,
      }),
    }
  }

  if (restoreOrigin.value) {
    return {
      name: 'restore',
      query: {
        client: restoreOrigin.value.client,
        director: restoreOrigin.value.director,
        jobid: restoreOrigin.value.jobid,
      },
    }
  }

  if (volumeOrigin.value) {
    return {
      name: 'volume-details',
      params: { name: volumeOrigin.value.name },
      query: volumeOrigin.value.director ? { director: volumeOrigin.value.director } : {},
    }
  }

  return {
    name: 'jobs',
    query: backToJobsQuery.value,
  }
})

// ── state ─────────────────────────────────────────────────────────────────────
const loading       = ref(true)
const jobData       = ref(null)
const logLines      = ref('')
const rawJobLog     = ref([])
const logContainer  = ref(null)
const volumes       = ref([])
const volumeDetailsByName = ref({})
const error         = ref(null)
const rerunLoading  = ref(false)
const cancelLoading = ref(false)
const volumesPagination = usePersistedTablePagination('job-details.volumes', {
  rowsPerPage: 15,
  sortBy: 'volumename',
  descending: false,
})

// Scroll the log panel to the bottom whenever new log content arrives.
watch(logLines, () => {
  nextTick(() => {
    if (logContainer.value) logContainer.value.scrollTop = logContainer.value.scrollHeight
  })
})

// ── data loading ──────────────────────────────────────────────────────────────
async function ensureJobDirector() {
  if (!requestedDirector.value) {
    return
  }

  if (auth.user?.director === requestedDirector.value && director.isConnected) {
    return
  }

  await switchActiveDirector(requestedDirector.value)
}

const loadGuard = createInflightGuard()
const currentScope = () => scopeKey.value

// One load per job and Director at a time, shared by the watcher, the timer
// and the actions.
function loadJob() {
  return loadGuard.run(currentScope(), loadJobNow)
}

async function loadJobNow() {
  const scope = currentScope()
  const directorName = currentJobDirector.value
  await ensureJobDirector()
  const [jobRes, logRes, mediaRes] = await Promise.allSettled([
    director.call(buildListJobCommand(currentJobId.value)),
    director.call(`list joblog jobid=${currentJobId.value}`),
    director.call(`list jobmedia jobid=${currentJobId.value}`),
  ])
  if (scope !== currentScope()) return

  if (jobRes.status === 'fulfilled') {
    const raw   = jobRes.value
    let entry = Array.isArray(raw?.jobs) ? raw.jobs[0] : (raw?.jobs ?? raw)
    if (entry && !entry.jobid && !entry.id && typeof entry === 'object') {
      const firstVal = Object.values(entry)[0]
      if (firstVal && typeof firstVal === 'object' && (firstVal.jobid || firstVal.id)) {
        entry = firstVal
      }
    }
    if (isValidJobRecord(entry)) {
      const loadedJob = {
        ...normaliseJob(entry),
        director: directorName || null,
      }
      if (isRunningJobStatus(loadedJob.status)) {
        const runtimeStatus = await Promise.allSettled([director.call('status director')])
        if (scope !== currentScope()) return
        jobData.value = runtimeStatus[0].status === 'fulfilled'
          ? overlayRuntimeStatuses([loadedJob], runtimeStatus[0].value?.running)[0]
          : loadedJob
      } else {
        jobData.value = loadedJob
      }
    } else {
      jobData.value = null
    }
  } else {
    error.value = jobRes.reason?.message ?? t('Failed to load job')
  }

  if (logRes.status === 'fulfilled') {
    const raw = logRes.value
    if (Array.isArray(raw?.joblog)) {
      rawJobLog.value = raw.joblog
      logLines.value = raw.joblog
        .map(l => `${(l.time ?? '').trim()} ${(l.logtext ?? '').trimEnd()}`.trim())
        .join('\n')
    } else if (typeof raw === 'string') {
      rawJobLog.value = [{ logtext: raw }]
      logLines.value = raw
    } else {
      rawJobLog.value = []
    }
  }

  if (mediaRes.status === 'fulfilled') {
    const mediaRows = mergeJobMediaByVolume(mediaRes.value?.jobmedia)
    const volumeNames = mediaRows.map(row => row.volumename)
    const volumeResults = await Promise.allSettled(
      volumeNames.map(name => director.call(`llist volume=${quoteDirectorString(name)}`))
    )

    volumeDetailsByName.value = volumeNames.reduce((acc, name, index) => {
      const result = volumeResults[index]
      if (result.status !== 'fulfilled') {
        return acc
      }

      const raw = result.value?.volumes ?? result.value?.volume ?? null
      acc[name] = Array.isArray(raw) ? (raw[0] ?? null) : raw
      return acc
    }, {})

    volumes.value = mediaRows.map(row => ({
      ...volumeDetailsByName.value[row.volumename],
      ...row,
    }))
  }
}

// Live FD and SD counters belong to the job and Director on screen.
const liveProgress = createLiveProgress()
const fdRunning = ref(null)
const sdRunning = ref(null)

function resetLive() {
  liveProgress.reset(currentScope())
  fdRunning.value = null
  sdRunning.value = null
}

watch(scopeKey, async () => {
  resetLive()
  loading.value = true
  error.value = null
  jobData.value = null
  logLines.value = ''
  rawJobLog.value = []
  volumes.value = []
  volumeDetailsByName.value = {}
  try {
    await loadJob()
  } catch (loadError) {
    error.value = loadError.message ?? t('Failed to load job')
  } finally {
    loading.value = false
  }
}, { immediate: true })

// ── computed ──────────────────────────────────────────────────────────────────
const job = computed(() => jobData.value)
const hasValidJob = computed(() => isValidJobRecord(job.value))
const isRestoreJob = computed(() => resolveJobTypeCode(job.value?.type) === 'R')
const showVolumesCard = computed(() => !isRestoreJob.value || volumes.value.length > 0)

const storageEvents = computed(() => parseJobStorageEvents(rawJobLog.value))
const showStorageCard = computed(() => (
  storageEvents.value.candidates !== null || Boolean(storageEvents.value.finalStorage)
))

const volumeCols = computed(() => [
  { name: 'volumename', label: 'Volume',
    field: 'volumename', align: 'left', sortable: true,
    headerClasses: 'text-weight-bold' },
  { name: 'storage', label: 'Storage',
    field: 'storage', align: 'left', sortable: true },
  { name: 'segments', label: 'Segments',
    field: 'segments', align: 'right', sortable: true },
].map((col) => ({
  ...col,
  label: t(col.label),
  headerTitle: col.headerTitle ? t(col.headerTitle) : col.headerTitle,
})))

const summaryRows = computed(() => {
  if (!job.value) return []
  const j = job.value
  const status = displayJobStatus(j)
  return [
    { label: t('Status'),     status, waiting: isWaitingJobStatus(status) },
    { label: t('Job ID'),     value: j.id },
    { label: t('Job Name'),   value: j.name },
    { label: t('Client'),     value: j.client },
    { label: t('Type'),       value: j.type  || '—', type: j.type  || null },
    { label: t('Level'),      value: jobLevelMap[j.level] || j.level || '—', level: j.level || null },
    { label: t('Start Time'), value: j.starttime || '—' },
    { label: t('End Time'),   value: j.endtime   || '—' },
    { label: t('Duration'),   value: j.duration  || '—' },
    { label: t('Files'),      value: formatNumber(j.files, settings.locale) },
    { label: t('Bytes'),      value: formatBytes(j.bytes) },
    { label: t('Speed'),      value: formatSpeed(j.bytes, j.duration) },
    { label: t('Errors'),     value: j.errors },
  ]
})

const jobLog = computed(() => logLines.value)

const highlightedLines = computed(() => {
  if (!logLines.value) return []
  return logLines.value.split('\n').map(line => {
    const l = line.toLowerCase()
    if (/\b(?:non-fatal\s+fd\s+errors|sd\s+errors|fd\s+errors|errors|warnings?)\s*:\s*0\b/.test(l)) {
      return { text: line, type: 'normal' }
    }
    if (/error|fatal|failed/.test(l))               return { text: line, type: 'error'   }
    if (/warning|warn/.test(l))                      return { text: line, type: 'warning' }
    if (/\bok\b|termination:.*ok|backup ok/.test(l)) return { text: line, type: 'ok'      }
    return { text: line, type: 'normal' }
  })
})

// ── outcome & throughput ──────────────────────────────────────────────────────
const outcome = computed(() => {
  return jobOutcome(job.value, rawJobLog.value)
})

const outcomeChipProps = computed(() => {
  switch (outcome.value.state) {
    case 'ok':
      return { color: 'green', textColor: 'white', icon: 'check_circle' }
    case 'warning':
      return { color: 'amber', textColor: 'black', icon: 'warning' }
    case 'failed':
      return { color: 'red', textColor: 'white', icon: 'error' }
    case 'canceled':
      return { color: 'grey', textColor: 'white', icon: 'cancel' }
    case 'running':
      return { color: 'blue', textColor: 'white', icon: 'sync' }
    case 'waiting':
      return { color: 'light-blue', textColor: 'white', icon: 'hourglass_empty' }
    default:
      return { color: 'grey', textColor: 'white', icon: 'help_outline' }
  }
})

const canRestoreFromThisJob = computed(() => {
  if (!job.value) return false
  return canRestoreFromJob(
    { ...job.value, state: outcome.value.state },
    acl.commands
  )
})

const restoreFromJobQuery = computed(() => {
  return buildRestoreFromJobQuery(job.value, currentJobDirector.value)
})

const showReasonLines = computed(() => {
  return (
    (outcome.value.state === 'warning' ||
      outcome.value.state === 'failed' ||
      outcome.value.state === 'canceled') &&
    outcome.value.reasonLines?.length > 0
  )
})

const shownProgress = computed(() => displayProgress({
  job: job.value,
  running: isRunning.value,
  fd: fdRunning.value,
  sd: sdRunning.value,
}))
const displayThroughputFiles = computed(() => shownProgress.value.files)
const displayThroughputBytes = computed(() => shownProgress.value.bytes)

const displayThroughputRate = computed(() => {
  const r = finalRate(job.value)
  if (r == null) return '—'
  return `${formatBytes(r)}/s`
})

async function pollRunningProgress() {
  if (!job.value || !isRunning.value) return
  const jobId = job.value.id
  const ticket = liveProgress.ticket()

  const client = job.value.client
  if (client) {
    try {
      const text = await director.rawCall(`status client=${quoteDirectorString(client)}`)
      if (typeof text === 'string') {
        const sample = parseFdRunning(text, jobId)
        if (liveProgress.accept(ticket, 'fd', sample)) fdRunning.value = sample
      }
    } catch {
      // ignore
    }
  }

  const storage = job.value.storage ?? storageEvents.value?.finalStorage ?? storageEvents.value?.selected
  if (storage) {
    try {
      const text = await director.rawCall(`status storage=${quoteDirectorString(storage)}`)
      if (typeof text === 'string') {
        const sample = parseSdRunning(text, jobId)
        if (liveProgress.accept(ticket, 'sd', sample)) sdRunning.value = sample
      }
    } catch {
      // ignore
    }
  }
}

const progressPoller = createPoller(pollRunningProgress, 5000)

// ── auto-refresh for running jobs ─────────────────────────────────────────────
const isRunning = computed(() => isRunningJobStatus(job.value?.status))

const jobPoller = createPoller(async () => {
  if (!isRunning.value) {
    jobPoller.stop()
    return
  }
  await loadJob()
}, 5000)

watch(isRunning, (running) => {
  if (running) {
    jobPoller.start()
    progressPoller.start()
  } else {
    jobPoller.stop()
    progressPoller.stop()
    resetLive()
  }
}, { immediate: true })

onUnmounted(() => {
  jobPoller.stop()
  progressPoller.stop()
})

// ── actions ───────────────────────────────────────────────────────────────────
function escapeHtml(value) {
  return String(value ?? '')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;')
}

function confirmRerun() {
  $q.dialog({
    title: t('Rerun Job'),
    message: `${t('Rerun job')} <b>${escapeHtml(job.value.name)}</b> (ID&nbsp;${escapeHtml(job.value.id)})?`,
    html: true,
    ok:     { label: t('Rerun'), color: 'primary', flat: true },
    cancel: { label: t('Cancel'), flat: true },
  }).onOk(doRerun)
}

async function doRerun() {
  rerunLoading.value = true
  try {
    const res = await director.call(buildRerunJobCommand(currentJobId.value))
    const newId = res?.run?.jobid ?? res?.jobid ?? null
    $q.notify({ type: 'positive', message: newId ? `${t('Job restarted as ID')} ${newId}.` : t('Job restarted.') })
    if (newId) {
      router.push({
        name: 'job-details',
        params: { id: newId },
        query: buildJobDetailsQuery({
          director: currentJobDirector.value,
          jobsAction: typeof route.query.jobsAction === 'string' ? route.query.jobsAction : '',
          jobsStatus: typeof route.query.jobsStatus === 'string' ? route.query.jobsStatus : '',
          jobsLevel: typeof route.query.jobsLevel === 'string' ? route.query.jobsLevel : '',
          jobsType: typeof route.query.jobsType === 'string' ? route.query.jobsType : '',
          jobsJob: typeof route.query.jobsJob === 'string' ? route.query.jobsJob : '',
          jobsClient: typeof route.query.jobsClient === 'string' ? route.query.jobsClient : '',
          jobsSearch: typeof route.query.jobsSearch === 'string' ? route.query.jobsSearch : '',
          clientName: clientOrigin.value?.name,
          clientDirector: clientOrigin.value?.director,
          clientsTab: clientOrigin.value?.clientsTab,
          clientsScopeDirector: clientOrigin.value?.scopeDirector,
          clientDashboardOrigin: clientOrigin.value?.dashboardOrigin,
          clientJobsAction: clientOrigin.value?.jobsAction,
          clientJobsStatus: clientOrigin.value?.jobsStatus,
          clientJobsLevel: clientOrigin.value?.jobsLevel,
          clientJobsType: clientOrigin.value?.jobsType,
          clientJobsJob: clientOrigin.value?.jobsJob,
          clientJobsClient: clientOrigin.value?.jobsClient,
          clientJobsSearch: clientOrigin.value?.jobsSearch,
          volumeName: volumeOrigin.value?.name,
          volumeDirector: volumeOrigin.value?.director,
          restoreClient: restoreOrigin.value?.client,
          restoreDirector: restoreOrigin.value?.director,
          restoreJobid: restoreOrigin.value?.jobid,
          directorTab: directorOrigin.value?.tab,
          directorTarget: directorOrigin.value?.targetDirector,
          dashboardOrigin: dashboardOrigin.value,
        }),
      })
    }
  } catch (e) {
    $q.notify({ type: 'negative', message: `${t('Rerun failed')}: ${e.message}` })
  } finally {
    rerunLoading.value = false
  }
}

function confirmCancel() {
  $q.dialog({
    title: t('Cancel Job'),
    message: `${t('Cancel job')} <b>${escapeHtml(job.value.name)}</b> (ID&nbsp;${escapeHtml(job.value.id)})?`,
    html: true,
    ok:     { label: t('Cancel Job'), color: 'negative', flat: true },
    cancel: { label: t('Keep Running'), flat: true },
  }).onOk(doCancel)
}

async function doCancel() {
  cancelLoading.value = true
  try {
    await director.call(buildCancelJobCommand(currentJobId.value))
    $q.notify({ type: 'positive', message: t('Job {id} cancelled.', { id: currentJobId.value }) })
    await loadJob()
  } catch (e) {
    $q.notify({ type: 'negative', message: `${t('Cancel failed')}: ${e.message}` })
  } finally {
    cancelLoading.value = false
  }
}

function copyLog() {
  navigator.clipboard?.writeText(jobLog.value)
  $q.notify({ message: t('Log copied to clipboard'), color: 'positive', icon: 'check', timeout: 1500 })
}
</script>

<style scoped>
.job-log {
  font-family: monospace;
  font-size: 0.8rem;
  max-height: 500px;
  overflow-y: auto;
  background: #1e1e1e;
  border-radius: 4px;
}

.log-line {
  white-space: pre-wrap;
  word-break: break-all;
  line-height: 1.55;
  padding: 0 2px;
  border-radius: 2px;
}

.log-line--normal  { color: #d4d4d4; }
.log-line--ok      { color: #89d185; }
.log-line--warning { color: #f2c037; }
.log-line--error   { color: #f48771; font-weight: 600; background: rgba(244, 135, 113, 0.08); }
</style>
