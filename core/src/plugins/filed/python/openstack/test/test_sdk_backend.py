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

"""Unit tests of the SDK backend against a mocked openstack module."""

import importlib
import json
import os
import sys
import tempfile
import threading
import types
import unittest
from unittest import mock

from bareos_openstack import devices, sdk_backend
from bareos_openstack.cloud import (
    CloudCancelled,
    CloudConfigError,
    CloudConflict,
    CloudError,
    CloudNotFound,
    CloudTimeout,
    create_backend,
)

PROXY = "proxy-1"
SECRET = "s3cr3t-value"


class SdkException(Exception):
    pass


class NotFoundException(SdkException):
    pass


class ResourceTimeout(SdkException):
    pass


class ConflictException(SdkException):
    pass


class BadRequestException(SdkException):
    pass


def make_openstack_module(conn):
    module = types.ModuleType("openstack")
    exceptions = types.ModuleType("openstack.exceptions")
    exceptions.SDKException = SdkException
    exceptions.NotFoundException = NotFoundException
    exceptions.ResourceTimeout = ResourceTimeout
    exceptions.ConflictException = ConflictException
    exceptions.BadRequestException = BadRequestException
    module.exceptions = exceptions
    module.connect = mock.Mock(return_value=conn)
    return module, exceptions


def obj(**kw):
    return types.SimpleNamespace(**kw)


def config(**kw):
    cfg = {
        "backend": "sdk",
        "cloud": "prod",
        "proxy_instance_id": PROXY,
        "verify_ssl": True,
        "password": "",
        "application_credential_secret": "",
    }
    cfg.update(kw)
    return cfg


