/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026-2026 Bareos GmbH & Co. KG

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
 */

export const CONFIG_TYPES = [
  { key: 'job', label: 'Jobs', showCommand: 'show jobs', writable: true },
  { key: 'jobdefs', label: 'JobDefs', showCommand: 'show jobdefs', writable: true },
  { key: 'client', label: 'Clients', showCommand: 'show clients', writable: true },
  { key: 'fileset', label: 'FileSets', showCommand: 'show filesets', writable: false },
  { key: 'schedule', label: 'Schedules', showCommand: 'show schedules', writable: true },
  { key: 'pool', label: 'Pools', showCommand: 'show pools', writable: true },
  { key: 'storage', label: 'Storages', showCommand: 'show storages', writable: true },
  { key: 'messages', label: 'Messages', showCommand: 'show messages', writable: true },
  { key: 'console', label: 'Consoles', showCommand: 'show consoles', writable: false },
  { key: 'profile', label: 'Profiles', showCommand: 'show profiles', writable: false },
]

export const MAIN_DIRECTIVES = {
  job: ['jobdefs', 'client', 'fileset', 'pool'],
  jobdefs: ['client', 'fileset', 'pool', 'storage'],
  client: ['address', 'uname', 'version', 'enabled'],
  fileset: ['description', 'include'],
  schedule: ['enabled', 'runs'],
  pool: ['pooltype', 'maximumvolumes', 'volretention', 'storage'],
  storage: ['address', 'mediatype', 'device', 'maximumconcurrentjobs'],
  messages: ['mailcommand', 'operatorcommand', 'destination'],
  console: ['tlsenable', 'tlsrequire'],
  profile: ['commandacl', 'jobacl', 'scheduleacl', 'poolacl'],
}

export function stripPasswords(val) {
  if (Array.isArray(val)) {
    return val.map(stripPasswords)
  }
  if (val && typeof val === 'object') {
    const out = {}
    for (const [k, v] of Object.entries(val)) {
      if (/password/i.test(k)) {
        continue
      }
      out[k] = stripPasswords(v)
    }
    return out
  }
  return val
}

export function normaliseResources(type, json) {
  if (!json) return []
  let raw = json?.result ?? json
  if (raw && typeof raw === 'object' && !Array.isArray(raw)) {
    const plural = type ? `${type}s` : ''
    if (type && raw[type] && typeof raw[type] === 'object' && !Array.isArray(raw[type])) {
      raw = raw[type]
    } else if (plural && raw[plural] && typeof raw[plural] === 'object' && !Array.isArray(raw[plural])) {
      raw = raw[plural]
    } else {
      const keys = Object.keys(raw)
      if (keys.length === 1 && typeof raw[keys[0]] === 'object' && raw[keys[0]] !== null && !Array.isArray(raw[keys[0]])) {
        const inner = raw[keys[0]]
        const firstVal = Object.values(inner)[0]
        if (firstVal && typeof firstVal === 'object') {
          raw = inner
        }
      }
    }
  }

  const list = []
  if (Array.isArray(raw)) {
    for (const item of raw) {
      if (!item || typeof item !== 'object') continue
      const name = item.name ?? ''
      const directives = stripPasswords(item)
      list.push({ name, directives })
    }
  } else if (raw && typeof raw === 'object') {
    for (const [k, v] of Object.entries(raw)) {
      if (!v || typeof v !== 'object') continue
      const name = v.name ?? k
      const directives = stripPasswords(v)
      list.push({ name, directives })
    }
  }

  return list.sort((a, b) => String(a.name).localeCompare(String(b.name)))
}

function valueMatches(val, target) {
  if (val === target) return true
  if (Array.isArray(val)) {
    return val.some((item) => valueMatches(item, target))
  }
  if (val && typeof val === 'object') {
    return Object.values(val).some((item) => valueMatches(item, target))
  }
  return false
}

