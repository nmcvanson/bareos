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

"""File-backed fake OpenStack cloud used by the functional tests, with its CLI."""

import argparse
import contextlib
import copy
import fcntl
import functools
import json
import os
import random
import shutil
import sys
import time
import uuid

from .cloud import (
    BOOT_PROPERTY_KEYS,
    GIB,
    ROLE_BACKUP_GROUP_SNAPSHOT,
    ROLE_BACKUP_SNAPSHOT,
    ROLE_BACKUP_TEMP_VOLUME,
    TAG_ROLE,
    CloudBackend,
    CloudConfigError,
    CloudConflict,
    CloudError,
    CloudNotFound,
    CloudTimeout,
    FlavorInfo,
    GroupSnapshotInfo,
    PortInfo,
    ServerInfo,
    SnapshotInfo,
    TransferInfo,
    VolumeInfo,
    parse_group_snapshot_description,
)

STATE_FORMAT = 1
BLOCK = 1024 * 1024
AFTER_OPS = (
    "create_snapshot",
    "create_volume",
    "create_server",
    "attach_to_proxy",
    "create_group_snapshot",
)
MIN_GROUP_MICROVERSION = (3, 14)


def _fault_wrapper(method):
    """Run the injected fault of the method name before and, for some, after it."""
    name = method.__name__

    @functools.wraps(method)
    def wrapper(self, *args, **kwargs):
        self._fault(name, "before")
        result = method(self, *args, **kwargs)
        self._fault(name, "after")
        return result

    return wrapper


def _new_id():
    return str(uuid.uuid4())


def _matches(metadata, wanted):
    return all(str(metadata.get(k)) == str(v) for k, v in wanted.items())


def _fill_random(path, offset, length, seed):
    """Overwrite a range with the byte stream of random.Random(seed)."""
    rng = random.Random(seed)
    with open(path, "r+b") as fh:
        fh.seek(offset)
        left = length
        while left > 0:
            size = min(BLOCK, left)
            fh.write(rng.randbytes(size))
            left -= size


def _fill_zero(path, offset, length):
    with open(path, "r+b") as fh:
        fh.seek(offset)
        left = length
        while left > 0:
            size = min(BLOCK, left)
            fh.write(b"\0" * size)
            left -= size


def _empty_state(project_id, proxy_id):
    return {
        "format": STATE_FORMAT,
        "project_id": project_id,
        "proxy_server_id": proxy_id,
        "flavors": {},
        "networks": {},
        "security_groups": {},
        "servers": {},
        "volumes": {},
        "snapshots": {},
        "ports": {},
        "faults": {},
        "log": [],
    }


