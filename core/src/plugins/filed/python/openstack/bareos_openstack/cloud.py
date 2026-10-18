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

"""Cloud interface of the OpenStack plugin: exceptions, data classes, backend base."""

import abc
import copy
import dataclasses
import time
from dataclasses import dataclass, field
from typing import Optional

GIB = 1024**3

TAG_CREATED_BY = "created-by"
TAG_CREATED_BY_VALUE = "bareos"
TAG_JOB_ID = "bareos-job-id"
TAG_SERVER_ID = "bareos-server-id"
TAG_ROLE = "bareos-role"
TAG_SOURCE_VOLUME = "bareos-source-volume-id"

ROLE_BACKUP_SNAPSHOT = "backup-snapshot"
ROLE_BACKUP_TEMP_VOLUME = "backup-temp-volume"
ROLE_RESTORE_VOLUME = "restore-volume"
ROLE_BACKUP_GROUP_SNAPSHOT = "backup-group-snapshot"

# Value of the document field "consistency" of a group snapshot backup.
CONSISTENCY_GROUP = "group-crash-consistent"
CONSISTENCY_PER_VOLUME = "per-volume"

# Volume entry id of the root disk of an image-booted server.
ROOT_VOLUME_ID = "root"

# Image properties that decide how a restored root volume boots.
BOOT_PROPERTY_KEYS = (
    "hw_firmware_type",
    "hw_machine_type",
    "hw_disk_bus",
    "hw_scsi_model",
    "hw_cdrom_bus",
    "hw_vif_model",
    "hw_video_model",
    "hw_qemu_guest_agent",
    "hw_boot_menu",
    "hw_rng_model",
    "os_type",
    "os_distro",
    "os_secure_boot",
    "architecture",
    "hw_architecture",
)


def bareos_tags(job_id, server_id, role, source_volume_id=""):
    """Return the metadata every snapshot and volume of the plugin carries."""
    tags = {
        TAG_CREATED_BY: TAG_CREATED_BY_VALUE,
        TAG_JOB_ID: str(job_id),
        TAG_ROLE: str(role),
    }
    if server_id:
        tags[TAG_SERVER_ID] = str(server_id)
    if source_volume_id:
        tags[TAG_SOURCE_VOLUME] = str(source_volume_id)
    return tags


def group_snapshot_name(job_id, server_id):
    """Name of the group snapshot of a job."""
    return "bareos-%s-gsnap-%s" % (job_id, server_id)


def group_snapshot_description(job_id, server_id):
    """Identity of a group snapshot, which has no metadata of its own."""
    return " ".join(
        "%s=%s" % item
        for item in (
            (TAG_CREATED_BY, TAG_CREATED_BY_VALUE),
            (TAG_JOB_ID, job_id),
            (TAG_SERVER_ID, server_id),
            (TAG_ROLE, ROLE_BACKUP_GROUP_SNAPSHOT),
        )
    )


def parse_group_snapshot_description(description):
    """Return the tags of a Bareos group snapshot description, {} for others."""
    tags = {}
    for word in str(description or "").split():
        key, sep, value = word.partition("=")
        if sep:
            tags[key] = value
    if (
        tags.get(TAG_CREATED_BY) != TAG_CREATED_BY_VALUE
        or tags.get(TAG_ROLE) != ROLE_BACKUP_GROUP_SNAPSHOT
    ):
        return {}
    return tags


def transfer_name(job_id, saved_volume_id):
    """Name of the volume transfer of a cross-project restore."""
    return "bareos-%s-xfer-%s" % (job_id, saved_volume_id)


class CloudError(Exception):
    """Base class of all cloud errors."""


class CloudNotFound(CloudError):
    """A resource does not exist."""


class CloudTimeout(CloudError):
    """A wait ran into its deadline."""


class CloudConflict(CloudError):
    """Wrong state, ambiguous name or a violated rule."""


class CloudConfigError(CloudError):
    """Bad or missing configuration or a missing library."""


class CloudTransferAccepted(CloudError):
    """A volume transfer was accepted, but a later step of the call failed."""


