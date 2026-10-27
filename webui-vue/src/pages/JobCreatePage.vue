<template>
  <q-page class="q-pa-md">
    <DirectorErrorsBanner :errors="directorErrors" />

    <div class="row items-center q-mb-md">
      <q-btn flat round dense icon="arrow_back" color="primary" :to="{ name: 'jobs' }" />
      <div class="text-h5 q-ml-sm">{{ t('New job') }}</div>
    </div>

    <q-banner v-if="loadError" dense class="bg-negative text-white q-mb-md rounded-borders">
      {{ loadError }}
    </q-banner>

    <q-banner
      v-if="!allowed('configure')"
      dense
      class="bg-warning text-dark q-mb-md rounded-borders"
      data-testid="job-create-not-allowed-banner"
    >
      {{ t('This console may not change the configuration.') }}
    </q-banner>

    <q-card flat bordered class="bareos-panel" style="max-width: 760px">
      <q-card-section class="panel-header">{{ t('Backup job') }}</q-card-section>
      <q-card-section>
        <q-form class="q-gutter-y-sm" :inert="Boolean(busy) || undefined" data-testid="job-create-form" @submit.prevent="onPreview">
          <q-input
            v-model="form.name"
            :label="t('Name')"
            dense
            outlined
            :error="Boolean(errors.name)"
            :error-message="errors.name ? t(errors.name) : ''"
            data-testid="job-name"
          />
          <q-select
            v-model="form.jobdefs"
            :options="lists.jobdefs"
            :label="t('JobDefs')"
            dense
            outlined
            clearable
            :error="Boolean(errors.jobdefs)"
            :error-message="errors.jobdefs ? t(errors.jobdefs) : ''"
            data-testid="job-jobdefs"
          />
          <q-select
            v-model="form.client"
            :options="lists.clients"
            :label="t('Client')"
            dense
            outlined
            :error="Boolean(errors.client)"
            :error-message="errors.client ? t(errors.client) : ''"
            data-testid="job-client"
          />
          <q-select
            v-model="form.fileset"
            :options="lists.filesets"
            :label="t('FileSet')"
            dense
            outlined
            :error="Boolean(errors.fileset)"
            :error-message="errors.fileset ? t(errors.fileset) : ''"
            data-testid="job-fileset"
          />
          <q-select
            v-model="form.schedule"
            :options="lists.schedules"
            :label="t('Schedule') + ' (' + t('optional') + ')'"
            dense
            outlined
            clearable
            :error="Boolean(errors.schedule)"
            :error-message="errors.schedule ? t(errors.schedule) : ''"
            data-testid="job-schedule"
          />
          <q-select
            v-model="form.pool"
            :options="lists.pools"
            :label="t('Pool')"
            dense
            outlined
            :error="Boolean(errors.pool)"
            :error-message="errors.pool ? t(errors.pool) : ''"
            data-testid="job-pool"
          />
          <q-select
            v-model="form.storage"
            :options="lists.storages"
            :label="t('Storage') + ' (' + t('ordered, optional') + ')'"
            dense
            outlined
            multiple
            use-chips
            :error="Boolean(errors.storage)"
            :error-message="errors.storage ? t(errors.storage) : ''"
            data-testid="job-storage"
          />
          <q-select
            v-model="form.storagegrouppolicy"
            :options="policyOptions"
            :label="t('Storage group policy')"
            dense
            outlined
            clearable
            :error="Boolean(errors.storagegrouppolicy)"
            :error-message="errors.storagegrouppolicy ? t(errors.storagegrouppolicy) : ''"
            data-testid="job-policy"
          />
          <q-select
            v-model="form.level"
            :options="levelOptions"
            :label="t('Level')"
            dense
            outlined
            :error="Boolean(errors.level)"
            :error-message="errors.level ? t(errors.level) : ''"
            data-testid="job-level"
          />
          <q-input
            v-model.number="form.priority"
            type="number"
            min="1"
            max="100"
            :label="t('Priority')"
            dense
            outlined
            :error="Boolean(errors.priority)"
            :error-message="errors.priority ? t(errors.priority) : ''"
            data-testid="job-priority"
          />
          <q-select
            v-model="form.messages"
            :options="lists.messages"
            :label="t('Messages')"
            dense
            outlined
            :error="Boolean(errors.messages)"
            :error-message="errors.messages ? t(errors.messages) : ''"
            data-testid="job-messages"
          />

          <div class="row items-center q-gutter-sm q-mt-md">
            <q-btn
              type="submit"
              color="primary"
              no-caps
              icon="visibility"
              :label="t('Preview')"
              :loading="busy === 'preview'"
              :disable="Boolean(busy) || !allowed('configure')"
              data-testid="job-preview"
            >
              <q-tooltip v-if="!allowed('configure')">{{ t('Not allowed for this console') }}</q-tooltip>
            </q-btn>
            <q-btn
              color="positive"
              no-caps
              icon="add"
              :label="t('Create')"
              :loading="busy === 'create'"
              :disable="Boolean(busy) || !previewOk || !allowed('configure') || !allowed('reload') || Boolean(createResult?.add?.created)"
              data-testid="job-create"
              @click="onCreate"
            />
          </div>
        </q-form>
      </q-card-section>
    </q-card>

    <q-card v-if="previewResult" flat bordered class="bareos-panel q-mt-md" style="max-width: 760px">
      <q-card-section class="panel-header">{{ t('Preview') }}</q-card-section>
      <q-card-section>
        <div v-if="previewResult.ok">
          <div class="text-caption text-grey-7">{{ t('Local preview; the Director validates when creating the resource.') }}</div>
          <pre class="job-file" data-testid="job-file-text">{{ previewResult.content }}</pre>
        </div>
        <q-banner v-else dense class="bg-negative text-white rounded-borders" data-testid="job-preview-error">
          {{ previewResult.error }}
        </q-banner>
      </q-card-section>
    </q-card>

    <q-card v-if="createResult" flat bordered class="bareos-panel q-mt-md" style="max-width: 760px">
      <q-card-section class="panel-header">{{ t('Result') }}</q-card-section>
      <q-card-section class="q-gutter-y-sm" data-testid="job-create-result">
        <q-banner
          dense
          class="rounded-borders text-white"
          :class="createResult.ok ? 'bg-positive' : 'bg-negative'"
        >
          {{ createResult.ok ? t('Job created and configuration reloaded.') : createResult.error }}
        </q-banner>
        <div v-if="createResult.add" data-testid="job-add-reply">
          <div>{{ t('Resource created') }}: {{ createResult.add.created ? t('yes') : t('no') }}</div>
          <div>{{ t('File') }}: {{ createResult.add.filename ?? t('Unknown') }}</div>
          <pre v-if="createResult.add.content" class="job-file">{{ createResult.add.content }}</pre>
          <div v-for="(message, index) in createResult.add.messages" :key="index" class="reply-message">{{ message }}</div>
        </div>
        <div v-if="createResult.reload" data-testid="job-reload-reply">
          {{ t('Reloaded') }}: {{ createResult.reloaded ? t('yes') : t('no') }}
          <div v-for="(message, index) in createResult.reload.messages" :key="index" class="reply-message">{{ message }}</div>
        </div>
        <q-banner v-if="createResult.add?.created && !createResult.ok" dense class="bg-warning text-dark rounded-borders">
          {{ t('The resource was created, but reload was not confirmed. Check the Director before retrying.') }}
        </q-banner>
        <div v-if="createResult.ok" class="row items-center q-gutter-sm">
          <q-btn
            color="primary"
            no-caps
            icon="play_arrow"
            :label="t('Run now')"
            :loading="busy === 'run'"
            :disable="Boolean(busy) || !allowed('run')"
            data-testid="job-run-now"
            @click="onRun"
          >
            <q-tooltip v-if="!allowed('run')">{{ t('Not allowed for this console') }}</q-tooltip>
          </q-btn>
          <q-btn
            flat
            no-caps
            color="primary"
            icon="work"
            :label="t('Show in Jobs')"
            :to="{ name: 'jobs', query: { search: createdName } }"
            data-testid="job-show"
          />
        </div>
        <q-banner v-if="runNotice" dense class="rounded-borders text-white"
          :class="runNotice.ok ? 'bg-positive' : 'bg-negative'">
          {{ runNotice.text }}
          <router-link
            v-if="runNotice.jobId"
            :to="{ name: 'job-details', params: { id: runNotice.jobId } }"
            class="text-white text-weight-bold q-ml-sm"
          >#{{ runNotice.jobId }}</router-link>
        </q-banner>
      </q-card-section>
    </q-card>
  </q-page>