class FakeBackend(CloudBackend):
    """Cloud backend whose state is a directory (see seed() and the CLI)."""

    def __init__(self, config, all_projects=False):
        super().__init__()
        root = config.get("fake_root") or ""
        if not root:
            raise CloudConfigError("fake_root is required for backend=fake")
        self._config = dict(config)
        # "" is the project of the cloud; every project is for the command line
        self._project = str(config.get("fake_project_id") or "")
        if self._project == "*":
            raise CloudConfigError("fake_project_id must name a project")
        self._all_projects = bool(all_projects)
        self._no_proxy = config.get("proxy") is False
        self._root = os.path.abspath(root)
        self._state_path = os.path.join(self._root, "state.json")
        self._lock_path = os.path.join(self._root, "state.lock")

    # State access ----------------------------------------------------------

    @contextlib.contextmanager
    def _locked(self, write=True):
        """Hold the exclusive lock, yield the loaded state, save it on success."""
        if not os.path.isfile(self._state_path):
            raise CloudConfigError("fake cloud state %s not found" % self._state_path)
        with open(self._lock_path, "a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                with open(self._state_path, "r", encoding="utf-8") as fh:
                    state = json.load(fh)
                yield state
                if write:
                    tmp = self._state_path + ".tmp"
                    with open(tmp, "w", encoding="utf-8") as fh:
                        json.dump(state, fh, indent=1, sort_keys=True)
                    os.replace(tmp, self._state_path)
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)

    def _vol_file(self, volume_id):
        return os.path.join(self._root, "volumes", volume_id + ".img")

    def _snap_file(self, snapshot_id):
        return os.path.join(self._root, "snapshots", snapshot_id + ".img")

    @staticmethod
    def _log(state, op, ident):
        state["log"].append({"op": op, "id": ident, "time": time.time()})

    # Projects --------------------------------------------------------------

    def _proj(self, state):
        if not self._project:
            return state["project_id"]
        return self._project

    def _visible(self, state, rec):
        if self._all_projects:
            return True
        # a record without an owner belongs to the project of the cloud
        return rec.get("project_id", state["project_id"]) == self._proj(state)

    def _rec(self, state, table, key):
        """The record of this project's resource, None when absent or foreign."""
        rec = (state.get(table) or {}).get(key)
        if rec is None or not self._visible(state, rec):
            return None
        return rec

    def _all(self, state, table):
        return [r for r in (state.get(table) or {}).values() if self._visible(state, r)]

    def _stamp(self, state, rec):
        proj = self._proj(state)
        if proj != state["project_id"]:
            rec["project_id"] = proj
        return rec

    def _catalog(self, state):
        """Flavors, networks and groups known to this project."""
        proj = self._proj(state)
        if proj == state["project_id"]:
            return state
        found = state.get("projects", {}).get(proj)
        if found is None:
            raise CloudConfigError("project %s is not part of the fake cloud" % proj)
        return found

    def _require_proxy(self):
        if self._no_proxy:
            raise CloudConfigError("this connection has no proxy server")

    def _lost_response(self, name):
        """True when a pending after-fault of `name` is marked lost (no id seen)."""
        with self._locked(write=False) as state:
            spec = state["faults"].get(name)
        return bool(
            spec
            and spec.get("lost")
            and spec.get("phase") == "after"
            and not spec.get("delay_only")
            and int(spec.get("remaining", 1)) > 0
        )

    # Faults ----------------------------------------------------------------

    def _fault(self, name, phase):
        """Apply the fault of `name` when it is scheduled for `phase`."""
        effective_after = name in AFTER_OPS
        if phase == "after" and not effective_after:
            return
        with self._locked(write=False) as state:
            spec = copy.deepcopy(state["faults"].get(name))
        if spec is None or int(spec.get("remaining", 1)) <= 0:
            return
        spec_phase = spec.get("phase", "before")
        if not effective_after:
            spec_phase = "before"
        if spec_phase != phase:
            return
        delay = float(spec.get("sleep", 0))
        if delay > 0:
            self.sleep(delay)
        fire = False
        with self._locked() as state:
            current = state["faults"].get(name)
            if current is not None and int(current.get("remaining", 1)) > 0:
                current["remaining"] = int(current.get("remaining", 1)) - 1
                fire = True
        if fire and not spec.get("delay_only"):
            message = spec.get("message") or "injected fault in %s" % name
            if phase == "after":
                raise CloudTimeout(message)
            raise CloudError(message)

    # Connection ------------------------------------------------------------

    def connect(self):
        with self._locked(write=False) as state:
            self._catalog(state)
            wanted = self._config.get("proxy_instance_id") or ""
            if wanted == "auto" or self._no_proxy:
                wanted = ""
            if wanted and wanted != state["proxy_server_id"]:
                raise CloudConfigError(
                    "proxy_instance_id %s is not the proxy of the fake cloud" % wanted
                )

    def close(self):
        return None

    @property
    def project_id(self):
        with self._locked(write=False) as state:
            return self._proj(state)

    @property
    def proxy_instance_id(self):
        if self._no_proxy:
            return ""
        with self._locked(write=False) as state:
            return state["proxy_server_id"]

    # Record conversion -----------------------------------------------------

    @staticmethod
    def _server_info(state, rec):
        flavor = state["flavors"].get(rec["flavor_id"])
        if flavor is None:
            for other in state.get("projects", {}).values():
                flavor = other.get("flavors", {}).get(rec["flavor_id"])
                if flavor is not None:
                    break
        if flavor is None:
            flavor = {
                "name": rec["flavor_id"],
                "vcpus": 0,
                "ram": 0,
                "disk": 0,
                "extra_specs": {},
            }
        flavor_info = {
            "original_name": flavor["name"],
            "vcpus": flavor["vcpus"],
            "ram": flavor["ram"],
            "disk": flavor["disk"],
            "extra_specs": copy.deepcopy(flavor.get("extra_specs", {})),
        }
        for key in ("ephemeral", "swap"):
            if key in flavor:
                flavor_info[key] = flavor[key]
        return ServerInfo(
            id=rec["id"],
            name=rec["name"],
            project_id=rec["project_id"],
            status=rec["status"],
            flavor=flavor_info,
            image_id=rec["image_id"],
            key_name=rec["key_name"],
            availability_zone=rec["availability_zone"],
            metadata=copy.deepcopy(rec["metadata"]),
            tags=list(rec["tags"]),
            security_groups=list(rec["security_groups"]),
        )

    @staticmethod
    def _volume_info(rec, attachment=None):
        info = VolumeInfo(
            id=rec["id"],
            name=rec["name"],
            size_gb=rec["size_gb"],
            volume_type=rec["volume_type"],
            bootable=rec["bootable"],
            encrypted=rec["encrypted"],
            multiattach=rec["multiattach"],
            status=rec["status"],
            availability_zone=rec["availability_zone"],
            metadata=copy.deepcopy(rec["metadata"]),
            attached_server_ids=list(rec["attached_server_ids"]),
            image_metadata={
                k: v
                for k, v in rec.get("image_metadata", {}).items()
                if k in BOOT_PROPERTY_KEYS
            },
        )
        if attachment is not None:
            info.device = attachment["device"]
            info.boot_index = attachment["boot_index"]
            info.delete_on_termination = bool(attachment["delete_on_termination"])
        return info

    @staticmethod
    def _port_info(rec):
        return PortInfo(
            id=rec["id"],
            network_id=rec["network_id"],
            mac_address=rec["mac_address"],
            fixed_ips=copy.deepcopy(rec["fixed_ips"]),
            security_group_ids=list(rec["security_group_ids"]),
            name=rec["name"],
            device_id=rec["device_id"],
        )

    @staticmethod
    def _snapshot_info(rec):
        return SnapshotInfo(
            id=rec["id"],
            volume_id=rec["volume_id"],
            name=rec["name"],
            status=rec["status"],
            size_gb=rec["size_gb"],
            metadata=copy.deepcopy(rec["metadata"]),
        )

    # Queries ---------------------------------------------------------------

    @_fault_wrapper
    def get_server(self, server_id):
        with self._locked(write=False) as state:
            rec = self._rec(state, "servers", server_id)
            if rec is None:
                raise CloudNotFound("server %s not found" % server_id)
            return self._server_info(state, rec)

    @_fault_wrapper
    def find_server(self, name):
        with self._locked(write=False) as state:
            found = [
                r
                for r in self._all(state, "servers")
                if r["name"] == name and r["id"] != state["proxy_server_id"]
            ]
            if not found:
                raise CloudNotFound("no server named %s" % name)
            if len(found) > 1:
                raise CloudConflict(
                    "%d servers are named %s; use server_id" % (len(found), name)
                )
            return self._server_info(state, found[0])

    @_fault_wrapper
    def list_servers(self):
        with self._locked(write=False) as state:
            return [
                self._server_info(state, r)
                for r in self._all(state, "servers")
                if r["id"] != state["proxy_server_id"]
            ]

    @_fault_wrapper
    def list_server_volumes(self, server_id):
        with self._locked(write=False) as state:
            rec = self._rec(state, "servers", server_id)
            if rec is None:
                raise CloudNotFound("server %s not found" % server_id)
            result = []
            for att in rec["attachments"]:
                vol = self._rec(state, "volumes", att["volume_id"])
                if vol is not None:
                    result.append(self._volume_info(vol, att))
            return result

    @_fault_wrapper
    def list_server_ports(self, server_id):
        with self._locked(write=False) as state:
            return [
                self._port_info(p)
                for p in self._all(state, "ports")
                if p["device_id"] == server_id
            ]

    @_fault_wrapper
    def get_volume(self, volume_id):
        with self._locked(write=False) as state:
            rec = self._rec(state, "volumes", volume_id)
            if rec is None:
                raise CloudNotFound("volume %s not found" % volume_id)
            return self._volume_info(rec)

    @_fault_wrapper
    def list_volumes(self, metadata_filter):
        with self._locked(write=False) as state:
            return [
                self._volume_info(v)
                for v in self._all(state, "volumes")
                if _matches(v["metadata"], metadata_filter)
            ]

    @_fault_wrapper
    def list_snapshots(self, metadata_filter):
        with self._locked(write=False) as state:
            return [
                self._snapshot_info(s)
                for s in self._all(state, "snapshots")
                if _matches(s["metadata"], metadata_filter)
            ]

    @_fault_wrapper
    def list_proxy_attachments(self):
        if self._no_proxy:
            return []
        with self._locked(write=False) as state:
            proxy = state["proxy_server_id"]
            return [
                v["id"]
                for v in self._all(state, "volumes")
                if proxy in v["attached_server_ids"]
            ]

    @_fault_wrapper
    def get_port(self, port_id):
        with self._locked(write=False) as state:
            rec = self._rec(state, "ports", port_id)
            return None if rec is None else self._port_info(rec)

    @_fault_wrapper
    def find_flavor(self, name_or_id):
        with self._locked(write=False) as state:
            flavors = self._catalog(state)["flavors"]
            rec = flavors.get(name_or_id)
            if rec is None:
                for cand in flavors.values():
                    if cand["name"] == name_or_id:
                        rec = cand
                        break
            if rec is None:
                raise CloudNotFound("flavor %s not found" % name_or_id)
            return FlavorInfo.from_dict(rec)

    @_fault_wrapper
    def network_exists(self, network_id):
        with self._locked(write=False) as state:
            return network_id in self._catalog(state)["networks"]

    # Snapshots and volumes -------------------------------------------------

    @_fault_wrapper
    def create_snapshot(
        self, volume_id, name, metadata, force, timeout, on_created=None
    ):
        with self._locked() as state:
            vol = self._rec(state, "volumes", volume_id)
            if vol is None:
                raise CloudNotFound("volume %s not found" % volume_id)
            if vol["attached_server_ids"] and not force:
                raise CloudConflict(
                    "volume %s is in use; a snapshot needs force" % volume_id
                )
            snap_id = _new_id()
            shutil.copyfile(self._vol_file(volume_id), self._snap_file(snap_id))
            state["snapshots"][snap_id] = self._stamp(
                state,
                {
                    "id": snap_id,
                    "volume_id": volume_id,
                    "name": name,
                    "status": "available",
                    "size_gb": vol["size_gb"],
                    "size_bytes": vol["size_bytes"],
                    "metadata": copy.deepcopy(metadata or {}),
                },
            )
            self._log(state, "create_snapshot", snap_id)
            info = self._snapshot_info(state["snapshots"][snap_id])
        if on_created is not None:
            on_created(snap_id)
        return info

    @_fault_wrapper
    def delete_snapshot(self, snapshot_id, timeout):
        with self._locked() as state:
            rec = self._rec(state, "snapshots", snapshot_id)
            if rec is not None:
                if rec.get("group_snapshot_id"):
                    raise CloudConflict(
                        "snapshot %s belongs to group snapshot %s; delete the "
                        "group snapshot" % (snapshot_id, rec["group_snapshot_id"])
                    )
                del state["snapshots"][snapshot_id]
                with contextlib.suppress(FileNotFoundError):
                    os.unlink(self._snap_file(snapshot_id))
            self._log(state, "delete_snapshot", snapshot_id)

    @_fault_wrapper
    def create_volume(
        self,
        name,
        size_gb,
        metadata,
        timeout,
        volume_type="",
        availability_zone="",
        snapshot_id="",
        on_created=None,
    ):
        with self._locked() as state:
            vol_id = _new_id()
            path = self._vol_file(vol_id)
            if snapshot_id:
                snap = self._rec(state, "snapshots", snapshot_id)
                if snap is None:
                    raise CloudNotFound("snapshot %s not found" % snapshot_id)
                shutil.copyfile(self._snap_file(snapshot_id), path)
                size_gb = max(int(size_gb), snap["size_gb"])
                size_bytes = snap["size_bytes"]
            else:
                size_bytes = int(size_gb) * GIB
                with open(path, "wb") as fh:
                    fh.truncate(size_bytes)
            state["volumes"][vol_id] = self._stamp(
                state,
                {
                    "id": vol_id,
                    "name": name,
                    "size_gb": int(size_gb),
                    "size_bytes": size_bytes,
                    "volume_type": volume_type,
                    "bootable": False,
                    "encrypted": False,
                    "multiattach": False,
                    "status": "available",
                    "availability_zone": availability_zone,
                    "metadata": copy.deepcopy(metadata or {}),
                    "attached_server_ids": [],
                    "source_snapshot_id": snapshot_id,
                    "source_image_id": "",
                    "image_metadata": {},
                },
            )
            self._log(state, "create_volume", vol_id)
            info = self._volume_info(state["volumes"][vol_id])
        if on_created is not None:
            on_created(vol_id)
        return info

    @_fault_wrapper
    def delete_volume(self, volume_id, timeout):
        with self._locked() as state:
            vol = self._rec(state, "volumes", volume_id)
            if vol is not None:
                if vol["attached_server_ids"]:
                    raise CloudConflict("volume %s is attached" % volume_id)
                if vol["status"] in ("uploading", "awaiting-transfer"):
                    raise CloudConflict(
                        "volume %s is %s and cannot be deleted"
                        % (volume_id, vol["status"])
                    )
                for snap in state["snapshots"].values():
                    if snap["volume_id"] == volume_id:
                        raise CloudConflict(
                            "volume %s still has snapshot %s" % (volume_id, snap["id"])
                        )
                del state["volumes"][volume_id]
                with contextlib.suppress(FileNotFoundError):
                    os.unlink(self._vol_file(volume_id))
            self._log(state, "delete_volume", volume_id)

    @_fault_wrapper
    def attach_to_proxy(self, volume_id, timeout):
        self._require_proxy()
        with self._locked() as state:
            vol = self._rec(state, "volumes", volume_id)
            if vol is None:
                raise CloudNotFound("volume %s not found" % volume_id)
            proxy = state["proxy_server_id"]
            if proxy not in vol["attached_server_ids"]:
                if vol["status"] != "available":
                    raise CloudConflict(
                        "volume %s is %s, not available" % (volume_id, vol["status"])
                    )
                vol["attached_server_ids"].append(proxy)
                vol["status"] = "in-use"
            self._log(state, "attach_to_proxy", volume_id)
        return self._vol_file(volume_id)

    @_fault_wrapper
    def detach_from_proxy(self, volume_id, timeout):
        self._require_proxy()
        with self._locked() as state:
            vol = self._rec(state, "volumes", volume_id)
            proxy = state["proxy_server_id"]
            if vol is not None and proxy in vol["attached_server_ids"]:
                vol["attached_server_ids"].remove(proxy)
                if not vol["attached_server_ids"]:
                    vol["status"] = "available"
            self._log(state, "detach_from_proxy", volume_id)

    @_fault_wrapper
    def set_volume_bootable(self, volume_id, bootable):
        with self._locked() as state:
            vol = self._rec(state, "volumes", volume_id)
            if vol is None:
                raise CloudNotFound("volume %s not found" % volume_id)
            vol["bootable"] = bool(bootable)
            self._log(state, "set_volume_bootable", volume_id)

    @_fault_wrapper
    def set_volume_image_metadata(self, volume_id, metadata):
        with self._locked() as state:
            vol = self._rec(state, "volumes", volume_id)
            if vol is None:
                raise CloudNotFound("volume %s not found" % volume_id)
            vol.setdefault("image_metadata", {}).update(
                {str(k): str(v) for k, v in (metadata or {}).items()}
            )
            self._log(state, "set_volume_image_metadata", volume_id)

    # Project resources -----------------------------------------------------

    @_fault_wrapper
    def get_security_group_name(self, group_id):
        with self._locked(write=False) as state:
            rec = self._catalog(state).get("security_groups", {}).get(group_id)
            if rec is None:
                raise CloudNotFound("security group %s not found" % group_id)
            return rec["name"]

    @_fault_wrapper
    def find_security_group(self, name):
        with self._locked(write=False) as state:
            found = [
                r
                for r in self._catalog(state).get("security_groups", {}).values()
                if r["name"] == name or r["id"] == name
            ]
            if not found:
                raise CloudNotFound("no security group named %s" % name)
            if len(found) > 1:
                raise CloudConflict(
                    "%d security groups are named %s" % (len(found), name)
                )
            return found[0]["id"]

    @_fault_wrapper
    def volume_type_usable(self, name):
        with self._locked(write=False) as state:
            types = self._catalog(state).get("volume_types")
            return types is None or name in types

    @_fault_wrapper
    def availability_zone_exists(self, name):
        with self._locked(write=False) as state:
            zones = self._catalog(state).get("availability_zones")
            return zones is None or name in zones

    # Transfers -------------------------------------------------------------

    @_fault_wrapper
    def create_transfer(self, volume_id, name, on_created=None):
        with self._locked() as state:
            vol = self._rec(state, "volumes", volume_id)
            if vol is None:
                raise CloudNotFound("volume %s not found" % volume_id)
            if vol["status"] != "available":
                raise CloudConflict(
                    "volume %s is %s; a transfer needs an available volume"
                    % (volume_id, vol["status"])
                )
            transfer_id = _new_id()
            key = _new_id().replace("-", "")
            state.setdefault("transfers", {})[transfer_id] = {
                "id": transfer_id,
                "name": name,
                "volume_id": volume_id,
                "auth_key": key,
                "project_id": self._proj(state),
            }
            vol["status"] = "awaiting-transfer"
            self._log(state, "create_transfer", transfer_id)
        if on_created is not None:
            on_created(transfer_id)
        return transfer_id, key

    @_fault_wrapper
    def accept_transfer(self, transfer_id, auth_key, timeout):
        with self._locked() as state:
            rec = state.get("transfers", {}).get(transfer_id)
            if rec is None:
                raise CloudNotFound("transfer %s not found" % transfer_id)
            if rec["auth_key"] != auth_key:
                raise CloudConflict("transfer %s: the auth key is wrong" % transfer_id)
            proj = self._proj(state)
            if rec["project_id"] == proj:
                raise CloudConflict(
                    "transfer %s cannot be accepted by its own project" % transfer_id
                )
            vol = state["volumes"][rec["volume_id"]]
            if proj == state["project_id"]:
                vol.pop("project_id", None)
            else:
                vol["project_id"] = proj
            vol["status"] = "available"
            del state["transfers"][transfer_id]
            self._log(state, "accept_transfer", transfer_id)

    @_fault_wrapper
    def delete_transfer(self, transfer_id):
        with self._locked() as state:
            rec = state.get("transfers", {}).get(transfer_id)
            if rec is not None and rec["project_id"] == self._proj(state):
                vol = state["volumes"].get(rec["volume_id"])
                if vol is not None and vol["status"] == "awaiting-transfer":
                    vol["status"] = "available"
                del state["transfers"][transfer_id]
            self._log(state, "delete_transfer", transfer_id)

    @_fault_wrapper
    def list_transfers(self, name_prefix):
        with self._locked(write=False) as state:
            proj = self._proj(state)
            return [
                TransferInfo(r["id"], r["name"], r["volume_id"])
                for r in state.get("transfers", {}).values()
                if r["project_id"] == proj and r["name"].startswith(name_prefix)
            ]

    # Consistency groups ----------------------------------------------------

    @staticmethod
    def _require_groups(state):
        text = str(state.get("block_storage_microversion", "3.50"))
        try:
            major, minor = text.split(".")
            offered = (int(major), int(minor))
        except ValueError:
            offered = (0, 0)
        if offered < MIN_GROUP_MICROVERSION:
            raise CloudConfigError(
                "volume groups need block-storage microversion 3.14 or later; "
                "the cloud offers %s" % text
            )

    @_fault_wrapper
    def find_consistency_group(self, volume_ids):
        need = set(volume_ids)
        with self._locked(write=False) as state:
            self._require_groups(state)
            groups = self._all(state, "groups")
            holders = {g["id"]: g for g in groups if need & set(g["volume_ids"])}
            loose = sorted(
                v for v in need if not any(v in g["volume_ids"] for g in groups)
            )
            if loose:
                raise CloudConflict(
                    "volume(s) %s are in no volume group" % ", ".join(loose)
                )
            if len(holders) > 1:
                raise CloudConflict(
                    "the volumes are spread over several groups: %s"
                    % ", ".join(sorted(holders))
                )
            group = next(iter(holders.values()))
            gtype = state.get("group_types", {}).get(group["group_type"], {})
            if not gtype.get("consistent_group_snapshot_enabled"):
                raise CloudConflict(
                    "group %s: its group type %s does not enable consistent "
                    "group snapshots" % (group["id"], group["group_type"])
                )
            members = set(group["volume_ids"])
            if members != need:
                raise CloudConflict(
                    "group %s does not hold exactly the volumes of the server "
                    "(extra: %s, missing: %s)"
                    % (
                        group["id"],
                        ", ".join(sorted(members - need)) or "-",
                        ", ".join(sorted(need - members)) or "-",
                    )
                )
            return group["id"]

    @_fault_wrapper
    def create_group_snapshot(
        self, group_id, name, description, timeout, on_created=None
    ):
        with self._locked() as state:
            self._require_groups(state)
            group = self._rec(state, "groups", group_id)
            if group is None:
                raise CloudNotFound("group %s not found" % group_id)
            gs_id = _new_id()
            members = {}
            for volume_id in group["volume_ids"]:
                vol = self._rec(state, "volumes", volume_id)
                if vol is None:
                    raise CloudNotFound("volume %s not found" % volume_id)
                snap_id = _new_id()
                shutil.copyfile(self._vol_file(volume_id), self._snap_file(snap_id))
                state["snapshots"][snap_id] = self._stamp(
                    state,
                    {
                        "id": snap_id,
                        "volume_id": volume_id,
                        "name": "snapshot-%s" % gs_id,
                        "status": "available",
                        "size_gb": vol["size_gb"],
                        "size_bytes": vol["size_bytes"],
                        "metadata": {},
                        "group_snapshot_id": gs_id,
                    },
                )
                members[volume_id] = snap_id
            state.setdefault("group_snapshots", {})[gs_id] = self._stamp(
                state,
                {
                    "id": gs_id,
                    "name": name,
                    "description": description,
                    "group_id": group_id,
                    "status": "available",
                    "snapshot_ids": dict(members),
                },
            )
            self._log(state, "create_group_snapshot", gs_id)
        if on_created is not None:
            on_created(gs_id)
        return dict(members)

    @_fault_wrapper
    def delete_group_snapshot(self, group_snapshot_id, timeout):
        with self._locked() as state:
            rec = self._rec(state, "group_snapshots", group_snapshot_id)
            if rec is not None:
                members = set(rec["snapshot_ids"].values())
                dependents = sorted(
                    v["id"]
                    for v in state["volumes"].values()
                    if v.get("source_snapshot_id") in members
                )
                if dependents:
                    raise CloudConflict(
                        "group snapshot %s still has dependent volume(s) %s"
                        % (group_snapshot_id, ", ".join(dependents))
                    )
                for snap_id in members:
                    state["snapshots"].pop(snap_id, None)
                    with contextlib.suppress(FileNotFoundError):
                        os.unlink(self._snap_file(snap_id))
                del state["group_snapshots"][group_snapshot_id]
            self._log(state, "delete_group_snapshot", group_snapshot_id)

    @_fault_wrapper
    def list_group_snapshots(self, name_prefix):
        with self._locked(write=False) as state:
            self._require_groups(state)
            return [
                GroupSnapshotInfo(
                    id=r["id"],
                    name=r["name"],
                    description=r["description"],
                    group_id=r["group_id"],
                    status=r["status"],
                    snapshot_ids=dict(r["snapshot_ids"]),
                )
                for r in self._all(state, "group_snapshots")
                if r["name"].startswith(name_prefix)
            ]

    # Ports -----------------------------------------------------------------

    @_fault_wrapper
    def create_port(
        self,
        network_id,
        name,
        security_group_ids,
        fixed_ips=None,
        mac_address="",
        tags=None,
    ):
        with self._locked() as state:
            if network_id not in self._catalog(state)["networks"]:
                raise CloudNotFound("network %s not found" % network_id)
            used_ips = {
                ip.get("ip_address")
                for p in state["ports"].values()
                for ip in p["fixed_ips"]
            }
            used_macs = {p["mac_address"] for p in state["ports"].values()}
            ips = []
            for entry in fixed_ips or []:
                address = entry.get("ip_address") or ""
                if not address:
                    number = 2
                    while "10.0.0.%d" % number in used_ips:
                        number += 1
                    address = "10.0.0.%d" % number
                if address in used_ips:
                    raise CloudConflict("address %s is already in use" % address)
                used_ips.add(address)
                ips.append(
                    {"subnet_id": entry.get("subnet_id", ""), "ip_address": address}
                )
            mac = mac_address
            if mac:
                if mac in used_macs:
                    raise CloudConflict("MAC address %s is already in use" % mac)
            else:
                while True:
                    mac = "fa:16:3e:%02x:%02x:%02x" % tuple(
                        random.randrange(256) for _ in range(3)
                    )
                    if mac not in used_macs:
                        break
            port_id = _new_id()
            state["ports"][port_id] = self._stamp(
                state,
                {
                    "id": port_id,
                    "network_id": network_id,
                    "mac_address": mac,
                    "fixed_ips": ips,
                    "security_group_ids": list(security_group_ids or []),
                    "name": name,
                    "device_id": "",
                    "tags": [str(t) for t in tags or []],
                },
            )
            self._log(state, "create_port", port_id)
            return self._port_info(state["ports"][port_id])

    @_fault_wrapper
    def delete_port(self, port_id):
        with self._locked() as state:
            if self._rec(state, "ports", port_id) is not None:
                del state["ports"][port_id]
            self._log(state, "delete_port", port_id)

    # Servers ---------------------------------------------------------------

    @_fault_wrapper
    def create_server(
        self,
        name,
        flavor_id,
        block_devices,
        port_ids,
        key_name,
        availability_zone,
        metadata,
        timeout,
        on_created=None,
    ):
        with self._locked() as state:
            if flavor_id not in self._catalog(state)["flavors"]:
                raise CloudNotFound("flavor %s not found" % flavor_id)
            volume_ids = [b["volume_id"] for b in block_devices]
            if len(set(volume_ids)) != len(volume_ids):
                raise CloudConflict("a volume is listed twice")
            for vid in volume_ids:
                vol = self._rec(state, "volumes", vid)
                if vol is None:
                    raise CloudNotFound("volume %s not found" % vid)
                if vol["status"] != "available":
                    raise CloudConflict(
                        "volume %s is %s, not available" % (vid, vol["status"])
                    )
            if sum(1 for b in block_devices if b.get("boot_index") == 0) != 1:
                raise CloudConflict("exactly one volume must have boot_index 0")
            for pid in port_ids:
                port = self._rec(state, "ports", pid)
                if port is None:
                    raise CloudNotFound("port %s not found" % pid)
                if port["device_id"]:
                    raise CloudConflict(
                        "port %s is bound to %s" % (pid, port["device_id"])
                    )
            srv_id = _new_id()
            attachments = []
            for index, dev in enumerate(block_devices):
                attachments.append(
                    {
                        "volume_id": dev["volume_id"],
                        "device": "/dev/vd" + chr(ord("a") + index),
                        "boot_index": dev.get("boot_index"),
                        "delete_on_termination": False,
                    }
                )
                vol = state["volumes"][dev["volume_id"]]
                vol["status"] = "in-use"
                vol["attached_server_ids"] = [srv_id]
            for pid in port_ids:
                state["ports"][pid]["device_id"] = srv_id
            state["servers"][srv_id] = {
                "id": srv_id,
                "name": name,
                "project_id": self._proj(state),
                "status": "ACTIVE",
                "flavor_id": flavor_id,
                "image_id": "",
                "key_name": key_name or "",
                "availability_zone": availability_zone or "",
                "metadata": copy.deepcopy(metadata or {}),
                "tags": [],
                "security_groups": [],
                "attachments": attachments,
            }
            self._log(state, "create_server", srv_id)
            info = self._server_info(state, state["servers"][srv_id])
        if on_created is not None:
            on_created(srv_id)
        return info

    def _set_status(self, op, server_id, status):
        with self._locked() as state:
            rec = self._rec(state, "servers", server_id)
            if rec is None:
                raise CloudNotFound("server %s not found" % server_id)
            rec["status"] = status
            self._log(state, op, server_id)

    @_fault_wrapper
    def stop_server(self, server_id, timeout):
        self._set_status("stop_server", server_id, "SHUTOFF")

    @_fault_wrapper
    def start_server(self, server_id, timeout):
        self._set_status("start_server", server_id, "ACTIVE")

    @_fault_wrapper
    def set_delete_on_termination(self, server_id, volume_id, value):
        with self._locked() as state:
            rec = self._rec(state, "servers", server_id)
            if rec is None:
                raise CloudNotFound("server %s not found" % server_id)
            for att in rec["attachments"]:
                if att["volume_id"] == volume_id:
                    att["delete_on_termination"] = bool(value)
                    break
            else:
                raise CloudNotFound(
                    "volume %s is not attached to server %s" % (volume_id, server_id)
                )
            self._log(state, "set_delete_on_termination", server_id)

    @_fault_wrapper
    def delete_server(self, server_id, timeout):
        with self._locked() as state:
            rec = self._rec(state, "servers", server_id)
            if rec is not None:
                del state["servers"][server_id]
                for att in rec["attachments"]:
                    vol = self._rec(state, "volumes", att["volume_id"])
                    if vol is None:
                        continue
                    if server_id in vol["attached_server_ids"]:
                        vol["attached_server_ids"].remove(server_id)
                    if not vol["attached_server_ids"]:
                        vol["status"] = "available"
                    if att["delete_on_termination"]:
                        del state["volumes"][vol["id"]]
                        with contextlib.suppress(FileNotFoundError):
                            os.unlink(self._vol_file(vol["id"]))
                for port in state["ports"].values():
                    if port["device_id"] == server_id:
                        port["device_id"] = ""
            self._log(state, "delete_server", server_id)


