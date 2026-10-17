# BAREOS - Backup Archiving REcovery Open Sourced
#
# Copyright (C) 2026-2026 Bareos GmbH & Co. KG
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

"""Cloud side of a backup job: snapshots, temporary volumes and their cleanup."""

import threading
import time
from dataclasses import dataclass

from .cloud import (
    CONSISTENCY_GROUP,
    CONSISTENCY_PER_VOLUME,
    ROLE_BACKUP_SNAPSHOT,
    ROLE_BACKUP_TEMP_VOLUME,
    TAG_CREATED_BY,
    TAG_CREATED_BY_VALUE,
    TAG_JOB_ID,
    TAG_ROLE,
    TAG_SERVER_ID,
    CloudCancelled,
    CloudConfigError,
    CloudConflict,
    CloudError,
    GroupSnapshotInfo,
    ServerInfo,
    bareos_tags,
    group_snapshot_description,
    group_snapshot_name,
)

USABLE_STATUSES = ("ACTIVE", "SHUTOFF", "PAUSED", "SUSPENDED")


@dataclass
class BackupInventory:
    server: ServerInfo
    volumes: list
    ports: list
    consistency: str = CONSISTENCY_PER_VOLUME


def _no_log(_level, _message):
    return None


def _tags(resource):
    """Metadata of a volume or snapshot, tags of a group snapshot."""
    if isinstance(resource, GroupSnapshotInfo):
        return resource.tags
    return resource.metadata


