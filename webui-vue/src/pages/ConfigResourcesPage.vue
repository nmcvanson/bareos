<template>
  <q-page class="q-pa-md">
    <DirectorErrorsBanner :errors="directorErrors" />

    <!-- Page Header and Actions -->
    <div class="row items-center justify-between q-mb-sm">
      <div class="row items-center q-gutter-sm">
        <div class="text-h5" data-testid="config-page-title">
          {{ currentConfigType ? t(currentConfigType.label) : t('Configuration') }}
        </div>
        <q-badge v-if="currentConfigType && !currentConfigType.writable" color="info" :label="t('Read only')" />
      </div>
      <div class="row items-center q-gutter-sm">
        <q-btn
          v-if="currentType === 'job' && canConfigure"
          color="primary"
          unelevated
          dense
          no-caps
          icon="add"
          :label="t('New job')"
          to="/jobs/new"
          data-testid="new-job-btn"
          class="q-px-sm"
        />
        <q-btn
          v-if="currentType !== 'job' && currentConfigType?.writable && canConfigure"
          color="primary" unelevated dense no-caps icon="add"
          :label="t('New {type}', { type: ADD_RESOURCE_TYPES[currentType] })"
          data-testid="new-resource-btn" class="q-px-sm" @click="createOpen = true"
        />
        <q-btn
          flat
          round
          dense
          icon="refresh"
          color="primary"
          :loading="loading"
          data-testid="config-refresh-btn"
          @click="refreshCurrent"
        >
          <q-tooltip>{{ t('Refresh') }}</q-tooltip>
        </q-btn>
      </div>
    </div>

    <!-- Type Switcher Chips -->
    <div class="row items-center q-gutter-xs q-mb-md">
      <q-btn
        v-for="tConfig in CONFIG_TYPES"
        :key="tConfig.key"
        dense
        no-caps
        :unelevated="tConfig.key === currentType"
        :flat="tConfig.key !== currentType"
        :color="tConfig.key === currentType ? 'primary' : 'grey-8'"
        :label="t(tConfig.label)"
        :to="'/config/' + tConfig.key"
        :data-testid="`config-type-${tConfig.key}`"
        class="q-px-sm"
      />
    </div>

    <!-- Read-only Banner for non-writable types (console, profile) -->
    <q-banner
      v-if="currentConfigType && !currentConfigType.writable"
      dense
      class="bg-info text-white q-mb-md rounded-borders"
      data-testid="read-only-banner"
    >
      {{ t('Read only') }}
      {{ t('Change it in the configuration files.') }}
    </q-banner>

    <!-- Error Banner -->
    <q-banner v-if="pageError" dense class="bg-negative text-white q-mb-md rounded-borders">
      {{ pageError }}
    </q-banner>

    <!-- Search / Filter -->
    <div class="row items-center q-mb-md">
      <div class="col-12 col-sm-6 col-md-4">
        <q-input
          v-model="filterText"
          dense
          outlined
          clearable
          :placeholder="t('Search resources…')"
          data-testid="config-search"
        >
          <template #prepend>
            <q-icon name="search" />
          </template>
        </q-input>
      </div>
    </div>

    <!-- Main Content: Resources Table + Side Panel -->
    <div class="row q-col-gutter-md">
      <!-- Left: Table of Resources -->
      <div class="col-12 col-md-7 col-lg-8">
        <q-table
          :rows="filteredResources"
          :columns="columns"
          row-key="name"
          dense
          flat
          bordered
          :loading="loading"
          :pagination="tablePagination"
          class="bareos-panel"
          data-testid="config-resources-table"
        >
          <template #body-cell-name="props">
            <q-td
              :props="props"
              class="cursor-pointer"
              :class="props.row.name === selectedResourceName ? 'text-weight-bold text-primary' : ''"
              @click="selectedResourceName = props.row.name"
            >
              <span class="text-primary text-weight-medium">{{ props.value }}</span>
            </q-td>
          </template>

          <template v-for="dir in mainDirectives" :key="dir" #[`body-cell-${dir}`]="props">
            <q-td :props="props">
              <span
                v-if="currentType === 'job' && isInherited(props.row, dir)"
                class="text-grey-6"
                data-testid="inherited-directive"
              >
                {{ props.value }} (JobDefs)
              </span>
              <span v-else>
                {{ props.value }}
              </span>
            </q-td>
          </template>

          <template #no-data>
            <div v-if="!loading" class="full-width row flex-center q-pa-md text-grey">
              {{ t('No resources of this type are visible to this console.') }}
            </div>
          </template>
        </q-table>
      </div>

      <!-- Right: Side Panel with Directives & Used by -->
      <div class="col-12 col-md-5 col-lg-4">
        <q-card flat bordered class="bareos-panel" data-testid="config-side-panel">
          <q-card-section class="panel-header row items-center justify-between">
            <div class="text-subtitle1 text-weight-bold ellipsis" :title="selectedResource?.name">
              {{ selectedResource ? selectedResource.name : t('Select a resource') }}
            </div>
            <q-badge v-if="currentConfigType" outline color="white">
              {{ currentConfigType.key }}
            </q-badge>
          </q-card-section>

          <q-separator />

          <q-card-section v-if="!selectedResource" class="text-grey-6 text-center q-pa-lg">
            {{ t('No resource selected.') }}
          </q-card-section>

          <q-card-section v-else class="q-gutter-y-md">
            <!-- Directives Section -->
            <div>
              <div class="text-subtitle2 text-weight-bold q-mb-sm text-grey-8">
                {{ t('Directives') }}
              </div>
              <div class="directives-list rounded-borders bg-grey-1 q-pa-sm" style="max-height: 400px; overflow-y: auto;">
                <div
                  v-for="(entry, idx) in selectedDirectiveEntries"
                  :key="idx"
                  class="row items-start no-wrap q-py-xs"
                  :style="{ paddingLeft: `${entry.depth * 14}px` }"
                >
                  <span class="text-caption text-weight-bold text-grey-8 q-mr-xs">{{ entry.key }}:</span>
                  <span
                    v-if="entry.value"
                    class="text-caption text-break"
                    :class="currentType === 'job' && isInherited(selectedResource, entry.key) ? 'text-grey-6' : 'text-grey-9'"
                  >
                    {{ entry.value }}<span v-if="currentType === 'job' && isInherited(selectedResource, entry.key)"> (JobDefs)</span>
                  </span>
                </div>
              </div>
            </div>

            <q-separator />

            <!-- Used by Section -->
            <div data-testid="used-by-section">
              <div class="row items-center justify-between q-mb-xs">
                <div class="text-subtitle2 text-weight-bold text-grey-8">
                  {{ t('Used by') }}
                </div>
                <q-spinner v-if="loadingUsedBy" size="xs" color="primary" />
              </div>

              <div
                v-if="usedByList.length === 0"
                class="text-caption text-grey-6 q-py-xs"
                data-testid="used-by-empty"
              >
                {{ loadingUsedBy ? t('Checking references…') : t('Not referenced by other resources.') }}
              </div>

              <q-list v-else dense bordered separator class="rounded-borders" data-testid="used-by-list">
                <q-item
                  v-for="item in usedByList"
                  :key="`${item.type}:${item.name}:${item.directive}`"
                  clickable
                  :to="'/config/' + item.type"
                  data-testid="used-by-item"
                >
                  <q-item-section>
                    <q-item-label class="text-weight-medium">
                      {{ item.name }}
                      <q-badge outline color="secondary" class="q-ml-xs">
                        {{ item.type }}
                      </q-badge>
                    </q-item-label>
                    <q-item-label caption class="text-grey-7">
                      {{ t('via') }} {{ item.directive }}
                    </q-item-label>
                  </q-item-section>
                  <q-item-section side>
                    <q-icon name="chevron_right" size="xs" />
                  </q-item-section>
                </q-item>
              </q-list>
            </div>
          </q-card-section>
        </q-card>
      </div>
    </div>
    <NativeResourceCreate
      v-if="currentConfigType?.writable && currentType !== 'job' && canConfigure"
      :key="currentType" v-model="createOpen" :resource-type="currentType" :resources="resources"
      @created="refreshCurrent"
    />
  </q-page>