# Seeding -------------------------------------------------------------------


def _seed_catalog(spec):
    catalog = {"flavors": {}, "networks": {}, "security_groups": {}}
    for flavor in spec.get("flavors", []):
        rec = {
            "id": flavor["id"],
            "name": flavor.get("name", flavor["id"]),
            "vcpus": flavor.get("vcpus", 1),
            "ram": flavor.get("ram", 512),
            "disk": flavor.get("disk", 1),
            "extra_specs": copy.deepcopy(flavor.get("extra_specs", {})),
        }
        for key in ("ephemeral", "swap"):
            if key in flavor:
                rec[key] = int(flavor[key])
        catalog["flavors"][rec["id"]] = rec
    for net in spec.get("networks", []):
        catalog["networks"][net["id"]] = {
            "id": net["id"],
            "name": net.get("name", net["id"]),
            "subnet_ids": list(net.get("subnet_ids", [])),
        }
    for grp in spec.get("security_groups", []):
        catalog["security_groups"][grp["id"]] = {
            "id": grp["id"],
            "name": grp.get("name", grp["id"]),
        }
    for key in ("volume_types", "availability_zones"):
        if key in spec:
            catalog[key] = [str(v) for v in spec[key]]
    return catalog


def _volume_record(spec, state, server_id=None):
    size_gb = int(spec.get("size_gb", 1))
    size_bytes = int(spec.get("size_bytes", size_gb * GIB))
    if size_bytes > size_gb * GIB or size_bytes < 0:
        raise CloudConfigError(
            "volume %s: size_bytes %d does not fit size_gb %d"
            % (spec.get("name", ""), size_bytes, size_gb)
        )
    vol_id = spec.get("id") or _new_id()
    attached = [server_id] if server_id else []
    rec = {
        "id": vol_id,
        "name": spec.get("name", ""),
        "size_gb": size_gb,
        "size_bytes": size_bytes,
        "volume_type": spec.get("volume_type", ""),
        "bootable": bool(spec.get("bootable", False)),
        "encrypted": bool(spec.get("encrypted", False)),
        "multiattach": bool(spec.get("multiattach", False)),
        "status": "in-use" if attached else "available",
        "availability_zone": spec.get("availability_zone", ""),
        "metadata": copy.deepcopy(spec.get("metadata", {})),
        "attached_server_ids": attached,
        "source_snapshot_id": "",
        "source_image_id": "",
        "image_metadata": {
            str(k): str(v) for k, v in spec.get("image_metadata", {}).items()
        },
    }
    if spec.get("project_id"):
        rec["project_id"] = spec["project_id"]
    return rec


