<template>
  <q-page class="q-pa-md">
    <DirectorErrorsBanner :errors="directorErrors" />

    <!-- Page Header and Actions -->
    <div class="row items-center justify-between q-mb-md">
      <div class="text-h5">{{ t('Storage Groups') }}</div>
      <q-btn
        flat
        round
        dense
        icon="refresh"
        color="primary"
        :loading="loadingData"
        @click="refreshAll"
      >
        <q-tooltip>{{ t('Refresh') }}</q-tooltip>
      </q-btn>
    </div>

    <!-- Error Banner -->
    <q-banner v-if="pageError" dense class="bg-negative text-white q-mb-md rounded-borders">
      {{ pageError }}
    </q-banner>

    <!-- Filters Section -->
    <div class="row items-center q-col-gutter-md q-mb-md">
      <div class="col-12 col-sm-6 col-md-4">
        <q-input
          v-model="filterText"
          dense
          outlined
          clearable
          :placeholder="t('Filter storage groups…')"
          data-testid="storage-groups-filter"
        >
          <template #prepend>
            <q-icon name="search" />
          </template>
        </q-input>
      </div>
      <div class="col-12 col-sm-6 col-md-4">
        <q-toggle
          v-model="problemsOnly"
          :label="t('Problems only')"
          data-testid="problems-only-toggle"
        />
      </div>
    </div>

    <!-- Storage Groups Cards -->
    <div v-if="loadingData && allGroups.length === 0" class="row justify-center q-my-xl">
      <q-spinner color="primary" size="40px" />
    </div>

    <div v-else-if="shownGroups.length === 0" class="text-grey-6 q-my-lg">
      {{ t('No storage groups match the current filter.') }}
    </div>

    <div v-else class="q-gutter-y-md q-mb-lg">
      <q-card
        v-for="group in shownGroups"
        :key="group.key"
        flat
        bordered
        class="bareos-panel"
        data-testid="storage-group-card"
      >
        <q-card-section class="panel-header row items-center">
          <q-icon name="hub" class="q-mr-sm" size="sm" />
          <span class="text-weight-bold">{{ group.ownerType }} {{ group.ownerName }}</span>
        </q-card-section>

        <q-card-section class="q-gutter-y-sm">
          <!-- Members list as ordered chips -->
          <div class="row items-center q-gutter-xs">
            <span class="text-caption text-weight-bold text-grey-7 q-mr-xs">{{ t('Members') }}:</span>
            <q-chip
              v-for="(member, idx) in group.members"
              :key="member"
              clickable
              dense
              :color="isStorageEnabled(member) ? 'positive' : 'grey'"
              text-color="white"
              data-testid="member-chip"
            >
              {{ idx + 1 }}. {{ formatMemberLabel(member) }}
              <q-menu auto-close>
                <q-list dense style="min-width: 240px">
                  <q-item
                    clickable
                    :disable="!allowed('disable')"
                    data-testid="member-disable-now"
                    @click="runtimeEnable(member, false)"
                  >
                    <q-item-section>{{ t('Disable now (until reload)') }}</q-item-section>
                    <q-tooltip v-if="!allowed('disable')">{{ t('Not allowed for this console') }}</q-tooltip>
                  </q-item>
                  <q-item
                    clickable
                    :disable="!allowed('enable')"
                    data-testid="member-enable-now"
                    @click="runtimeEnable(member, true)"
                  >
                    <q-item-section>{{ t('Enable now') }}</q-item-section>
                    <q-tooltip v-if="!allowed('enable')">{{ t('Not allowed for this console') }}</q-tooltip>
                  </q-item>
                  <q-separator />
                  <q-item
                    clickable
                    :disable="!allowed('configure')"
                    data-testid="member-disable-config"
                    @click="openEnabledDialog(member, 'no')"
                  >
                    <q-item-section>{{ t('Disable in configuration') }}</q-item-section>
                    <q-tooltip v-if="!allowed('configure')">{{ t('Not allowed for this console') }}</q-tooltip>
                  </q-item>
                  <q-item
                    clickable
                    :disable="!allowed('configure')"
                    data-testid="member-enable-config"
                    @click="openEnabledDialog(member, 'yes')"
                  >
                    <q-item-section>{{ t('Enable in configuration') }}</q-item-section>
                    <q-tooltip v-if="!allowed('configure')">{{ t('Not allowed for this console') }}</q-tooltip>
                  </q-item>
                </q-list>
              </q-menu>
            </q-chip>
          </div>

          <!-- Policy line -->
          <div class="text-body2">
            <span class="text-weight-bold">{{ t('Policy') }}:</span>
            {{ group.policy }} ({{ group.policySource === 'default' ? t('default') : `${t('from')} ${group.policySource}` }})
          </div>
          <div
            v-for="override in group.policyOverrides"
            :key="override.job"
            class="text-caption text-grey-7"
            data-testid="policy-override"
          >
            {{ override.job }}: {{ t('effective policy') }} {{ override.policy }}
            ({{ override.policySource === 'default' ? t('default') : `${t('from')} ${override.policySource}` }})
          </div>

          <div class="text-body2 text-grey-8" data-testid="applies-to-jobs">
            {{ t('A change here applies to {n} jobs', { n: group.jobs.length }) }}
          </div>

          <!-- Associated Jobs as chips linking to Jobs page -->
          <div class="row items-center q-gutter-xs" v-if="group.jobs.length">
            <span class="text-caption text-weight-bold text-grey-7 q-mr-xs">{{ t('Jobs') }}:</span>
            <span v-for="jobName in group.jobs" :key="jobName" class="row items-center no-wrap">
              <router-link
                :to="{ path: '/jobs', query: { search: jobName } }"
                class="text-decoration-none"
              >
                <q-chip clickable dense outline color="primary" icon="work" :label="jobName" />
              </router-link>
              <q-btn
                flat
                round
                dense
                size="sm"
                icon="play_arrow"
                color="positive"
                :loading="runningJob === jobName"
                :disable="!allowed('run') || Boolean(runningJob)"
                :data-testid="`run-now-${jobName}`"
                @click="runNow(jobName)"
              >
                <q-tooltip>
                  {{ allowed('run') ? t('Run now') : t('Not allowed for this console') }}
                </q-tooltip>
              </q-btn>
            </span>
          </div>
          <q-banner
            v-for="notice in jobNotices(group)"
            :key="notice.job"
            dense
            class="rounded-borders text-white"
            :class="notice.ok ? 'bg-positive' : 'bg-negative'"
          >
            {{ notice.text }}
            <router-link
              v-if="notice.jobId"
              :to="{ name: 'job-details', params: { id: notice.jobId } }"
              class="text-white text-weight-bold q-ml-sm"
            >
              #{{ notice.jobId }}
            </router-link>
          </q-banner>

          <q-banner v-if="memberNotice && memberNotice.group === group.key" dense
            class="rounded-borders text-white" :class="memberNotice.ok ? 'bg-positive' : 'bg-negative'">
            {{ memberNotice.text }}
          </q-banner>

          <div class="group-actions row items-center q-gutter-sm">
            <q-select
              :model-value="policyChoice[group.key] ?? group.policy" @update:model-value="policyChoice[group.key] = $event"
              :options="policyOptions"
              dense
              outlined
              style="min-width: 150px"
              :disable="!allowed('configure')"
              data-testid="policy-select"
            />
            <q-btn
              color="primary"
              no-caps
              dense
              class="q-px-sm"
              :label="t('Change policy')"
              :disable="!allowed('configure') || (policyChoice[group.key] ?? group.policy) === group.policy"
              data-testid="change-policy"
              @click="openPolicyDialog(group)"
            >
              <q-tooltip v-if="!allowed('configure')">{{ t('Not allowed for this console') }}</q-tooltip>
            </q-btn>
            <q-btn
              outline
              color="primary"
              no-caps
              dense
              class="q-px-sm"
              :label="t('Change members')"
              :disable="!allowed('configure')"
              data-testid="change-members"
              @click="toggleMemberEditor(group)"
            >
              <q-tooltip v-if="!allowed('configure')">{{ t('Not allowed for this console') }}</q-tooltip>
            </q-btn>
          </div>

          <div v-if="editor && editor.group === group.key" class="member-editor" data-testid="member-editor">
            <q-list dense bordered separator class="rounded-borders">
              <q-item v-for="(name, idx) in editor.list" :key="name">
                <q-item-section>{{ idx + 1 }}. {{ name }}</q-item-section>
                <q-item-section side>
                  <div class="row no-wrap">
                    <q-btn flat round dense size="sm" icon="arrow_upward" :disable="idx === 0"
                      :data-testid="`member-up-${name}`" @click="editor.list = moveMember(editor.list, idx, -1)" />
                    <q-btn flat round dense size="sm" icon="arrow_downward" :disable="idx === editor.list.length - 1"
                      :data-testid="`member-down-${name}`" @click="editor.list = moveMember(editor.list, idx, 1)" />
                    <q-btn flat round dense size="sm" icon="delete" color="negative" :disable="editor.list.length <= 1"
                      :data-testid="`member-remove-${name}`" @click="editor.list = removeMember(editor.list, idx)" />
                  </div>
                </q-item-section>
              </q-item>
            </q-list>
            <div class="row items-center q-gutter-sm q-mt-sm">
              <q-select
                v-model="editor.toAdd"
                :options="addableMembers(allStorageNames, editor.list)"
                :label="t('Add storage')"
                dense
                outlined
                clearable
                style="min-width: 200px"
                data-testid="member-add-select"
              />
              <q-btn flat dense no-caps color="primary" icon="add" :label="t('Add')" :disable="!editor.toAdd"
                data-testid="member-add" @click="editor.list = addMember(editor.list, editor.toAdd); editor.toAdd = null" />
              <q-space />
              <q-btn flat dense no-caps :label="t('Cancel')" @click="editor = null" />
              <q-btn color="primary" dense no-caps class="q-px-sm" :label="t('Preview change')"
                :disable="!membersChanged(group.members, editor.list)" data-testid="member-preview"
                @click="openMembersDialog(group)" />
            </div>
          </div>
        </q-card-section>
      </q-card>
    </div>

    <!-- Tabs under the cards -->
    <q-card flat bordered class="bareos-panel q-mt-lg">
      <q-tabs
        v-model="activeTab"
        dense
        align="left"
        class="panel-header text-white"
        active-color="amber-4"
        indicator-color="amber-4"
      >
        <q-tab name="volumes" :label="t('Volumes per member')" data-testid="tab-volumes" />
        <q-tab name="jobs" :label="t('Jobs per member')" data-testid="tab-jobs" />
      </q-tabs>

      <q-separator />

      <q-tab-panels v-model="activeTab" animated class="q-pa-none">
        <!-- Tab: Volumes per member -->
        <q-tab-panel name="volumes" class="q-pa-none">
          <q-table
            :rows="volumesFiltered"
            :columns="volumeColumns"
            row-key="volumename"
            dense
            flat
            :loading="loadingVolumes"
            :pagination="volumesPagination"
            data-testid="volumes-per-member-table"
          >
            <template #body-cell-volumename="props">
              <q-td :props="props">
                <router-link
                  :to="{ name: 'volume-details', params: { name: props.row.volumename } }"
                  class="text-primary text-weight-bold"
                >
                  {{ props.value }}
                </router-link>
              </q-td>
            </template>
            <template #body-cell-bytes="props">
              <q-td :props="props" class="text-right">
                {{ formatBytes(props.value) }}
              </q-td>
            </template>
          </q-table>
        </q-tab-panel>

        <!-- Tab: Jobs per member -->
        <q-tab-panel name="jobs" class="q-pa-md">
          <div class="row items-center q-gutter-md q-mb-md">
            <div class="text-caption text-weight-bold text-grey-7">{{ t('Select Member') }}:</div>
            <q-select
              v-model="selectedMember"
              :options="allMemberOptions"
              outlined
              dense
              style="min-width: 220px"
              data-testid="jobs-member-select"
            />
          </div>

          <q-table
            :rows="memberJobs"
            :columns="jobColumns"
            row-key="id"
            dense
            flat
            :loading="loadingMemberJobs"
            :pagination="jobsPagination"
            data-testid="jobs-per-member-table"
          >
            <template #body-cell-jobid="props">
              <q-td :props="props">
                <router-link
                  :to="{ name: 'job-details', params: { id: props.value } }"
                  class="text-primary text-weight-bold"
                >
                  #{{ props.value }}
                </router-link>
              </q-td>
            </template>
            <template #body-cell-status="props">
              <q-td :props="props" class="text-center">
                <JobStatusBadge :status="props.value" />
              </q-td>
            </template>
            <template #body-cell-bytes="props">
              <q-td :props="props" class="text-right">
                {{ formatBytes(props.value) }}
              </q-td>
            </template>
          </q-table>
        </q-tab-panel>
      </q-tab-panels>
    </q-card>
    <ConfigureChangeDialog
      v-model="dialog.open"
      :title="dialog.title"
      kind="set"
      :build-command="dialog.build"
      @changed="onConfigChanged"
    />
  </q-page>