</template>

<script setup>
import { ref, computed, watch, onMounted } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { useDirectorStore } from '../stores/director.js'
import { useDirectorAclStore } from '../stores/directorAcl.js'
import DirectorErrorsBanner from '../components/DirectorErrorsBanner.vue'
import NativeResourceCreate from '../components/NativeResourceCreate.vue'
import { ADD_RESOURCE_TYPES } from '../utils/configureAdd.js'
import { actionAllowed } from '../utils/acl.js'
import {
  CONFIG_TYPES,
  MAIN_DIRECTIVES,
  normaliseResources,
  referencesTo,
  isInherited,
  withInherited,
} from '../utils/configResources.js'

const route = useRoute()
const router = useRouter()
const { t } = useI18n()
const director = useDirectorStore()
const acl = useDirectorAclStore()

const loading = ref(false)
const pageError = ref(null)
const directorErrors = ref([])
const resources = ref([])
const createOpen = ref(false)
const selectedResourceName = ref('')
const filterText = ref('')
const tablePagination = ref({ rowsPerPage: 15 })

const allResourcesMap = ref({})
const loadingUsedBy = ref(false)
const allLoaded = ref(false)

const currentType = computed(() => route.params.type)
const currentConfigType = computed(() =>
  CONFIG_TYPES.find((ct) => ct.key === currentType.value)
)
const canConfigure = computed(() => actionAllowed(acl.commands, 'configure'))