class SdkBackendTest(unittest.TestCase):
    def setUp(self):
        self.conn = mock.MagicMock()
        self.conn.current_project_id = "proj-1"
        self.module, self.exceptions = make_openstack_module(self.conn)
        self._saved = {
            k: sys.modules.get(k) for k in ("openstack", "openstack.exceptions")
        }
        sys.modules["openstack"] = self.module
        sys.modules["openstack.exceptions"] = self.exceptions
        patcher = mock.patch.object(sdk_backend.SdkBackend, "POLL_INTERVAL", 0.01)
        patcher.start()
        self.addCleanup(patcher.stop)

    def tearDown(self):
        for key, value in self._saved.items():
            if value is None:
                sys.modules.pop(key, None)
            else:
                sys.modules[key] = value

    def backend(self, **kw):
        be = sdk_backend.SdkBackend(config(**kw))
        be.connect()
        return be

    # connect ----------------------------------------------------------------

    def test_connect_with_cloud(self):
        be = self.backend(region_name="r1")
        kwargs = self.module.connect.call_args.kwargs
        self.assertEqual(kwargs["cloud"], "prod")
        self.assertEqual(kwargs["region_name"], "r1")
        self.assertEqual(kwargs["app_name"], "bareos-fd-openstack")
        self.assertEqual(kwargs["api_timeout"], 60)
        self.assertIs(kwargs["verify"], True)
        self.assertEqual(be.project_id, "proj-1")
        self.assertEqual(be.proxy_instance_id, PROXY)

    def test_connect_with_application_credential(self):
        self.backend(
            cloud="",
            auth_url="http://keystone/v3",
            application_credential_id="id1",
            application_credential_secret=SECRET,
            cacert="/ca.pem",
        )
        kwargs = self.module.connect.call_args.kwargs
        self.assertEqual(kwargs["auth_type"], "v3applicationcredential")
        self.assertEqual(
            kwargs["auth"],
            {
                "auth_url": "http://keystone/v3",
                "application_credential_id": "id1",
                "application_credential_secret": SECRET,
            },
        )
        self.assertEqual(kwargs["verify"], "/ca.pem")
        self.assertNotIn("cloud", kwargs)

    def test_connect_with_password(self):
        self.backend(
            cloud="",
            auth_url="http://keystone/v3",
            username="u",
            password=SECRET,
            project_name="p",
            user_domain_name="Default",
            project_domain_name="Default",
            verify_ssl=False,
        )
        kwargs = self.module.connect.call_args.kwargs
        self.assertEqual(kwargs["auth"]["username"], "u")
        self.assertEqual(kwargs["auth"]["password"], SECRET)
        self.assertEqual(kwargs["auth"]["project_name"], "p")
        self.assertEqual(kwargs["api_timeout"], 60)
        self.assertIs(kwargs["verify"], False)

    def test_connect_without_credentials(self):
        be = sdk_backend.SdkBackend(config(cloud=""))
        with self.assertRaises(CloudConfigError):
            be.connect()

    def test_missing_proxy(self):
        be = sdk_backend.SdkBackend(config(proxy_instance_id=""))
        with self.assertRaises(CloudConfigError):
            be.connect()

    def test_missing_module(self):
        sys.modules["openstack"] = None
        be = sdk_backend.SdkBackend(config())
        with self.assertRaises(CloudConfigError) as ctx:
            be.connect()
        self.assertIn("python3-openstacksdk", str(ctx.exception))

    def test_create_backend_sdk(self):
        self.assertIsInstance(create_backend(config()), sdk_backend.SdkBackend)

    def test_secret_is_masked_in_connect_error(self):
        self.module.connect.side_effect = SdkException("auth failed for " + SECRET)
        be = sdk_backend.SdkBackend(config(cloud="", username="u", password=SECRET))
        with self.assertRaises(CloudError) as ctx:
            be.connect()
        text = str(ctx.exception)
        self.assertNotIn(SECRET, text)
        self.assertIn("***", text)
        self.assertTrue(text.startswith("connecting to OpenStack failed: SdkException"))
        self.assertIsNone(ctx.exception.__cause__)

    def test_secret_is_masked_and_text_cut(self):
        be = self.backend(password=SECRET, application_credential_secret="other")
        self.conn.block_storage.get_volume.side_effect = SdkException(
            SECRET + " other " + "x" * 500
        )
        with self.assertRaises(CloudError) as ctx:
            be.get_volume("v")
        text = str(ctx.exception)
        self.assertNotIn(SECRET, text)
        self.assertNotIn("other", text)
        self.assertLessEqual(len(text.split(": ", 2)[2]), 300)

    # error mapping -----------------------------------------------------------

    def test_error_mapping(self):
        be = self.backend()
        get = self.conn.block_storage.get_volume
        for exc, expected in (
            (NotFoundException("x"), CloudNotFound),
            (ResourceTimeout("x"), CloudTimeout),
            (ConflictException("x"), CloudConflict),
            (BadRequestException("x"), CloudConflict),
            (SdkException("x"), CloudError),
            (RuntimeError("x"), CloudError),
        ):
            get.side_effect = exc
            with self.assertRaises(expected) as ctx:
                be.get_volume("v")
            self.assertIs(type(ctx.exception), expected)
            self.assertIn("reading volume v failed", str(ctx.exception))

    # snapshots and volumes -----------------------------------------------------

    def test_create_snapshot_arguments(self):
        be = self.backend()
        bs = self.conn.block_storage
        bs.create_snapshot.return_value = obj(id="snap-1")
        bs.get_snapshot.return_value = obj(
            id="snap-1",
            volume_id="v",
            name="n",
            status="available",
            size=2,
            metadata={},
        )
        seen = []
        info = be.create_snapshot("v", "n", {"a": "b"}, True, 5, on_created=seen.append)
        bs.create_snapshot.assert_called_once_with(
            volume_id="v", name="n", metadata={"a": "b"}, force=True
        )
        self.assertEqual(seen, ["snap-1"])
        self.assertEqual((info.id, info.size_gb), ("snap-1", 2))

    def test_create_snapshot_wait_failure_after_on_created(self):
        be = self.backend()
        bs = self.conn.block_storage
        bs.create_snapshot.return_value = obj(id="snap-1")
        bs.get_snapshot.return_value = obj(id="snap-1", status="error")
        seen = []
        with self.assertRaises(CloudError):
            be.create_snapshot("v", "n", {}, True, 5, on_created=seen.append)
        self.assertEqual(seen, ["snap-1"])

    def test_create_snapshot_timeout(self):
        be = self.backend()
        bs = self.conn.block_storage
        bs.create_snapshot.return_value = obj(id="snap-1")
        bs.get_snapshot.return_value = obj(id="snap-1", status="creating")
        with self.assertRaises(CloudTimeout):
            be.create_snapshot("v", "n", {}, True, 0.05)

    def test_create_volume_arguments_and_wait_failure(self):
        be = self.backend()
        bs = self.conn.block_storage
        bs.create_volume.return_value = obj(id="vol-1")
        bs.get_volume.return_value = obj(id="vol-1", status="error")
        seen = []
        with self.assertRaises(CloudError):
            be.create_volume(
                "n",
                3,
                {"a": "b"},
                5,
                volume_type="ssd",
                availability_zone="az",
                snapshot_id="snap-1",
                on_created=seen.append,
            )
        self.assertEqual(seen, ["vol-1"])
        bs.create_volume.assert_called_once_with(
            name="n",
            size=3,
            metadata={"a": "b"},
            volume_type="ssd",
            availability_zone="az",
            snapshot_id="snap-1",
        )
        bs.create_volume.reset_mock()
        bs.get_volume.return_value = obj(id="vol-1", status="available", size=3)
        be.create_volume("n", 3, {}, 5)
        self.assertEqual(
            bs.create_volume.call_args.kwargs, {"name": "n", "size": 3, "metadata": {}}
        )

    def test_delete_volume_rules(self):
        be = self.backend()
        bs = self.conn.block_storage
        bs.get_volume.side_effect = NotFoundException("gone")
        be.delete_volume("v", 5)
        bs.delete_volume.assert_not_called()
        bs.get_volume.side_effect = None
        bs.get_volume.return_value = obj(id="v", status="in-use")
        with self.assertRaises(CloudConflict):
            be.delete_volume("v", 5)
        responses = [obj(id="v", status="available"), NotFoundException("gone")]
        bs.get_volume.side_effect = responses
        be.delete_volume("v", 5)
        bs.delete_volume.assert_called_once_with("v")

    def test_attach_to_proxy(self):
        be = self.backend()
        conn = self.conn
        conn.block_storage.get_volume.side_effect = [
            obj(id="v", status="available", size=2, attachments=[]),
            obj(id="v", status="in-use", size=2, attachments=[{"server_id": PROXY}]),
        ]
        with mock.patch.object(
            devices, "find_volume_device", return_value="/dev/vdz"
        ) as find:
            path = be.attach_to_proxy("v", 5)
        self.assertEqual(path, "/dev/vdz")
        conn.compute.create_volume_attachment.assert_called_once_with(PROXY, volume="v")
        args, kwargs = find.call_args
        self.assertEqual(args, ("v", 5))
        self.assertEqual(kwargs["expected_min_size"], 2 * 1024**3)
        self.assertEqual(kwargs["cancel_check"], be.check_cancel)

    def test_attach_discovery_failure_propagates_after_attachment(self):
        be = self.backend()
        conn = self.conn
        conn.block_storage.get_volume.side_effect = [
            obj(id="v", status="available", size=1, attachments=[]),
            obj(id="v", status="in-use", size=1, attachments=[{"server_id": PROXY}]),
        ]
        with mock.patch.object(
            devices, "find_volume_device", side_effect=CloudTimeout("no device")
        ):
            with self.assertRaises(CloudTimeout):
                be.attach_to_proxy("v", 5)
        conn.compute.create_volume_attachment.assert_called_once()

    def test_attach_already_attached_skips_request(self):
        be = self.backend()
        self.conn.block_storage.get_volume.return_value = obj(
            id="v", status="in-use", size=1, attachments=[{"server_id": PROXY}]
        )
        with mock.patch.object(devices, "find_volume_device", return_value="/d"):
            be.attach_to_proxy("v", 5)
        self.conn.compute.create_volume_attachment.assert_not_called()

    def test_detach_from_proxy(self):
        be = self.backend()
        conn = self.conn
        conn.block_storage.get_volume.side_effect = [
            obj(id="v", status="in-use", attachments=[{"server_id": PROXY}]),
            obj(id="v", status="available", attachments=[]),
        ]
        be.detach_from_proxy("v", 5)
        conn.compute.delete_volume_attachment.assert_called_once_with(PROXY, "v")
        conn.compute.delete_volume_attachment.reset_mock()
        conn.block_storage.get_volume.side_effect = None
        conn.block_storage.get_volume.return_value = obj(
            id="v", status="available", attachments=[]
        )
        be.detach_from_proxy("v", 5)
        conn.compute.delete_volume_attachment.assert_not_called()

    def test_cancel_during_wait(self):
        be = self.backend()
        bs = self.conn.block_storage
        bs.create_snapshot.return_value = obj(id="snap-1")
        bs.get_snapshot.return_value = obj(id="snap-1", status="creating")
        event = threading.Event()
        be.set_cancel_event(event)
        threading.Timer(0.1, event.set).start()
        with self.assertRaises(CloudCancelled):
            be.create_snapshot("v", "n", {}, True, 30)

    # servers ------------------------------------------------------------------

    def test_create_server_builds_mapping(self):
        be = self.backend()
        compute = self.conn.compute
        compute.create_server.return_value = obj(id="srv-1")
        compute.get_server.return_value = obj(
            id="srv-1",
            name="n",
            status="ACTIVE",
            project_id="proj-1",
            image=None,
            flavor={"original_name": "m1", "vcpus": 1},
            metadata={},
            tags=[],
            security_groups=[],
        )
        seen = []
        info = be.create_server(
            "n",
            "f-1",
            [
                {"volume_id": "v1", "boot_index": 0},
                {"volume_id": "v2", "boot_index": None},
            ],
            [],
            "key",
            "az",
            {"m": "1"},
            5,
            on_created=seen.append,
        )
        attrs = compute.create_server.call_args.kwargs
        self.assertEqual(attrs["networks"], "none")
        self.assertEqual(attrs["flavor_id"], "f-1")
        self.assertEqual(attrs["key_name"], "key")
        self.assertEqual(attrs["availability_zone"], "az")
        self.assertEqual(attrs["metadata"], {"m": "1"})
        self.assertEqual(
            attrs["block_device_mapping"][0],
            {
                "uuid": "v1",
                "boot_index": 0,
                "source_type": "volume",
                "destination_type": "volume",
                "delete_on_termination": False,
            },
        )
        self.assertEqual(attrs["block_device_mapping"][1]["boot_index"], -1)
        self.assertEqual(seen, ["srv-1"])
        self.assertEqual(info.id, "srv-1")
        be.create_server(
            "n", "f-1", [{"volume_id": "v1", "boot_index": 0}], ["p1"], "", "", {}, 5
        )
        attrs = compute.create_server.call_args.kwargs
        self.assertEqual(attrs["networks"], [{"port": "p1"}])
        self.assertNotIn("key_name", attrs)
        self.assertNotIn("availability_zone", attrs)

    def test_create_server_error_status(self):
        be = self.backend()
        compute = self.conn.compute
        compute.create_server.return_value = obj(id="srv-1")
        compute.get_server.return_value = obj(id="srv-1", status="ERROR")
        seen = []
        with self.assertRaises(CloudError):
            be.create_server("n", "f", [], [], "", "", {}, 5, on_created=seen.append)
        self.assertEqual(seen, ["srv-1"])

    def test_get_server_mapping(self):
        be = self.backend()
        self.conn.compute.get_server.return_value = obj(
            id="s",
            name="n",
            status="ACTIVE",
            project_id="p",
            flavor=obj(
                original_name="m1",
                vcpus=2,
                ram=512,
                disk=1,
                ephemeral=0,
                swap=0,
                extra_specs=None,
            ),
            image={"id": "img"},
            key_name="k",
            availability_zone="az",
            metadata={"a": "b"},
            tags=["t"],
            security_groups=[{"name": "default"}],
        )
        srv = be.get_server("s")
        self.assertEqual(srv.flavor["original_name"], "m1")
        self.assertEqual(srv.flavor["extra_specs"], {})
        self.assertEqual(srv.image_id, "img")
        self.assertFalse(srv.boot_from_volume)
        self.assertEqual(srv.security_groups, ["default"])

    def _volumes_for_boot_rule(self, root, image, devices_):
        be = self.backend()
        compute = self.conn.compute
        compute.get_server.return_value = obj(
            id="s", root_device_name=root, image=image
        )
        compute.volume_attachments.return_value = [
            obj(volume_id=vid, device=dev, delete_on_termination=False)
            for vid, dev, _ in devices_
        ]
        self.conn.block_storage.get_volume.side_effect = [
            obj(id=vid, status="in-use", size=1, is_bootable=boot, attachments=[])
            for vid, _, boot in devices_
        ]
        return {v.id: v.boot_index for v in be.list_server_volumes("s")}

    def test_boot_index_by_root_device_name(self):
        result = self._volumes_for_boot_rule(
            "/dev/vdb", "", [("a", "/dev/vda", True), ("b", "/dev/vdb", False)]
        )
        self.assertEqual(result, {"a": None, "b": 0})

    def test_boot_index_by_da_suffix(self):
        result = self._volumes_for_boot_rule(
            "", "", [("a", "/dev/vdb", False), ("b", "/dev/sda", False)]
        )
        self.assertEqual(result, {"a": None, "b": 0})

    def test_boot_index_by_smallest_bootable(self):
        result = self._volumes_for_boot_rule(
            "",
            "",
            [
                ("a", "/dev/vdc", True),
                ("b", "/dev/vdb", True),
                ("c", "/dev/vdd", False),
            ],
        )
        self.assertEqual(result, {"a": None, "b": 0, "c": None})

    def test_list_filters_and_ports(self):
        be = self.backend()
        self.conn.block_storage.volumes.return_value = [
            obj(id="a", metadata={"k": "1"}, attachments=[]),
            obj(id="b", metadata={"k": "2"}, attachments=[]),
        ]
        self.assertEqual([v.id for v in be.list_volumes({"k": "1"})], ["a"])
        self.conn.network.ports.return_value = [
            obj(
                id="p",
                network_id="n",
                mac_address="m",
                fixed_ips=[{"subnet_id": "s", "ip_address": "10.0.0.1"}],
                security_group_ids=["g"],
                name="x",
                device_id="s",
            )
        ]
        port = be.list_server_ports("s")[0]
        self.assertEqual(port.fixed_ips[0]["ip_address"], "10.0.0.1")
        self.conn.network.ports.assert_called_once_with(device_id="s")

    def test_get_port_not_found_is_none(self):
        be = self.backend()
        self.conn.network.get_port.side_effect = NotFoundException("x")
        self.assertIsNone(be.get_port("p"))

    def test_delete_server_waits_until_gone(self):
        be = self.backend()
        compute = self.conn.compute
        compute.get_server.side_effect = [
            obj(id="s", status="ACTIVE"),
            obj(id="s", status="ACTIVE"),
            NotFoundException("gone"),
        ]
        be.delete_server("s", 5)
        compute.delete_server.assert_called_once_with("s")

    def test_set_delete_on_termination(self):
        be = self.backend()
        be.set_delete_on_termination("s", "v", False)
        self.conn.compute.update_volume_attachment.assert_called_once_with(
            "s", "v", delete_on_termination=False
        )

    def test_project_id_and_close_are_safe(self):
        be = self.backend()
        type(self.conn).current_project_id = mock.PropertyMock(
            side_effect=RuntimeError("x")
        )
        self.assertEqual(be.project_id, "")
        self.conn.close.side_effect = RuntimeError("x")
        be.close()
        be.close()

    # images ------------------------------------------------------------------

    @staticmethod
    def image_obj(**kw):
        values = dict(
            id="img-1",
            name="n",
            status="active",
            disk_format="qcow2",
            container_format="bare",
            size=100,
            virtual_size=2048,
            min_disk=3,
            properties={"a": "b"},
        )
        values.update(kw)
        return obj(**values)

    def test_set_volume_image_metadata(self):
        be = self.backend()
        be.set_volume_image_metadata("v", {"hw_firmware_type": "uefi"})
        self.conn.block_storage.set_volume_image_metadata.assert_called_once_with(
            "v", hw_firmware_type="uefi"
        )

    def test_volume_image_metadata_is_limited_to_boot_properties(self):
        be = self.backend()
        self.conn.block_storage.get_volume.return_value = obj(
            id="v",
            status="available",
            size=1,
            attachments=[],
            volume_image_metadata={
                "hw_firmware_type": "uefi",
                "image_id": "x",
                "kernel_id": "k",
                "os_distro": "debian",
            },
        )
        self.assertEqual(
            be.get_volume("v").image_metadata,
            {"hw_firmware_type": "uefi", "os_distro": "debian"},
        )

    def test_flavor_ephemeral_and_swap_only_when_reported(self):
        be = self.backend()
        base = dict(id="s", name="n", status="ACTIVE", project_id="p", image={})
        self.conn.compute.get_server.return_value = obj(
            flavor=obj(original_name="m1", vcpus=1, ram=1, disk=1), **base
        )
        flavor = be.get_server("s").flavor
        self.assertNotIn("ephemeral", flavor)
        self.assertNotIn("swap", flavor)
        self.conn.compute.get_server.return_value = obj(
            flavor=obj(
                original_name="m1", vcpus=1, ram=1, disk=1, ephemeral=5, swap=""
            ),
            **base
        )
        flavor = be.get_server("s").flavor
        self.assertEqual((flavor["ephemeral"], flavor["swap"]), (5, 0))

    def test_image_booted_server_has_no_boot_volume_guess(self):
        result = self._volumes_for_boot_rule(
            "", {"id": "img"}, [("a", "/dev/vdb", True), ("b", "/dev/vdc", False)]
        )
        self.assertEqual(result, {"a": None, "b": None})

    # proxy_instance_id=auto -----------------------------------------------

    class _Reply:
        def __init__(self, payload):
            self._payload = payload

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

        def read(self):
            return json.dumps(self._payload).encode("utf-8")

    def auto_backend(self, urlopen, instance_file=None, server=None, missing=False):
        self.conn.compute.get_server.side_effect = (
            NotFoundException("gone") if missing else None
        )
        self.conn.compute.get_server.return_value = server or obj(
            id="u1", project_id="proj-1"
        )
        patches = [
            mock.patch.object(sdk_backend.urllib.request, "urlopen", urlopen),
            mock.patch.object(
                sdk_backend,
                "INSTANCE_ID_FILE",
                instance_file or "/nonexistent/instance-id",
            ),
        ]
        for patcher in patches:
            patcher.start()
            self.addCleanup(patcher.stop)
        return self.backend(proxy_instance_id="auto")

    def test_auto_uses_the_metadata_service(self):
        urlopen = mock.Mock(return_value=self._Reply({"uuid": "u1"}))
        be = self.auto_backend(urlopen)
        self.assertEqual(be.proxy_instance_id, "u1")
        self.assertEqual(urlopen.call_args.kwargs["timeout"], 5)
        self.assertIn("169.254.169.254", urlopen.call_args.args[0])
        self.conn.compute.get_server.assert_called_once_with("u1")

    def test_auto_falls_back_to_the_instance_id_file(self):
        with tempfile.NamedTemporaryFile("w", suffix=".id") as fh:
            fh.write("u1\n")
            fh.flush()
            be = self.auto_backend(
                mock.Mock(side_effect=OSError("unreachable")), fh.name
            )
        self.assertEqual(be.proxy_instance_id, "u1")

    def test_auto_fails_when_no_source_works(self):
        with self.assertRaises(CloudConfigError) as ctx:
            self.auto_backend(mock.Mock(side_effect=OSError("unreachable")))
        text = str(ctx.exception)
        self.assertIn("metadata service", text)
        self.assertIn("instance-id file", text)

    def test_auto_rejects_a_server_of_another_project(self):
        urlopen = mock.Mock(return_value=self._Reply({"uuid": "u1"}))
        with self.assertRaises(CloudConfigError) as ctx:
            self.auto_backend(urlopen, server=obj(id="u1", project_id="other"))
        self.assertIn("project", str(ctx.exception))

    def test_auto_rejects_an_unknown_server(self):
        urlopen = mock.Mock(return_value=self._Reply({"uuid": "u1"}))
        with self.assertRaises(CloudConfigError):
            self.auto_backend(urlopen, missing=True)

    # port tags and server listing --------------------------------------------

    def test_create_port_sets_tags(self):
        be = self.backend()
        net = self.conn.network
        net.create_port.return_value = obj(id="p1", network_id="n")
        port = be.create_port("n", "name", [], tags=["a", "b"])
        net.set_tags.assert_called_once_with(net.create_port.return_value, ["a", "b"])
        self.assertEqual(port.id, "p1")
        net.set_tags.reset_mock()
        be.create_port("n", "name", [])
        net.set_tags.assert_not_called()

    def test_tagging_failure_is_a_warning_only(self):
        be = self.backend()
        net = self.conn.network
        net.create_port.return_value = obj(id="p1", network_id="n")
        net.set_tags.side_effect = SdkException("no tags")
        with self.assertLogs(sdk_backend.LOG, level="WARNING"):
            port = be.create_port("n", "name", [], tags=["a"])
        self.assertEqual(port.id, "p1")

    def test_list_servers_skips_the_proxy(self):
        be = self.backend()
        self.conn.compute.servers.return_value = [
            obj(id=PROXY, name="proxy"),
            obj(id="s1", name="web", status="ACTIVE", image=""),
        ]
        self.assertEqual([s.id for s in be.list_servers()], ["s1"])
        self.conn.compute.servers.assert_called_once_with(details=True)

    # Batch D ----------------------------------------------------------------

    def test_project_lookups(self):
        be = self.backend()
        net = self.conn.network
        net.get_security_group.return_value = obj(id="g1", name="web")
        self.assertEqual(be.get_security_group_name("g1"), "web")
        net.security_groups.return_value = [
            obj(id="g2", name="web", project_id="proj-1")
        ]
        self.assertEqual(be.find_security_group("web"), "g2")
        net.security_groups.return_value = []
        with self.assertRaises(CloudNotFound):
            be.find_security_group("web")
        net.security_groups.return_value = [
            obj(id="a", name="web", project_id="proj-1"),
            obj(id="b", name="web", project_id="proj-1"),
        ]
        with self.assertRaises(CloudConflict):
            be.find_security_group("web")
        bs = self.conn.block_storage
        bs.find_type.return_value = obj(id="t")
        self.assertTrue(be.volume_type_usable("ssd"))
        bs.find_type.return_value = None
        self.assertFalse(be.volume_type_usable("ssd"))
        bs.availability_zones.return_value = [obj(name="nova")]
        self.assertTrue(be.availability_zone_exists("nova"))
        self.assertFalse(be.availability_zone_exists("x"))

    def test_transfer_calls_and_secret_masking(self):
        be = self.backend()
        bs = self.conn.block_storage
        bs.create_transfer.return_value = obj(id="t-1", auth_key="KEY123")
        seen = []
        self.assertEqual(
            be.create_transfer("v1", "nm", on_created=seen.append), ("t-1", "KEY123")
        )
        self.assertEqual(seen, ["t-1"])
        bs.create_transfer.assert_called_once_with(volume_id="v1", name="nm")
        bs.accept_transfer.side_effect = SdkException("denied for KEY123")
        with self.assertRaises(CloudError) as ctx:
            be.accept_transfer("t-1", "KEY123", 5)
        self.assertNotIn("KEY123", str(ctx.exception))
        bs.accept_transfer.side_effect = None
        bs.accept_transfer.return_value = obj(volume_id="v1")
        bs.get_volume.return_value = obj(id="v1", status="available")
        be.accept_transfer("t-1", "KEY123", 5)
        bs.accept_transfer.assert_called_with("t-1", auth_key="KEY123")
        be.delete_transfer("t-1")
        bs.delete_transfer.assert_called_once_with("t-1", ignore_missing=True)
        bs.transfers.return_value = [
            obj(id="a", name="bareos-1-xfer-x", volume_id="v"),
            obj(id="b", name="other", volume_id="w"),
        ]
        self.assertEqual([t.id for t in be.list_transfers("bareos-")], ["a"])

    def test_connection_without_proxy(self):
        be = self.backend(proxy_instance_id="", proxy=False)
        self.assertEqual(be.list_proxy_attachments(), [])
        with self.assertRaises(CloudConfigError):
            be.attach_to_proxy("v1", 5)
        with self.assertRaises(CloudConfigError):
            be.detach_from_proxy("v1", 5)
        self.assertIsInstance(
            create_backend(config(), proxy=False), sdk_backend.SdkBackend
        )

    def groups_backend(self, version=(3, 50)):
        be = self.backend()
        bs = self.conn.block_storage
        bs.get_endpoint_data.return_value = obj(max_microversion=version)
        bs.default_microversion = None
        return be, bs

    def test_groups_need_microversion_3_14(self):
        be, bs = self.groups_backend((3, 13))
        for call in (
            lambda: be.find_consistency_group(["v1"]),
            lambda: be.create_group_snapshot("g", "n", "d", 5),
            lambda: be.delete_group_snapshot("gs", 5),
            lambda: be.list_group_snapshots("bareos-"),
        ):
            with self.assertRaises(CloudConfigError) as ctx:
                call()
            self.assertIn("3.14", str(ctx.exception))
        bs.groups.assert_not_called()

    def test_find_consistency_group(self):
        be, bs = self.groups_backend()
        bs.groups.return_value = [obj(id="g1", group_type="gt")]
        bs.volumes.return_value = [
            obj(id="v1", group_id="g1"),
            obj(id="v2", group_id="g1"),
            obj(id="v3", group_id=""),
        ]
        bs.get_group_type.return_value = obj(
            group_specs={"consistent_group_snapshot_enabled": "<is> True"}
        )
        self.assertEqual(be.find_consistency_group(["v1", "v2"]), "g1")
        self.assertIsNone(bs.default_microversion)
        with self.assertRaises(CloudConflict) as ctx:
            be.find_consistency_group(["v1"])
        self.assertIn("v2", str(ctx.exception))
        with self.assertRaises(CloudConflict) as ctx:
            be.find_consistency_group(["v1", "v3"])
        self.assertIn("v3", str(ctx.exception))
        bs.get_group_type.return_value = obj(group_specs={})
        with self.assertRaises(CloudConflict):
            be.find_consistency_group(["v1", "v2"])

    def test_group_snapshot_calls(self):
        be, bs = self.groups_backend()
        bs.create_group_snapshot.return_value = obj(id="gs1")
        bs.get_group_snapshot.return_value = obj(id="gs1", status="available")
        bs.snapshots.return_value = [
            obj(id="s1", volume_id="v1", group_snapshot_id="gs1"),
            obj(id="s2", volume_id="v2", group_snapshot_id="gs1"),
            obj(id="s3", volume_id="v3", group_snapshot_id=""),
        ]
        seen = []
        members = be.create_group_snapshot("g1", "nm", "desc", 5, seen.append)
        self.assertEqual(members, {"v1": "s1", "v2": "s2"})
        self.assertEqual(seen, ["gs1"])
        bs.create_group_snapshot.assert_called_once_with(
            group_id="g1", name="nm", description="desc"
        )
        bs.get_group_snapshot.side_effect = NotFoundException("gone")
        be.delete_group_snapshot("gs1", 5)
        bs.delete_group_snapshot.assert_called_once_with("gs1", ignore_missing=True)
        bs.group_snapshots.return_value = [
            obj(id="a", name="bareos-1-gsnap-s", description="d", group_id="g"),
            obj(id="b", name="mine", description="", group_id="g"),
        ]
        self.assertEqual([g.id for g in be.list_group_snapshots("bareos-")], ["a"])

    def test_namespace_package_is_a_config_error(self):
        # a directory named openstack without __init__.py shadows the SDK
        for proxy in (True, False):
            with tempfile.TemporaryDirectory() as tmp:
                os.mkdir(os.path.join(tmp, "openstack"))
                saved_modules = {
                    k: sys.modules.pop(k)
                    for k in list(sys.modules)
                    if k == "openstack" or k.startswith("openstack.")
                }
                saved_path = list(sys.path)
                # no entry that holds a regular openstack package may win
                sys.path[:] = [tmp] + [
                    p
                    for p in saved_path
                    if not os.path.isfile(
                        os.path.join(p or ".", "openstack", "__init__.py")
                    )
                ]
                importlib.invalidate_caches()
                try:
                    kw = {} if proxy else {"proxy_instance_id": "", "proxy": False}
                    be = sdk_backend.SdkBackend(config(**kw))
                    with self.assertRaises(CloudConfigError) as ctx:
                        be.connect()
                    self.assertIn("python3-openstacksdk", str(ctx.exception))
                    self.assertIsNotNone(ctx.exception.__cause__)
                finally:
                    sys.path[:] = saved_path
                    importlib.invalidate_caches()
                    for key in [k for k in sys.modules if k.startswith("openstack")]:
                        del sys.modules[key]
                    sys.modules.update(saved_modules)

    def test_module_without_connect_is_a_config_error(self):
        del self.module.connect
        with self.assertRaises(CloudConfigError):
            sdk_backend.SdkBackend(config()).connect()

    def test_missing_exceptions_module_is_a_config_error(self):
        del self.module.exceptions
        sys.modules["openstack.exceptions"] = None
        with self.assertRaises(CloudConfigError):
            sdk_backend.SdkBackend(config()).connect()