def _create_file(path, size_bytes, content):
    with open(path, "wb") as fh:
        fh.truncate(size_bytes)
    content = content or {}
    if "random_seed" in content:
        _fill_random(path, 0, size_bytes, content["random_seed"])
    for offset, length in content.get("zero_ranges", []):
        _fill_zero(path, int(offset), int(length))


def _create_volume_file(root, rec, content):
    path = os.path.join(root, "volumes", rec["id"] + ".img")
    _create_file(path, rec["size_bytes"], content)


def _disk_sizes(spec, what):
    size_gb = int(spec.get("size_gb", 1))
    size_bytes = int(spec.get("size_bytes", size_gb * GIB))
    if size_bytes > size_gb * GIB or size_bytes < 0:
        raise CloudConfigError(
            "%s: size_bytes %d does not fit size_gb %d" % (what, size_bytes, size_gb)
        )
    return size_gb, size_bytes


def seed(root, spec, reset=False):
    """Create a fake cloud in `root` from `spec`; returns the created ids by name."""
    root = os.path.abspath(root)
    state_path = os.path.join(root, "state.json")
    if os.path.exists(state_path) and not reset:
        raise CloudConflict("%s already holds a fake cloud; use reset" % root)
    if reset:
        for name in ("state.json", "state.lock", "state.json.tmp"):
            with contextlib.suppress(FileNotFoundError):
                os.unlink(os.path.join(root, name))
        for name in ("volumes", "snapshots"):
            shutil.rmtree(os.path.join(root, name), ignore_errors=True)
    for name in ("volumes", "snapshots"):
        os.makedirs(os.path.join(root, name), exist_ok=True)

    if "images" in spec:
        raise CloudConfigError("the fake cloud has no images; remove the images section")
    proxy_id = spec.get("proxy_server_id") or _new_id()
    state = _empty_state(spec.get("project_id") or _new_id(), proxy_id)
    state.update(_seed_catalog(spec))
    for extra in spec.get("projects", []):
        state.setdefault("projects", {})[extra["id"]] = _seed_catalog(extra)
    for key in (
        "block_storage_microversion",
    ):
        if key in spec:
            state[key] = spec[key]
    if "group_types" in spec:
        state["group_types"] = {
            str(name): {
                "consistent_group_snapshot_enabled": bool(
                    value.get("consistent_group_snapshot_enabled", False)
                )
            }
            for name, value in spec["group_types"].items()
        }
    state["servers"][proxy_id] = {
        "id": proxy_id,
        "name": "bareos-proxy",
        "project_id": state["project_id"],
        "status": "ACTIVE",
        "flavor_id": "",
        "image_id": "",
        "key_name": "",
        "availability_zone": "",
        "metadata": {},
        "tags": [],
        "security_groups": [],
        "attachments": [],
    }

    result = {"servers": {}, "volumes": {}, "ports": {}}
    for srv in spec.get("servers", []):
        srv_id = srv.get("id") or _new_id()
        attachments = []
        for index, vspec in enumerate(srv.get("volumes", [])):
            rec = _volume_record(vspec, state, srv_id)
            _create_volume_file(root, rec, vspec.get("content"))
            state["volumes"][rec["id"]] = rec
            result["volumes"][rec["name"] or rec["id"]] = rec["id"]
            attachments.append(
                {
                    "volume_id": rec["id"],
                    "device": vspec.get("device") or "/dev/vd" + chr(ord("a") + index),
                    "boot_index": vspec.get("boot_index"),
                    "delete_on_termination": bool(
                        vspec.get("delete_on_termination", False)
                    ),
                }
            )
        for pspec in srv.get("ports", []):
            port_id = pspec.get("id") or _new_id()
            state["ports"][port_id] = {
                "id": port_id,
                "network_id": pspec["network_id"],
                "mac_address": pspec.get("mac_address")
                or "fa:16:3e:%02x:%02x:%02x"
                % tuple(random.randrange(256) for _ in range(3)),
                "fixed_ips": copy.deepcopy(pspec.get("fixed_ips", [])),
                "security_group_ids": list(pspec.get("security_group_ids", [])),
                "name": pspec.get("name", ""),
                "device_id": srv_id,
            }
            result["ports"][pspec.get("name") or port_id] = port_id
        if "root_disk" in srv:
            raise CloudConfigError(
                "server %s: root_disk is no longer supported" % srv["name"]
            )
        server_rec = {
            "id": srv_id,
            "name": srv["name"],
            "project_id": srv.get("project_id") or state["project_id"],
            "status": srv.get("status", "ACTIVE"),
            "flavor_id": srv.get("flavor_id", ""),
            "image_id": srv.get("image_id", ""),
            "key_name": srv.get("key_name", ""),
            "availability_zone": srv.get("availability_zone", ""),
            "metadata": copy.deepcopy(srv.get("metadata", {})),
            "tags": list(srv.get("tags", [])),
            "security_groups": list(srv.get("security_groups", [])),
            "attachments": attachments,
        }
        state["servers"][srv_id] = server_rec
        result["servers"][srv["name"]] = srv_id
    for vspec in spec.get("volumes", []):
        rec = _volume_record(vspec, state)
        _create_volume_file(root, rec, vspec.get("content"))
        state["volumes"][rec["id"]] = rec
        result["volumes"][rec["name"] or rec["id"]] = rec["id"]
    for gspec in spec.get("groups", []):
        group_id = gspec.get("id") or _new_id()
        members = [result["volumes"].get(v, v) for v in gspec.get("volumes", [])]
        for member in members:
            if member not in state["volumes"]:
                raise CloudConfigError(
                    "group %s: unknown volume %s" % (group_id, member)
                )
        grec = {
            "id": group_id,
            "name": gspec.get("name", group_id),
            "group_type": gspec.get("group_type", ""),
            "volume_ids": members,
        }
        if gspec.get("project_id"):
            grec["project_id"] = gspec["project_id"]
        state.setdefault("groups", {})[group_id] = grec
        result.setdefault("groups", {})[grec["name"]] = group_id

    tmp = state_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(state, fh, indent=1, sort_keys=True)
    os.replace(tmp, state_path)
    return result


