<template>
  <q-dialog :model-value="modelValue" persistent @update:model-value="onToggle">
    <q-card style="min-width: 480px; max-width: 760px" data-testid="configure-dialog">
      <q-card-section class="row items-center">
        <div class="text-h6">{{ title }}</div>
        <q-space />
        <q-btn flat round dense icon="close" :disable="busy" @click="close" />
      </q-card-section>

      <q-card-section class="q-gutter-y-sm">
        <div v-if="busy && !result" class="row items-center q-gutter-sm">
          <q-spinner size="20px" color="primary" />
          <span>{{ phase === 'confirming' ? t('Applying…') : t('Checking…') }}</span>
        </div>

        <template v-if="preview">
          <div v-if="preview.ok" data-testid="configure-preview">
            <div class="text-caption text-grey-7">{{ t('File') }}</div>
            <div class="text-body2 q-mb-sm" style="word-break: break-all">{{ preview.filename }}</div>
            <template v-if="kind === 'set'">
              <div class="text-caption text-grey-7">{{ t('Change') }}</div>
              <div class="text-body1" data-testid="configure-change">
                <code>{{ show(preview.old) }}</code>
                <q-icon name="arrow_forward" class="q-mx-sm" />
                <code>{{ show(preview.new) }}</code>
              </div>
            </template>
            <template v-else>
              <div class="text-caption text-grey-7">{{ t('New file') }}</div>
              <pre class="configure-file" data-testid="configure-file-text">{{ preview.content }}</pre>
            </template>
          </div>
          <q-banner v-else dense class="bg-negative text-white rounded-borders" data-testid="configure-preview-error">
            {{ preview.error }}
          </q-banner>
        </template>

        <template v-if="result">
          <q-banner
            dense
            class="rounded-borders text-white"
            :class="result.ok ? 'bg-positive' : 'bg-negative'"
            data-testid="configure-result"
          >
            <template v-if="result.ok">{{ t('Configuration reloaded.') }}</template>
            <template v-else>{{ result.error }}</template>
          </q-banner>
          <div class="text-caption" data-testid="configure-result-details">
            {{ t('Reloaded') }}: {{ result.reloaded ? t('yes') : t('no') }} ·
            {{ t('Rollback') }}: {{ result.rollback ?? 'none' }}
          </div>
        </template>
      </q-card-section>

      <q-card-actions align="right">
        <q-btn flat no-caps :label="result?.ok ? t('Close') : t('Cancel')" :disable="busy" @click="close" />
        <q-btn
          v-if="!result?.ok"
          color="primary"
          no-caps
          :label="t('Confirm')"
          :loading="phase === 'confirming'"
          :disable="busy || !preview?.ok || Boolean(result)"
          data-testid="configure-confirm"
          @click="confirm"
        />
      </q-card-actions>
    </q-card>
  </q-dialog>
</template>

<script setup>
import { ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useDirectorStore } from '../stores/director.js'
import { parseConfigureSetResult } from '../utils/storageGroups.js'

const props = defineProps({
  modelValue: { type: Boolean, default: false },
  title: { type: String, default: '' },
  kind: { type: String, default: 'set' },
  // Command builders, called when the dialog opens and when confirmed.
  buildCommand: { type: Function, required: true },
})
const emit = defineEmits(['update:modelValue', 'changed'])

const { t } = useI18n()
const director = useDirectorStore()

const phase = ref('idle')
const preview = ref(null)
const result = ref(null)
const busy = ref(false)

const parser = () => parseConfigureSetResult

function show(value) {
  if (Array.isArray(value)) return value.join(', ')
  return value === null || value === undefined || value === '' ? t('(not set)') : String(value)
}

async function run(isPreview) {
  let reply
  try {
    reply = await director.call(props.buildCommand(isPreview))
  } catch (err) {
    reply = { error: { message: err?.message ?? String(err) } }
  }
  return parser()(reply, { preview: isPreview })
}

async function open() {
  phase.value = 'previewing'
  preview.value = null
  result.value = null
  busy.value = true
  try {
    preview.value = await run(true)
  } catch (err) {
    preview.value = { ok: false, error: err?.message ?? String(err) }
  } finally {
    busy.value = false
    phase.value = 'preview'
  }
}

async function confirm() {
  if (!preview.value?.ok || busy.value) return
  phase.value = 'confirming'
  busy.value = true
  try {
    result.value = await run(false)
  } catch (err) {
    result.value = { ok: false, reloaded: false, rollback: null, error: err?.message ?? String(err) }
  } finally {
    busy.value = false
    phase.value = 'done'
  }
  if (result.value?.ok) emit('changed')
}

function close() {
  if (busy.value) return
  emit('update:modelValue', false)
}

function onToggle(value) {
  if (!value) close()
}

watch(() => props.modelValue, (isOpen) => {
  if (isOpen) open()
})
</script>

<style scoped>
.configure-file {
  font-family: monospace;
  font-size: 12px;
  white-space: pre;
  overflow: auto;
  max-height: 320px;
  background: rgba(127, 127, 127, 0.12);
  padding: 8px;
  border-radius: 4px;
  margin: 0;
}
</style>
