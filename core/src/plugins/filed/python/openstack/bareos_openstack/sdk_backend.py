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

"""Cloud backend on top of openstacksdk (imported lazily by connect())."""

import contextlib
import importlib
import json
import logging
import time
import urllib.request

from . import devices
from .cloud import (
    BOOT_PROPERTY_KEYS,
    GIB,
    CloudBackend,
    CloudCancelled,
    CloudConfigError,
    CloudConflict,
    CloudError,
    CloudNotFound,
    CloudTimeout,
    CloudTransferAccepted,
    FlavorInfo,
    GroupSnapshotInfo,
    PortInfo,
    ServerInfo,
    SnapshotInfo,
    TransferInfo,
    VolumeInfo,
)

GROUP_MICROVERSION = (3, 14)

SECRET_KEYS = ("password", "application_credential_secret")
METADATA_URL = "http://169.254.169.254/openstack/latest/meta_data.json"
INSTANCE_ID_FILE = "/var/lib/cloud/data/instance-id"
METADATA_TIMEOUT = 5
LOG = logging.getLogger(__name__)
MAX_MESSAGE_TEXT = 300


def _get(obj, key, default=None):
    """Read `key` from a dict or an attribute object."""
    if obj is None:
        return default
    if isinstance(obj, dict):
        value = obj.get(key, default)
    else:
        value = getattr(obj, key, default)
    return default if value is None else value


def _truthy(value):
    if isinstance(value, str):
        return value.strip().lower() in ("true", "yes", "1")
    return bool(value)


def _to_int(value):
    """Return a non-negative integer; an unusable value counts as 0."""
    try:
        return max(0, int(value))
    except (TypeError, ValueError):
        return 0


def _matches(metadata, wanted):
    metadata = metadata or {}
    return all(str(metadata.get(k)) == str(v) for k, v in wanted.items())


def _image_id(image):
    if not image:
        return ""
    if isinstance(image, str):
        return image
    return str(_get(image, "id", "") or "")


