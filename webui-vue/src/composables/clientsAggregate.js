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

import {
  directorCollection,
  normaliseClient,
} from './useDirectorFetch.js'
import {
  directorAggregateErrors,
  fulfilledDirectorValues,
  runDirectorAggregates,
} from './directorAggregateRunner.js'
import { markConfiguredClients } from '../utils/clients.js'

function decorateClients(entries, dotResult, director) {
  return markConfiguredClients(directorCollection(entries), dotResult).map((entry) => {
    const client = normaliseClient(entry)
    return {
      ...client,
      configured: entry.configured,
      director,
      scopeKey: `${director}:${client.name}`,
    }
  })
}

function sortClients(clients) {
  return [...clients].sort((left, right) => {
    const nameCompare = String(left.name ?? '').localeCompare(String(right.name ?? ''))
    if (nameCompare !== 0) {
      return nameCompare
    }

    return String(left.director ?? '').localeCompare(String(right.director ?? ''))
  })
}

export async function fetchAggregatedClients(credentials, directors) {
  const results = await runDirectorAggregates(credentials, directors, async ({ client, director }) => {
    const [listResult, dotResult] = await Promise.all([
      client.call('llist clients'),
      client.call('.clients').catch(() => null),
    ])

    return {
      director,
      clients: decorateClients(listResult?.clients, dotResult, director),
    }
  })

  return {
    clients: sortClients(fulfilledDirectorValues(results).flatMap(value => value.clients)),
    directorErrors: directorAggregateErrors(results, directors, 'Failed to load clients.'),
  }
}