# Command-line interface ----------------------------------------------------


def _build_parser():
    parser = argparse.ArgumentParser(
        prog="python3 -m bareos_openstack.fake_backend",
        description="Control a file-backed fake OpenStack cloud.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    def command(name, help_text):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("--root", required=True, help="fake cloud directory")
        return p

    p = command("init", "seed the cloud from a specification")
    p.add_argument("--spec", required=True)
    p.add_argument("--reset", action="store_true")
    p = command("write", "overwrite a range of a volume")
    p.add_argument("--volume", required=True)
    p.add_argument("--offset", type=int, required=True)
    p.add_argument("--length", type=int, required=True)
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument("--seed", type=int)
    group.add_argument("--zero", action="store_true")
    p = command("resize", "grow a volume")
    p.add_argument("--volume", required=True)
    p.add_argument("--size-bytes", type=int, required=True)
    p = command("set-status", "set the status of a server")
    p.add_argument("--server", required=True)
    p.add_argument("--status", required=True)
    p = command("fault", "inject a fault into a backend method")
    p.add_argument("--op", required=True)
    p.add_argument("--remaining", type=int, default=1)
    p.add_argument("--message", default=None)
    p.add_argument("--sleep", type=float, default=0.0)
    p.add_argument("--phase", choices=("before", "after"), default="before")
    p.add_argument(
        "--delay-only",
        action="store_true",
        help="sleep, then let the call succeed (no error)",
    )
    command("clear-faults", "remove all faults")
    command("show", "print the state")
    p = command("volume-path", "print the file of a volume")
    p.add_argument("--volume", required=True)
    p = command("servers", "list the servers")
    p.add_argument("--name", default=None)
    command("check-clean", "report backup resources left behind")
    command("log", "print the operation log")
    return parser


def _run(args):
    if args.command == "init":
        with open(args.spec, "r", encoding="utf-8") as fh:
            spec = json.load(fh)
        print(json.dumps(seed(args.root, spec, reset=args.reset), sort_keys=True))
        return 0

    cloud = FakeBackend({"backend": "fake", "fake_root": args.root}, all_projects=True)
    if args.command == "volume-path":
        with cloud._locked(write=False) as state:
            if args.volume not in state["volumes"]:
                raise CloudNotFound("volume %s not found" % args.volume)
        print(cloud._vol_file(args.volume))
    elif args.command == "write":
        with cloud._locked(write=False) as state:
            vol = state["volumes"].get(args.volume)
            if vol is None:
                raise CloudNotFound("volume %s not found" % args.volume)
            if (
                args.offset < 0
                or args.length < 0
                or args.offset + args.length > vol["size_bytes"]
            ):
                raise CloudError(
                    "range %d+%d is outside the volume (%d bytes)"
                    % (args.offset, args.length, vol["size_bytes"])
                )
            path = cloud._vol_file(args.volume)
            if args.zero:
                _fill_zero(path, args.offset, args.length)
            else:
                _fill_random(path, args.offset, args.length, args.seed)
    elif args.command == "resize":
        with cloud._locked() as state:
            vol = state["volumes"].get(args.volume)
            if vol is None:
                raise CloudNotFound("volume %s not found" % args.volume)
            if args.size_bytes < vol["size_bytes"]:
                raise CloudConflict("a volume cannot be shrunk")
            with open(cloud._vol_file(args.volume), "r+b") as fh:
                fh.truncate(args.size_bytes)
            vol["size_bytes"] = args.size_bytes
            vol["size_gb"] = max(vol["size_gb"], -(-args.size_bytes // GIB))
    elif args.command == "set-status":
        with cloud._locked() as state:
            if args.server not in state["servers"]:
                raise CloudNotFound("server %s not found" % args.server)
            state["servers"][args.server]["status"] = args.status
    elif args.command == "fault":
        with cloud._locked() as state:
            state["faults"][args.op] = {
                "remaining": args.remaining,
                "message": args.message or "injected fault in %s" % args.op,
                "sleep": args.sleep,
                "phase": args.phase,
                "delay_only": args.delay_only,
            }
    elif args.command == "clear-faults":
        with cloud._locked() as state:
            state["faults"] = {}
    elif args.command == "show":
        with cloud._locked(write=False) as state:
            print(json.dumps(state, indent=1, sort_keys=True))
    elif args.command == "servers":
        with cloud._locked(write=False) as state:
            listing = []
            for rec in state["servers"].values():
                if rec["id"] == state["proxy_server_id"]:
                    continue
                if args.name is not None and rec["name"] != args.name:
                    continue
                listing.append(
                    {
                        "id": rec["id"],
                        "name": rec["name"],
                        "status": rec["status"],
                        "volume_ids": [a["volume_id"] for a in rec["attachments"]],
                        "port_ids": sorted(
                            p["id"]
                            for p in state["ports"].values()
                            if p["device_id"] == rec["id"]
                        ),
                    }
                )
            print(json.dumps(listing, sort_keys=True))
    elif args.command == "check-clean":
        lines = []
        with cloud._locked(write=False) as state:
            for snap in state["snapshots"].values():
                if snap["metadata"].get(TAG_ROLE) == ROLE_BACKUP_SNAPSHOT:
                    lines.append(
                        "snapshot %s role=%s" % (snap["id"], ROLE_BACKUP_SNAPSHOT)
                    )
            for gsnap in state.get("group_snapshots", {}).values():
                if parse_group_snapshot_description(gsnap["description"]):
                    lines.append(
                        "group-snapshot %s role=%s"
                        % (gsnap["id"], ROLE_BACKUP_GROUP_SNAPSHOT)
                    )
            for transfer in state.get("transfers", {}).values():
                lines.append("transfer %s pending" % transfer["id"])
            for vol in state["volumes"].values():
                if vol["metadata"].get(TAG_ROLE) == ROLE_BACKUP_TEMP_VOLUME:
                    lines.append(
                        "volume %s role=%s" % (vol["id"], ROLE_BACKUP_TEMP_VOLUME)
                    )
                if state["proxy_server_id"] in vol["attached_server_ids"]:
                    lines.append("volume %s attached to the proxy" % vol["id"])
        if lines:
            print("\n".join(lines))
            return 1
        print("clean")
    elif args.command == "log":
        with cloud._locked(write=False) as state:
            for entry in state["log"]:
                print(json.dumps(entry, sort_keys=True))
    return 0


def main(argv=None):
    """Command-line entry point; returns the process exit code."""
    parser = _build_parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return exc.code if isinstance(exc.code, int) else 2
    try:
        return _run(args)
    except CloudError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    except (OSError, ValueError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
