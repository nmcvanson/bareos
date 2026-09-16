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


def create_snapshot(conn, volume, tags, timeout=900):
    """
    Create a crash-consistent snapshot of `volume`. On failure, deletes
    the snapshot if it was created but never reached 'available'; a
    cleanup failure there is attached to the raised exception as
    `cleanup_warnings` (a list of strings) rather than swallowed.
    """
    snapshot = conn.block_storage.create_snapshot(
        volume_id=volume.id,
        name=f"bareos-{volume.id}",
        force=True,
        metadata=tags,
    )
    try:
        conn.block_storage.wait_for_status(
            snapshot, status="available", failures=["error"], wait=timeout
        )
    except Exception as e:
        try:
            conn.block_storage.delete_snapshot(snapshot.id)
        except Exception as cleanup_e:  # pylint: disable=broad-except
            e.cleanup_warnings = [
                f"delete of snapshot {snapshot.id} failed: {cleanup_e}"
            ]
        raise
    return snapshot


def create_volume_from_snapshot_and_attach(conn, snapshot, proxy, tags, timeout=900):
    """
    Create a new volume from an already-taken snapshot and attach it to
    `proxy`. On any failure, cleans up whatever was already created and
    attaches the resulting warnings to the raised exception as
    `cleanup_warnings` (empty list if cleanup fully succeeded).
    """
    new_volume = None
    attached = False
    try:
        new_volume = conn.block_storage.create_volume(
            name=f"bareos-restore-{snapshot.id}",
            snapshot_id=snapshot.id,
            size=snapshot.size,
            metadata=tags,
        )
        conn.block_storage.wait_for_status(
            new_volume, status="available", failures=["error"], wait=timeout
        )

        conn.compute.create_volume_attachment(proxy, volume_id=new_volume.id)
        attached = True
        new_volume = conn.block_storage.wait_for_status(
            new_volume, status="in-use", failures=["error"], wait=timeout
        )
        # Re-fetch: the attachment object returned by create_volume_attachment()
        # can have device=None - Nova assigns the device path asynchronously
        # as the hypervisor completes the hotplug, not in the creation response.
        attachment = conn.compute.get_volume_attachment(proxy, new_volume.id)

        return AttachedVolumeCopy(
            original_volume_id=snapshot.volume_id,
            snapshot_id=snapshot.id,
            volume_id=new_volume.id,
            device=attachment.device,
        )
    except Exception as e:
        e.cleanup_warnings = _cleanup_partial_volume(
            conn, proxy, new_volume, attached, timeout
        )
        raise


def _cleanup_partial_volume(conn, proxy, new_volume, attached, timeout):
    """
    Best-effort cleanup of a partially-created temp volume/attachment.
    Never raises; returns a list of warning strings for anything that
    couldn't be cleaned up (same convention as detach_and_cleanup()).
    """
    warnings = []
    if attached and new_volume is not None:
        try:
            conn.compute.delete_volume_attachment(new_volume.id, proxy)
            conn.block_storage.wait_for_status(
                conn.block_storage.get_volume(new_volume.id),
                status="available",
                failures=["error"],
                wait=timeout,
            )
        except Exception as e:  # pylint: disable=broad-except
            warnings.append(f"detach of volume {new_volume.id} failed: {e}")
    if new_volume is not None:
        try:
            conn.block_storage.delete_volume(new_volume.id)
        except Exception as e:  # pylint: disable=broad-except
            warnings.append(f"delete of temp volume {new_volume.id} failed: {e}")
    return warnings


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