class SdkBackend(CloudBackend):
    """OpenStack cloud reached through openstacksdk."""

    POLL_INTERVAL = 2

    def __init__(self, config):
        super().__init__()
        self._config = dict(config)
        self._conn = None
        self._exceptions = None
        self._no_proxy = self._config.get("proxy") is False
        # values masked in error texts, such as a transfer auth key
        self._secrets = set()

    # Errors and waits -------------------------------------------------------

    def _sanitize(self, text):
        for key in SECRET_KEYS:
            secret = self._config.get(key) or ""
            if secret:
                text = text.replace(str(secret), "***")
        for secret in self._secrets:
            text = text.replace(secret, "***")
        return text[:MAX_MESSAGE_TEXT]

    def _map_error(self, operation, exc):
        text = self._sanitize(str(exc))
        message = "%s failed: %s: %s" % (operation, type(exc).__name__, text)
        mod = self._exceptions

        def is_a(name):
            cls = getattr(mod, name, None)
            return isinstance(cls, type) and isinstance(exc, cls)

        if is_a("NotFoundException"):
            return CloudNotFound(message)
        if is_a("ResourceTimeout"):
            return CloudTimeout(message)
        if is_a("ConflictException") or is_a("BadRequestException"):
            return CloudConflict(message)
        return CloudError(message)

    def _call(self, operation, fn, *args, **kwargs):
        try:
            return fn(*args, **kwargs)
        except CloudError:
            raise
        except Exception as exc:  # pylint: disable=broad-except
            raise self._map_error(operation, exc) from None

    def _wait(self, operation, fetch, ok_statuses, fail_statuses, timeout):
        ok = [s.lower() for s in ok_statuses]
        bad = [s.lower() for s in fail_statuses]
        deadline = time.monotonic() + timeout
        while True:
            resource = self._call(operation, fetch)
            status = str(_get(resource, "status", "") or "")
            if status.lower() in ok:
                return resource
            if status.lower() in bad:
                raise CloudError(
                    "%s failed: resource went to status %s" % (operation, status)
                )
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise CloudTimeout(
                    "%s timed out after %d seconds (status %s)"
                    % (operation, timeout, status or "unknown")
                )
            self.sleep(min(self.POLL_INTERVAL, remaining))

    def _wait_gone(self, operation, fetch, timeout):
        deadline = time.monotonic() + timeout
        while True:
            try:
                self._call(operation, fetch)
            except CloudNotFound:
                return
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise CloudTimeout(
                    "%s timed out after %d seconds" % (operation, timeout)
                )
            self.sleep(min(self.POLL_INTERVAL, remaining))

    # Connection -------------------------------------------------------------

    def connect(self):
        cfg = self._config
        if not cfg.get("proxy_instance_id") and not self._no_proxy:
            raise CloudConfigError("proxy_instance_id is required for backend=sdk")
        try:
            import openstack  # pylint: disable=import-outside-toplevel

            if not callable(getattr(openstack, "connect", None)):
                raise ImportError("module openstack has no connect")
            self._exceptions = getattr(openstack, "exceptions", None)
            if self._exceptions is None:
                self._exceptions = importlib.import_module("openstack.exceptions")
        except Exception as exc:  # pylint: disable=broad-except
            raise CloudConfigError(
                "the openstack python module is not available; "
                "install python3-openstacksdk"
            ) from exc

        kwargs = {
            "app_name": "bareos-fd-openstack",
            "api_timeout": 60,
            "verify": cfg.get("cacert") or bool(cfg.get("verify_ssl", True)),
        }
        if cfg.get("region_name"):
            kwargs["region_name"] = cfg["region_name"]
        if cfg.get("cloud"):
            kwargs["cloud"] = cfg["cloud"]
            # an empty value drops the other key of the clouds.yaml entry
            if cfg.get("project_id"):
                kwargs["project_id"] = cfg["project_id"]
                kwargs["project_name"] = ""
            elif cfg.get("project_name"):
                kwargs["project_name"] = cfg["project_name"]
                kwargs["project_id"] = ""
            if cfg.get("project_domain_name"):
                kwargs["project_domain_name"] = cfg["project_domain_name"]
        elif cfg.get("application_credential_id"):
            kwargs["auth_type"] = "v3applicationcredential"
            kwargs["auth"] = {
                "auth_url": cfg.get("auth_url", ""),
                "application_credential_id": cfg["application_credential_id"],
                "application_credential_secret": cfg.get(
                    "application_credential_secret", ""
                ),
            }
        elif cfg.get("username"):
            auth = {
                "auth_url": cfg.get("auth_url", ""),
                "username": cfg["username"],
                "password": cfg.get("password", ""),
            }
            if cfg.get("project_id"):
                auth["project_id"] = cfg["project_id"]
            elif cfg.get("project_name"):
                auth["project_name"] = cfg["project_name"]
            for key in ("user_domain_name", "project_domain_name"):
                if cfg.get(key):
                    auth[key] = cfg[key]
            kwargs["auth"] = auth
        else:
            raise CloudConfigError(
                "no OpenStack credentials: set cloud, application_credential_id or "
                "username"
            )

        def make():
            conn = openstack.connect(**kwargs)
            authorize = getattr(conn, "authorize", None)
            if callable(authorize):
                authorize()
            return conn

        self._conn = self._call("connecting to OpenStack", make)
        if cfg.get("proxy_instance_id") == "auto" and not self._no_proxy:
            self._resolve_proxy()

    @staticmethod
    def _read_metadata_service():
        with urllib.request.urlopen(METADATA_URL, timeout=METADATA_TIMEOUT) as resp:
            return str(json.loads(resp.read().decode("utf-8")).get("uuid") or "")

    @staticmethod
    def _read_instance_id_file():
        with open(INSTANCE_ID_FILE, "r", encoding="utf-8") as fh:
            return fh.read().strip()

    def _resolve_proxy(self):
        """Replace proxy_instance_id=auto by the id of the server we run on."""
        tried = []
        sources = (
            ("the metadata service", self._read_metadata_service),
            ("the cloud-init instance-id file", self._read_instance_id_file),
        )
        for source, reader in sources:
            try:
                value = reader()
            except Exception as exc:  # pylint: disable=broad-except
                tried.append("%s: %s" % (source, self._sanitize(str(exc))))
                continue
            if not value:
                tried.append("%s: no instance id" % source)
                continue
            try:
                srv = self._call(
                    "reading server %s" % value, self._conn.compute.get_server, value
                )
            except CloudError as exc:
                tried.append("%s gave %s: %s" % (source, value, exc))
                continue
            project = self.project_id
            owner = _get(srv, "project_id", "")
            if project and owner and project != owner:
                tried.append(
                    "%s gave %s, a server of project %s, not of the connected "
                    "project" % (source, value, owner)
                )
                continue
            self._config["proxy_instance_id"] = value
            LOG.info("proxy_instance_id=auto resolved to %s from %s", value, source)
            return
        raise CloudConfigError(
            "proxy_instance_id=auto found no proxy server (%s); set "
            "proxy_instance_id explicitly" % "; ".join(tried)
        )

    def close(self):
        conn, self._conn = self._conn, None
        closer = getattr(conn, "close", None)
        if callable(closer):
            try:
                closer()
            except Exception:  # pylint: disable=broad-except
                pass

    @property
    def project_id(self):
        try:
            return str(self._conn.current_project_id or "")
        except Exception:  # pylint: disable=broad-except
            return ""

    @property
    def proxy_instance_id(self):
        return self._config.get("proxy_instance_id", "")

    def _require_proxy(self):
        if self._no_proxy:
            raise CloudConfigError("this connection has no proxy server")

    # Mapping ----------------------------------------------------------------

    @staticmethod
    def _flavor_dict(flavor):
        info = {
            "original_name": _get(flavor, "original_name", "")
            or _get(flavor, "name", ""),
            "vcpus": _get(flavor, "vcpus", 0),
            "ram": _get(flavor, "ram", 0),
            "disk": _get(flavor, "disk", 0),
            "extra_specs": dict(_get(flavor, "extra_specs", {}) or {}),
        }
        for key in ("ephemeral", "swap"):
            value = _get(flavor, key, None)
            if value is not None:
                info[key] = _to_int(value)
        return info

    def _server_info(self, srv):
        groups = []
        for group in _get(srv, "security_groups", []) or []:
            name = _get(group, "name", "")
            if name:
                groups.append(name)
        return ServerInfo(
            id=_get(srv, "id", ""),
            name=_get(srv, "name", ""),
            project_id=_get(srv, "project_id", ""),
            status=_get(srv, "status", ""),
            flavor=self._flavor_dict(_get(srv, "flavor", None)),
            image_id=_image_id(_get(srv, "image", None)),
            key_name=_get(srv, "key_name", "") or "",
            availability_zone=_get(srv, "availability_zone", "") or "",
            metadata=dict(_get(srv, "metadata", {}) or {}),
            tags=list(_get(srv, "tags", []) or []),
            security_groups=groups,
        )

    @staticmethod
    def _volume_info(vol):
        encrypted = _get(vol, "is_encrypted", None)
        if encrypted is None:
            encrypted = _get(vol, "encrypted", False)
        attached = []
        for att in _get(vol, "attachments", []) or []:
            server_id = _get(att, "server_id", "")
            if server_id:
                attached.append(server_id)
        return VolumeInfo(
            id=_get(vol, "id", ""),
            name=_get(vol, "name", "") or "",
            size_gb=int(_get(vol, "size", 0) or 0),
            volume_type=_get(vol, "volume_type", "") or "",
            bootable=_truthy(_get(vol, "is_bootable", False)),
            encrypted=_truthy(encrypted),
            multiattach=_truthy(_get(vol, "is_multiattach", False)),
            status=_get(vol, "status", "") or "",
            availability_zone=_get(vol, "availability_zone", "") or "",
            metadata=dict(_get(vol, "metadata", {}) or {}),
            attached_server_ids=attached,
            image_metadata=SdkBackend._boot_properties(
                _get(vol, "volume_image_metadata", {})
            ),
        )

    @staticmethod
    def _boot_properties(properties):
        if not isinstance(properties, dict):
            return {}
        return {
            key: str(properties[key])
            for key in BOOT_PROPERTY_KEYS
            if properties.get(key) is not None
        }

    @staticmethod
    def _snapshot_info(snap):
        return SnapshotInfo(
            id=_get(snap, "id", ""),
            volume_id=_get(snap, "volume_id", ""),
            name=_get(snap, "name", "") or "",
            status=_get(snap, "status", "") or "",
            size_gb=int(_get(snap, "size", 0) or 0),
            metadata=dict(_get(snap, "metadata", {}) or {}),
        )

    @staticmethod
    def _port_info(port):
        ips = [
            {
                "subnet_id": _get(ip, "subnet_id", ""),
                "ip_address": _get(ip, "ip_address", ""),
            }
            for ip in _get(port, "fixed_ips", []) or []
        ]
        return PortInfo(
            id=_get(port, "id", ""),
            network_id=_get(port, "network_id", ""),
            mac_address=_get(port, "mac_address", "") or "",
            fixed_ips=ips,
            security_group_ids=list(_get(port, "security_group_ids", []) or []),
            name=_get(port, "name", "") or "",
            device_id=_get(port, "device_id", "") or "",
        )

    # Queries ----------------------------------------------------------------

    def get_server(self, server_id):
        srv = self._call(
            "reading server %s" % server_id, self._conn.compute.get_server, server_id
        )
        return self._server_info(srv)

    def find_server(self, name):
        project = self._scope("server %s" % name)
        found = self._call(
            "listing servers",
            lambda: list(self._conn.compute.servers(details=True, name=name)),
        )
        found = self._in_project(
            (s for s in found if _get(s, "name", "") == name), project
        )
        if not found:
            raise CloudNotFound("no server named %s" % name)
        if len(found) > 1:
            raise CloudConflict(
                "%d servers are named %s; use server_id" % (len(found), name)
            )
        return self._server_info(found[0])

    def list_servers(self):
        found = self._call(
            "listing servers",
            lambda: list(self._conn.compute.servers(details=True)),
        )
        proxy = self.proxy_instance_id
        return [self._server_info(s) for s in found if _get(s, "id", "") != proxy]

    def list_server_volumes(self, server_id):
        conn = self._conn
        srv = self._call(
            "reading server %s" % server_id, conn.compute.get_server, server_id
        )
        attachments = self._call(
            "listing volume attachments",
            lambda: list(conn.compute.volume_attachments(server_id)),
        )
        infos = []
        for att in attachments:
            volume_id = _get(att, "volume_id", "")
            vol = self._call(
                "reading volume %s" % volume_id,
                conn.block_storage.get_volume,
                volume_id,
            )
            info = self._volume_info(vol)
            info.device = _get(att, "device", "") or ""
            info.delete_on_termination = bool(_get(att, "delete_on_termination", False))
            infos.append(info)
        boot = None
        root = _get(srv, "root_device_name", "") or ""
        if root:
            boot = next((i for i in infos if i.device == root), None)
        from_volume = not _image_id(_get(srv, "image", None))
        if boot is None and from_volume:
            boot = next((i for i in infos if i.device.endswith("da")), None)
        if boot is None and from_volume:
            bootable = sorted((i for i in infos if i.bootable), key=lambda i: i.device)
            boot = bootable[0] if bootable else None
        if boot is not None:
            boot.boot_index = 0
        return infos

    def list_server_ports(self, server_id):
        ports = self._call(
            "listing ports",
            lambda: list(self._conn.network.ports(device_id=server_id)),
        )
        return [self._port_info(p) for p in ports]

    def get_volume(self, volume_id):
        vol = self._call(
            "reading volume %s" % volume_id,
            self._conn.block_storage.get_volume,
            volume_id,
        )
        return self._volume_info(vol)

    def list_volumes(self, metadata_filter):
        vols = self._call(
            "listing volumes",
            lambda: list(self._conn.block_storage.volumes(details=True)),
        )
        return [
            self._volume_info(v)
            for v in vols
            if _matches(_get(v, "metadata", {}), metadata_filter)
        ]

    def list_snapshots(self, metadata_filter):
        snaps = self._call(
            "listing snapshots",
            lambda: list(self._conn.block_storage.snapshots(details=True)),
        )
        return [
            self._snapshot_info(s)
            for s in snaps
            if _matches(_get(s, "metadata", {}), metadata_filter)
        ]

    def list_proxy_attachments(self):
        if self._no_proxy:
            return []
        proxy = self.proxy_instance_id
        atts = self._call(
            "listing proxy attachments",
            lambda: list(self._conn.compute.volume_attachments(proxy)),
        )
        return [_get(a, "volume_id", "") for a in atts]

    def get_port(self, port_id):
        try:
            port = self._call(
                "reading port %s" % port_id, self._conn.network.get_port, port_id
            )
        except CloudNotFound:
            return None
        return self._port_info(port)

    def find_flavor(self, name_or_id):
        flavor = self._call(
            "finding flavor %s" % name_or_id,
            self._conn.compute.find_flavor,
            name_or_id,
            ignore_missing=False,
            get_extra_specs=True,
        )
        return FlavorInfo(
            id=_get(flavor, "id", ""),
            name=_get(flavor, "name", ""),
            vcpus=int(_get(flavor, "vcpus", 0) or 0),
            ram=int(_get(flavor, "ram", 0) or 0),
            disk=int(_get(flavor, "disk", 0) or 0),
            extra_specs=dict(_get(flavor, "extra_specs", {}) or {}),
        )

    def network_exists(self, network_id):
        """True for an id, or for a name that is unambiguous in the project.

        A name is looked up in the connected project first, then among the
        shared networks; two matches inside the chosen set are a conflict.
        """
        net = self._conn.network
        try:
            found = self._call(
                "reading network %s" % network_id, net.get_network, network_id
            )
        except CloudNotFound:
            found = None
        if found is not None:
            return True
        project = self._scope("network %s" % network_id)
        named = self._call(
            "listing networks",
            lambda: list(net.networks(name=network_id)),
        )
        named = [n for n in named if _get(n, "name", "") == network_id]
        own = self._in_project(named, project)
        shared = [
            n
            for n in named
            if _get(n, "is_shared", None) is True or _get(n, "shared", None) is True
        ]
        for group in (own, shared):
            if len(group) > 1:
                raise CloudConflict(
                    "%d networks are named %s; use the network id"
                    % (len(group), network_id)
                )
            if group:
                return True
        return False

    # Snapshots and volumes --------------------------------------------------

    def create_snapshot(
        self, volume_id, name, metadata, force, timeout, on_created=None
    ):
        bs = self._conn.block_storage
        snap = self._call(
            "creating snapshot of volume %s" % volume_id,
            bs.create_snapshot,
            volume_id=volume_id,
            name=name,
            metadata=dict(metadata),
            force=force,
        )
        snap_id = _get(snap, "id", "")
        if on_created is not None:
            on_created(snap_id)
        done = self._wait(
            "waiting for snapshot %s" % snap_id,
            lambda: bs.get_snapshot(snap_id),
            ["available"],
            ["error"],
            timeout,
        )
        return self._snapshot_info(done)

    def delete_snapshot(self, snapshot_id, timeout):
        bs = self._conn.block_storage
        self._call(
            "deleting snapshot %s" % snapshot_id,
            bs.delete_snapshot,
            snapshot_id,
            ignore_missing=True,
        )
        self._wait_gone(
            "waiting for snapshot %s to disappear" % snapshot_id,
            lambda: bs.get_snapshot(snapshot_id),
            timeout,
        )

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
        bs = self._conn.block_storage
        attrs = {"name": name, "size": size_gb, "metadata": dict(metadata)}
        if volume_type:
            attrs["volume_type"] = volume_type
        if availability_zone:
            attrs["availability_zone"] = availability_zone
        if snapshot_id:
            attrs["snapshot_id"] = snapshot_id
        vol = self._call("creating volume %s" % name, bs.create_volume, **attrs)
        vol_id = _get(vol, "id", "")
        if on_created is not None:
            on_created(vol_id)
        done = self._wait(
            "waiting for volume %s" % vol_id,
            lambda: bs.get_volume(vol_id),
            ["available"],
            ["error"],
            timeout,
        )
        return self._volume_info(done)

    def delete_volume(self, volume_id, timeout):
        bs = self._conn.block_storage
        try:
            vol = self._call("reading volume %s" % volume_id, bs.get_volume, volume_id)
        except CloudNotFound:
            return
        if str(_get(vol, "status", "")).lower() == "in-use":
            raise CloudConflict("volume %s is in use" % volume_id)
        self._call("deleting volume %s" % volume_id, bs.delete_volume, volume_id)
        self._wait_gone(
            "waiting for volume %s to disappear" % volume_id,
            lambda: bs.get_volume(volume_id),
            timeout,
        )

    def attach_to_proxy(self, volume_id, timeout):
        self._require_proxy()
        conn = self._conn
        proxy = self.proxy_instance_id
        vol = self._call(
            "reading volume %s" % volume_id, conn.block_storage.get_volume, volume_id
        )
        attached = [
            _get(a, "server_id", "") for a in _get(vol, "attachments", []) or []
        ]
        if proxy not in attached:
            self._call(
                "attaching volume %s to the proxy" % volume_id,
                conn.compute.create_volume_attachment,
                proxy,
                volume=volume_id,
            )
            self._wait(
                "waiting for volume %s to attach" % volume_id,
                lambda: conn.block_storage.get_volume(volume_id),
                ["in-use"],
                ["error", "error_attaching"],
                timeout,
            )
        size_gb = int(_get(vol, "size", 0) or 0)
        return devices.find_volume_device(
            volume_id,
            timeout,
            expected_min_size=size_gb * GIB,
            cancel_check=self.check_cancel,
        )

    def detach_from_proxy(self, volume_id, timeout):
        self._require_proxy()
        conn = self._conn
        proxy = self.proxy_instance_id
        try:
            vol = self._call(
                "reading volume %s" % volume_id,
                conn.block_storage.get_volume,
                volume_id,
            )
        except CloudNotFound:
            return
        attached = [
            _get(a, "server_id", "") for a in _get(vol, "attachments", []) or []
        ]
        if proxy not in attached:
            return
        self._call(
            "detaching volume %s from the proxy" % volume_id,
            conn.compute.delete_volume_attachment,
            proxy,
            volume_id,
        )
        self._wait(
            "waiting for volume %s to detach" % volume_id,
            lambda: conn.block_storage.get_volume(volume_id),
            ["available"],
            ["error", "error_detaching"],
            timeout,
        )

    def set_volume_bootable(self, volume_id, bootable):
        self._call(
            "setting bootable flag of volume %s" % volume_id,
            self._conn.block_storage.set_volume_bootable_status,
            volume_id,
            bootable,
        )

    def _volume_action(self, operation, volume_id, body):
        """POST volumes/<id>/action through the block-storage proxy."""
        bs = self._conn.block_storage

        def post():
            response = bs.post(
                "/volumes/%s/action" % volume_id, json=body, raise_exc=False
            )
            self._exceptions.raise_from_response(response)

        self._call(operation, post)

    def set_volume_image_metadata(self, volume_id, metadata):
        values = {str(k): str(v) for k, v in dict(metadata).items()}
        operation = "setting image properties of volume %s" % volume_id
        method = getattr(self._conn.block_storage, "set_volume_image_metadata", None)
        if callable(method):
            self._call(operation, method, volume_id, **values)
        else:
            self._volume_action(
                operation, volume_id, {"os-set_image_metadata": {"metadata": values}}
            )

    # Project resources and transfers ------------------------------------------

    def get_security_group_name(self, group_id):
        group = self._call(
            "reading security group %s" % group_id,
            self._conn.network.get_security_group,
            group_id,
        )
        return str(_get(group, "name", "") or "")

    @staticmethod
    def _owner(item):
        return str(_get(item, "project_id", "") or _get(item, "tenant_id", "") or "")

    def _scope(self, what):
        """The connected project; a name lookup without it is refused."""
        project = self.project_id
        if not project:
            raise CloudError(
                "cannot look up %s by name: the connected project is unknown" % what
            )
        return project

    def _in_project(self, items, project):
        """Keep the objects whose owner is the project; unknown owners are dropped."""
        return [i for i in items if self._owner(i) == project]

    def find_security_group(self, name):
        project = self._scope("security group %s" % name)
        found = self._call(
            "listing security groups",
            lambda: list(
                self._conn.network.security_groups(name=name, project_id=project)
            ),
        )
        found = self._in_project(
            (g for g in found if _get(g, "name", "") == name), project
        )
        if not found:
            raise CloudNotFound("no security group named %s" % name)
        if len(found) > 1:
            raise CloudConflict("%d security groups are named %s" % (len(found), name))
        return str(_get(found[0], "id", ""))

    def volume_type_usable(self, name):
        found = self._call(
            "finding volume type %s" % name,
            self._conn.block_storage.find_type,
            name,
            ignore_missing=True,
        )
        return found is not None

    def availability_zone_exists(self, name):
        zones = self._call(
            "listing volume availability zones",
            lambda: list(self._conn.block_storage.availability_zones()),
        )
        return any(
            name in (_get(z, "name", ""), _get(z, "zoneName", "")) for z in zones
        )

    def create_transfer(self, volume_id, name, on_created=None):
        transfer = self._call(
            "creating a transfer of volume %s" % volume_id,
            self._conn.block_storage.create_transfer,
            volume_id=volume_id,
            name=name,
        )
        key = str(_get(transfer, "auth_key", "") or "")
        if key:
            self._secrets.add(key)
        transfer_id = str(_get(transfer, "id", "") or "")
        if on_created is not None:
            on_created(transfer_id)
        return transfer_id, key

    def accept_transfer(self, transfer_id, auth_key, timeout):
        bs = self._conn.block_storage
        if auth_key:
            self._secrets.add(str(auth_key))
        accepted = self._call(
            "accepting transfer %s" % transfer_id,
            bs.accept_transfer,
            transfer_id,
            auth_key=auth_key,
        )
        volume_id = str(_get(accepted, "volume_id", "") or "")
        if volume_id:
            try:
                self._wait(
                    "waiting for volume %s after the transfer" % volume_id,
                    lambda: bs.get_volume(volume_id),
                    ["available"],
                    ["error"],
                    timeout,
                )
            except CloudCancelled:
                raise
            except CloudError as exc:
                raise CloudTransferAccepted(
                    "transfer %s was accepted but volume %s is not ready: %s"
                    % (transfer_id, volume_id, exc)
                ) from None

    def delete_transfer(self, transfer_id):
        self._call(
            "deleting transfer %s" % transfer_id,
            self._conn.block_storage.delete_transfer,
            transfer_id,
            ignore_missing=True,
        )

    def list_transfers(self, name_prefix):
        found = self._call(
            "listing transfers",
            lambda: list(self._conn.block_storage.transfers()),
        )
        return [
            TransferInfo(
                str(_get(t, "id", "")),
                str(_get(t, "name", "") or ""),
                str(_get(t, "volume_id", "") or ""),
            )
            for t in found
            if str(_get(t, "name", "") or "").startswith(name_prefix)
        ]

    # Volume groups ----------------------------------------------------------

    @contextlib.contextmanager
    def _groups(self):
        """The block-storage proxy at microversion 3.14, or CloudConfigError."""
        bs = self._conn.block_storage
        data = self._call("reading the block-storage versions", bs.get_endpoint_data)
        top = _get(data, "max_microversion", None)
        offered = None
        if isinstance(top, (tuple, list)) and len(top) >= 2:
            offered = (int(top[0]), int(top[1]))
        elif isinstance(top, str) and "." in top:
            major, _, minor = top.partition(".")
            if major.isdigit() and minor.isdigit():
                offered = (int(major), int(minor))
        if offered is None or offered < GROUP_MICROVERSION:
            raise CloudConfigError(
                "volume groups need block-storage microversion 3.14 or later; "
                "the cloud offers %s" % (str(top) if top else "no microversions")
            )
        previous = getattr(bs, "default_microversion", None)
        bs.default_microversion = "3.14"
        try:
            yield bs
        finally:
            bs.default_microversion = previous

    @staticmethod
    def _group_consistent(spec_value):
        return "true" in str(spec_value).lower()

    def find_consistency_group(self, volume_ids):
        need = set(volume_ids)
        with self._groups() as bs:
            groups = self._call(
                "listing volume groups", lambda: list(bs.groups(details=True))
            )
            volumes = self._call(
                "listing volumes", lambda: list(bs.volumes(details=True))
            )
            members = {}
            for vol in volumes:
                gid = _get(vol, "group_id", "")
                if gid:
                    members.setdefault(gid, set()).add(_get(vol, "id", ""))
            holders = [
                g for g in groups if need & members.get(_get(g, "id", ""), set())
            ]
            held = set()
            for g in holders:
                held |= members.get(_get(g, "id", ""), set())
            loose = sorted(need - held)
            if loose:
                raise CloudConflict(
                    "volume(s) %s are in no volume group" % ", ".join(loose)
                )
            if len(holders) > 1:
                raise CloudConflict(
                    "the volumes are spread over several groups: %s"
                    % ", ".join(sorted(_get(g, "id", "") for g in holders))
                )
            group = holders[0]
            gid = _get(group, "id", "")
            gtype = self._call(
                "reading group type",
                bs.get_group_type,
                _get(group, "group_type", ""),
            )
            specs = _get(gtype, "group_specs", {}) or {}
            if not self._group_consistent(
                specs.get("consistent_group_snapshot_enabled", "")
            ):
                raise CloudConflict(
                    "group %s: its group type does not enable consistent group "
                    "snapshots" % gid
                )
            have = members.get(gid, set())
            if have != need:
                raise CloudConflict(
                    "group %s does not hold exactly the volumes of the server "
                    "(extra: %s, missing: %s)"
                    % (
                        gid,
                        ", ".join(sorted(have - need)) or "-",
                        ", ".join(sorted(need - have)) or "-",
                    )
                )
            return gid

    def create_group_snapshot(
        self, group_id, name, description, timeout, on_created=None
    ):
        with self._groups() as bs:
            gsnap = self._call(
                "creating a snapshot of group %s" % group_id,
                bs.create_group_snapshot,
                group_id=group_id,
                name=name,
                description=description,
            )
            gs_id = str(_get(gsnap, "id", "") or "")
            if on_created is not None:
                on_created(gs_id)
            self._wait(
                "waiting for group snapshot %s" % gs_id,
                lambda: bs.get_group_snapshot(gs_id),
                ["available"],
                ["error"],
                timeout,
            )
            snaps = self._call(
                "listing snapshots",
                lambda: list(bs.snapshots(details=True)),
            )
        return {
            str(_get(s, "volume_id", "")): str(_get(s, "id", ""))
            for s in snaps
            if _get(s, "group_snapshot_id", "") == gs_id
        }

    def delete_group_snapshot(self, group_snapshot_id, timeout):
        with self._groups() as bs:
            self._call(
                "deleting group snapshot %s" % group_snapshot_id,
                bs.delete_group_snapshot,
                group_snapshot_id,
                ignore_missing=True,
            )
            self._wait_gone(
                "waiting for group snapshot %s to disappear" % group_snapshot_id,
                lambda: bs.get_group_snapshot(group_snapshot_id),
                timeout,
            )

    def list_group_snapshots(self, name_prefix):
        with self._groups() as bs:
            found = self._call(
                "listing group snapshots",
                lambda: list(bs.group_snapshots(details=True)),
            )
        return [
            GroupSnapshotInfo(
                id=str(_get(g, "id", "")),
                name=str(_get(g, "name", "") or ""),
                description=str(_get(g, "description", "") or ""),
                group_id=str(_get(g, "group_id", "") or ""),
                status=str(_get(g, "status", "") or ""),
            )
            for g in found
            if str(_get(g, "name", "") or "").startswith(name_prefix)
        ]

    # Ports ------------------------------------------------------------------

    def create_port(
        self,
        network_id,
        name,
        security_group_ids,
        fixed_ips=None,
        mac_address="",
        tags=None,
    ):
        attrs = {
            "network_id": network_id,
            "name": name,
            "security_group_ids": list(security_group_ids or []),
        }
        if fixed_ips:
            attrs["fixed_ips"] = [dict(ip) for ip in fixed_ips]
        if mac_address:
            attrs["mac_address"] = mac_address
        port = self._call(
            "creating port %s" % name, self._conn.network.create_port, **attrs
        )
        if tags:
            try:
                self._call(
                    "tagging port %s" % _get(port, "id", ""),
                    self._conn.network.set_tags,
                    port,
                    list(tags),
                )
            except CloudCancelled:
                raise
            except CloudError as exc:
                LOG.warning("%s", exc)
        return self._port_info(port)

    def delete_port(self, port_id):
        self._call(
            "deleting port %s" % port_id,
            self._conn.network.delete_port,
            port_id,
            ignore_missing=True,
        )

    # Servers ----------------------------------------------------------------

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
        compute = self._conn.compute
        attrs = {
            "name": name,
            "flavor_id": flavor_id,
            "metadata": dict(metadata),
            "block_device_mapping": [
                {
                    "uuid": dev["volume_id"],
                    "boot_index": (
                        dev["boot_index"] if dev.get("boot_index") is not None else -1
                    ),
                    "source_type": "volume",
                    "destination_type": "volume",
                    "delete_on_termination": False,
                }
                for dev in block_devices
            ],
            "networks": [{"port": p} for p in port_ids] if port_ids else "none",
        }
        if key_name:
            attrs["key_name"] = key_name
        if availability_zone:
            attrs["availability_zone"] = availability_zone
        srv = self._call("creating server %s" % name, compute.create_server, **attrs)
        srv_id = _get(srv, "id", "")
        if on_created is not None:
            on_created(srv_id)
        self._wait(
            "waiting for server %s" % srv_id,
            lambda: compute.get_server(srv_id),
            ["ACTIVE"],
            ["ERROR"],
            timeout,
        )
        return self.get_server(srv_id)

    def stop_server(self, server_id, timeout):
        compute = self._conn.compute
        self._call("stopping server %s" % server_id, compute.stop_server, server_id)
        self._wait(
            "waiting for server %s to stop" % server_id,
            lambda: compute.get_server(server_id),
            ["SHUTOFF"],
            ["ERROR"],
            timeout,
        )

    def start_server(self, server_id, timeout):
        compute = self._conn.compute
        self._call("starting server %s" % server_id, compute.start_server, server_id)
        self._wait(
            "waiting for server %s to start" % server_id,
            lambda: compute.get_server(server_id),
            ["ACTIVE"],
            ["ERROR"],
            timeout,
        )

    def set_delete_on_termination(self, server_id, volume_id, value):
        self._call(
            "updating attachment of volume %s" % volume_id,
            self._conn.compute.update_volume_attachment,
            server_id,
            volume_id,
            delete_on_termination=bool(value),
        )

    def delete_server(self, server_id, timeout):
        compute = self._conn.compute
        try:
            self._call("reading server %s" % server_id, compute.get_server, server_id)
        except CloudNotFound:
            return
        self._call("deleting server %s" % server_id, compute.delete_server, server_id)
        self._wait_gone(
            "waiting for server %s to disappear" % server_id,
            lambda: compute.get_server(server_id),
            timeout,
        )