class CloudCancelled(CloudError):
    """The cancel event was set during an operation."""


class _Record:
    """Dictionary conversion shared by the data classes."""

    def to_dict(self):
        return copy.deepcopy(dataclasses.asdict(self))

    @classmethod
    def from_dict(cls, data):
        names = {f.name for f in dataclasses.fields(cls)}
        return cls(**{k: copy.deepcopy(v) for k, v in data.items() if k in names})


@dataclass
class FlavorInfo(_Record):
    id: str
    name: str
    vcpus: int = 0
    ram: int = 0
    disk: int = 0
    extra_specs: dict = field(default_factory=dict)


@dataclass
class ServerInfo(_Record):
    id: str
    name: str
    project_id: str
    status: str
    flavor: dict = field(default_factory=dict)
    image_id: str = ""
    key_name: str = ""
    availability_zone: str = ""
    metadata: dict = field(default_factory=dict)
    tags: list = field(default_factory=list)
    security_groups: list = field(default_factory=list)

    @property
    def boot_from_volume(self):
        return not self.image_id


@dataclass
class VolumeInfo(_Record):
    id: str
    name: str = ""
    size_gb: int = 0
    volume_type: str = ""
    bootable: bool = False
    encrypted: bool = False
    multiattach: bool = False
    status: str = ""
    availability_zone: str = ""
    metadata: dict = field(default_factory=dict)
    device: str = ""
    boot_index: Optional[int] = None
    delete_on_termination: bool = False
    attached_server_ids: list = field(default_factory=list)
    source: str = "volume"
    image_metadata: dict = field(default_factory=dict)


@dataclass
class SnapshotInfo(_Record):
    id: str
    volume_id: str
    name: str = ""
    status: str = ""
    size_gb: int = 0
    metadata: dict = field(default_factory=dict)


@dataclass
class TransferInfo(_Record):
    id: str
    name: str = ""
    volume_id: str = ""


@dataclass
class GroupSnapshotInfo(_Record):
    id: str
    name: str = ""
    description: str = ""
    group_id: str = ""
    status: str = ""
    snapshot_ids: dict = field(default_factory=dict)

    @property
    def tags(self):
        return parse_group_snapshot_description(self.description)


@dataclass
class PortInfo(_Record):
    id: str
    network_id: str
    mac_address: str = ""
    fixed_ips: list = field(default_factory=list)
    security_group_ids: list = field(default_factory=list)
    name: str = ""
    device_id: str = ""