class BackupCloudSession:
    """Prepares a server for backup and removes everything the job created."""

    def __init__(
        self,
        backend,
        job_id,
        server_id="",
        server_name="",
        poll_timeout=900,
        snapshot_force=True,
        cleanup_leftovers=False,
        log=None,
        snapshot_mode="per-volume",
    ):
        if snapshot_mode not in ("per-volume", "group"):
            raise CloudConfigError(
                "snapshot_mode must be per-volume or group, not %r" % (snapshot_mode,)
            )
        self.snapshot_mode = snapshot_mode
        self.group_snapshot_id = ""
        self.group_members = {}
        self.group_pending = set()
        self.backend = backend
        self.job_id = job_id
        self.server_id = server_id
        self.server_name = server_name
        self.poll_timeout = poll_timeout
        self.snapshot_force = snapshot_force
        self.cleanup_leftovers = cleanup_leftovers
        self._logger = log or _no_log
        self.inventory = None
        self.snapshots = {}
        self.temp_volumes = {}
        self.attach_intents = set()
        self.closed = False
        self._lock = threading.Lock()

    def _log(self, level, message):
        try:
            self._logger(level, message)
        except Exception:  # pylint: disable=broad-except
            pass

    # Preparation ------------------------------------------------------------

    def prepare(self):
        """Check the server, report or remove leftovers and snapshot its volumes."""
        be = self.backend
        if bool(self.server_id) == bool(self.server_name):
            raise CloudConfigError("set exactly one of server_id and server_name")
        server = (
            be.get_server(self.server_id)
            if self.server_id
            else be.find_server(self.server_name)
        )
        if server.status not in USABLE_STATUSES:
            raise CloudConflict(
                "server %s is %s; a backup needs ACTIVE, SHUTOFF, PAUSED or SUSPENDED"
                % (server.id, server.status)
            )
        project = be.project_id
        if project and server.project_id and project != server.project_id:
            raise CloudConflict(
                "server %s belongs to project %s, not to the connected project %s"
                % (server.id, server.project_id, project)
            )
        volumes = sorted(
            be.list_server_volumes(server.id),
            key=lambda v: (0 if v.boot_index == 0 else 1, v.device, v.id),
        )
        if not server.boot_from_volume:
            raise CloudConflict(
                "server %s boots from an image; only servers that boot from a "
                "Cinder volume are supported" % server.id
            )
        encrypted = [v.id for v in volumes if v.encrypted]
        if encrypted:
            raise CloudConflict(
                "encrypted volumes cannot be backed up: %s" % ", ".join(encrypted)
            )
        if not volumes:
            raise CloudConflict("server %s has no volumes to back up" % server.id)
        group_mode = self.snapshot_mode == "group"
        ports = be.list_server_ports(server.id)
        self.inventory = BackupInventory(server, volumes, ports)
        if group_mode:
            self.inventory.consistency = CONSISTENCY_GROUP
        self._handle_leftovers(server)
        if group_mode:
            self._group_snapshot(server, volumes)
        else:
            for vol in volumes:
                self._snapshot(server, vol)
        return self.inventory

    def _group_snapshot(self, server, volumes):
        """One group snapshot of the group that holds exactly the volumes."""
        be = self.backend
        group_id = be.find_consistency_group([v.id for v in volumes])

        def record(group_snapshot_id):
            with self._lock:
                self.group_snapshot_id = group_snapshot_id

        members = self._timed(
            "group snapshot",
            "group %s" % group_id,
            be.create_group_snapshot,
            group_id,
            group_snapshot_name(self.job_id, server.id),
            group_snapshot_description(self.job_id, server.id),
            self.poll_timeout,
            on_created=record,
        )
        missing = [v.id for v in volumes if v.id not in members]
        if missing:
            raise CloudConflict(
                "the group snapshot has no snapshot of volume(s) %s"
                % ", ".join(missing)
            )
        with self._lock:
            self.group_members = dict(members)
            self.group_pending = {v.id for v in volumes}
        self._log(
            "info",
            "group snapshot %s of group %s created"
            % (self.group_snapshot_id, group_id),
        )

    def _leftovers(self, server):
        """Return (kind, resource) of earlier jobs for this server."""
        wanted = {TAG_CREATED_BY: TAG_CREATED_BY_VALUE, TAG_SERVER_ID: server.id}
        found = []
        groups = []
        try:
            if self.snapshot_mode == "group":
                for gsnap in self.backend.list_group_snapshots("bareos-"):
                    tags = gsnap.tags
                    if tags.get(TAG_SERVER_ID) == server.id:
                        groups.append(gsnap)
            for vol in self.backend.list_volumes(
                dict(wanted, **{TAG_ROLE: ROLE_BACKUP_TEMP_VOLUME})
            ):
                found.append(("volume", vol))
            for snap in self.backend.list_snapshots(
                dict(wanted, **{TAG_ROLE: ROLE_BACKUP_SNAPSHOT})
            ):
                found.append(("snapshot", snap))
        except CloudError as exc:
            self._log("warning", "cannot look for leftover resources: %s" % exc)
            return []
        result = []
        for kind, res in found:
            if _tags(res).get(TAG_JOB_ID) == str(self.job_id):
                continue
            result.append((kind, res))
        for gsnap in groups:
            if gsnap.tags.get(TAG_JOB_ID) != str(self.job_id):
                result.append(("group snapshot", gsnap))
        return result

    def _lower_job(self, job):
        return str(job).isdigit() and int(job) < int(self.job_id)

    def _handle_leftovers(self, server):
        leftovers = self._leftovers(server)
        if not leftovers:
            return
        if not self.cleanup_leftovers:
            items = [
                "%s %s (job %s)" % (kind, res.id, _tags(res).get(TAG_JOB_ID))
                for kind, res in leftovers
            ]
            self._log(
                "warning",
                "resources of earlier jobs found: %s; set cleanup_leftovers=yes to "
                "remove them" % ", ".join(items),
            )
            return
        self._log(
            "warning",
            "cleanup_leftovers=yes assumes no other job backs up server %s at the "
            "same time" % server.id,
        )
        removable, kept = [], []
        for kind, res in leftovers:
            job = _tags(res).get(TAG_JOB_ID, "")
            if self._lower_job(job):
                removable.append((kind, res))
            else:
                kept.append("%s %s" % (kind, res.id))
        if kept:
            self._log(
                "warning",
                "leftover resources not removed (job id not lower than this job): %s"
                % ", ".join(kept),
            )
        try:
            attached = set(self.backend.list_proxy_attachments())
        except CloudError as exc:
            self._log("warning", "cannot list proxy attachments: %s" % exc)
            attached = set()
        for kind, res in [
            r
            for name in ("volume", "snapshot", "group snapshot")
            for r in removable
            if r[0] == name
        ]:
            try:
                if kind == "volume":
                    if res.id in attached:
                        self.backend.detach_from_proxy(res.id, self.poll_timeout)
                    self.backend.delete_volume(res.id, self.poll_timeout)
                elif kind == "snapshot":
                    self.backend.delete_snapshot(res.id, self.poll_timeout)
                else:
                    self.backend.delete_group_snapshot(res.id, self.poll_timeout)
                self._log("info", "removed leftover %s %s" % (kind, res.id))
            except CloudError as exc:
                self._log(
                    "warning",
                    "could not remove leftover %s %s: %s" % (kind, res.id, exc),
                )
    def _timed(self, step, subject, func, *args, **kwargs):
        """Run func and log "<step> of <subject> took <s>.<ms> s"."""
        started = time.monotonic()
        result = func(*args, **kwargs)
        self._log(
            "info",
            "%s of %s took %.3f s" % (step, subject, time.monotonic() - started),
        )
        return result

    def _record(self, table, key, kind):
        """Return an on_created callback recording the id under `key`."""

        def record(resource_id):
            with self._lock:
                table[key] = resource_id
                closed = self.closed
            if closed:
                self._log(
                    "warning",
                    "%s %s was created after cleanup started; it will be reported "
                    "as a leftover" % (kind, resource_id),
                )

        return record

    def _snapshot(self, server, vol):
        snap = self._timed(
            "snapshot",
            "volume %s" % vol.id,
            self.backend.create_snapshot,
            vol.id,
            "bareos-%s-snap-%s" % (self.job_id, vol.id),
            bareos_tags(self.job_id, server.id, ROLE_BACKUP_SNAPSHOT, vol.id),
            force=self.snapshot_force,
            timeout=self.poll_timeout,
            on_created=self._record(self.snapshots, vol.id, "snapshot"),
        )
        self._log("info", "snapshot %s of volume %s created" % (snap.id, vol.id))

    # Reading ----------------------------------------------------------------

    def open_volume(self, volume_id):
        """Create a temporary volume from the snapshot, attach it, return its path."""
        if self.closed:
            raise CloudCancelled("session closed")
        if self.inventory is None:
            raise CloudError("the session is not prepared")
        src = next((v for v in self.inventory.volumes if v.id == volume_id), None)
        snap_id = self.snapshots.get(volume_id) or self.group_members.get(volume_id, "")
        if src is None or not snap_id:
            raise CloudError("volume %s has no snapshot" % volume_id)
        server = self.inventory.server
        temp = self._timed(
            "temporary volume create",
            "volume %s" % volume_id,
            self.backend.create_volume,
            "bareos-%s-tmp-%s" % (self.job_id, volume_id),
            src.size_gb,
            bareos_tags(self.job_id, server.id, ROLE_BACKUP_TEMP_VOLUME, volume_id),
            self.poll_timeout,
            availability_zone=src.availability_zone,
            snapshot_id=snap_id,
            on_created=self._record(self.temp_volumes, volume_id, "volume"),
        )
        with self._lock:
            if self.closed:
                raise CloudCancelled("session closed")
            self.attach_intents.add(temp.id)
        path = self._timed(
            "attach until the device was found",
            "volume %s" % volume_id,
            self.backend.attach_to_proxy,
            temp.id,
            self.poll_timeout,
        )
        self._log("info", "volume %s attached as %s" % (temp.id, path))
        return path

    # Cleanup ----------------------------------------------------------------

    def release_volume(self, volume_id):
        """Remove the cloud resources of one source volume."""
        try:
            self._release_cloud(volume_id)
        except Exception as exc:  # pylint: disable=broad-except
            self._log("warning", "release of volume %s failed: %s" % (volume_id, exc))

    def _release_cloud(self, volume_id):
        """Remove the temporary volume and snapshot of one source volume."""
        with self._lock:
            temp = self.temp_volumes.get(volume_id)
            snap = self.snapshots.get(volume_id)
            intent = temp in self.attach_intents
        deletable = True
        if temp and intent:
            try:
                self._timed(
                    "detach",
                    "volume %s" % volume_id,
                    self.backend.detach_from_proxy,
                    temp,
                    self.poll_timeout,
                )
                with self._lock:
                    self.attach_intents.discard(temp)
            except Exception as exc:  # pylint: disable=broad-except
                deletable = False
                self._log("warning", "could not detach volume %s: %s" % (temp, exc))
        if temp and deletable:
            try:
                self._timed(
                    "delete of temporary volume",
                    "volume %s" % volume_id,
                    self.backend.delete_volume,
                    temp,
                    self.poll_timeout,
                )
                with self._lock:
                    self.temp_volumes.pop(volume_id, None)
            except Exception as exc:  # pylint: disable=broad-except
                self._log("warning", "could not delete volume %s: %s" % (temp, exc))
        if snap:
            try:
                self._timed(
                    "delete of snapshot",
                    "volume %s" % volume_id,
                    self.backend.delete_snapshot,
                    snap,
                    self.poll_timeout,
                )
                with self._lock:
                    self.snapshots.pop(volume_id, None)
            except Exception as exc:  # pylint: disable=broad-except
                self._log("warning", "could not delete snapshot %s: %s" % (snap, exc))
        self._release_group_member(volume_id)

    def _release_group_member(self, volume_id):
        """Delete the group snapshot once the last temporary volume is gone."""
        with self._lock:
            if not self.group_snapshot_id:
                return
            if volume_id not in self.temp_volumes:
                self.group_pending.discard(volume_id)
            ready = not self.group_pending and not self.temp_volumes
            group_snapshot_id = self.group_snapshot_id
        if not ready:
            return
        try:
            self._timed(
                "delete of group snapshot",
                "group snapshot %s" % group_snapshot_id,
                self.backend.delete_group_snapshot,
                group_snapshot_id,
                self.poll_timeout,
            )
            with self._lock:
                self.group_snapshot_id = ""
                self.group_members = {}
        except Exception as exc:  # pylint: disable=broad-except
            self._log(
                "warning",
                "could not delete group snapshot %s: %s" % (group_snapshot_id, exc),
            )

    def close(self):
        with self._lock:
            self.closed = True

    def cleanup(self):
        """Remove every resource of this job; return the ones that remain."""
        failed = {}
        try:
            self._cleanup(failed)
        except Exception as exc:  # pylint: disable=broad-except
            self._log("warning", "cleanup failed unexpectedly: %s" % exc)
        for entry in failed:
            self._log("warning", "left behind: %s" % entry)
        return list(failed)

    def _detach(self, volume_id, failed):
        try:
            self._timed(
                "detach",
                "volume %s" % volume_id,
                self.backend.detach_from_proxy,
                volume_id,
                self.poll_timeout,
            )
            failed.pop("attachment %s" % volume_id, None)
            return True
        except Exception as exc:  # pylint: disable=broad-except
            failed["attachment %s" % volume_id] = str(exc)
            return False

    def _delete_volume(self, volume_id, failed):
        try:
            self._timed(
                "delete of volume",
                "volume %s" % volume_id,
                self.backend.delete_volume,
                volume_id,
                self.poll_timeout,
            )
            failed.pop("volume %s" % volume_id, None)
            return True
        except Exception as exc:  # pylint: disable=broad-except
            failed["volume %s" % volume_id] = str(exc)
            return False

    def _delete_snapshot(self, snapshot_id, failed):
        try:
            self._timed(
                "delete of snapshot",
                "snapshot %s" % snapshot_id,
                self.backend.delete_snapshot,
                snapshot_id,
                self.poll_timeout,
            )
            failed.pop("snapshot %s" % snapshot_id, None)
            return True
        except Exception as exc:  # pylint: disable=broad-except
            failed["snapshot %s" % snapshot_id] = str(exc)
            return False

    def _delete_group_snapshot(self, group_snapshot_id, failed):
        try:
            self._timed(
                "delete of group snapshot",
                "group snapshot %s" % group_snapshot_id,
                self.backend.delete_group_snapshot,
                group_snapshot_id,
                self.poll_timeout,
            )
            failed.pop("group snapshot %s" % group_snapshot_id, None)
            return True
        except Exception as exc:  # pylint: disable=broad-except
            failed["group snapshot %s" % group_snapshot_id] = str(exc)
            return False

    def _cleanup(self, failed):
        self.close()
        with self._lock:
            intents = sorted(self.attach_intents)
            temps = dict(self.temp_volumes)
            snaps = dict(self.snapshots)
        for volume_id in intents:
            if self._detach(volume_id, failed):
                with self._lock:
                    self.attach_intents.discard(volume_id)
        for source, volume_id in temps.items():
            if self._delete_volume(volume_id, failed):
                with self._lock:
                    if self.temp_volumes.get(source) == volume_id:
                        del self.temp_volumes[source]
        for source, snapshot_id in snaps.items():
            if self._delete_snapshot(snapshot_id, failed):
                with self._lock:
                    if self.snapshots.get(source) == snapshot_id:
                        del self.snapshots[source]
        wanted = {TAG_CREATED_BY: TAG_CREATED_BY_VALUE, TAG_JOB_ID: str(self.job_id)}
        try:
            volumes = self.backend.list_volumes(
                dict(wanted, **{TAG_ROLE: ROLE_BACKUP_TEMP_VOLUME})
            )
            attached = set(self.backend.list_proxy_attachments())
        except Exception as exc:  # pylint: disable=broad-except
            self._log(
                "warning", "cannot list volumes of job %s: %s" % (self.job_id, exc)
            )
            volumes, attached = [], set()
        for vol in volumes:
            if vol.id in attached and not self._detach(vol.id, failed):
                continue
            self._delete_volume(vol.id, failed)
        try:
            snapshots = self.backend.list_snapshots(
                dict(wanted, **{TAG_ROLE: ROLE_BACKUP_SNAPSHOT})
            )
        except Exception as exc:  # pylint: disable=broad-except
            self._log(
                "warning", "cannot list snapshots of job %s: %s" % (self.job_id, exc)
            )
            snapshots = []
        for snap in snapshots:
            self._delete_snapshot(snap.id, failed)
        if self.snapshot_mode == "group":
            self._cleanup_group_snapshots(failed)

    def _cleanup_group_snapshots(self, failed):
        """Remove the tracked group snapshot and those found by description."""
        with self._lock:
            tracked = self.group_snapshot_id
        if tracked and self._delete_group_snapshot(tracked, failed):
            with self._lock:
                self.group_snapshot_id = ""
                self.group_members = {}
        try:
            found = self.backend.list_group_snapshots("bareos-%s-" % self.job_id)
        except Exception as exc:  # pylint: disable=broad-except
            self._log(
                "warning",
                "cannot list group snapshots of job %s: %s" % (self.job_id, exc),
            )
            return
        for gsnap in found:
            tags = gsnap.tags
            if tags.get(TAG_JOB_ID) == str(self.job_id) and gsnap.id != tracked:
                self._delete_group_snapshot(gsnap.id, failed)