onMounted(() => {
  acl.ensureLoaded().catch(() => {})
})

watch(
  () => route.params.type,
  (type) => {
    createOpen.value = false
    const match = CONFIG_TYPES.find((ct) => ct.key === type)
    if (!match) {
      router.replace('/config/job')
      return
    }
    loadType(type)
  },
  { immediate: true }
)

async function loadType(typeKey) {
  const configType = CONFIG_TYPES.find((ct) => ct.key === typeKey)
  if (!configType) return

  loading.value = true
  pageError.value = null
  try {
    let jobdefsList = allResourcesMap.value['jobdefs']
    if (typeKey === 'job' && !jobdefsList) {
      try {
        const jdRes = await director.call('show jobdefs')
        jobdefsList = normaliseResources('jobdefs', jdRes)
        allResourcesMap.value['jobdefs'] = jobdefsList
      } catch {
        jobdefsList = []
      }
    }

    const res = await director.call(configType.showCommand)
    let list = normaliseResources(typeKey, res)

    if (typeKey === 'job' && jobdefsList) {
      list = list.map((job) => withInherited(job, jobdefsList))
    }

    resources.value = list
    allResourcesMap.value[typeKey] = list
    if (list.length > 0 && !list.some((r) => r.name === selectedResourceName.value)) {
      selectedResourceName.value = list[0].name
    } else if (list.length === 0) {
      selectedResourceName.value = ''
    }
  } catch (err) {
    pageError.value = err.message || String(err)
    resources.value = []
  } finally {
    loading.value = false
  }

  lazyLoadAllTypes()
}

async function refreshCurrent() {
  if (currentType.value) {
    if (currentType.value === 'job') {
      delete allResourcesMap.value['jobdefs']
    }
    allLoaded.value = false
    await loadType(currentType.value)
  }
}