class CloudBackend(abc.ABC):
    """Operations the plugin needs from a cloud; see plan C.3.2 for the rules."""

    def __init__(self):
        self._cancel_event = None

    def set_cancel_event(self, event):
        self._cancel_event = event

    def check_cancel(self):
        event = self._cancel_event
        if event is not None and event.is_set():
            raise CloudCancelled("operation cancelled")

    def sleep(self, seconds):
        """Sleep in steps of at most 0.1 s, raising CloudCancelled when cancelled."""
        deadline = time.monotonic() + max(0.0, seconds)
        while True:
            self.check_cancel()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            time.sleep(min(0.1, remaining))

    @abc.abstractmethod
    def connect(self):
        """Authenticate or load the state."""

    @abc.abstractmethod
    def close(self):
        """Release the connection; idempotent."""

    @property
    @abc.abstractmethod
    def project_id(self):
        """Project of the connection, empty when unknown."""

    @property
    @abc.abstractmethod
    def proxy_instance_id(self):
        """Nova id of the proxy server."""

    @abc.abstractmethod
    def get_server(self, server_id):
        pass

    @abc.abstractmethod
    def find_server(self, name):
        pass

    @abc.abstractmethod
    def list_servers(self):
        """Every server of the project except the proxy."""

    @abc.abstractmethod
    def list_server_volumes(self, server_id):
        pass

    @abc.abstractmethod
    def list_server_ports(self, server_id):
        pass

    @abc.abstractmethod
    def get_volume(self, volume_id):
        pass

    @abc.abstractmethod
    def list_volumes(self, metadata_filter):
        pass

    @abc.abstractmethod
    def list_snapshots(self, metadata_filter):
        pass

    @abc.abstractmethod
    def list_proxy_attachments(self):
        pass

    @abc.abstractmethod
    def get_port(self, port_id):
        pass

    @abc.abstractmethod
    def find_flavor(self, name_or_id):
        pass

    @abc.abstractmethod
    def network_exists(self, network_id):
        pass

    @abc.abstractmethod
    def create_snapshot(
        self, volume_id, name, metadata, force, timeout, on_created=None
    ):
        pass

    @abc.abstractmethod
    def delete_snapshot(self, snapshot_id, timeout):
        pass

    @abc.abstractmethod
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
        pass

    @abc.abstractmethod
    def delete_volume(self, volume_id, timeout):
        pass

    @abc.abstractmethod
    def attach_to_proxy(self, volume_id, timeout):
        pass

    @abc.abstractmethod
    def detach_from_proxy(self, volume_id, timeout):
        pass

    @abc.abstractmethod
    def set_volume_bootable(self, volume_id, bootable):
        pass

    @abc.abstractmethod
    def create_port(
        self,
        network_id,
        name,
        security_group_ids,
        fixed_ips=None,
        mac_address="",
        tags=None,
    ):
        pass

    @abc.abstractmethod
    def delete_port(self, port_id):
        pass

    @abc.abstractmethod
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
        pass

    @abc.abstractmethod
    def stop_server(self, server_id, timeout):
        pass

    @abc.abstractmethod
    def start_server(self, server_id, timeout):
        pass

    @abc.abstractmethod
    def set_delete_on_termination(self, server_id, volume_id, value):
        pass

    @abc.abstractmethod
    def delete_server(self, server_id, timeout):
        pass

    @abc.abstractmethod
    def set_volume_image_metadata(self, volume_id, metadata):
        pass

    @abc.abstractmethod
    def get_security_group_name(self, group_id):
        pass

    @abc.abstractmethod
    def find_security_group(self, name):
        """Id of the security group; missing or ambiguous is an error."""

    @abc.abstractmethod
    def volume_type_usable(self, name):
        pass

    @abc.abstractmethod
    def availability_zone_exists(self, name):
        pass

    @abc.abstractmethod
    def create_transfer(self, volume_id, name, on_created=None):
        """Return (transfer id, auth key); the key is a secret."""

    @abc.abstractmethod
    def accept_transfer(self, transfer_id, auth_key, timeout):
        pass

    @abc.abstractmethod
    def delete_transfer(self, transfer_id):
        """Delete a transfer; missing is success."""

    @abc.abstractmethod
    def list_transfers(self, name_prefix):
        pass

    def volume_limits(self):
        """Remaining volume quota by Cinder name, or None when it cannot be read."""
        return None

    @abc.abstractmethod
    def find_consistency_group(self, volume_ids):
        """Id of the group holding exactly these volumes, with consistent snapshots."""

    @abc.abstractmethod
    def create_group_snapshot(
        self, group_id, name, description, timeout, on_created=None
    ):
        """Return {volume id: member snapshot id}."""

    @abc.abstractmethod
    def delete_group_snapshot(self, group_snapshot_id, timeout):
        pass

    @abc.abstractmethod
    def list_group_snapshots(self, name_prefix):
        pass


def create_backend(config, proxy=True):
    """Build the backend named by config["backend"]; never connects.

    With proxy=False the backend resolves no proxy and refuses attach calls.
    """
    config = dict(config)
    if not proxy:
        config["proxy"] = False
    name = config.get("backend", "sdk")
    if name == "fake":
        try:
            from . import fake_backend
        except ImportError as exc:
            raise CloudConfigError("the fake backend is not installed") from exc
        return fake_backend.FakeBackend(config)
    if name == "sdk":
        from . import sdk_backend

        return sdk_backend.SdkBackend(config)
    raise CloudConfigError("unknown backend %r (use sdk or fake)" % (name,))
