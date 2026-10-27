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

import { computed } from 'vue'
import { liveScope } from './liveProgress.js'

// Job, requested and resolved Director of the job details page. Without a
// Director query the resolved Director is the authenticated or configured one,
// so the scope changes when the active Director changes.
export function useJobScope({ route, auth, settings }) {
  const jobId = computed(() => route.params.id)
  const requestedDirector = computed(() => (
    typeof route.query.director === 'string' ? route.query.director : ''
  ))
  const jobDirector = computed(() => (
    requestedDirector.value || auth.user?.director || settings.directorName || ''
  ))
  const scopeKey = computed(() => liveScope(jobId.value, jobDirector.value))
  return { jobId, requestedDirector, jobDirector, scopeKey }
}