export function referencesTo(name, type, allResources) {
  if (!name || !allResources) return []
  const matches = []

  const inspectResource = (resType, resName, directives) => {
    if (!resType || resType === type || !directives || typeof directives !== 'object') return
    for (const [dirKey, dirVal] of Object.entries(directives)) {
      if (dirKey === 'name') continue
      if (valueMatches(dirVal, name)) {
        matches.push({
          type: resType,
          name: resName,
          directive: dirKey,
        })
      }
    }
  }

  if (Array.isArray(allResources)) {
    for (const item of allResources) {
      if (!item) continue
      inspectResource(item.type, item.name, item.directives ?? item)
    }
  } else if (allResources && typeof allResources === 'object') {
    for (const [resType, group] of Object.entries(allResources)) {
      if (resType === type || !group) continue
      if (Array.isArray(group)) {
        for (const item of group) {
          if (!item) continue
          inspectResource(resType, item.name, item.directives ?? item)
        }
      } else if (typeof group === 'object') {
        const norm = normaliseResources(resType, group)
        for (const item of norm) {
          inspectResource(resType, item.name, item.directives)
        }
      }
    }
  }

  const seen = new Set()
  const result = []
  for (const m of matches) {
    const key = `${m.type}:${m.name}:${m.directive}`
    if (!seen.has(key)) {
      seen.add(key)
      result.push(m)
    }
  }

  return result.sort((a, b) => {
    const typeCmp = a.type.localeCompare(b.type)
    if (typeCmp !== 0) return typeCmp
    const nameCmp = a.name.localeCompare(b.name)
    if (nameCmp !== 0) return nameCmp
    return a.directive.localeCompare(b.directive)
  })
}

export function isInherited(job, directive) {
  if (!job || !directive) return false
  if (job.inherited && job.inherited[directive] !== undefined) return true
  if (Array.isArray(job.inheritedKeys) && job.inheritedKeys.includes(directive)) return true
  return false
}

export function withInherited(job, jobdefs) {
  if (!job || typeof job !== 'object') return job

  const jobDirectives = job.directives && typeof job.directives === 'object'
    ? job.directives
    : job
  const defName = jobDirectives.jobdefs ?? job.jobdefs

  if (!defName || !jobdefs) {
    return {
      ...job,
      inherited: {},
      inheritedKeys: [],
    }
  }

  let def = null
  if (jobdefs instanceof Map) {
    def = jobdefs.get(defName)
  } else if (Array.isArray(jobdefs)) {
    def = jobdefs.find((d) => (d.name ?? d.directives?.name) === defName)
  } else if (typeof jobdefs === 'object') {
    const rawJobdefs = jobdefs.result?.jobdefs ?? jobdefs.jobdefs ?? jobdefs
    if (Array.isArray(rawJobdefs)) {
      def = rawJobdefs.find((d) => (d.name ?? d.directives?.name) === defName)
    } else if (rawJobdefs[defName]) {
      def = rawJobdefs[defName]
    } else if ((rawJobdefs.name ?? rawJobdefs.directives?.name) === defName) {
      def = rawJobdefs
    }
  }

  if (!def || typeof def !== 'object') {
    return {
      ...job,
      inherited: {},
      inheritedKeys: [],
    }
  }

  const defDirectives = def.directives && typeof def.directives === 'object'
    ? def.directives
    : def

  const inherited = {}
  const mergedDirectives = { ...jobDirectives }

  for (const [key, val] of Object.entries(defDirectives)) {
    if (key === 'name' || key === 'directives') continue
    const jobVal = jobDirectives[key]
    const isUnset = jobVal === undefined || jobVal === null || jobVal === ''
    if (isUnset && val !== undefined && val !== null && val !== '') {
      mergedDirectives[key] = val
      inherited[key] = val
    }
  }

  const inheritedKeys = Object.keys(inherited)

  if (job.directives && typeof job.directives === 'object') {
    return {
      ...mergedDirectives,
      ...job,
      directives: mergedDirectives,
      inherited,
      inheritedKeys,
    }
  }

  return {
    ...mergedDirectives,
    directives: mergedDirectives,
    inherited,
    inheritedKeys,
  }
}
