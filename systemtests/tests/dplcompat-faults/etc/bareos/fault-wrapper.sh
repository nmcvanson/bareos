#!/bin/bash
#   BAREOS® - Backup Archiving REcovery Open Sourced
#
#   Copyright (C) 2026-2026 Bareos GmbH & Co. KG
#
#   This program is Free Software; you can redistribute it and/or
#   modify it under the terms of version three of the GNU Affero General Public
#   License as published by the Free Software Foundation and included
#   in the file LICENSE.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#   Affero General Public License for more details.
#
#   You should have received a copy of the GNU Affero General Public License
#   along with this program; if not, write to the Free Software
#   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
#   02110-1301, USA.

# The local-directory wrapper of the dplcompat test with injected faults.
# Files in $fault_dir switch the faults on:
#   fail-uploads           every upload fails
#   fail-uploads-until     uploads fail until this epoch second
#   fail-uploads-once      the next upload fails (the file is removed)
#   fail-list-once         the next "list" fails (the file is removed)
#   list-empty-after-ok    "list" of a volume prints nothing once this many
#                          uploads of that volume succeeded
#   slow-uploads           every upload waits this many seconds before it
#                          reads its data
# Every upload try is logged to $fault_dir/upload.log as
# "<volume>.<chunk> ok|failed <epoch seconds>".

set -Eeuo pipefail

: "${fault_dir:=/nonexistent}"

get_filesize()
{
  if [ "$(uname)" = "FreeBSD" ]; then
    stat -f %z "$1"
  else
    stat "--format=%s" "$1"
  fi
}

upload_fails()
{
  [ -e "$fault_dir/fail-uploads" ] && return 0
  rm "$fault_dir/fail-uploads-once" 2>/dev/null && return 0
  if [ -e "$fault_dir/fail-uploads-until" ]; then
    [ "$(date +%s)" -lt "$(cat "$fault_dir/fail-uploads-until")" ] && return 0
  fi
  return 1
}

log_upload()
{
  if [ -d "$fault_dir" ]; then
    echo "$1 $2 $(date +%s)" >>"$fault_dir/upload.log"
  fi
}

list_is_empty()
{
  [ -e "$fault_dir/list-empty-after-ok" ] || return 1
  local ok
  ok=$(grep -c "^$1\.[0-9]* ok " "$fault_dir/upload.log" || :)
  [ "${ok:-0}" -ge "$(cat "$fault_dir/list-empty-after-ok")" ]
}

case "$1" in
  options)
    cat <<'_EOT_'
storage_path
fault_dir
_EOT_
    ;;
  testconnection)
    [ -d "$storage_path" ]
    ;;
  list)
    if rm "$fault_dir/fail-list-once" 2>/dev/null; then
      echo "injected list failure for $2" >&2
      exit 1
    fi
    list_is_empty "$2" && exit 0
    for f in "$storage_path/$2".*; do
      base="$(basename "$f")"
      printf "%s %d\n" "${base##*.}" "$(get_filesize "$f")"
    done
    ;;
  stat)
    get_filesize "$storage_path/$2.$3"
    ;;
  upload)
    if [ -e "$fault_dir/slow-uploads" ]; then
      sleep "$(cat "$fault_dir/slow-uploads")"
    fi
    if upload_fails; then
      cat >/dev/null
      log_upload "$2.$3" failed
      echo "injected upload failure for $2/$3" >&2
      exit 1
    fi
    cat >"$storage_path/$2.$3"
    log_upload "$2.$3" ok
    ;;
  download)
    exec cat "$storage_path/$2.$3"
    ;;
  remove)
    exec rm "$storage_path/$2.$3"
    ;;
  *)
    exit 2
    ;;
esac
