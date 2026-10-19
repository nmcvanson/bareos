/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026 Bareos GmbH & Co. KG

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

export function filesetIncludePaths(showFilesetJson, name) {
  if (!showFilesetJson || typeof showFilesetJson !== 'object') {
    return []
  }

  const root = showFilesetJson.result ?? showFilesetJson
  const filesets = root.filesets ?? root

  let targetFileset = null

  if (Array.isArray(filesets)) {
    if (name) {
      targetFileset = filesets.find((f) => f && (f.name === name || f.fileset === name))
    } else if (filesets.length === 1) {
      targetFileset = filesets[0]
    }
  } else if (filesets && typeof filesets === 'object') {
    if (name && filesets[name]) {
      targetFileset = filesets[name]
    } else if (name && (filesets.name === name || filesets.fileset === name)) {
      targetFileset = filesets
    } else if (!name) {
      const keys = Object.keys(filesets)
      if (keys.length === 1) {
        targetFileset = filesets[keys[0]]
      } else if (filesets.include) {
        targetFileset = filesets
      }
    } else {
      for (const val of Object.values(filesets)) {
        if (val && typeof val === 'object' && (val.name === name || val.fileset === name)) {
          targetFileset = val
          break
        }
      }
    }
  }

  if (!targetFileset) {
    return []
  }

  const rawIncludes = targetFileset.include ?? targetFileset.Include
  const includes = Array.isArray(rawIncludes)
    ? rawIncludes
    : (rawIncludes ? [rawIncludes] : [])

  const paths = []
  for (const inc of includes) {
    if (!inc) continue
    const files = inc.file ?? inc.File ?? inc.files ?? inc.Files
    if (Array.isArray(files)) {
      for (const f of files) {
        if (typeof f === 'string' && f.length > 0) {
          paths.push(f)
        }
      }
    } else if (typeof files === 'string' && files.length > 0) {
      paths.push(files)
    }
  }

  return paths
}
