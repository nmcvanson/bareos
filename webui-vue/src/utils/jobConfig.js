/*
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
 */

import { checkedDirectorValue } from './storageGroups.js'
import { buildConfigureAddCommand, buildConfigureAddPreview, validResourceName } from './configureAdd.js'

// Common backup directives; quote exactly as config_add_directive does.
const jobDirectives = [
  'name', 'description', 'type', 'jobdefs', 'client', 'fileset', 'schedule',
  'pool', 'storage', 'storagegrouppolicy', 'level', 'priority', 'messages',
]
const listNames = values => (values ?? []).map(value => (
  typeof value === 'string' ? value : value.name ?? value.value
))

// Returns field-keyed English errors; an empty object is valid.
export function validateJobForm(form, lists = {}) {
  const errors = {}
  if (!form.name) errors.name = 'Name is required'
  else if (!validResourceName(form.name)) {
    errors.name = 'Name must use letters, digits, dot, underscore or hyphen'
  } else if (listNames(lists.jobs).includes(form.name)) errors.name = 'Job already exists'
  for (const [field, list] of [
    ['client', 'clients'], ['fileset', 'filesets'], ['pool', 'pools'], ['messages', 'messages'],
  ]) {
    if (!listNames(lists[list]).includes(form[field])) errors[field] = 'Select an available resource'
  }
  for (const [field, list] of [['jobdefs', 'jobdefs'], ['schedule', 'schedules']]) {
    if (form[field] && !listNames(lists[list]).includes(form[field])) {
      errors[field] = 'Select an available resource'
    }
  }
  const members = form.storage ?? []
  if (!Array.isArray(members) || members.some(member => !listNames(lists.storages).includes(member))) {
    errors.storage = 'Select available storage members'
  } else if (new Set(members).size !== members.length) errors.storage = 'Duplicate storage member'
  if (!['', 'ListedOrder', 'LeastUsed'].includes(form.storagegrouppolicy ?? '')) {
    errors.storagegrouppolicy = 'Select an available storage group policy'
  }
  if (!['number', 'string'].includes(typeof form.priority) || !/^\d+$/.test(String(form.priority))
    || !Number.isInteger(Number(form.priority)) || Number(form.priority) < 1 || Number(form.priority) > 100) {
    errors.priority = 'Priority must be an integer from 1 to 100'
  }
  if (!['Full', 'Incremental', 'Differential'].includes(form.level)) errors.level = 'Select a backup level'
  for (const field of jobDirectives) {
    if (field === 'type') continue
    const values = Array.isArray(form[field]) ? form[field] : [form[field]]
    try {
      for (const value of values) {
        if (value !== undefined && value !== null) checkedDirectorValue(String(value))
      }
    } catch {
      errors[field] = 'Forbidden character in Director argument'
    }
  }
  return errors
}

const jobValues = form => jobDirectives.flatMap(key => {
  let value = key === 'type' ? 'Backup' : form[key]
  if (key === 'storage' && value !== undefined && value !== null) {
    if (!Array.isArray(value)) throw new TypeError('Storage members must be a list')
    value = value.map(member => {
      if (!checkedDirectorValue(member) || /[,\s#]/.test(member)) {
        throw new TypeError('Invalid storage member')
      }
      return member
    }).join(',')
  }
  if (value === undefined || value === null || value === '') return []
  value = checkedDirectorValue(String(value))
  if (key === 'name' && !validResourceName(value)) throw new TypeError('Invalid job name')
  if (key === 'level' && !['Full', 'Incremental', 'Differential'].includes(value)) throw new TypeError('Invalid backup level')
  if (key === 'priority' && (!/^\d+$/.test(value) || Number(value) < 1 || Number(value) > 100)) throw new TypeError('Invalid priority')
  if (key === 'storagegrouppolicy' && !['ListedOrder', 'LeastUsed'].includes(value)) throw new TypeError('Invalid policy')
  const canonical = { name: 'Name', description: 'Description', type: 'Type', jobdefs: 'JobDefs',
    client: 'Client', fileset: 'FileSet', schedule: 'Schedule', pool: 'Pool', storage: 'Storage',
    storagegrouppolicy: 'StorageGroupPolicy', level: 'Level', priority: 'Priority', messages: 'Messages' }
  return [{ key: canonical[key], value, quoted: !['type', 'storage', 'level', 'priority'].includes(key) }]
})

export function buildConfigureAddJobCommand(form) {
  if (!form.name) throw new TypeError('Job name required')
  return buildConfigureAddCommand('job', jobValues(form))
}

export function buildJobResourcePreview(form) {
  if (!form.name) throw new TypeError('Job name required')
  return buildConfigureAddPreview('job', jobValues(form))
}