</template>

<script setup>
import { ref, computed, watch, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { useDirectorStore } from '../stores/director.js'
import { useDirectorAclStore } from '../stores/directorAcl.js'
import { DOT_LISTS, dotListNames } from '../utils/dotLists.js'
import { actionAllowed } from '../utils/acl.js'
import { validateJobForm, buildConfigureAddJobCommand, buildJobResourcePreview } from '../utils/jobConfig.js'
import { configureAddAndReload } from '../utils/configureAdd.js'
import { buildRunJobCommand } from '../utils/jobs.js'
import DirectorErrorsBanner from '../components/DirectorErrorsBanner.vue'

const { t } = useI18n()
const director = useDirectorStore()
const acl = useDirectorAclStore()

const directorErrors = ref([])
const loadError = ref(null)
const busy = ref('')
const previewResult = ref(null)
const createResult = ref(null)
const runNotice = ref(null)
const createdName = ref('')

const policyOptions = ['ListedOrder', 'LeastUsed']
const levelOptions = ['Full', 'Incremental', 'Differential']

const form = ref({
  name: '',
  jobdefs: null,
  client: null,
  fileset: null,
  schedule: null,
  pool: null,
  storage: [],
  storagegrouppolicy: null,
  level: 'Incremental',
  priority: 10,
  messages: null,
})

const lists = ref({
  jobs: [], jobdefs: [], clients: [], filesets: [], schedules: [], pools: [], storages: [], messages: [],
})
const errors = ref({})

const allowed = (command) => actionAllowed(acl.commands, command)
const previewOk = computed(() => previewResult.value?.ok === true)

// A changed form invalidates the shown preview.
watch(form, () => {
  previewResult.value = null
  createResult.value = null
  runNotice.value = null
}, { deep: true })

async function loadLists() {
  const names = Object.keys(DOT_LISTS)
  try {
    const results = await Promise.all(names.map(name => director.call(DOT_LISTS[name].command)))
    const next = {}
    names.forEach((name, i) => {
      next[name] = dotListNames(name, results[i])
    })
    lists.value = next
  } catch (err) {
    loadError.value = err.message || String(err)
  }
}

function formForCommand() {
  const f = form.value
  return {
    name: f.name,
    jobdefs: f.jobdefs || undefined,
    client: f.client,
    fileset: f.fileset,
    schedule: f.schedule || undefined,
    pool: f.pool,
    storage: f.storage?.length ? [...f.storage] : undefined,
    storagegrouppolicy: f.storagegrouppolicy || undefined,
    level: f.level,
    priority: f.priority,
    messages: f.messages,
  }
}

function check() {
  errors.value = validateJobForm({ ...formForCommand(), storage: form.value.storage ?? [] }, lists.value)
  return Object.keys(errors.value).length === 0
}

async function onPreview() {
  if (busy.value) return
  previewResult.value = null
  createResult.value = null
  runNotice.value = null
  if (!check() || !allowed('configure')) return
  busy.value = 'preview'
  try {
    previewResult.value = { ok: true, content: buildJobResourcePreview(formForCommand()) }
  } catch (error) {
    previewResult.value = { ok: false, error: error.message || String(error) }
  } finally {
    busy.value = ''
  }
}

async function onCreate() {
  if (busy.value || createResult.value?.add?.created || !previewOk.value || !check()
    || !allowed('configure') || !allowed('reload')) return
  busy.value = 'create'
  runNotice.value = null
  try {
    createResult.value = await configureAddAndReload(command => director.call(command), buildConfigureAddJobCommand(formForCommand()))
    if (createResult.value.ok) {
      createdName.value = form.value.name
      await loadLists()
    }
  } finally {
    busy.value = ''
  }
}

async function onRun() {
  busy.value = 'run'
  try {
    const res = await director.call(buildRunJobCommand({ job: createdName.value }))
    const rawJobId = res?.run?.jobid ?? res?.jobid ?? res?.result?.run?.jobid ?? res?.result?.jobid
    if (!rawJobId) {
      throw new Error(res?.error?.message ?? res?.error ?? t('No JobId returned from run command'))
    }
    runNotice.value = { ok: true, jobId: Number(rawJobId), text: t('Started job {name}', { name: createdName.value }) }
  } catch (err) {
    runNotice.value = { ok: false, jobId: null, text: err.message || String(err) }
  } finally {
    busy.value = ''
  }
}

onMounted(() => {
  acl.ensureLoaded()
  loadLists()
})
</script>

<style scoped>
.job-file {
  font-family: monospace;
  font-size: 12px;
  white-space: pre;
  overflow: auto;
  max-height: 360px;
  background: rgba(127, 127, 127, 0.12);
  padding: 8px;
  border-radius: 4px;
  margin: 0;
}
.reply-message {
  white-space: pre-wrap;
}
</style>
