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


def list_attached_volumes(conn, server, attachments=None):
    """
    Return the Cinder volumes attached to a Nova server. Pass a
    precomputed `attachments` list to avoid re-fetching it when the caller
    already has one (e.g. also needed for instance_metadata).
    """
    if attachments is None:
        attachments = conn.compute.volume_attachments(server)
    return [conn.block_storage.get_volume(a.volume_id) for a in attachments]


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
    it to `proxy`. On any failure, cleans up whatever was already created
    (best-effort) before re-raising - callers do not need to call
    detach_and_cleanup() themselves on the exception path.
    """
    snapshot = None
    new_volume = None
    attached = False
    try:
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
        attached = True
        new_volume = conn.block_storage.wait_for_status(
            new_volume, status="in-use", failures=["error"], wait=timeout
        )

        return AttachedVolumeCopy(
            original_volume_id=volume.id,
            snapshot_id=snapshot.id,
            volume_id=new_volume.id,
            device=attachment.device,
        )
    except Exception:
        _cleanup_partial(conn, proxy, snapshot, new_volume, attached, timeout)
        raise


def _cleanup_partial(conn, proxy, snapshot, new_volume, attached, timeout):
    """
    Best-effort cleanup of whatever snapshot_and_attach() managed to create
    before it failed. Never raises - matches detach_and_cleanup()'s
    philosophy that anything left over is caught by tagging-based garbage
    collection, not by this call succeeding.
    """
    if attached and new_volume is not None:
        try:
            conn.compute.delete_volume_attachment(new_volume.id, proxy)
            conn.block_storage.wait_for_status(
                conn.block_storage.get_volume(new_volume.id),
                status="available",
                failures=["error"],
                wait=timeout,
            )
        except Exception:  # pylint: disable=broad-except
            pass
    if new_volume is not None:
        try:
            conn.block_storage.delete_volume(new_volume.id)
        except Exception:  # pylint: disable=broad-except
            pass
    if snapshot is not None:
        try:
            conn.block_storage.delete_snapshot(snapshot.id)
        except Exception:  # pylint: disable=broad-except
            pass


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