async function lazyLoadAllTypes() {
  if (allLoaded.value || loadingUsedBy.value) return
  loadingUsedBy.value = true
  try {
    const typesToFetch = CONFIG_TYPES.filter((ct) => !allResourcesMap.value[ct.key])
    await Promise.allSettled(
      typesToFetch.map(async (ct) => {
        try {
          const res = await director.call(ct.showCommand)
          allResourcesMap.value[ct.key] = normaliseResources(ct.key, res)
        } catch {
          allResourcesMap.value[ct.key] = []
        }
      })
    )
    allLoaded.value = true
  } finally {
    loadingUsedBy.value = false
  }
}

function formatDirectiveValue(val) {
  if (val === null || val === undefined) return '-'
  if (typeof val === 'boolean') return val ? 'yes' : 'no'
  if (Array.isArray(val)) {
    return val.map((v) => (typeof v === 'object' ? JSON.stringify(v) : String(v))).join(', ')
  }
  if (typeof val === 'object') return JSON.stringify(val)
  return String(val)
}

const mainDirectives = computed(() => {
  return MAIN_DIRECTIVES[currentType.value] || []
})

const columns = computed(() => {
  const cols = [
    {
      name: 'name',
      label: t('Name'),
      field: 'name',
      align: 'left',
      sortable: true,
    },
  ]
  for (const dir of mainDirectives.value) {
    cols.push({
      name: dir,
      label: dir.charAt(0).toUpperCase() + dir.slice(1),
      field: (row) => formatDirectiveValue(row.directives?.[dir]),
      align: 'left',
      sortable: true,
    })
  }
  return cols
})

const filteredResources = computed(() => {
  const term = filterText.value.trim().toLowerCase()
  if (!term) return resources.value
  return resources.value.filter((r) => {
    if (r.name.toLowerCase().includes(term)) return true
    return Object.entries(r.directives || {}).some(([k, v]) => {
      if (k.toLowerCase().includes(term)) return true
      const strVal = formatDirectiveValue(v).toLowerCase()
      return strVal.includes(term)
    })
  })
})

const selectedResource = computed(() => {
  if (!selectedResourceName.value) return filteredResources.value[0] || null
  return resources.value.find((r) => r.name === selectedResourceName.value) || filteredResources.value[0] || null
})

const usedByList = computed(() => {
  if (!selectedResource.value || !currentType.value) return []
  return referencesTo(selectedResource.value.name, currentType.value, allResourcesMap.value)
})

function formatDirectiveEntries(obj, depth = 0) {
  if (!obj || typeof obj !== 'object') return []
  const entries = []
  for (const [key, val] of Object.entries(obj)) {
    if (Array.isArray(val)) {
      if (val.length === 0) {
        entries.push({ key, value: '[]', depth, isComplex: false })
      } else if (val.every((item) => typeof item !== 'object')) {
        entries.push({ key, value: val.join(', '), depth, isComplex: false })
      } else {
        entries.push({ key, value: '', depth, isComplex: true })
        val.forEach((item, idx) => {
          if (typeof item === 'object' && item !== null) {
            entries.push({ key: `[${idx}]`, value: '', depth: depth + 1, isComplex: true })
            entries.push(...formatDirectiveEntries(item, depth + 2))
          } else {
            entries.push({ key: `[${idx}]`, value: String(item), depth: depth + 1, isComplex: false })
          }
        })
      }
    } else if (val !== null && typeof val === 'object') {
      entries.push({ key, value: '', depth, isComplex: true })
      entries.push(...formatDirectiveEntries(val, depth + 1))
    } else {
      entries.push({
        key,
        value: formatDirectiveValue(val),
        depth,
        isComplex: false,
      })
    }
  }
  return entries
}

const selectedDirectiveEntries = computed(() => {
  if (!selectedResource.value?.directives) return []
  return formatDirectiveEntries(selectedResource.value.directives)
})
</script>

<style scoped>
.text-break {
  word-break: break-all;
  overflow-wrap: anywhere;
}
</style>
