<!--
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
-->
<template>
  <q-page class="q-pa-md">
    <!-- Back -->
    <q-btn
      flat
      no-caps
      icon="arrow_back"
      :label="backLabel"
      class="q-mb-md"
      :to="backLocation"
    />

    <!-- Loading / error -->
    <q-spinner v-if="loading" size="40px" class="block q-mx-auto q-mt-xl" />
    <q-banner v-else-if="error" class="bg-negative text-white q-mb-md">{{ error }}</q-banner>

    <template v-else-if="vol">
      <!-- ── Header ────────────────────────────────────────────────────── -->
      <div class="row items-center q-mb-md q-gutter-sm">
        <q-icon name="mdi-tape-drive" size="2rem" color="primary" />
        <div>
          <div class="row items-center q-gutter-xs">
            <div class="text-h5">{{ vol.volumename }}</div>
            <q-icon
              v-if="hasEncryptionKey"
              name="vpn_key"
              color="amber-8"
            >
              <q-tooltip>{{ t('Encryption key stored in catalog') }}</q-tooltip>
            </q-icon>
          </div>
          <div class="row q-gutter-xs q-mt-xs">
            <q-badge :color="statusColor(vol.volstatus)" :label="vol.volstatus" />
            <q-badge :color="vol.enabled !== '0' && vol.enabled !== false ? 'positive' : 'grey'">
               {{ vol.enabled !== '0' && vol.enabled !== false ? t('Enabled') : t('Disabled') }}
            </q-badge>
            <q-badge v-if="vol.inchanger === '1' || vol.inchanger === true"
                     color="teal" :label="t('In Changer')" />
          </div>
        </div>
        <q-space />
        <div class="row items-center q-gutter-sm">
          <q-btn
            outline
            no-caps
            icon="edit"
            :label="t('Update status')"
            :disable="!canUpdate || actionBusy"
            data-testid="volume-action-update-status"
            @click="openUpdateStatusDialog"
          />
          <q-btn
            outline
            no-caps
            color="warning"
            icon="delete_sweep"
            :label="t('Prune')"
            :disable="!canPrune || actionBusy"
            data-testid="volume-action-prune"
            @click="openConfirmDialog('prune')"
          />
          <q-btn
            outline
            no-caps
            color="negative"
            icon="delete_forever"
            :label="t('Purge')"
            :disable="!canPurge || actionBusy"
            data-testid="volume-action-purge"
            @click="openConfirmDialog('purge')"
          />
          <q-btn
            outline
            no-caps
            color="negative"
            icon="delete"
            :label="t('Delete')"
            :disable="!canDelete || actionBusy"
            data-testid="volume-action-delete"
            @click="openConfirmDialog('delete')"
          />
        </div>
      </div>

      <!-- ── Two-column layout ─────────────────────────────────────────── -->
      <div class="row q-col-gutter-md">

        <!-- Left: details -->
        <div class="col-12 col-md-5">
          <q-card flat bordered class="bareos-panel q-mb-md">
            <q-card-section class="panel-header">{{ t('Volume Properties') }}</q-card-section>
            <q-card-section class="q-pa-none">
              <q-list dense separator>
                <q-item v-for="row in detailRows" :key="row.label">
                  <q-item-section class="text-grey-6" style="max-width:160px">
                    {{ row.label }}
                  </q-item-section>
                  <q-item-section>
                    <router-link v-if="row.link" :to="row.link" class="text-primary">
                      {{ row.value }}
                    </router-link>
                    <component :is="row.component ?? 'span'" v-else-if="row.component"
                               v-bind="row.componentProps" />
                    <template v-else>{{ row.value }}</template>
                  </q-item-section>
                </q-item>
              </q-list>
            </q-card-section>
          </q-card>

          <!-- Gauges card -->
          <q-card flat bordered class="bareos-panel">
            <q-card-section class="panel-header">{{ t('Usage') }}</q-card-section>
            <q-card-section>
              <!-- Used bytes gauge -->
              <div class="q-mb-md">
                <div class="row justify-between text-caption text-grey-6 q-mb-xs">
                  <span>{{ t('Used Bytes') }}</span>
                  <span>{{ formatBytes(vol.volbytes) }}
                    <template v-if="Number(vol.maxvolbytes) > 0">
                      / {{ formatBytes(vol.maxvolbytes) }}
                    </template>
                  </span>
                </div>
                <q-linear-progress
                  :value="bytesRatio"
                  :color="bytesRatio > 0.9 ? 'negative' : bytesRatio > 0.7 ? 'warning' : 'primary'"
                  track-color="grey-3" size="10px" rounded
                />
              </div>

              <!-- Retention gauge -->
              <div class="q-mb-md">
                <div class="row justify-between text-caption text-grey-6 q-mb-xs">
                  <span>{{ t('Retention') }}</span>
                  <span>{{ formatDuration(vol.volretention) }}</span>
                </div>
                <q-linear-progress
                  :value="retentionGauge"
                  color="orange" track-color="grey-3"
                  size="10px" rounded
                />
              </div>

              <!-- Vol jobs / max jobs -->
              <div v-if="Number(vol.maxvoljobs) > 0">
                <div class="row justify-between text-caption text-grey-6 q-mb-xs">
                  <span>{{ t('Jobs on Volume') }}</span>
                  <span>{{ vol.voljobs }} / {{ vol.maxvoljobs }}</span>
                </div>
                <q-linear-progress
                  :value="Number(vol.voljobs) / Number(vol.maxvoljobs)"
                  color="deep-purple" track-color="grey-3"
                  size="10px" rounded
                />
              </div>

              <!-- Job usage breakdown -->
              <div v-if="jobUsageSegments.length" class="q-mt-md">
                <div class="row justify-between text-caption text-grey-6 q-mb-xs">
                  <span>{{ t('Space by Job') }}</span>
                  <span>{{ formatJobCount(jobUsageSegments.length) }}</span>
                </div>
                <!-- stacked bar -->
                <div style="height:14px; display:flex; overflow:hidden; background:#e0e0e0; border-radius:4px">
                  <q-tooltip>
                    <div v-for="seg in jobUsageSegments" :key="seg.jobid" class="text-caption">
                      #{{ seg.jobid }} {{ seg.name }}: {{ formatBytes(seg.jobbytes) }} ({{ seg.pct.toFixed(1) }}%)
                    </div>
                  </q-tooltip>
                  <div
                    v-for="seg in jobUsageSegments"
                    :key="seg.jobid"
                    :style="{ width: seg.width + '%', background: seg.color, flexShrink: 0 }"
                  />
                </div>
                <!-- legend: top 8 jobs -->
                <div class="q-mt-sm">
                  <div
                    v-for="seg in jobUsageSegments.slice(0, 8)"
                    :key="seg.jobid"
                    class="row items-center q-mb-xs text-caption"
                  >
                    <div
                      :style="{ background: seg.color, width: '10px', height: '10px', borderRadius: '2px', flexShrink: 0 }"
                      class="q-mr-sm"
                    />
                    <router-link
                      :to="{
                        name: 'job-details',
                        params: { id: seg.jobid },
                        query: buildVolumeJobDetailsQuery(currentVolumeDirector),
                      }"
                      class="text-primary q-mr-xs"
                      style="white-space:nowrap"
                    >#{{ seg.jobid }}</router-link>
                    <span class="text-grey-7 ellipsis">{{ seg.name }}</span>
                    <q-space />
                    <span class="text-grey-6 q-ml-sm" style="white-space:nowrap">
                      {{ formatBytes(seg.jobbytes) }} ({{ seg.pct.toFixed(1) }}%)
                    </span>
                  </div>
                  <div v-if="jobUsageSegments.length > 8" class="text-caption text-grey-5">
                     + {{ formatMoreJobsCount(jobUsageSegments.length - 8) }}
                  </div>
                </div>
              </div>
            </q-card-section>
          </q-card>
        </div>

        <!-- Right: jobs that used this volume -->
        <div class="col-12 col-md-7">
          <q-card flat bordered class="bareos-panel">
            <q-card-section class="panel-header row items-center">
               <span>{{ t('Jobs using this Volume') }} ({{ jobs.length }})</span>
              <q-space />
              <q-spinner v-if="jobsLoading" size="18px" />
            </q-card-section>
            <q-card-section class="q-pa-none">
              <q-table :rows="jobs" :columns="jobCols" row-key="jobid"
                       dense flat v-model:pagination="jobsPagination">
                <template #body-cell-jobid="props">
                  <q-td :props="props">
                    <router-link
                      :to="{
                        name: 'job-details',
                        params: { id: props.value },
                        query: buildVolumeJobDetailsQuery(props.row.director),
                      }"
                      class="text-primary"
                    >
                      {{ props.value }}
                    </router-link>
                  </q-td>
                </template>
                <template #body-cell-jobstatus="props">
                  <q-td :props="props" class="text-center">
                    <q-badge :color="jobStatusColor(props.value)" :label="props.value" />
                  </q-td>
                </template>
                <template #body-cell-jobbytes="props">
                  <q-td :props="props" class="text-right">
                    {{ formatBytes(props.value) }}
                  </q-td>
                </template>
              </q-table>
            </q-card-section>
          </q-card>
        </div>
      </div>
    </template>

    <!-- Update Status Dialog -->
    <q-dialog v-model="statusDlg.open" data-testid="volume-status-dialog">
      <q-card style="min-width: 360px">
        <q-card-section class="panel-header row items-center">
          <div class="text-h6">{{ t('Update Volume Status') }}</div>
          <q-space />
          <q-btn flat round dense icon="close" :disable="actionBusy" v-close-popup />
        </q-card-section>
        <q-card-section>
          <div class="text-body2 q-mb-md">
            {{ t('Select new status for volume "{name}":', { name: vol?.volumename }) }}
          </div>
          <q-select
            v-model="statusDlg.status"
            :options="VOLUME_STATUS_OPTIONS"
            :label="t('Volume status')"
            outlined
            dense
            data-testid="volume-status-select"
          />
        </q-card-section>
        <q-card-actions align="right">
          <q-btn flat no-caps :label="t('Cancel')" :disable="actionBusy" v-close-popup />
          <q-btn
            color="primary"
            no-caps
            :label="t('Update')"
            :loading="actionBusy"
            :disable="!statusDlg.status || actionBusy"
            data-testid="volume-status-submit"
            @click="executeUpdateStatus"
          />
        </q-card-actions>
      </q-card>
    </q-dialog>

    <!-- Destructive Action Confirmation Dialog -->
    <q-dialog v-model="confirmDlg.open" persistent data-testid="volume-confirm-dialog">
      <q-card style="min-width: 400px; max-width: 600px">
        <q-card-section class="panel-header row items-center">
          <div class="text-h6">{{ confirmDlg.title }}</div>
          <q-space />
          <q-btn flat round dense icon="close" :disable="actionBusy" v-close-popup />
        </q-card-section>
        <q-card-section>
          <div class="text-body1 q-mb-sm">{{ confirmDlg.message }}</div>
          <div class="text-caption text-negative">{{ confirmDlg.lostText }}</div>
        </q-card-section>
        <q-card-actions align="right">
          <q-btn flat no-caps :label="t('Cancel')" :disable="actionBusy" v-close-popup />
          <q-btn
            :color="confirmDlg.color"
            no-caps
            :label="confirmDlg.confirmLabel"
            :loading="actionBusy"
            :disable="actionBusy"
            data-testid="volume-confirm-submit"
            @click="executeConfirmedAction"
          />
        </q-card-actions>
      </q-card>
    </q-dialog>

    <!-- Director Reply Dialog -->
    <q-dialog v-model="replyDlg.open" persistent data-testid="director-reply-dialog">
      <q-card style="min-width: 480px; max-width: 800px">
        <q-card-section class="panel-header row items-center">
          <div class="text-h6">{{ replyDlg.title }}</div>
          <q-space />
          <q-btn flat round dense icon="close" @click="closeReplyDialog" />
        </q-card-section>
        <q-card-section>
          <pre class="console-output" data-testid="director-reply-text">{{ replyDlg.text }}</pre>
        </q-card-section>
        <q-card-actions align="right">
          <q-btn flat no-caps :label="t('Close')" color="primary" @click="closeReplyDialog" />
        </q-card-actions>
      </q-card>
    </q-dialog>
  </q-page>
