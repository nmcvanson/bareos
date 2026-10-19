<template>
  <q-dialog :model-value="modelValue" persistent @update:model-value="emit('update:modelValue', $event)">
    <q-card style="width: 740px; max-width: 95vw" data-testid="native-resource-create">
      <q-card-section class="panel-header">{{ t('New {type}', { type: ADD_RESOURCE_TYPES[resourceType] }) }}</q-card-section>
      <q-card-section class="q-gutter-y-sm">
        <q-banner v-if="!canCreate" dense class="bg-warning text-dark">
          {{ t('This console may not change the configuration or reload it.') }}
        </q-banner>
        <q-input v-model="name" dense outlined :label="t('Name')" :disable="locked" data-testid="native-resource-name" />
        <div v-for="(row, index) in rows" :key="index" class="row q-col-gutter-sm items-start">
          <div class="col-5">
            <q-select :model-value="row.key" :options="options" use-input dense outlined
              :label="t('Directive')" :disable="locked" :data-testid="`native-directive-${index}`"
              @update:model-value="rows[index] = { key: $event, value: '' }" />
          </div>
          <div class="col">
            <q-input v-model="row.value" dense outlined :label="t('Value')"
              :type="/password/i.test(row.key || '') ? 'password' : 'text'"
              autocomplete="off" :disable="locked" :data-testid="`native-value-${index}`" />
          </div>
          <q-btn flat round icon="remove_circle_outline" :disable="locked" :aria-label="t('Remove directive')"
            @click="rows.splice(index, 1)" />
        </div>
        <q-btn flat no-caps icon="add" :label="t('Add directive')" :disable="locked"
          @click="rows.push({ key: '', value: '' })" />
        <div class="text-caption text-grey-7">{{ t('Passwords are hidden in previews and results.') }}</div>
        <q-banner v-if="error" dense class="bg-negative text-white reply-text" data-testid="native-resource-error">{{ error }}</q-banner>
        <template v-if="preview">
          <div class="text-subtitle2">{{ t('Preview') }}</div>
          <div class="text-caption">{{ t('Local preview; the Director validates when creating the resource.') }}</div>
          <pre class="resource-file" data-testid="native-resource-preview">{{ preview }}</pre>
        </template>
        <template v-if="result">
          <q-banner dense :class="result.ok ? 'bg-positive text-white' : 'bg-negative text-white'" class="reply-text">
            {{ result.ok ? t('Resource created and configuration reloaded.') : result.error }}
          </q-banner>
          <div v-if="result.add" data-testid="native-add-reply">
            <div>{{ t('Resource created') }}: {{ result.add.created ? t('yes') : t('no') }}</div>
            <div>{{ t('File') }}: {{ result.add.filename ?? t('Unknown') }}</div>
            <pre v-if="result.add.content" class="resource-file">{{ result.add.content }}</pre>
            <div v-for="(message, index) in result.add.messages" :key="index" class="reply-text">{{ message }}</div>
          </div>
          <div v-if="result.reload" data-testid="native-reload-reply">
            {{ t('Reloaded') }}: {{ result.reloaded ? t('yes') : t('no') }}
            <div v-for="(message, index) in result.reload.messages" :key="index" class="reply-text">{{ message }}</div>
          </div>
          <q-banner v-if="result.add?.created && !result.ok" dense class="bg-warning text-dark">
            {{ t('The resource was created, but reload was not confirmed. Check the Director before retrying.') }}
          </q-banner>
        </template>
      </q-card-section>
      <q-card-actions align="right">
        <q-btn flat no-caps :label="result?.ok ? t('Close') : t('Cancel')" :disable="busy"
          @click="emit('update:modelValue', false)" />
        <q-btn no-caps color="primary" :label="t('Preview')" :disable="locked || !canCreate"
          data-testid="native-resource-preview-button" @click="onPreview" />
        <q-btn no-caps color="positive" :label="t('Create')" :loading="busy"
          :disable="locked || !canCreate || !preview" data-testid="native-resource-create-button" @click="onCreate" />
      </q-card-actions>
    </q-card>
  </q-dialog>
</template>

<script setup>
/*
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
 */

import { computed, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useDirectorStore } from '../stores/director.js'
import { useDirectorAclStore } from '../stores/directorAcl.js'
import { actionAllowed } from '../utils/acl.js'
import {
  ADD_RESOURCE_TYPES, buildConfigureAddCommand, buildConfigureAddPreview, configureAddAndReload,
} from '../utils/configureAdd.js'
import { nativeResourceFields, nativeDirectiveOptions, newResourceRows } from '../utils/nativeResourceForm.js'

const props = defineProps({
  modelValue: Boolean,
  resourceType: { type: String, required: true },
  resources: { type: Array, default: () => [] },
})
const emit = defineEmits(['update:modelValue', 'created'])
const { t } = useI18n()
const director = useDirectorStore()
const acl = useDirectorAclStore()
const name = ref('')
const rows = ref([])
const preview = ref(null)
const result = ref(null)
const error = ref(null)
const busy = ref(false)
const locked = computed(() => busy.value || Boolean(result.value?.add?.created))
const canCreate = computed(() => actionAllowed(acl.commands, 'configure') && actionAllowed(acl.commands, 'reload'))
const options = computed(() => nativeDirectiveOptions(props.resourceType, props.resources))

watch([name, rows], () => { preview.value = null; error.value = null }, { deep: true, flush: 'sync' })
watch(() => props.modelValue, open => {
  name.value = ''
  rows.value = open ? newResourceRows(props.resourceType) : []
  result.value = null
  preview.value = null
  error.value = null
}, { immediate: true })

function onPreview() {
  if (locked.value || !canCreate.value) return
  result.value = null
  error.value = null
  try {
    if (props.resources.some(resource => resource.name === name.value)) throw new Error(t('Resource already exists'))
    preview.value = buildConfigureAddPreview(props.resourceType, nativeResourceFields(props.resourceType, name.value, rows.value))
  } catch (err) {
    preview.value = null
    error.value = err.message || String(err)
  }
}

async function onCreate() {
  if (locked.value || !canCreate.value || !preview.value) return
  busy.value = true
  error.value = null
  try {
    const fields = nativeResourceFields(props.resourceType, name.value, rows.value)
    const secrets = fields.filter(field => /password/i.test(field.key)).map(field => field.value)
    const outcome = await configureAddAndReload(command => director.call(command), buildConfigureAddCommand(props.resourceType, fields), { secrets })
    for (const row of rows.value) if (/password/i.test(row.key)) row.value = ''
    result.value = outcome
    if (outcome.ok) emit('created', outcome.add.name)
  } catch (err) {
    error.value = err.message || String(err)
  } finally {
    busy.value = false
  }
}
</script>

<style scoped>
.resource-file {
  font-family: monospace;
  font-size: 12px;
  white-space: pre;
  overflow: auto;
  max-height: 320px;
  background: rgba(127, 127, 127, 0.12);
  padding: 8px;
}
.reply-text { white-space: pre-wrap; }
</style>
