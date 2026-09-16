# BAREOS - Backup Archiving REcovery Open Sourced
#
# Copyright (C) 2026 Bareos GmbH & Co. KG
#
# This program is Free Software; you can redistribute it and/or
# modify it under the terms of version three of the GNU Affero General Public
# License as published by the Free Software Foundation, which is
# listed in the file LICENSE.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
# 02110-1301, USA.

"""Cinder volume snapshot/attach handling for the volume-path ingestion.

Kept free of any `bareosfd` dependency so this is unit-testable with plain
`unittest`. Cleanup never raises - it does its best and returns a list of
warning strings for anything that failed, since callers are expected to
attempt cleanup unconditionally (e.g. from a finally block) even when
earlier steps already failed.
"""

from dataclasses import dataclass


def list_attached_volumes(conn, server):
    """Return the Cinder volumes attached to a Nova server."""
    volumes = []
    for attachment in conn.compute.volume_attachments(server):
        volumes.append(conn.block_storage.get_volume(attachment.volume_id))
    return volumes


@dataclass
class AttachedVolumeCopy:
    """A temp volume, cloned from a snapshot of the original and attached
    to the proxy - the state needed to clean it up again later."""

    original_volume_id: str
    snapshot_id: str
    volume_id: str
    device: str


def snapshot_and_attach(conn, volume, proxy, tags, timeout=900):
    """
    Snapshot `volume`, create a new volume from that snapshot, and attach
    it to `proxy`. Raises on failure - the caller is responsible for
    calling detach_and_cleanup() on whatever was already created.
    """
    snapshot = conn.block_storage.create_snapshot(
        volume_id=volume.id,
        name=f"bareos-{volume.id}",
        force=True,
        metadata=tags,
    )
    conn.block_storage.wait_for_status(
        snapshot, status="available", failures=["error"], wait=timeout
    )

    new_volume = conn.block_storage.create_volume(
        name=f"bareos-restore-{snapshot.id}",
        snapshot_id=snapshot.id,
        size=volume.size,
        metadata=tags,
    )
    conn.block_storage.wait_for_status(
        new_volume, status="available", failures=["error"], wait=timeout
    )

    attachment = conn.compute.create_volume_attachment(
        proxy, volume_id=new_volume.id
    )
    new_volume = conn.block_storage.wait_for_status(
        new_volume, status="in-use", failures=["error"], wait=timeout
    )

    return AttachedVolumeCopy(
        original_volume_id=volume.id,
        snapshot_id=snapshot.id,
        volume_id=new_volume.id,
        device=attachment.device,
    )


def detach_and_cleanup(conn, proxy, copy, keep_snapshot=False, timeout=900):
    """
    Detach and delete the temp volume, then delete the snapshot (unless
    keep_snapshot). Best-effort: never raises, returns a list of warning
    strings for whatever couldn't be cleaned up.
    """
    warnings = []

    try:
        conn.compute.delete_volume_attachment(copy.volume_id, proxy)
        conn.block_storage.wait_for_status(
            conn.block_storage.get_volume(copy.volume_id),
            status="available",
            failures=["error"],
            wait=timeout,
        )
    except Exception as e:  # pylint: disable=broad-except
        warnings.append(f"detach of volume {copy.volume_id} failed: {e}")

    try:
        conn.block_storage.delete_volume(copy.volume_id)
    except Exception as e:  # pylint: disable=broad-except
        warnings.append(f"delete of temp volume {copy.volume_id} failed: {e}")

    if not keep_snapshot:
        try:
            conn.block_storage.delete_snapshot(copy.snapshot_id)
        except Exception as e:  # pylint: disable=broad-except
            warnings.append(f"delete of snapshot {copy.snapshot_id} failed: {e}")

    return warnings