</template>

<script setup>
import { ref, computed, watch, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { useRoute, useRouter } from 'vue-router'
import { switchActiveDirector } from '../composables/useDirectorSession.js'
import { useAuthStore } from '../stores/auth.js'
import { useDirectorStore } from '../stores/director.js'
import { useDirectorAclStore } from '../stores/directorAcl.js'
import { useSettingsStore } from '../stores/settings.js'
import { usePersistedTablePagination } from '../composables/usePersistedTablePagination.js'
import { actionAllowed } from '../utils/acl.js'
import { quoteDirectorString } from '../utils/directorStrings.js'
import { buildDirectorPageQuery } from '../utils/director.js'
import { buildJobDetailsQuery, resolveJobDetailsQuery } from '../utils/jobs.js'
import { formatBytes, formatDuration } from '../mock/index.js'
import { formatNumber } from '../utils/locales.js'
import { buildPoolDetailsQuery } from '../utils/pools.js'
import {
  VOLUME_STATUS_OPTIONS,
  buildDeleteVolumeCommand,
  buildPruneVolumeCommand,
  buildPurgeVolumeCommand,
  buildUpdateVolumeStatusCommand,
  captureVolumeActionTarget,
  formatDirectorReply,
  parseDirectorActionReply,
} from '../utils/volumeActions.js'
import { createActionTokenGuard } from '../utils/actionToken.js'
import {
  buildAutochangerSelectionQuery,
  buildStoragesTabQuery,
  resolveAutochangerSelectionQuery,
  withStoragesScopeDirectorQuery,
} from '../utils/storagesRoute.js'
import {
  resolveVolumeDetailsDirectorOrigin,
  resolveVolumeDetailsJobOrigin,
  resolveVolumeDetailsPoolOrigin,
  resolveVolumeDetailsStoragesOrigin,
  volumeHasEncryptionKey,
} from '../utils/volumes.js'

const route      = useRoute()
const router     = useRouter()
const auth       = useAuthStore()
const director   = useDirectorStore()
const acl        = useDirectorAclStore()
const settings   = useSettingsStore()
const { t } = useI18n()
const actionBusy = ref(false)

const canUpdate = computed(() => actionAllowed(acl.commands, 'update'))
const canPrune  = computed(() => actionAllowed(acl.commands, 'prune'))
const canPurge  = computed(() => actionAllowed(acl.commands, 'purge'))
const canDelete = computed(() => actionAllowed(acl.commands, 'delete'))

onMounted(() => {
  acl.ensureLoaded().catch(() => {})
})

const statusDlg = ref({
  open: false,
  status: 'Append',
})

const confirmDlg = ref({
  open: false,
  actionType: '',
  target: null,
  token: null,
  title: '',
  message: '',
  lostText: '',
  confirmLabel: '',
  color: 'negative',
})

const replyDlg = ref({
  open: false,
  title: '',
  text: '',
})

// One token per confirmation; a scope or Director change invalidates it for good.
const confirmGuard = createActionTokenGuard()
const statusGuard = createActionTokenGuard()
const activeDirectorName = () => auth.user?.director || settings.directorName || ''
const volumeScope = () => `${volumeName.value}\u0000${currentVolumeDirector.value}`

async function closeReplyDialog() {
  const { navigateTo, token } = replyDlg.value
  replyDlg.value.open = false
  replyDlg.value.navigateTo = null
  if (navigateTo && confirmGuard.isCurrent(token, volumeScope())) await router.push(navigateTo)
}

function openUpdateStatusDialog() {
  statusDlg.value = {
    open: true,
    status: vol.value?.volstatus && VOLUME_STATUS_OPTIONS.includes(vol.value.volstatus)
      ? vol.value.volstatus
      : 'Append',
  }
}

async function executeUpdateStatus() {
  if (!statusDlg.value.status || actionBusy.value || !canUpdate.value) return
  const target = captureVolumeActionTarget(volumeName.value, currentVolumeDirector.value, activeDirectorName())
  const token = statusGuard.open(target, volumeScope(), activeDirectorName())
  const status = statusDlg.value.status
  const isCurrent = () => statusGuard.isCurrent(token, volumeScope())
  actionBusy.value = true
  try {
    await ensureVolumeDirector()
    if (!canUpdate.value || !statusGuard.canWrite(token, volumeScope(), activeDirectorName())) return
    const cmd = buildUpdateVolumeStatusCommand(token.target.volume, status)
    statusGuard.consume(token)
    const reply = await director.call(cmd)
    const result = parseDirectorActionReply(reply)
    statusDlg.value.open = false
    replyDlg.value = {
      open: true,
      title: result.ok ? t('Update Volume Status') : t('Update Status Failed'),
      text: result.text,
    }
    if (result.ok && isCurrent()) await loadVolume()
  } catch (err) {
    replyDlg.value = {
      open: true,
      title: t('Update Status Failed'),
      text: formatDirectorReply(err),
    }
  } finally {
    actionBusy.value = false
  }
}

function openConfirmDialog(actionType) {
  if (actionBusy.value || !({ prune: canPrune.value, purge: canPurge.value, delete: canDelete.value })[actionType]) return
  const target = captureVolumeActionTarget(volumeName.value, currentVolumeDirector.value, activeDirectorName())
  const token = confirmGuard.open(target, volumeScope(), activeDirectorName())
  const name = target.volume
  if (actionType === 'prune') {
    confirmDlg.value = {
      open: true,
      actionType: 'prune',
      target,
      token,
      title: t('Prune Volume'),
      message: t('Are you sure you want to prune volume "{name}"?', { name }),
      lostText: t('Catalog records for expired jobs on this volume will be removed.'),
      confirmLabel: t('Prune'),
      color: 'warning',
    }
  } else if (actionType === 'purge') {
    confirmDlg.value = {
      open: true,
      actionType: 'purge',
      target,
      token,
      title: t('Purge Volume'),
      message: t('Are you sure you want to purge volume "{name}"?', { name }),
      lostText: t('All catalog job and file records for this volume will be permanently removed, making its data inaccessible.'),
      confirmLabel: t('Purge'),
      color: 'negative',
    }
  } else if (actionType === 'delete') {
    confirmDlg.value = {
      open: true,
      actionType: 'delete',
      target,
      token,
      title: t('Delete Volume'),
      message: t('Are you sure you want to delete volume "{name}"?', { name }),
      lostText: t('The volume and its records will be permanently deleted from the catalog.'),
      confirmLabel: t('Delete'),
      color: 'negative',
    }
  }
}

async function executeConfirmedAction() {
  const type = confirmDlg.value.actionType
  const token = confirmDlg.value.token
  const isCurrent = () => confirmDlg.value.open && confirmGuard.isCurrent(token, volumeScope())
  const permitted = () => ({ prune: canPrune.value, purge: canPurge.value, delete: canDelete.value })[type] === true
  if (!type || actionBusy.value || !isCurrent() || !permitted()) return
  const target = token.target
  actionBusy.value = true
  try {
    await ensureVolumeDirector()
    if (!isCurrent() || !permitted() || !confirmGuard.canWrite(token, volumeScope(), activeDirectorName())) return
    let cmd = ''
    let title = ''
    if (type === 'prune') {
      cmd = buildPruneVolumeCommand(target.volume)
      title = t('Prune Volume')
    } else if (type === 'purge') {
      cmd = buildPurgeVolumeCommand(target.volume)
      title = t('Purge Volume')
    } else if (type === 'delete') {
      cmd = buildDeleteVolumeCommand(target.volume)
      title = t('Delete Volume')
    }
    confirmGuard.consume(token)
    const reply = await director.call(cmd)
    if (!isCurrent()) return
    const result = parseDirectorActionReply(reply)
    confirmDlg.value.open = false
    replyDlg.value = {
      open: true,
      title: result.ok ? title : t('Action Failed'),
      text: result.text,
      token,
      navigateTo: result.ok && type === 'delete' ? backLocation.value : null,
    }
    if (result.ok && type !== 'delete') {
      await loadVolume()
    }
  } catch (err) {
    if (!isCurrent()) return
    replyDlg.value = {
      open: true,
      title: t('Action Failed'),
      text: formatDirectorReply(err),
    }
  } finally {
    actionBusy.value = false
  }
}

const jobsPagination = usePersistedTablePagination('volume-details.jobs', {
  rowsPerPage: 15,
  sortBy: 'jobid',
  descending: true,
})
const volumeName = computed(() => route.params.name)
const requestedDirector = computed(() => (
  typeof route.query.director === 'string' ? route.query.director : ''
))
const currentVolumeDirector = computed(() => (
  requestedDirector.value || auth.user?.director || settings.directorName || ''
))

// A change of volume or Director invalidates a pending confirmation for good;
// only the page switching to the target's own Director leaves it valid.
watch([volumeName, currentVolumeDirector, activeDirectorName], () => {
  confirmGuard.observe(volumeScope(), activeDirectorName())
  statusGuard.observe(volumeScope(), activeDirectorName())
  if (confirmGuard.isCurrent(confirmDlg.value.token, volumeScope())) return
  confirmDlg.value.open = false
  confirmDlg.value.target = null
  confirmDlg.value.token = null
}, { flush: 'sync' })
const autochangerOrigin = computed(() => resolveAutochangerSelectionQuery(route.query))
const jobOrigin = computed(() => resolveVolumeDetailsJobOrigin(route.query))
const poolOrigin = computed(() => resolveVolumeDetailsPoolOrigin(route.query))
const storagesOrigin = computed(() => resolveVolumeDetailsStoragesOrigin(route.query))
const directorOrigin = computed(() => resolveVolumeDetailsDirectorOrigin(route.query))
const backLabel = computed(() => {
  if (jobOrigin.value) {
    return t('Back to Job')
  }

  if (poolOrigin.value) {
    return t('Back to Pool')
  }

  if (autochangerOrigin.value) {
    return t('Back to Autochanger')
  }

  if (storagesOrigin.value) {
    return t('Back to Storages')
  }

  return directorOrigin.value ? t('Back to Director') : t('Volumes')
})
const backLocation = computed(() => {
  if (jobOrigin.value) {
    return {
      name: 'job-details',
      params: { id: jobOrigin.value.id },
      query: resolveJobDetailsQuery(route.query),
    }
  }

  if (poolOrigin.value) {
    return {
      name: 'pool-details',
      params: { name: poolOrigin.value.name },
      query: buildPoolDetailsQuery({
        director: requestedDirector.value,
        poolName: poolOrigin.value.name,
        volumeQuery: route.query,
      }),
    }
  }

  if (autochangerOrigin.value) {
    return {
      name: 'storages',
      query: buildAutochangerSelectionQuery({}, autochangerOrigin.value),
    }
  }

  if (storagesOrigin.value) {
    return {
      name: 'storages',
      query: withStoragesScopeDirectorQuery(
        buildStoragesTabQuery({}, storagesOrigin.value.tab),
        storagesOrigin.value.scopeDirector
      ),
    }
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

  return {
    name: 'storages',
    query: { tab: 'volumes' },
  }
})

function buildVolumeJobDetailsQuery(jobDirector) {
  return buildJobDetailsQuery({
    director: jobDirector,
    volumeName: volumeName.value,
    volumeDirector: currentVolumeDirector.value,
  })
}

const vol         = ref(null)
const jobs        = ref([])
const loading     = ref(true)
const jobsLoading = ref(false)
const error       = ref(null)
function formatJobCount(count) {
  return `${formatNumber(count, settings.locale)} ${t('job(s)')}`
}

function formatMoreJobsCount(count) {
  return `${formatNumber(count, settings.locale)} ${t('more jobs')}`
}

// Retention reference scale: 1 year in seconds
const ONE_YEAR_S = 365 * 24 * 3600

const RETENTION_STEPS = [
  7 * 24 * 3600,       // 1 week
  30 * 24 * 3600,      // 1 month
  90 * 24 * 3600,      // 3 months
  365 * 24 * 3600,     // 1 year
  3 * 365 * 24 * 3600, // 3 years
]

async function ensureVolumeDirector() {
  if (!requestedDirector.value) {
    return
  }

  if (auth.user?.director === requestedDirector.value && director.isConnected) {
    return
  }

  // The command list belongs to the Director; reload it after switching.
  if (await switchActiveDirector(requestedDirector.value)) await acl.refresh()
}

async function loadVolume() {
  await ensureVolumeDirector()

  const [volRes, jobRes] = await Promise.all([
    director.call(`llist volume=${quoteDirectorString(volumeName.value)}`),
    fetchJobs(),
  ])
  const raw = volRes?.volumes ?? volRes?.volume ?? null
  if (Array.isArray(raw)) {
    vol.value = raw[0] ?? null
  } else {
    vol.value = raw
  }
  if (vol.value) {
    vol.value = {
      ...vol.value,
      director: currentVolumeDirector.value || null,
    }
  }
  jobs.value = jobRes
}

watch(() => `${volumeName.value}\u0000${requestedDirector.value}`, async () => {
  loading.value = true
  error.value = null
  vol.value = null
  jobs.value = []
  try {
    await loadVolume()
  } catch (loadError) {
    error.value = loadError.message
  } finally {
    loading.value = false
  }
}, { immediate: true })

async function fetchJobs() {
  jobsLoading.value = true
  try {
    const res = await director.call(`llist jobs volume=${quoteDirectorString(volumeName.value)}`)
    const raw = res?.jobs ?? []
    const rows = Array.isArray(raw) ? raw : Object.values(raw).flat()
    return rows.map(job => ({
      ...job,
      director: currentVolumeDirector.value || null,
    }))
  } catch {
    return []
  } finally {
    jobsLoading.value = false
  }
}

// ── Computed ──────────────────────────────────────────────────────────────────

const bytesRatio = computed(() => {
  if (!vol.value) return 0
  const used = Number(vol.value.volbytes) || 0
  const max  = Number(vol.value.maxvolbytes)
  if (max > 0) return Math.min(1, used / max)
  // No cap: show relative to 1 TiB as 100%
  return Math.min(1, used / (1024 ** 4))
})

const retentionGauge = computed(() => {
  if (!vol.value) return 0
  const secs = Number(vol.value.volretention) || 0
  // Find smallest reference step larger than this value, or use 3 years
  const ref = RETENTION_STEPS.find(s => s >= secs) ?? RETENTION_STEPS[RETENTION_STEPS.length - 1]
  return Math.min(1, secs / ref)
})

const hasEncryptionKey = computed(() => volumeHasEncryptionKey(vol.value))

const detailRows = computed(() => {
  if (!vol.value) return []
  const v = vol.value
  return [
    { label: t('Media ID'), value: v.mediaid ?? v.mediaId ?? '—' },
    {
      label: t('Pool'),
      value: v.pool ?? v.Pool ?? '—',
      link: v.pool
        ? {
          name: 'pool-details',
          params: { name: v.pool },
          query: buildPoolDetailsQuery({
            director: currentVolumeDirector.value,
            volumeName: volumeName.value,
            volumeQuery: route.query,
          }),
        }
        : null,
    },
    { label: t('Storage'),     value: v.storage ?? v.storagename ?? '—' },
    { label: t('Media Type'),  value: v.mediatype ?? v.MediaType ?? '—' },
    { label: t('Encryption Key'), value: hasEncryptionKey.value ? t('Present') : '—' },
    { label: t('Slot'),        value: v.slot ?? '0' },
    { label: t('Label Date'),  value: v.labeldate ?? '—' },
    { label: t('First Written'), value: v.firstwritten ?? '—' },
    { label: t('Last Written'),  value: v.lastwritten ?? '—' },
    { label: t('Jobs on Vol'), value: `${v.voljobs ?? 0}${Number(v.maxvoljobs) > 0 ? ` / ${v.maxvoljobs}` : ''}` },
    { label: t('Files on Vol'), value: v.volfiles ?? '0' },
    { label: t('Blocks on Vol'), value: v.volblocks ?? '0' },
    { label: t('Vol Writes'), value: v.volwrites ?? '0' },
    { label: t('Vol Reads'), value: v.volreads ?? '0' },
    { label: t('Read Time'), value: v.volreadtime ? formatDuration(v.volreadtime) : '—' },
    { label: t('Write Time'), value: v.volwritetime ? formatDuration(v.volwritetime) : '—' },
    { label: t('Used Bytes'), value: formatBytes(v.volbytes) },
    { label: t('Max Bytes'), value: Number(v.maxvolbytes) > 0 ? formatBytes(v.maxvolbytes) : '∞' },
    { label: t('Retention'), value: formatDuration(v.volretention) },
    { label: t('Use Duration'), value: Number(v.voluseduration) > 0 ? formatDuration(v.voluseduration) : '—' },
    { label: t('Max Jobs/Vol'), value: Number(v.maxvoljobs) > 0 ? v.maxvoljobs : '∞' },
    { label: t('Max Files/Vol'), value: Number(v.maxvolfiles) > 0 ? v.maxvolfiles : '∞' },
    { label: t('Recycle'), value: v.recycle === '1' ? t('Yes') : t('No') },
    { label: t('Auto Prune'), value: v.autoprune === '1' ? t('Yes') : t('No') },
    { label: t('Recycle Count'), value: v.recyclecount ?? '0' },
    { label: t('Comment'), value: v.comment || '—' },
  ].filter(r => r.value !== '—' || r.label === t('Storage') || r.label === t('Comment'))
})

// ── Jobs table ────────────────────────────────────────────────────────────────

const JOB_COLORS = [
  '#1976D2', '#388E3C', '#F57C00', '#7B1FA2', '#C62828',
  '#00838F', '#558B2F', '#EF6C00', '#6A1B9A', '#AD1457',
  '#00695C', '#283593', '#E65100', '#4527A0', '#2E7D32',
]

const jobUsageSegments = computed(() => {
  if (!jobs.value.length || !vol.value) return []
  const totalBytes = Number(vol.value.volbytes) || 1
  const sorted = [...jobs.value]
    .filter(j => Number(j.jobbytes) > 0)
    .sort((a, b) => Number(b.jobbytes) - Number(a.jobbytes))
  // Normalize widths: each segment is its share of the total volume bytes,
  // capped so the sum never exceeds 100 % in the bar.
  let remaining = 100
  return sorted.map((j, i) => {
    const pct = (Number(j.jobbytes) / totalBytes) * 100
    const width = Math.min(pct, remaining)
    remaining = Math.max(0, remaining - width)
    return {
      jobid:    j.jobid,
      name:     j.name,
      jobbytes: Number(j.jobbytes),
      pct,
      width,
      color: JOB_COLORS[i % JOB_COLORS.length],
    }
  })
})

const jobCols = computed(() => [
  { name: 'jobid',      label: t('ID'),       field: 'jobid',      align: 'right',  sortable: true },
  { name: 'name',       label: t('Job Name'), field: 'name',       align: 'left',   sortable: true },
  { name: 'client',     label: t('Client'),   field: 'client',     align: 'left',   sortable: true },
  { name: 'level',      label: t('Level'),    field: 'level',      align: 'center', sortable: true },
  { name: 'starttime',  label: t('Start'),    field: 'starttime',  align: 'left',   sortable: true },
  { name: 'jobstatus',  label: t('Status'),   field: 'jobstatus',  align: 'center', sortable: true },
  { name: 'jobbytes',   label: t('Bytes'),    field: 'jobbytes',   align: 'right',  sortable: true },
])

// ── Color helpers ─────────────────────────────────────────────────────────────

function statusColor(s) {
  return {
    Full: 'warning', Append: 'positive', Recycled: 'grey', Error: 'negative',
    Purged: 'grey', Used: 'orange', 'Read-Only': 'blue-grey', Cleaning: 'teal',
  }[s] ?? 'info'
}

function jobStatusColor(s) {
  return {
    T: 'positive', W: 'warning', E: 'negative', f: 'negative',
    R: 'blue', C: 'blue', A: 'warning', e: 'negative',
  }[s] ?? 'grey'
}
</script>
