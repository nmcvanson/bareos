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

"""Cloud side of a restore job: volumes, ports, server and the replace rules."""

from dataclasses import dataclass, field

from .cloud import (
    ROLE_RESTORE_VOLUME,
    CloudCancelled,
    CloudConflict,
    CloudError,
    CloudNotFound,
    CloudTransferAccepted,
    bareos_tags,
    transfer_name,
)


@dataclass
class RestoreCloudOptions:
    name: str = ""
    flavor: str = ""
    network: str = ""
    availability_zone: str = ""
    volume_type: str = ""
    powerstate: str = "previous"
    force: bool = False
    force_keep_old_volumes: bool = False
    keep_addresses: bool = False
    target: str = "cloud"
    security_groups: tuple = ()


@dataclass
class RestoreResult:
    server_id: str
    volume_ids: dict = field(default_factory=dict)
    port_ids: list = field(default_factory=list)
    deleted_old_volume_ids: list = field(default_factory=list)
    kept_old_volume_ids: list = field(default_factory=list)


class RestoreError(CloudError):
    """The restore cannot continue; the message names what the operator can do."""


def _no_log(_level, _message):
    return None


class RestoreCloudSession:
    """Re-creates a server from a validated document."""

    def __init__(
        self,
        backend,
        document,
        job_id,
        options,
        poll_timeout=900,
        log=None,
        target_backend=None,
    ):
        self.backend = backend
        # server, ports, flavor and networks live in the target project
        self.tbackend = target_backend or backend
        self.cross = target_backend is not None
        self.doc = document
        self.job_id = job_id
        self.options = options
        self.poll_timeout = poll_timeout
        self._logger = log or _no_log
        self.restored = {}
        self.attach_intents = set()
        self.created_ports = []
        self.reused_ports = []
        self.old_server = None
        self.old_volume_ids = []
        self.old_port_ids = []
        self.old_server_deleted = False
        self.flavor_id = ""
        self.new_server_id = ""
        self.new_server_active = False
        self.new_server_gone = False
        self.old_delete_started = False
        self.removed = set()
        self.pending_transfers = {}
        self.moved = []
        self.unknown_owner = []
        self.port_groups = {}
        self.override_groups = None

    def _log(self, level, message):
        try:
            self._logger(level, message)
        except Exception:  # pylint: disable=broad-except
            pass

    @property
    def _saved_server(self):
        return self.doc["server"]

    def _restorable(self):
        """True when the document has a root volume a server can boot from."""
        doc = self.doc
        return bool(doc.get("root_restorable", doc.get("boot_from_volume", False)))

    def _recovery_line(self):
        def ids(values):
            values = list(values)
            return ", ".join(values) if values else "-"

        if self.old_server is None:
            old_state, old_id = "none", "-"
        else:
            old_id = self.old_server.id
            old_state = "deleted" if self.old_server_deleted else "present"
        if not self.new_server_id:
            new_state, new_id = "none", "-"
        else:
            new_id = self.new_server_id
            new_state = "deleted" if self.new_server_gone else "kept"
        new_ports = list(self.created_ports) + list(self.reused_ports)
        line = (
            "recovery: new server %s (%s), new ports %s still existing (kept), "
            "old server %s (%s), old volumes %s (kept), old ports %s (kept), "
            "restored volumes %s (kept)"
            % (
                new_id,
                new_state,
                ids(new_ports),
                old_id,
                old_state,
                ids(self.old_volume_ids),
                ids(self.old_port_ids),
                ids(self._kept_volumes()),
            )
        )
        if self.cross:
            pending = list(self.pending_transfers.values())
            unmoved = [
                v
                for v in self._kept_volumes()
                if v not in self.moved and v not in self.unknown_owner
            ]
            line += (
                ", volumes moved to the target project %s, not moved %s, "
                "pending transfers %s" % (ids(self.moved), ids(unmoved), ids(pending))
            )
            line += ", ownership unknown %s" % ids(self.unknown_owner)
        return line

    def _kept_volumes(self):
        return [v for v in self.restored.values() if v not in self.removed]

    def _log_recovery(self):
        self._log("error", self._recovery_line())

    # Checks -----------------------------------------------------------------

    def precheck(self):
        """Check flavor, networks and the force preconditions before any change."""
        opts = self.options
        be = self.tbackend
        doc = self.doc
        if self.cross:
            if opts.force or opts.keep_addresses:
                raise RestoreError(
                    "force and restore_keep_addresses are refused across projects"
                )
            src, dst = self.backend.project_id, be.project_id
            if src and dst and src == dst:
                raise RestoreError(
                    "the target project is the project of the backup proxy; use a "
                    "normal restore"
                )
        if opts.force_keep_old_volumes and not opts.force:
            self._log("warning", "force_keep_old_volumes has no effect without force")
        if not self._restorable():
            self._log(
                "warning",
                "the backup has no restorable root disk; its volumes are restored "
                "without a server",
            )
            if opts.force:
                raise RestoreError(
                    "force=yes needs a backup with a restorable root disk "
                    "(a server that boots from a volume)"
                )
            if self.cross:
                self._precheck_target_volumes()
            return
        wanted = opts.flavor or self._saved_server["flavor"].get("original_name", "")
        try:
            self.flavor_id = be.find_flavor(wanted).id
        except CloudNotFound as exc:
            raise RestoreError(
                "flavor %s not found; set restore_flavor" % wanted
            ) from exc
        if opts.network:
            if not be.network_exists(opts.network):
                raise RestoreError("network %s not found" % opts.network)
        else:
            for port in doc["ports"]:
                if not be.network_exists(port["network_id"]):
                    raise RestoreError(
                        "network %s of port %s no longer exists; set restore_network"
                        % (port["network_id"], port["id"])
                    )
        if self.cross:
            self._precheck_target_volumes()
            self._map_security_groups()
        if opts.force:
            sid = self._saved_server["id"]
            try:
                old = be.get_server(sid)
            except CloudNotFound:
                self._log(
                    "warning",
                    "server %s no longer exists; nothing to replace, a new server "
                    "is created" % sid,
                )
                return
            if old.status != "SHUTOFF":
                raise RestoreError(
                    "server %s is %s; force=yes requires it to be SHUTOFF"
                    % (sid, old.status)
                )
            if old.project_id != self._saved_server["project_id"]:
                raise RestoreError(
                    "server %s belongs to project %s, not to the saved project %s"
                    % (sid, old.project_id, self._saved_server["project_id"])
                )
            self.old_server = old

    def _precheck_target_volumes(self):
        """The volume types and zones of the volumes must exist in the target."""
        opts = self.options
        tbe = self.tbackend
        types, zones = set(), set()
        for vol in self.doc["volumes"]:
            types.add(opts.volume_type or vol.get("volume_type", ""))
            zones.add(opts.availability_zone or vol.get("availability_zone", ""))
        for name in sorted(t for t in types if t):
            if not tbe.volume_type_usable(name):
                raise RestoreError(
                    "volume type %s cannot be used in the target project; set "
                    "restore_volume_type" % name
                )
        for name in sorted(z for z in zones if z):
            if not tbe.availability_zone_exists(name):
                raise RestoreError(
                    "availability zone %s does not exist in the target project; "
                    "set restore_availability_zone" % name
                )

    def _target_group(self, name):
        try:
            return self.tbackend.find_security_group(name)
        except (CloudNotFound, CloudConflict) as exc:
            raise RestoreError(
                "security group %s in the target project: %s" % (name, exc)
            ) from exc

    def _map_security_groups(self):
        """Map the saved security groups to the target project by name."""
        opts = self.options
        if opts.security_groups:
            self.override_groups = [
                self._target_group(name) for name in opts.security_groups
            ]
            return
        names = {}
        for port in self.doc["ports"]:
            mapped = []
            for group_id in port.get("security_group_ids", []):
                if group_id not in names:
                    try:
                        name = self.backend.get_security_group_name(group_id)
                    except CloudNotFound as exc:
                        raise RestoreError(
                            "security group %s of port %s no longer exists in the "
                            "source project; set restore_security_groups"
                            % (group_id, port["id"])
                        ) from exc
                    names[group_id] = self._target_group(name)
                mapped.append(names[group_id])
            self.port_groups[port["id"]] = mapped

    def _groups_for(self, port):
        if not self.cross:
            return port.get("security_group_ids", [])
        if self.override_groups is not None:
            return list(self.override_groups)
        return list(self.port_groups.get(port["id"], []))

    # Volumes ----------------------------------------------------------------

    def prepare_volumes(self):
        """Create and attach the new volumes; return saved id -> device path."""
        opts = self.options
        paths = {}
        for vol in self.doc["volumes"]:
            saved_id = vol["id"]

            def record(new_id, saved_id=saved_id):
                self.restored[saved_id] = new_id

            new = self.backend.create_volume(
                "%s-restore-%s" % (vol.get("name") or saved_id, self.job_id),
                vol["size_gb"],
                bareos_tags(
                    self.job_id,
                    self._saved_server["id"],
                    ROLE_RESTORE_VOLUME,
                    saved_id,
                ),
                self.poll_timeout,
                volume_type=opts.volume_type or vol.get("volume_type", ""),
                availability_zone=opts.availability_zone
                or vol.get("availability_zone", ""),
                on_created=record,
            )
            self.restored[saved_id] = new.id
            self.attach_intents.add(new.id)
            paths[saved_id] = self.backend.attach_to_proxy(new.id, self.poll_timeout)
            self._log("info", "restore volume %s ready for %s" % (new.id, saved_id))
        return paths

    # Finish -----------------------------------------------------------------

    def _detach_all(self):
        failures = []
        for new_id in sorted(self.attach_intents):
            try:
                self.backend.detach_from_proxy(new_id, self.poll_timeout)
                self.attach_intents.discard(new_id)
            except Exception as exc:  # pylint: disable=broad-except
                failures.append("%s (%s)" % (new_id, exc))
        return failures

    def finish(self, complete):
        """Detach the volumes and, when complete, create the server."""
        try:
            return self._finish(complete)
        except RestoreError:
            raise
        except CloudCancelled:
            # abort() runs the rollback once the caller released the cancel event
            self._log_recovery()
            raise
        except CloudError as exc:
            self._discard_new_resources()
            self._rollback_extras()
            self._log_recovery()
            raise RestoreError("restore failed: %s" % exc) from exc

    def _rollback_extras(self):
        """Delete unaccepted volume transfers."""
        self._drop_transfers()

    def _drop_transfers(self):
        for new_id, transfer_id in list(self.pending_transfers.items()):
            try:
                self.backend.delete_transfer(transfer_id)
                del self.pending_transfers[new_id]
            except Exception as exc:  # pylint: disable=broad-except
                self._log(
                    "warning", "could not delete transfer %s: %s" % (transfer_id, exc)
                )

    def _new_server_committed(self):
        """The replacement is the only working copy once the old one is going."""
        return self.new_server_active and (
            self.old_delete_started or self.old_server_deleted
        )

    def _discard_new_resources(self):
        """Delete an uncommitted new server, then the ports created for it."""
        be = self.tbackend
        if self.new_server_id and not self.new_server_gone:
            if self._new_server_committed():
                self._log(
                    "warning",
                    "new server %s is kept with its ports: the old server is "
                    "already being replaced" % self.new_server_id,
                )
            else:
                try:
                    be.delete_server(self.new_server_id, self.poll_timeout)
                    self.new_server_gone = True
                except Exception as exc:  # pylint: disable=broad-except
                    self._log(
                        "warning",
                        "could not delete new server %s, it is kept: %s"
                        % (self.new_server_id, exc),
                    )
        if not self.new_server_id or self.new_server_gone:
            self._drop_created_ports()

    def _drop_created_ports(self):
        for port_id in list(self.created_ports):
            try:
                self.tbackend.delete_port(port_id)
                self.created_ports.remove(port_id)
            except Exception as exc:  # pylint: disable=broad-except
                self._log("warning", "could not delete port %s: %s" % (port_id, exc))

    def _transfer_all(self):
        """Move every restored volume to the target project."""
        be, tbe = self.backend, self.tbackend
        for vol in self.doc["volumes"]:
            saved_id = vol["id"]
            new_id = self.restored[saved_id]
            if new_id in self.moved:
                continue

            def record(transfer_id, new_id=new_id):
                self.pending_transfers[new_id] = transfer_id

            key = ""
            try:
                transfer_id, key = be.create_transfer(
                    new_id,
                    transfer_name(self.job_id, saved_id),
                    on_created=record,
                )
                self.pending_transfers[new_id] = transfer_id
                tbe.accept_transfer(transfer_id, key, self.poll_timeout)
            except CloudCancelled:
                self._reconcile_owners()
                raise
            except CloudError as exc:
                if isinstance(exc, CloudTransferAccepted):
                    self._note_moved(new_id)
                self._reconcile_owners()
                self._drop_transfers()
                self._log_recovery()
                raise RestoreError(
                    "transferring volume %s to the target project failed (%s): %s"
                    % (new_id, self._owner_text(new_id), exc)
                ) from exc
            finally:
                key = ""
            self.pending_transfers.pop(new_id, None)
            self.moved.append(new_id)

    def _owner_text(self, new_id):
        if new_id in self.moved:
            return "the volume is in the target project"
        if new_id in self.unknown_owner:
            return "the owner of the volume could not be established"
        return "the volume is still in the source project"

    def _note_moved(self, new_id):
        self.pending_transfers.pop(new_id, None)
        if new_id not in self.moved:
            self.moved.append(new_id)

    @staticmethod
    def _seen(backend, new_id):
        """True or False when the connection answers, None when the probe fails."""
        try:
            backend.get_volume(new_id)
            return True
        except CloudNotFound:
            return False
        except Exception:  # pylint: disable=broad-except
            return None

    def _reconcile_owners(self):
        """Probe both projects for every restored volume not known as moved."""
        for new_id in self.restored.values():
            if new_id in self.moved or new_id in self.removed:
                continue
            in_target = self._seen(self.tbackend, new_id)
            in_source = self._seen(self.backend, new_id)
            if in_target is True and in_source is False:
                self._note_moved(new_id)
                if new_id in self.unknown_owner:
                    self.unknown_owner.remove(new_id)
            elif in_source is True and in_target is False:
                if new_id in self.unknown_owner:
                    self.unknown_owner.remove(new_id)
            elif new_id not in self.unknown_owner:
                # failed probe, visible in both or in neither project
                self.unknown_owner.append(new_id)

    def _finish(self, complete):
        be = self.backend
        tbe = self.tbackend
        opts = self.options
        doc = self.doc
        failures = self._detach_all()
        if failures:
            self._log_recovery()
            raise RestoreError(
                "could not detach restored volumes: %s" % ", ".join(failures)
            )
        if not complete:
            self._log("error", "restore incomplete; no server is created")
            self._log_recovery()
            return RestoreResult("", dict(self.restored))

        for vol in doc["volumes"]:
            if vol.get("boot_index") == 0 or vol.get("bootable"):
                be.set_volume_bootable(self.restored[vol["id"]], True)

        self._set_image_metadata()
        if self.cross:
            self._transfer_all()
        if not self._restorable():
            self._log(
                "info",
                "restored volumes without a server: %s"
                % ", ".join(self.restored.values()),
            )
            return RestoreResult("", dict(self.restored))

        old = None
        if opts.force and self.old_server is not None:
            try:
                old = tbe.get_server(self.old_server.id)
            except CloudNotFound:
                self._log("warning", "server %s is gone" % self.old_server.id)
                self.old_server = None
            if old is not None:
                if old.status != "SHUTOFF":
                    self._log_recovery()
                    raise RestoreError(
                        "server %s is %s; force=yes requires it to be SHUTOFF"
                        % (old.id, old.status)
                    )
                self.old_server = old
                self.old_volume_ids = [v.id for v in be.list_server_volumes(old.id)]
                self.old_port_ids = [p.id for p in be.list_server_ports(old.id)]
                if opts.keep_addresses:
                    self._delete_old_server()
                    self._log("info", "old server %s deleted first" % old.id)
        # Ports ---------------------------------------------------------------
        port_ids = self._create_ports()
        # Server --------------------------------------------------------------
        saved = self._saved_server
        block_devices = [
            {
                "volume_id": self.restored[v["id"]],
                "boot_index": 0 if v.get("boot_index") == 0 else None,
            }
            for v in doc["volumes"]
        ]
        metadata = dict(saved.get("metadata") or {})
        metadata["bareos-restored-from"] = saved["id"]
        metadata["bareos-restore-job-id"] = str(self.job_id)

        def record(server_id):
            self.new_server_id = server_id

        try:
            new = tbe.create_server(
                opts.name or "%s-restore-%s" % (saved["name"], self.job_id),
                self.flavor_id or self._resolve_flavor(),
                block_devices,
                port_ids,
                saved.get("key_name", ""),
                opts.availability_zone or saved.get("availability_zone", ""),
                metadata,
                self.poll_timeout,
                on_created=record,
            )
        except CloudCancelled:
            raise
        except CloudError as exc:
            self._discard_new_resources()
            self._rollback_extras()
            self._log_recovery()
            raise RestoreError("creating the server failed: %s" % exc) from exc
        self.new_server_id = new.id
        self.new_server_active = True

        state = opts.powerstate
        if state == "off" or (state == "previous" and saved.get("status") == "SHUTOFF"):
            tbe.stop_server(new.id, self.poll_timeout)

        if opts.force and self.old_server is not None:
            if not self.old_server_deleted:
                try:
                    self._delete_old_server()
                except CloudError as exc:
                    self._log(
                        "error",
                        "new server %s is running but old server %s could not be "
                        "deleted: %s" % (new.id, self.old_server.id, exc),
                    )
                    self._log_recovery()
                    raise RestoreError(
                        "old server %s could not be deleted: %s"
                        % (self.old_server.id, exc)
                    ) from exc
        deleted, kept = self._delete_old_resources()
        self._log(
            "info",
            "server %s created with volumes %s and ports %s"
            % (
                new.id,
                ", ".join(self.restored.values()),
                ", ".join(port_ids) or "-",
            ),
        )
        return RestoreResult(new.id, dict(self.restored), list(port_ids), deleted, kept)

    def _set_image_metadata(self):
        """Give each restored volume the boot properties saved with it."""
        for vol in self.doc["volumes"]:
            metadata = vol.get("image_metadata") or {}
            if not metadata:
                continue
            new_id = self.restored[vol["id"]]
            try:
                self.backend.set_volume_image_metadata(new_id, dict(metadata))
            except CloudCancelled:
                raise
            except CloudError as exc:
                self._log_recovery()
                raise RestoreError(
                    "setting the image properties of volume %s failed: %s"
                    % (new_id, exc)
                ) from exc

    def _resolve_flavor(self):
        wanted = self.options.flavor or self._saved_server["flavor"].get(
            "original_name", ""
        )
        try:
            self.flavor_id = self.tbackend.find_flavor(wanted).id
        except CloudNotFound as exc:
            raise RestoreError(
                "flavor %s not found; set restore_flavor" % wanted
            ) from exc
        return self.flavor_id

    def _delete_old_server(self):
        be = self.backend
        old = self.old_server
        self.old_delete_started = True
        for vol_id in self.old_volume_ids:
            be.set_delete_on_termination(old.id, vol_id, False)
        be.delete_server(old.id, self.poll_timeout)
        self.old_server_deleted = True

    def _port_tags(self):
        return [
            "bareos-restore-job-%s" % self.job_id,
            "bareos-restored-from-%s" % self._saved_server["id"],
        ]

    def _create_ports(self):
        be = self.tbackend
        opts = self.options
        port_ids = []
        try:
            for port in self.doc["ports"]:
                name = "%s-restore-%s" % (port.get("name") or "port", self.job_id)
                groups = self._groups_for(port)
                if opts.network:
                    new = be.create_port(
                        opts.network, name, groups, tags=self._port_tags()
                    )
                elif opts.keep_addresses:
                    new = self._port_with_addresses(port, name, groups)
                else:
                    new = be.create_port(
                        port["network_id"], name, groups, tags=self._port_tags()
                    )
                if new.id not in self.reused_ports:
                    self.created_ports.append(new.id)
                port_ids.append(new.id)
        except CloudCancelled:
            self._drop_created_ports()
            raise
        except CloudError as exc:
            self._drop_created_ports()
            self._log_recovery()
            raise RestoreError("creating ports failed: %s" % exc) from exc
        return port_ids

    def _port_with_addresses(self, port, name, groups):
        be = self.tbackend
        existing = be.get_port(port["id"])
        if existing is not None and not existing.device_id:
            self.reused_ports.append(existing.id)
            return existing
        if existing is not None:
            self._log(
                "warning",
                "port %s is still bound to %s; the restored server gets a new port "
                "without the saved addresses" % (port["id"], existing.device_id),
            )
            return be.create_port(
                port["network_id"], name, groups, tags=self._port_tags()
            )
        return be.create_port(
            port["network_id"],
            name,
            groups,
            fixed_ips=[dict(ip) for ip in port.get("fixed_ips", [])],
            mac_address=port.get("mac_address", ""),
            tags=self._port_tags(),
        )

    def _delete_old_resources(self):
        be = self.backend
        deleted, kept = [], []
        if not (self.options.force and self.old_server is not None):
            return deleted, kept
        for vol_id in self.old_volume_ids:
            if self.options.force_keep_old_volumes:
                kept.append(vol_id)
                continue
            try:
                be.delete_volume(vol_id, self.poll_timeout)
                deleted.append(vol_id)
            except CloudError as exc:
                kept.append(vol_id)
                kind = "conflict" if isinstance(exc, CloudConflict) else "error"
                self._log(
                    "warning", "old volume %s kept (%s): %s" % (vol_id, kind, exc)
                )
        for port_id in self.old_port_ids:
            if port_id in self.reused_ports:
                continue
            try:
                port = be.get_port(port_id)
                if port is not None and not port.device_id:
                    be.delete_port(port_id)
            except CloudError as exc:
                self._log("warning", "old port %s kept: %s" % (port_id, exc))
        self._log(
            "info",
            "old volumes deleted: %s; kept: %s"
            % (", ".join(deleted) or "-", ", ".join(kept) or "-"),
        )
        return deleted, kept

    # Abort ------------------------------------------------------------------

    def abort(self):
        """Detach the restored volumes, drop created ports, keep the volumes."""
        try:
            for failure in self._detach_all():
                self._log("warning", "could not detach %s" % failure)
            self._discard_new_resources()
            self._rollback_extras()
            self._log(
                "warning",
                "restore aborted; restored volumes are kept. " + self._recovery_line(),
            )
        except Exception as exc:  # pylint: disable=broad-except
            self._log("warning", "abort failed: %s" % exc)
        return self._kept_volumes()