</template>

<script setup>
import { ref, computed, watch, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { useDirectorStore } from '../stores/director.js'
import { useDirectorAclStore } from '../stores/directorAcl.js'
import { directorCollection, normaliseVolume, normaliseJob } from '../composables/useDirectorFetch.js'
import { listGroups } from '../utils/storageGroupList.js'
import { quoteDirectorString } from '../utils/directorStrings.js'
import { formatBytes } from '../mock/index.js'
import JobStatusBadge from '../components/JobStatusBadge.vue'
import DirectorErrorsBanner from '../components/DirectorErrorsBanner.vue'
import ConfigureChangeDialog from '../components/ConfigureChangeDialog.vue'
import { actionAllowed } from '../utils/acl.js'
import { buildConfigureSetCommand, buildStorageEnabledCommand } from '../utils/storageGroups.js'
import { buildRunJobCommand } from '../utils/jobs.js'
import {
  moveMember, removeMember, addMember, membersChanged, addableMembers, ownerResourceType,
} from '../utils/memberEditor.js'

const { t } = useI18n()
const director = useDirectorStore()
const acl = useDirectorAclStore()

const directorErrors = ref([])
const pageError = ref(null)
const loadingData = ref(false)
const loadingVolumes = ref(false)
const loadingMemberJobs = ref(false)

const allGroups = ref([])
const storagesMap = ref({})
const enabledMap = ref(new Map())
const allVolumes = ref([])
const memberJobs = ref([])

const allStorageNames = ref([])
const policyOptions = ['ListedOrder', 'LeastUsed']
const policyChoice = ref({})
const editor = ref(null)
const dialog = ref({ open: false, title: '', build: () => '' })
const runningJob = ref('')
const runNotices = ref({})
const memberNotice = ref(null)

const filterText = ref('')
const problemsOnly = ref(false)
const activeTab = ref('volumes')
const selectedMember = ref('')

const volumesPagination = ref({ rowsPerPage: 15 })
const jobsPagination = ref({ rowsPerPage: 15 })

const volumeColumns = computed(() => [
  { name: 'volumename', label: t('Volume Name'), field: 'volumename', align: 'left', sortable: true },
  { name: 'pool', label: t('Pool'), field: 'pool', align: 'left', sortable: true },
  { name: 'storage', label: t('Storage'), field: 'storage', align: 'left', sortable: true },
  { name: 'status', label: t('Status'), field: (row) => row.volstatus ?? row.status, align: 'center', sortable: true },
  { name: 'bytes', label: t('Bytes'), field: (row) => row.volbytes ?? row.bytes, align: 'right', sortable: true },
])

const jobColumns = computed(() => [
  { name: 'jobid', label: t('Job ID'), field: (row) => row.jobid ?? row.id, align: 'left', sortable: true },
  { name: 'name', label: t('Job Name'), field: 'name', align: 'left', sortable: true },
  { name: 'level', label: t('Level'), field: 'level', align: 'center', sortable: true },
  { name: 'status', label: t('Status'), field: 'status', align: 'center', sortable: true },
  { name: 'starttime', label: t('Start time'), field: 'starttime', align: 'left', sortable: true },
  { name: 'bytes', label: t('Bytes'), field: 'bytes', align: 'right', sortable: true },
])

const shownGroups = computed(() => {
  let list = allGroups.value

  if (problemsOnly.value) {
    list = list.filter((g) => groupHasProblem(g))
  }

  const term = filterText.value.trim().toLowerCase()
  if (term) {
    list = list.filter((g) => {
      if (g.key.toLowerCase().includes(term)) return true
      if (g.ownerType.toLowerCase().includes(term)) return true
      if (g.ownerName.toLowerCase().includes(term)) return true
      if (g.policy.toLowerCase().includes(term)) return true
      if (g.policySource.toLowerCase().includes(term)) return true
      if (g.members.some((m) => m.toLowerCase().includes(term))) return true
      if (g.jobs.some((j) => j.toLowerCase().includes(term))) return true
      return false
    })
  }

  return list
})

const shownMemberSet = computed(() => new Set(shownGroups.value.flatMap((g) => g.members)))

const volumesFiltered = computed(() =>
  allVolumes.value.filter((v) => shownMemberSet.value.has(v.storage))
)

const allMemberOptions = computed(() => {
  const members = Array.from(new Set(shownGroups.value.flatMap((g) => g.members)))
  return members.sort((a, b) => a.localeCompare(b))
})

watch(allMemberOptions, (options) => {
  if (!options.includes(selectedMember.value)) {
    selectedMember.value = options[0] ?? ''
  }
})

watch(selectedMember, (member) => {
  if (member) {
    loadJobsForMember(member)
  } else {
    memberJobs.value = []
  }
})

const allowed = (command) => actionAllowed(acl.commands, command)

function openDialog(title, build) {
  dialog.value = { open: true, title, build }
}

function openPolicyDialog(group) {
  const value = policyChoice.value[group.key] ?? group.policy
  openDialog(
    `${t('Change policy')}: ${group.ownerType} ${group.ownerName}`,
    (isPreview) => buildConfigureSetCommand({
      resourceType: ownerResourceType(group.ownerType),
      name: group.ownerName,
      directive: 'storagegrouppolicy',
      value,
      preview: isPreview,
    })
  )
}

function toggleMemberEditor(group) {
  editor.value = editor.value?.group === group.key
    ? null
    : { group: group.key, list: [...group.members], toAdd: null }
}

function openMembersDialog(group) {
  const list = [...editor.value.list]
  openDialog(
    `${t('Change members')}: ${group.ownerType} ${group.ownerName}`,
    (isPreview) => buildConfigureSetCommand({
      resourceType: ownerResourceType(group.ownerType),
      name: group.ownerName,
      directive: 'storage',
      value: list,
      preview: isPreview,
    })
  )
}

function openEnabledDialog(member, value) {
  openDialog(
    `${value === 'no' ? t('Disable in configuration') : t('Enable in configuration')}: ${member}`,
    (isPreview) => buildConfigureSetCommand({
      resourceType: 'storage', name: member, directive: 'enabled', value, preview: isPreview,
    })
  )
}

async function onConfigChanged() {
  editor.value = null
  policyChoice.value = {}
  await refreshAll()
}

async function runtimeEnable(member, enabled) {
  const group = shownGroups.value.find((g) => g.members.includes(member))
  try {
    await director.call(buildStorageEnabledCommand(member, enabled))
    memberNotice.value = {
      group: group?.key,
      ok: true,
      text: enabled
        ? t('Storage {name} enabled until the next reload.', { name: member })
        : t('Storage {name} disabled until the next reload.', { name: member }),
    }
    await refreshAll()
  } catch (err) {
    memberNotice.value = { group: group?.key, ok: false, text: err.message || String(err) }
  }
}

async function runNow(jobName) {
  runningJob.value = jobName
  try {
    const res = await director.call(buildRunJobCommand({ job: jobName }))
    const rawJobId = res?.run?.jobid ?? res?.jobid ?? res?.result?.run?.jobid ?? res?.result?.jobid
    if (!rawJobId) {
      throw new Error(res?.error?.message ?? res?.error ?? t('No JobId returned from run command'))
    }
    runNotices.value = {
      ...runNotices.value,
      [jobName]: { job: jobName, ok: true, jobId: Number(rawJobId), text: t('Started job {name}', { name: jobName }) },
    }
  } catch (err) {
    runNotices.value = {
      ...runNotices.value,
      [jobName]: { job: jobName, ok: false, jobId: null, text: err.message || String(err) },
    }
  } finally {
    runningJob.value = ''
  }
}

function jobNotices(group) {
  return group.jobs.map((j) => runNotices.value[j]).filter(Boolean)
}

function isMemberDisabledInConfig(member) {
  const cfg = storagesMap.value?.[member]
  if (!cfg) return false
  const val = cfg.enabled
  return val === false || val === 0 || val === '0' || val === 'no' || val === 'false'
}

function isMemberDisabledAtRuntime(member) {
  if (enabledMap.value.has(member)) {
    return enabledMap.value.get(member) === false
  }
  return false
}

function isStorageEnabled(member) {
  if (isMemberDisabledAtRuntime(member)) {
    return false
  }
  if (isMemberDisabledInConfig(member)) {
    return false
  }
  return true
}

function groupHasProblem(group) {
  return group.members.some((m) => !isStorageEnabled(m))
}

function formatMemberLabel(member) {
  const addr = storagesMap.value[member]?.address
  const isEnabled = isStorageEnabled(member)
  let label = member
  if (addr) {
    label += ` (${addr})`
  }
  if (!isEnabled) {
    label += ` (${t('disabled')})`
  }
  return label
}

async function loadJobsForMember(member) {
  if (!member) return
  loadingMemberJobs.value = true
  try {
    const res = await director.call(`list jobs storage=${quoteDirectorString(member)}`)
    const jobs = directorCollection(res?.jobs ?? res?.result?.jobs ?? res)
    memberJobs.value = jobs.map((j) => {
      const norm = normaliseJob(j)
      return { ...norm, jobid: norm.id }
    })
  } catch (err) {
    memberJobs.value = []
  } finally {
    loadingMemberJobs.value = false
  }
}

async function refreshAll() {
  loadingData.value = true
  pageError.value = null

  try {
    const [jobsRes, poolsRes, storagesRes, dotStoragesRes, jobdefsRes] = await Promise.all([
      director.call('show jobs verbose'),
      director.call('show pools'),
      director.call('show storages'),
      director.call('.storages'),
      director.call('show jobdefs').catch(() => ({})),
    ])

    const jobs = jobsRes?.jobs ?? jobsRes?.result?.jobs ?? jobsRes ?? {}
    const pools = poolsRes?.pools ?? poolsRes?.result?.pools ?? poolsRes ?? {}
    const storages = storagesRes?.storages ?? storagesRes?.result?.storages ?? storagesRes ?? {}
    const jobdefs = jobdefsRes?.jobdefs ?? jobdefsRes?.result?.jobdefs ?? jobdefsRes ?? {}
    storagesMap.value = storages

    const rawDotStorages = directorCollection(dotStoragesRes?.storages ?? dotStoragesRes?.result?.storages ?? dotStoragesRes)
    const newEnabledMap = new Map()
    for (const item of rawDotStorages) {
      if (item && typeof item === 'object') {
        const name = item.name
        const enabled = item.enabled !== false && item.enabled !== 0 && item.enabled !== '0'
        newEnabledMap.set(name, enabled)
      }
    }
    enabledMap.value = newEnabledMap
    allStorageNames.value = rawDotStorages
      .map((item) => (typeof item === 'string' ? item : item?.name))
      .filter(Boolean)

    allGroups.value = listGroups(jobs, pools, jobdefs)
  } catch (err) {
    pageError.value = err.message || String(err)
  } finally {
    loadingData.value = false
  }

  // Load volumes once
  loadingVolumes.value = true
  try {
    const volRes = await director.call('llist volumes')
    const rawVolumes = directorCollection(volRes?.volumes ?? volRes?.result?.volumes ?? volRes)
    allVolumes.value = rawVolumes.map(normaliseVolume)
  } catch (err) {
    allVolumes.value = []
  } finally {
    loadingVolumes.value = false
  }

  // If a member is selected, refresh its jobs
  if (selectedMember.value) {
    loadJobsForMember(selectedMember.value)
  }
}

onMounted(() => {
  acl.ensureLoaded()
  refreshAll()
})
</script>