try:
    from openstack.config import OpenStackConfig
except Exception:  # pylint: disable=broad-except
    OpenStackConfig = None


class SdkConnectTest(unittest.TestCase):
    """Project overrides of connect() and name lookups."""

    def setUp(self):
        self.conn = mock.MagicMock()
        self.conn.current_project_id = "proj-1"
        self.module, self.exceptions = make_openstack_module(self.conn)
        self.exceptions.raise_from_response = mock.Mock()
        self._saved = {
            k: sys.modules.get(k) for k in ("openstack", "openstack.exceptions")
        }
        sys.modules["openstack"] = self.module
        sys.modules["openstack.exceptions"] = self.exceptions
        patcher = mock.patch.object(sdk_backend.SdkBackend, "POLL_INTERVAL", 0.01)
        patcher.start()
        self.addCleanup(patcher.stop)

    def tearDown(self):
        for key, value in self._saved.items():
            if value is None:
                sys.modules.pop(key, None)
            else:
                sys.modules[key] = value

    def backend(self, **kw):
        be = sdk_backend.SdkBackend(config(**kw))
        be.connect()
        return be

    @staticmethod
    def image_obj(**kw):
        values = dict(
            id="img-1",
            name="n",
            status="active",
            disk_format="qcow2",
            container_format="bare",
            size=100,
            virtual_size=2048,
            min_disk=3,
            properties={},
        )
        values.update(kw)
        return obj(**values)

    # connect overrides --------------------------------------------------------------

    def test_project_name_override_drops_the_project_id_of_the_entry(self):
        self.backend(project_name="team", project_domain_name="Default")
        kwargs = self.module.connect.call_args.kwargs
        self.assertEqual(kwargs["cloud"], "prod")
        self.assertEqual(kwargs["project_name"], "team")
        self.assertEqual(kwargs["project_id"], "")
        self.assertEqual(kwargs["project_domain_name"], "Default")

    def test_project_id_override_drops_the_project_name_of_the_entry(self):
        self.backend(project_id="pid-1")
        kwargs = self.module.connect.call_args.kwargs
        self.assertEqual(kwargs["project_id"], "pid-1")
        self.assertEqual(kwargs["project_name"], "")
        self.assertNotIn("project_domain_name", kwargs)

    def test_no_override_without_options(self):
        self.backend()
        kwargs = self.module.connect.call_args.kwargs
        for key in ("project_id", "project_name", "project_domain_name"):
            self.assertNotIn(key, kwargs)

    @unittest.skipUnless(OpenStackConfig, "openstacksdk is not installed")
    def test_overrides_reach_the_openstacksdk_configuration(self):
        text = (
            "clouds:\n  prod:\n    auth:\n      auth_url: http://127.0.0.1:5000/v3\n"
            "      username: u\n      password: dummy-password\n"
            "      project_name: admin\n      user_domain_name: Default\n"
            "      project_domain_name: Default\n    region_name: RegionOne\n"
        )
        id_text = text.replace("project_name: admin", "project_id: entry-id")
        with tempfile.TemporaryDirectory() as tmp:
            for label, body, options, expect in (
                ("name over name", text, {"project_name": "team"}, ("team", None)),
                ("id over name", text, {"project_id": "pid"}, (None, "pid")),
                ("name over id", id_text, {"project_name": "team"}, ("team", None)),
                ("id over id", id_text, {"project_id": "pid"}, (None, "pid")),
            ):
                path = os.path.join(tmp, "clouds.yaml")
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write(body)
                self.module.connect.reset_mock()
                self.backend(**options)
                kwargs = dict(self.module.connect.call_args.kwargs)
                sdk_cfg = OpenStackConfig(
                    config_files=[path], vendor_files=[], secure_files=[]
                )
                auth = sdk_cfg.get_one(**kwargs).config["auth"]
                self.assertEqual(
                    (auth.get("project_name"), auth.get("project_id")), expect, label
                )

    # openstacksdk without the volume image metadata methods -------------------

    def old_block_storage(self, volume_meta=None):
        """A proxy of openstacksdk 3.0.0: no (un)set image metadata method."""
        bs = types.SimpleNamespace(
            post=mock.Mock(return_value="response"),
            get_volume=mock.Mock(
                return_value=obj(id="v1", volume_image_metadata=volume_meta)
            ),
        )
        self.conn.block_storage = bs
        return bs

    def test_set_image_metadata_falls_back_to_the_volume_action(self):
        bs = self.old_block_storage()
        be = self.backend()
        be.set_volume_image_metadata("v1", {"hw_firmware_type": "uefi", "n": 3})
        bs.post.assert_called_once_with(
            "/volumes/v1/action",
            json={
                "os-set_image_metadata": {
                    "metadata": {"hw_firmware_type": "uefi", "n": "3"}
                }
            },
            raise_exc=False,
        )
        self.exceptions.raise_from_response.assert_called_once_with("response")

    def test_fallback_errors_are_mapped(self):
        self.old_block_storage()
        self.exceptions.raise_from_response.side_effect = NotFoundException("gone")
        be = self.backend()
        with self.assertRaises(CloudNotFound):
            be.set_volume_image_metadata("v1", {"a": "b"})
        self.exceptions.raise_from_response.side_effect = ConflictException("busy")
        with self.assertRaises(CloudConflict):
            be.set_volume_image_metadata("v1", {"a": "b"})

    # names that exist in several projects -------------------------------------

    def test_security_group_is_looked_up_in_the_current_project(self):
        net = self.conn.network
        net.security_groups.return_value = [
            obj(id="other", name="web", project_id="proj-2"),
            obj(id="mine", name="web", project_id="proj-1"),
        ]
        be = self.backend()
        self.assertEqual(be.find_security_group("web"), "mine")
        net.security_groups.assert_called_once_with(name="web", project_id="proj-1")
        net.security_groups.return_value = [
            obj(id="other", name="web", project_id="proj-2")
        ]
        with self.assertRaises(CloudNotFound):
            be.find_security_group("web")
        net.security_groups.return_value = [
            obj(id="a", name="web", project_id="proj-1"),
            obj(id="b", name="web", project_id="proj-1"),
        ]
        with self.assertRaises(CloudConflict):
            be.find_security_group("web")

    def network_lookup(self, named, by_id=None):
        net = self.conn.network
        if by_id is None:
            net.get_network.side_effect = NotFoundException("no such id")
        else:
            net.get_network.return_value = by_id
        net.networks.return_value = named
        return self.backend()

    def test_network_id_is_found_directly(self):
        be = self.network_lookup([], by_id=obj(id="n1", name="lab"))
        self.assertTrue(be.network_exists("n1"))
        self.conn.network.networks.assert_not_called()

    def test_network_name_prefers_the_current_project_then_shared(self):
        other = obj(id="o", name="lab", project_id="proj-2", is_shared=False)
        mine = obj(id="m", name="lab", project_id="proj-1", is_shared=False)
        shared = obj(id="s", name="lab", project_id="proj-3", is_shared=True)
        shared2 = obj(id="t", name="lab", project_id="proj-4", is_shared=True)
        be = self.network_lookup([other, mine, shared])
        self.assertTrue(be.network_exists("lab"))
        self.assertTrue(self.network_lookup([other, shared]).network_exists("lab"))
        self.assertFalse(self.network_lookup([other]).network_exists("lab"))
        self.assertFalse(self.network_lookup([]).network_exists("lab"))
        with self.assertRaises(CloudConflict):
            self.network_lookup([mine, mine, shared]).network_exists("lab")
        with self.assertRaises(CloudConflict):
            self.network_lookup([other, shared, shared2]).network_exists("lab")

    def test_server_name_is_looked_up_in_the_current_project(self):
        compute = self.conn.compute
        compute.servers.return_value = [
            obj(id="a", name="vm", project_id="proj-2"),
            obj(id="b", name="vm", project_id="proj-1", status="ACTIVE"),
        ]
        be = self.backend()
        self.assertEqual(be.find_server("vm").id, "b")

    # name lookups fail closed -----------------------------------------------

    def no_project(self):
        type(self.conn).current_project_id = mock.PropertyMock(return_value="")
        self.addCleanup(delattr, type(self.conn), "current_project_id")
        return self.backend()

    def test_name_lookups_need_a_known_project(self):
        net = self.conn.network
        net.security_groups.return_value = [
            obj(id="g", name="web", project_id="proj-1")
        ]
        self.conn.compute.servers.return_value = [
            obj(id="s", name="vm", project_id="proj-1")
        ]
        net.get_network.side_effect = NotFoundException("no such id")
        net.networks.return_value = [
            obj(id="n", name="lab", project_id="proj-1", is_shared=False)
        ]
        be = self.no_project()
        for call, word in (
            (lambda: be.find_security_group("web"), "security group web"),
            (lambda: be.find_server("vm"), "server vm"),
            (lambda: be.network_exists("lab"), "network lab"),
        ):
            with self.assertRaises(CloudError) as ctx:
                call()
            self.assertIn(word, str(ctx.exception))
            self.assertIn("project is unknown", str(ctx.exception))
        net.security_groups.assert_not_called()
        self.conn.compute.servers.assert_not_called()
        net.networks.assert_not_called()

    def test_objects_without_a_known_owner_are_not_matched(self):
        net = self.conn.network
        net.security_groups.return_value = [obj(id="g", name="web")]
        be = self.backend()
        with self.assertRaises(CloudNotFound):
            be.find_security_group("web")
        self.conn.compute.servers.return_value = [
            obj(id="s", name="vm", project_id=None, tenant_id="")
        ]
        with self.assertRaises(CloudNotFound):
            be.find_server("vm")
        net.get_network.side_effect = NotFoundException("no such id")
        net.networks.return_value = [obj(id="n", name="lab")]
        self.assertFalse(be.network_exists("lab"))

    def test_the_tenant_id_alias_is_the_owner(self):
        net = self.conn.network
        net.security_groups.return_value = [
            obj(id="x", name="web", project_id=None, tenant_id="proj-2"),
            obj(id="g", name="web", project_id=None, tenant_id="proj-1"),
        ]
        be = self.backend()
        self.assertEqual(be.find_security_group("web"), "g")
        net.get_network.side_effect = NotFoundException("no such id")
        net.networks.return_value = [
            obj(id="n", name="lab", tenant_id="proj-1", is_shared=False)
        ]
        self.assertTrue(be.network_exists("lab"))


if __name__ == "__main__":
    unittest.main()
