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

"""Unit tests of the file-backed fake cloud."""

import json
import os
import tempfile
import threading
import time
import unittest

from bareos_openstack import fake_backend
from bareos_openstack.cloud import (
    CloudCancelled,
    CloudConfigError,
    CloudConflict,
    CloudNotFound,
    CloudTimeout,
    CloudError,
    create_backend,
)

PROJECT = "11111111-1111-4111-8111-111111111111"
PROXY = "22222222-2222-4222-8222-222222222222"
SERVER = "33333333-3333-4333-8333-333333333333"
ROOT_VOL = "44444444-4444-4444-8444-444444444444"
DATA_VOL = "66666666-6666-4666-8666-666666666666"
PORT = "55555555-5555-4555-8555-555555555555"
MIB = 1024 * 1024
GIB_BYTES = 1024**3


def base_spec():
    """Return the seed specification shared by the cloud tests."""
    return {
        "project_id": PROJECT,
        "proxy_server_id": PROXY,
        "flavors": [
            {"id": "f-small", "name": "m1.small", "vcpus": 1, "ram": 512, "disk": 1}
        ],
        "networks": [{"id": "net-1", "name": "net1", "subnet_ids": ["sub-1"]}],
        "security_groups": [{"id": "sg-1", "name": "default"}],
        "servers": [
            {
                "id": SERVER,
                "name": "web01",
                "status": "ACTIVE",
                "flavor_id": "f-small",
                "image_id": "",
                "key_name": "k1",
                "availability_zone": "nova",
                "metadata": {"role": "web"},
                "tags": ["t1"],
                "security_groups": ["default"],
                "volumes": [
                    {
                        "id": ROOT_VOL,
                        "name": "web01-root",
                        "size_gb": 1,
                        "size_bytes": 8 * MIB,
                        "volume_type": "ssd",
                        "bootable": True,
                        "availability_zone": "nova",
                        "device": "/dev/vda",
                        "boot_index": 0,
                        "content": {
                            "random_seed": 1,
                            "zero_ranges": [[2 * MIB, MIB]],
                        },
                    },
                    {
                        "id": DATA_VOL,
                        "name": "web01-data",
                        "size_gb": 1,
                        "size_bytes": 4 * MIB,
                        "volume_type": "ssd",
                        "availability_zone": "nova",
                        "device": "/dev/vdb",
                        "content": {"random_seed": 2},
                    },
                ],
                "ports": [
                    {
                        "id": PORT,
                        "network_id": "net-1",
                        "mac_address": "fa:16:3e:00:00:01",
                        "fixed_ips": [{"subnet_id": "sub-1", "ip_address": "10.0.0.5"}],
                        "security_group_ids": ["sg-1"],
                        "name": "web01-port",
                    }
                ],
            }
        ],
        "volumes": [],
    }


IMG_SERVER = "77777777-7777-4777-8777-777777777777"
IMG_DATA_VOL = "99999999-9999-4999-8999-999999999999"


def image_booted_spec(volumes=False, flavor=None, **server):
    """base_spec() plus a server that boots from an image (no root disk record)."""
    spec = base_spec()
    spec["flavors"][0].update(flavor or {"ephemeral": 0, "swap": 0})
    srv = {
        "id": IMG_SERVER,
        "name": "img01",
        "status": "ACTIVE",
        "flavor_id": "f-small",
        "image_id": "88888888-8888-4888-8888-888888888888",
        "availability_zone": "nova",
        "volumes": [],
        "ports": [],
    }
    if volumes:
        srv["volumes"] = [
            {
                "id": IMG_DATA_VOL,
                "name": "img01-data",
                "size_gb": 1,
                "size_bytes": 2 * MIB,
                "device": "/dev/vdb",
                "content": {"random_seed": 3},
            }
        ]
    srv.update(server)
    spec["servers"].append(srv)
    return spec


TARGET = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
TARGET_SG = "tsg-1"


def add_groups(spec, volume_ids, group_type="gt", types=True):
    """Add a volume group (and group types) to a seed specification."""
    if types:
        spec["group_types"] = {
            "gt": {"consistent_group_snapshot_enabled": True},
            "plain": {"consistent_group_snapshot_enabled": False},
        }
    spec["groups"] = [
        {"name": "g1", "group_type": group_type, "volumes": list(volume_ids)}
    ]
    return spec


def batch_d_spec(groups=True, **extra):
    """base_spec() with a second project and a consistency group of web01."""
    spec = base_spec()
    spec["projects"] = [
        {
            "id": TARGET,
            "flavors": [
                {"id": "f-small", "name": "m1.small", "vcpus": 1, "ram": 512, "disk": 1}
            ],
            "networks": [{"id": "net-1", "name": "net1", "subnet_ids": ["sub-1"]}],
            "security_groups": [{"id": TARGET_SG, "name": "default"}],
            "volume_types": ["ssd"],
            "availability_zones": ["nova"],
        }
    ]
    if groups:
        add_groups(spec, [ROOT_VOL, DATA_VOL])
    spec.update(extra)
    return spec


def target_backend(source, project=TARGET, **config):
    """A connection of another project of the cloud of `source`; no proxy."""
    cfg = {
        "backend": "fake",
        "fake_root": source._root,
        "fake_project_id": project,
        "proxy": False,
    }
    cfg.update(config)
    backend = fake_backend.FakeBackend(cfg)
    backend.connect()
    return backend


def make_cloud(tmp, spec=None, **config):
    """Seed a fake cloud in `tmp` and return (connected backend, seed result)."""
    root = os.path.join(tmp, "cloud")
    result = fake_backend.seed(root, spec or base_spec())
    cfg = {"backend": "fake", "fake_root": root, "proxy_instance_id": PROXY}
    cfg.update(config)
    backend = fake_backend.FakeBackend(cfg)
    backend.connect()
    return backend, result


def read_state(backend):
    with open(os.path.join(backend._root, "state.json"), encoding="utf-8") as fh:
        return json.load(fh)


class FakeCloudTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = self._tmp.name
        self.be, self.seeded = make_cloud(self.tmp)

    def tearDown(self):
        self._tmp.cleanup()

    def read_volume(self, volume_id):
        with open(self.be._vol_file(volume_id), "rb") as fh:
            return fh.read()

    # seed -------------------------------------------------------------------

    def test_seed_result_and_files(self):
        self.assertEqual(self.seeded["servers"], {"web01": SERVER})
        self.assertEqual(self.seeded["volumes"]["web01-root"], ROOT_VOL)
        self.assertEqual(self.seeded["ports"], {"web01-port": PORT})
        data = self.read_volume(ROOT_VOL)
        self.assertEqual(len(data), 8 * MIB)
        self.assertNotEqual(data[:1024], b"\0" * 1024)
        self.assertEqual(data[2 * MIB : 3 * MIB], b"\0" * MIB)
        self.assertEqual(self.be.project_id, PROJECT)
        self.assertEqual(self.be.proxy_instance_id, PROXY)

    def test_seed_refuses_second_run_and_reset_is_scoped(self):
        root = self.be._root
        with self.assertRaises(CloudConflict):
            fake_backend.seed(root, base_spec())
        foreign = os.path.join(root, "keep.txt")
        with open(foreign, "w", encoding="utf-8") as fh:
            fh.write("mine")
        fake_backend.seed(root, base_spec(), reset=True)
        self.assertTrue(os.path.exists(foreign))
        self.assertEqual(self.be.get_server(SERVER).name, "web01")

    def test_connect_checks_proxy_and_state(self):
        bad = fake_backend.FakeBackend(
            {"fake_root": self.be._root, "proxy_instance_id": "other"}
        )
        with self.assertRaises(CloudConfigError):
            bad.connect()
        missing = fake_backend.FakeBackend({"fake_root": os.path.join(self.tmp, "no")})
        with self.assertRaises(CloudConfigError):
            missing.connect()
        with self.assertRaises(CloudConfigError):
            fake_backend.FakeBackend({"fake_root": ""})
        self.assertIsInstance(
            create_backend({"backend": "fake", "fake_root": self.be._root}),
            fake_backend.FakeBackend,
        )

    # reading ----------------------------------------------------------------

    def test_server_queries(self):
        srv = self.be.get_server(SERVER)
        self.assertEqual(srv.flavor["original_name"], "m1.small")
        self.assertTrue(srv.boot_from_volume)
        vols = self.be.list_server_volumes(SERVER)
        self.assertEqual([v.id for v in vols], [ROOT_VOL, DATA_VOL])
        self.assertEqual(vols[0].boot_index, 0)
        self.assertEqual(vols[0].device, "/dev/vda")
        self.assertIsNone(vols[1].boot_index)
        self.assertEqual([p.id for p in self.be.list_server_ports(SERVER)], [PORT])
        self.assertIsNone(self.be.get_port("nope"))
        with self.assertRaises(CloudNotFound):
            self.be.get_server("nope")
        with self.assertRaises(CloudNotFound):
            self.be.get_volume("nope")
        self.assertEqual(self.be.find_flavor("m1.small").id, "f-small")
        self.assertEqual(self.be.find_flavor("f-small").name, "m1.small")
        with self.assertRaises(CloudNotFound):
            self.be.find_flavor("huge")
        self.assertTrue(self.be.network_exists("net-1"))
        self.assertFalse(self.be.network_exists("net-9"))

    def test_find_server_none_one_several(self):
        self.assertEqual(self.be.find_server("web01").id, SERVER)
        with self.assertRaises(CloudNotFound):
            self.be.find_server("bareos-proxy")
        with self.assertRaises(CloudNotFound):
            self.be.find_server("zzz")
        spec = base_spec()
        spec["servers"].append(
            {"name": "web01", "flavor_id": "f-small", "volumes": [], "ports": []}
        )
        root = os.path.join(self.tmp, "two-cloud")
        fake_backend.seed(root, spec)
        two = fake_backend.FakeBackend({"fake_root": root})
        with self.assertRaises(CloudConflict):
            two.find_server("web01")

    def test_metadata_filters(self):
        snap = self.be.create_snapshot(ROOT_VOL, "s", {"a": "1", "b": "2"}, True, 5)
        self.be.create_volume("v", 1, {"a": "1"}, 5)
        self.assertEqual([s.id for s in self.be.list_snapshots({"a": "1"})], [snap.id])
        self.assertEqual(self.be.list_snapshots({"a": "1", "b": "3"}), [])
        self.assertEqual(len(self.be.list_volumes({"a": "1"})), 1)
        self.assertEqual(len(self.be.list_volumes({})), 3)

    # snapshots and volumes --------------------------------------------------

    def test_snapshot_and_volume_copy_data(self):
        called = []
        snap = self.be.create_snapshot(
            ROOT_VOL, "s", {"k": "v"}, True, 5, on_created=called.append
        )
        self.assertEqual(called, [snap.id])
        self.assertEqual(snap.status, "available")
        self.assertEqual(snap.metadata, {"k": "v"})
        _write(self.be, ROOT_VOL, 0, 16, b"\xaa" * 16)
        vol = self.be.create_volume("c", 1, {}, 5, snapshot_id=snap.id)
        copy = self.read_volume(vol.id)
        self.assertEqual(len(copy), 8 * MIB)
        self.assertNotEqual(copy[:16], b"\xaa" * 16)
        self.assertEqual(copy[3 * MIB :], self.read_volume(ROOT_VOL)[3 * MIB :])
        self.assertEqual(self.be.get_volume(vol.id).size_gb, 1)

    def test_create_volume_plain_and_delete_rules(self):
        vol = self.be.create_volume(
            "p", 2, {}, 5, volume_type="t", availability_zone="z"
        )
        self.assertEqual(os.path.getsize(self.be._vol_file(vol.id)), 2 * 1024**3)
        snap = self.be.create_snapshot(vol.id, "s", {}, True, 5)
        with self.assertRaises(CloudConflict):
            self.be.delete_volume(vol.id, 5)
        self.be.delete_snapshot(snap.id, 5)
        self.be.delete_snapshot(snap.id, 5)
        self.be.delete_volume(vol.id, 5)
        self.be.delete_volume(vol.id, 5)
        with self.assertRaises(CloudNotFound):
            self.be.get_volume(vol.id)
        with self.assertRaises(CloudConflict):
            self.be.delete_volume(ROOT_VOL, 5)
        with self.assertRaises(CloudNotFound):
            self.be.create_snapshot("nope", "s", {}, True, 5)

    def test_attach_returns_file_and_is_idempotent(self):
        vol = self.be.create_volume("p", 1, {}, 5)
        path = self.be.attach_to_proxy(vol.id, 5)
        self.assertEqual(path, self.be._vol_file(vol.id))
        self.assertEqual(self.be.attach_to_proxy(vol.id, 5), path)
        self.assertEqual(self.be.list_proxy_attachments(), [vol.id])
        self.assertEqual(self.be.get_volume(vol.id).status, "in-use")
        with self.assertRaises(CloudConflict):
            self.be.delete_volume(vol.id, 5)
        self.be.detach_from_proxy(vol.id, 5)
        self.be.detach_from_proxy(vol.id, 5)
        self.assertEqual(self.be.get_volume(vol.id).status, "available")
        self.assertEqual(self.be.list_proxy_attachments(), [])
        with self.assertRaises(CloudConflict):
            self.be.attach_to_proxy(ROOT_VOL, 5)

    # ports and servers ------------------------------------------------------

    def test_create_port_rules(self):
        port = self.be.create_port(
            "net-1", "p", ["sg-1"], fixed_ips=[{"subnet_id": "sub-1"}]
        )
        self.assertTrue(port.mac_address.startswith("fa:16:3e:"))
        self.assertEqual(port.fixed_ips[0]["subnet_id"], "sub-1")
        self.assertTrue(port.fixed_ips[0]["ip_address"].startswith("10.0.0."))
        with self.assertRaises(CloudConflict):
            self.be.create_port(
                "net-1", "q", [], fixed_ips=[{"ip_address": "10.0.0.5"}]
            )
        with self.assertRaises(CloudConflict):
            self.be.create_port("net-1", "q", [], mac_address="fa:16:3e:00:00:01")
        with self.assertRaises(CloudNotFound):
            self.be.create_port("net-9", "q", [])
        self.be.delete_port(port.id)
        self.be.delete_port(port.id)
        self.assertIsNone(self.be.get_port(port.id))

    def _free_volume(self, name="n", **kw):
        return self.be.create_volume(name, 1, {}, 5, **kw)

    def test_create_server_rules(self):
        boot = self._free_volume("boot")
        other = self._free_volume("other")
        with self.assertRaises(CloudConflict):
            self.be.create_server(
                "x",
                "f-small",
                [{"volume_id": boot.id, "boot_index": None}],
                [],
                "",
                "",
                {},
                5,
            )
        with self.assertRaises(CloudConflict):
            self.be.create_server(
                "x",
                "f-small",
                [
                    {"volume_id": boot.id, "boot_index": 0},
                    {"volume_id": other.id, "boot_index": 0},
                ],
                [],
                "",
                "",
                {},
                5,
            )
        with self.assertRaises(CloudConflict):
            self.be.create_server(
                "x",
                "f-small",
                [{"volume_id": boot.id, "boot_index": 0}],
                [PORT],
                "",
                "",
                {},
                5,
            )
        with self.assertRaises(CloudNotFound):
            self.be.create_server(
                "x",
                "f-huge",
                [{"volume_id": boot.id, "boot_index": 0}],
                [],
                "",
                "",
                {},
                5,
            )
        with self.assertRaises(CloudConflict):
            self.be.create_server(
                "x",
                "f-small",
                [{"volume_id": ROOT_VOL, "boot_index": 0}],
                [],
                "",
                "",
                {},
                5,
            )
        port = self.be.create_port("net-1", "p", [])
        created = []
        srv = self.be.create_server(
            "new",
            "f-small",
            [
                {"volume_id": boot.id, "boot_index": 0},
                {"volume_id": other.id, "boot_index": None},
            ],
            [port.id],
            "k",
            "az",
            {"m": "1"},
            5,
            on_created=created.append,
        )
        self.assertEqual(created, [srv.id])
        self.assertEqual(srv.status, "ACTIVE")
        self.assertEqual(srv.metadata, {"m": "1"})
        vols = self.be.list_server_volumes(srv.id)
        self.assertEqual([v.device for v in vols], ["/dev/vda", "/dev/vdb"])
        self.assertEqual(self.be.get_volume(boot.id).status, "in-use")
        self.assertEqual(self.be.get_port(port.id).device_id, srv.id)
        self.be.stop_server(srv.id, 5)
        self.assertEqual(self.be.get_server(srv.id).status, "SHUTOFF")
        self.be.start_server(srv.id, 5)
        self.assertEqual(self.be.get_server(srv.id).status, "ACTIVE")

    def test_delete_server_honours_delete_on_termination(self):
        self.be.set_delete_on_termination(SERVER, DATA_VOL, True)
        self.assertTrue(self.be.list_server_volumes(SERVER)[1].delete_on_termination)
        with self.assertRaises(CloudNotFound):
            self.be.set_delete_on_termination(SERVER, "nope", True)
        self.be.delete_server(SERVER, 5)
        self.be.delete_server(SERVER, 5)
        with self.assertRaises(CloudNotFound):
            self.be.get_volume(DATA_VOL)
        root = self.be.get_volume(ROOT_VOL)
        self.assertEqual(root.status, "available")
        self.assertEqual(root.attached_server_ids, [])
        self.assertEqual(self.be.get_port(PORT).device_id, "")
        self.assertFalse(os.path.exists(self.be._vol_file(DATA_VOL)))

    def test_operation_log(self):
        self.be.create_volume("v", 1, {}, 5)
        entry = read_state(self.be)["log"][-1]
        self.assertEqual(entry["op"], "create_volume")
        self.assertTrue(entry["id"])
        self.assertIsInstance(entry["time"], float)

    # faults -----------------------------------------------------------------

    def _fault(self, op, **spec):
        with self.be._locked() as state:
            state["faults"][op] = spec

    def test_fault_before_raises_then_stops(self):
        self._fault("create_volume", remaining=2, message="boom")
        for _ in range(2):
            with self.assertRaises(CloudError) as ctx:
                self.be.create_volume("v", 1, {}, 5)
            self.assertEqual(str(ctx.exception), "boom")
        self.assertEqual(self.be.list_volumes({"x": "y"}), [])
        self.be.create_volume("v", 1, {}, 5)
        self.assertEqual(len(self.be.list_volumes({})), 3)

    def test_fault_defaults(self):
        self._fault("get_server")
        with self.assertRaises(CloudError) as ctx:
            self.be.get_server(SERVER)
        self.assertIn("injected fault in get_server", str(ctx.exception))
        self.assertEqual(self.be.get_server(SERVER).id, SERVER)

    def test_delay_only_fault_sleeps_and_succeeds(self):
        for phase in ("before", "after"):
            self._fault(
                "create_volume", remaining=1, sleep=0.2, phase=phase, delay_only=True
            )
            started = time.monotonic()
            vol = self.be.create_volume("v", 1, {}, 5)
            self.assertGreaterEqual(time.monotonic() - started, 0.2)
            self.assertEqual(vol.status, "available")
            self.assertEqual(
                read_state(self.be)["faults"]["create_volume"]["remaining"], 0
            )
            started = time.monotonic()
            self.be.create_volume("w", 1, {}, 5)
            self.assertLess(time.monotonic() - started, 0.2)

    def test_delay_only_fault_is_cancel_aware(self):
        self._fault("create_volume", remaining=1, sleep=30, delay_only=True)
        event = threading.Event()
        self.be.set_cancel_event(event)
        threading.Timer(0.2, event.set).start()
        with self.assertRaises(CloudCancelled):
            self.be.create_volume("v", 1, {}, 5)

    def test_fault_after_create_volume_keeps_volume(self):
        self._fault("create_volume", remaining=1, message="late", phase="after")
        created = []
        with self.assertRaises(CloudTimeout):
            self.be.create_volume("v", 1, {"t": "1"}, 5, on_created=created.append)
        self.assertEqual(len(created), 1)
        self.assertIn(created[0], read_state(self.be)["volumes"])
        self.be.create_volume("w", 1, {}, 5)

    def test_fault_after_attach_leaves_attached(self):
        vol = self.be.create_volume("v", 1, {}, 5)
        self._fault("attach_to_proxy", phase="after")
        with self.assertRaises(CloudTimeout):
            self.be.attach_to_proxy(vol.id, 5)
        self.assertEqual(self.be.list_proxy_attachments(), [vol.id])

    def test_fault_after_is_before_for_other_methods(self):
        self._fault("delete_port", phase="after", message="x")
        with self.assertRaises(CloudError) as ctx:
            self.be.delete_port("p")
        self.assertNotIsInstance(ctx.exception, CloudTimeout)

    def test_cancel_during_fault_sleep(self):
        self._fault("create_volume", remaining=1, sleep=30)
        event = threading.Event()
        self.be.set_cancel_event(event)
        threading.Timer(0.2, event.set).start()
        started = time.monotonic()
        with self.assertRaises(CloudCancelled):
            self.be.create_volume("v", 1, {}, 5)
        self.assertLess(time.monotonic() - started, 1.0)

    def test_cancel_during_after_sleep(self):
        self._fault("create_snapshot", sleep=30, phase="after")
        event = threading.Event()
        self.be.set_cancel_event(event)
        threading.Timer(0.2, event.set).start()
        started = time.monotonic()
        with self.assertRaises(CloudCancelled):
            self.be.create_snapshot(ROOT_VOL, "s", {}, True, 5)
        self.assertLess(time.monotonic() - started, 1.0)

    # concurrency ------------------------------------------------------------

    def test_two_instances_one_root(self):
        errors = []

        def work(tag):
            try:
                be = fake_backend.FakeBackend({"fake_root": self.be._root})
                for i in range(50):
                    vol = be.create_volume("%s-%d" % (tag, i), 1, {"t": tag}, 5)
                    be.delete_volume(vol.id, 5)
                    be.create_volume("%s-keep-%d" % (tag, i), 1, {"keep": tag}, 5)
            except Exception as exc:  # pylint: disable=broad-except
                errors.append(exc)

        threads = [threading.Thread(target=work, args=(t,)) for t in ("a", "b")]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(errors, [])
        state = read_state(self.be)
        self.assertEqual(len(state["volumes"]), 2 + 100)
        self.assertEqual(len(state["log"]), 300)
        files = os.listdir(os.path.join(self.be._root, "volumes"))
        self.assertEqual(len(files), len(state["volumes"]))
        self.assertEqual(len(self.be.list_volumes({"keep": "a"})), 50)


def _write(be, volume_id, offset, length, data):
    with open(be._vol_file(volume_id), "r+b") as fh:
        fh.seek(offset)
        fh.write(data[:length])


class FakeVolumeImageMetadataTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.be, self.seeded = make_cloud(self._tmp.name, image_booted_spec())

    def tearDown(self):
        self._tmp.cleanup()

    def test_image_booted_server_keeps_its_image_id(self):
        self.assertEqual(
            self.be.get_server(IMG_SERVER).image_id,
            "88888888-8888-4888-8888-888888888888",
        )
        self.assertFalse(self.be.get_server(IMG_SERVER).boot_from_volume)
        self.assertTrue(self.be.get_server(SERVER).boot_from_volume)

    def test_the_spec_refuses_an_images_section_and_a_root_disk(self):
        spec = base_spec()
        spec["images"] = [{"name": "ubuntu"}]
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(CloudConfigError):
                make_cloud(tmp, spec)
        spec = image_booted_spec()
        spec["servers"][-1]["root_disk"] = {"size_gb": 1}
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(CloudConfigError):
                make_cloud(tmp, spec)

    def test_set_volume_image_metadata(self):
        self.be.set_volume_image_metadata(ROOT_VOL, {"hw_firmware_type": "uefi"})
        self.be.set_volume_image_metadata(ROOT_VOL, {"os_type": "linux"})
        self.assertEqual(
            self.be.get_volume(ROOT_VOL).image_metadata,
            {"hw_firmware_type": "uefi", "os_type": "linux"},
        )
        with self.assertRaises(CloudNotFound):
            self.be.set_volume_image_metadata("nope", {})

    def test_flavor_reports_ephemeral_and_swap_only_when_known(self):
        flavor = self.be.get_server(IMG_SERVER).flavor
        self.assertEqual((flavor["ephemeral"], flavor["swap"]), (0, 0))
        with tempfile.TemporaryDirectory() as tmp:
            be, _ = make_cloud(tmp, image_booted_spec(flavor={"ephemeral": 5, "swap": 64}))
            flavor = be.get_server(IMG_SERVER).flavor
            self.assertEqual((flavor["ephemeral"], flavor["swap"]), (5, 64))
        with tempfile.TemporaryDirectory() as tmp:
            be, _ = make_cloud(tmp, image_booted_spec(flavor={"vcpus": 1}))
            flavor = be.get_server(IMG_SERVER).flavor
            self.assertNotIn("ephemeral", flavor)
            self.assertNotIn("swap", flavor)

    def test_seeded_volume_image_metadata(self):
        spec = base_spec()
        spec["servers"][0]["volumes"][0]["image_metadata"] = {"os_type": "linux"}
        with tempfile.TemporaryDirectory() as tmp:
            be, _ = make_cloud(tmp, spec)
            self.assertEqual(
                be.list_server_volumes(SERVER)[0].image_metadata, {"os_type": "linux"}
            )


class FakeBatchCTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.be, _ = make_cloud(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_port_tags_are_stored(self):
        port = self.be.create_port("net-1", "p", [], tags=["a", "b"])
        self.assertEqual(read_state(self.be)["ports"][port.id]["tags"], ["a", "b"])
        plain = self.be.create_port("net-1", "q", [])
        self.assertEqual(read_state(self.be)["ports"][plain.id]["tags"], [])

    def test_list_servers_skips_the_proxy(self):
        self.assertEqual([s.id for s in self.be.list_servers()], [SERVER])

    def test_auto_proxy_is_the_fake_proxy(self):
        root = os.path.join(self._tmp.name, "cloud")
        be = fake_backend.FakeBackend(
            {"backend": "fake", "fake_root": root, "proxy_instance_id": "auto"}
        )
        be.connect()
        self.assertEqual(be.proxy_instance_id, PROXY)


class FakeBatchDTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.be, _ = make_cloud(self._tmp.name, batch_d_spec())
        self.tbe = target_backend(self.be)

    def tearDown(self):
        self._tmp.cleanup()

    def volume(self, name="v", size_gb=1, **meta):
        return self.be.create_volume(name, size_gb, meta, 5)

    def state(self):
        return read_state(self.be)

    def set_state(self, **values):
        with self.be._locked() as state:
            state.update(values)

    # projects --------------------------------------------------------------

    def test_a_project_connection_sees_only_its_resources(self):
        self.assertEqual(self.tbe.project_id, TARGET)
        self.assertEqual(self.tbe.proxy_instance_id, "")
        self.assertEqual(self.tbe.list_servers(), [])
        self.assertEqual(self.tbe.list_volumes({}), [])
        with self.assertRaises(CloudNotFound):
            self.tbe.get_volume(ROOT_VOL)
        with self.assertRaises(CloudNotFound):
            self.tbe.get_server(SERVER)
        vol = self.volume()
        self.assertEqual(self.be.get_volume(vol.id).id, vol.id)
        self.assertEqual(self.be.project_id, PROJECT)
        self.assertEqual(self.be.get_server(SERVER).name, "web01")

    def test_a_connection_without_proxy_refuses_attach(self):
        vol = self.tbe.create_volume("t", 1, {}, 5)
        with self.assertRaises(CloudConfigError):
            self.tbe.attach_to_proxy(vol.id, 5)
        with self.assertRaises(CloudConfigError):
            self.tbe.detach_from_proxy(vol.id, 5)
        self.assertEqual(self.tbe.list_proxy_attachments(), [])
        self.assertEqual(self.state()["volumes"][vol.id]["project_id"], TARGET)

    def test_catalog_is_per_project(self):
        self.assertEqual(self.tbe.find_flavor("m1.small").id, "f-small")
        self.assertTrue(self.tbe.network_exists("net-1"))
        self.assertEqual(self.tbe.find_security_group("default"), TARGET_SG)
        self.assertEqual(self.be.find_security_group("default"), "sg-1")
        self.assertEqual(self.be.get_security_group_name("sg-1"), "default")
        with self.assertRaises(CloudNotFound):
            self.tbe.get_security_group_name("sg-1")
        with self.assertRaises(CloudNotFound):
            self.tbe.find_security_group("nope")
        self.assertTrue(self.tbe.volume_type_usable("ssd"))
        self.assertFalse(self.tbe.volume_type_usable("gold"))
        self.assertTrue(self.be.volume_type_usable("anything"))
        self.assertTrue(self.tbe.availability_zone_exists("nova"))
        self.assertFalse(self.tbe.availability_zone_exists("zone-x"))

    def test_ambiguous_security_group_name(self):
        with self.be._locked() as state:
            state["projects"][TARGET]["security_groups"]["tsg-2"] = {
                "id": "tsg-2",
                "name": "default",
            }
        with self.assertRaises(CloudConflict):
            self.tbe.find_security_group("default")

    def test_unknown_project_is_a_config_error(self):
        with self.assertRaises(CloudConfigError):
            target_backend(self.be, project="nope")

    # uploads ---------------------------------------------------------------

    # transfers -------------------------------------------------------------

    def test_transfer_moves_a_volume_between_projects(self):
        vol = self.volume()
        seen = []
        tid, key = self.be.create_transfer(vol.id, "bareos-7-xfer-x", seen.append)
        self.assertEqual(seen, [tid])
        self.assertEqual(self.be.get_volume(vol.id).status, "awaiting-transfer")
        with self.assertRaises(CloudConflict):
            self.be.delete_volume(vol.id, 5)
        with self.assertRaises(CloudConflict):
            self.be.create_transfer(vol.id, "again")
        pending = self.be.list_transfers("bareos-")
        self.assertEqual([(t.id, t.volume_id) for t in pending], [(tid, vol.id)])
        self.assertEqual(self.be.list_transfers("other-"), [])
        self.assertEqual(self.tbe.list_transfers("bareos-"), [])
        with self.assertRaises(CloudConflict):
            self.tbe.accept_transfer(tid, "wrong", 5)
        with self.assertRaises(CloudConflict):
            self.be.accept_transfer(tid, key, 5)
        self.tbe.accept_transfer(tid, key, 5)
        self.assertEqual(self.tbe.get_volume(vol.id).status, "available")
        with self.assertRaises(CloudNotFound):
            self.be.get_volume(vol.id)
        self.assertEqual(self.be.list_transfers("bareos-"), [])
        self.assertNotIn(key, json.dumps(self.state()["log"]))

    def test_transfer_needs_an_available_volume(self):
        with self.assertRaises(CloudConflict):
            self.be.create_transfer(ROOT_VOL, "x")
        with self.assertRaises(CloudNotFound):
            self.tbe.create_transfer(ROOT_VOL, "x")

    def test_delete_transfer_frees_the_volume(self):
        vol = self.volume()
        tid, _ = self.be.create_transfer(vol.id, "x")
        self.be.delete_transfer(tid)
        self.assertEqual(self.be.get_volume(vol.id).status, "available")
        self.be.delete_transfer(tid)
        self.be.delete_transfer("nope")

    def test_accept_fault_keeps_the_transfer_pending(self):
        vol = self.volume()
        tid, key = self.be.create_transfer(vol.id, "x")
        with self.be._locked() as state:
            state["faults"]["accept_transfer"] = {"remaining": 1, "message": "no"}
        with self.assertRaises(CloudError):
            self.tbe.accept_transfer(tid, key, 5)
        self.assertEqual(self.be.get_volume(vol.id).status, "awaiting-transfer")
        self.be.delete_transfer(tid)
        self.assertEqual(self.be.get_volume(vol.id).status, "available")

    def test_server_of_another_project_uses_its_catalog(self):
        vol = self.tbe.create_volume("t", 1, {}, 5)
        self.tbe.set_volume_bootable(vol.id, True)
        port = self.tbe.create_port("net-1", "p", [TARGET_SG], tags=["a"])
        srv = self.tbe.create_server(
            "s",
            "f-small",
            [{"volume_id": vol.id, "boot_index": 0}],
            [port.id],
            "",
            "",
            {},
            5,
        )
        self.assertEqual(srv.project_id, TARGET)
        self.assertEqual(srv.flavor["original_name"], "m1.small")
        self.assertEqual([s.id for s in self.tbe.list_servers()], [srv.id])
        self.assertNotIn(srv.id, [s.id for s in self.be.list_servers()])
        self.assertIsNone(self.be.get_port(port.id))
        self.tbe.delete_server(srv.id, 5)
        self.tbe.delete_port(port.id)
        self.assertIsNone(self.tbe.get_port(port.id))

    # groups ----------------------------------------------------------------

    def test_group_lookup_rules(self):
        self.assertEqual(
            self.be.find_consistency_group([ROOT_VOL, DATA_VOL]),
            self.state()["groups"].popitem()[0],
        )
        with self.assertRaises(CloudConflict) as ctx:
            self.be.find_consistency_group([ROOT_VOL])
        self.assertIn(DATA_VOL, str(ctx.exception))
        extra = self.volume("loose")
        with self.assertRaises(CloudConflict) as ctx:
            self.be.find_consistency_group([ROOT_VOL, DATA_VOL, extra.id])
        self.assertIn("no volume group", str(ctx.exception))

    def test_group_type_must_enable_consistent_snapshots(self):
        with self.be._locked() as state:
            for group in state["groups"].values():
                group["group_type"] = "plain"
        with self.assertRaises(CloudConflict) as ctx:
            self.be.find_consistency_group([ROOT_VOL, DATA_VOL])
        self.assertIn("consistent", str(ctx.exception))

    def test_groups_need_microversion_3_14(self):
        self.set_state(block_storage_microversion="3.13")
        for call in (
            lambda: self.be.find_consistency_group([ROOT_VOL, DATA_VOL]),
            lambda: self.be.list_group_snapshots("bareos-"),
        ):
            with self.assertRaises(CloudConfigError) as ctx:
                call()
            self.assertIn("3.14", str(ctx.exception))
        self.set_state(block_storage_microversion="3.14")
        self.assertEqual(self.be.list_group_snapshots("bareos-"), [])

    def test_group_snapshot_members_follow_the_group_snapshot(self):
        group = self.be.find_consistency_group([ROOT_VOL, DATA_VOL])
        seen = []
        members = self.be.create_group_snapshot(
            group, "bareos-9-gsnap-s", "created-by=bareos", 5, seen.append
        )
        self.assertEqual(set(members), {ROOT_VOL, DATA_VOL})
        listed = self.be.list_group_snapshots("bareos-9-")
        self.assertEqual([g.id for g in listed], seen)
        self.assertEqual(self.be.list_group_snapshots("zz"), [])
        for snap_id in members.values():
            self.assertEqual(self.state()["snapshots"][snap_id]["metadata"], {})
            with self.assertRaises(CloudConflict):
                self.be.delete_snapshot(snap_id, 5)
        temp = self.be.create_volume("t", 1, {}, 5, snapshot_id=members[ROOT_VOL])
        with self.assertRaises(CloudConflict) as ctx:
            self.be.delete_group_snapshot(seen[0], 5)
        self.assertIn(temp.id, str(ctx.exception))
        self.be.delete_volume(temp.id, 5)
        self.be.delete_group_snapshot(seen[0], 5)
        self.be.delete_group_snapshot(seen[0], 5)
        for snap_id in members.values():
            self.assertNotIn(snap_id, self.state()["snapshots"])


class FakeIsolationTest(unittest.TestCase):
    """A scoped connection never sees or changes a foreign owner's resources."""

    FOREIGN = ("unknown-foreign-project", TARGET)

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        spec = batch_d_spec()
        spec["volumes"] = [
            {
                "id": "fv-unknown",
                "name": "fu",
                "size_gb": 1,
                "project_id": "unknown-foreign-project",
            },
            {"id": "fv-known", "name": "fk", "size_gb": 1, "project_id": TARGET},
        ]
        spec["servers"].append(
            {
                "id": "fsrv",
                "name": "foreign",
                "flavor_id": "f-small",
                "project_id": "unknown-foreign-project",
            }
        )
        self.be, _ = make_cloud(self._tmp.name, spec)

    def tearDown(self):
        self._tmp.cleanup()

    def frozen(self):
        state = read_state(self.be)
        state["log"] = len(state["log"])
        return json.dumps(state, sort_keys=True)

    def test_reads_do_not_show_foreign_owners(self):
        for vol_id in ("fv-unknown", "fv-known"):
            with self.assertRaises(CloudNotFound):
                self.be.get_volume(vol_id)
        self.assertNotIn("fv-unknown", [v.id for v in self.be.list_volumes({})])
        self.assertNotIn("fv-known", [v.id for v in self.be.list_volumes({})])
        with self.assertRaises(CloudNotFound):
            self.be.get_server("fsrv")
        self.assertNotIn("fsrv", [s.id for s in self.be.list_servers()])
        self.assertEqual(self.be.get_volume(ROOT_VOL).id, ROOT_VOL)

    def test_mutations_are_refused_without_state_change(self):
        before = self.frozen()
        for vol_id in ("fv-unknown", "fv-known"):
            for call in (
                lambda: self.be.attach_to_proxy(vol_id, 5),
                lambda: self.be.create_snapshot(vol_id, "s", {}, True, 5),
                lambda: self.be.set_volume_bootable(vol_id, True),
                lambda: self.be.set_volume_image_metadata(vol_id, {"a": "b"}),
                lambda: self.be.create_transfer(vol_id, "x"),
                lambda: self.be.create_volume("c", 1, {}, 5, snapshot_id=vol_id),
            ):
                with self.assertRaises(CloudNotFound):
                    call()
            self.be.delete_volume(vol_id, 5)
        for call in (
            lambda: self.be.stop_server("fsrv", 5),
            lambda: self.be.start_server("fsrv", 5),
            lambda: self.be.set_delete_on_termination("fsrv", ROOT_VOL, True),
        ):
            with self.assertRaises(CloudNotFound):
                call()
        self.be.delete_server("fsrv", 5)
        state = json.loads(before)
        after = json.loads(self.frozen())
        for key in ("volumes", "servers", "snapshots", "ports"):
            self.assertEqual(after[key], state[key], key)
        self.assertEqual(after["log"], state["log"] + 3)

    def test_records_without_an_owner_belong_to_the_seed_project(self):
        state = read_state(self.be)
        self.assertNotIn("project_id", state["volumes"][ROOT_VOL])
        self.assertEqual(self.be.get_volume(DATA_VOL).id, DATA_VOL)
        other = target_backend(self.be)
        with self.assertRaises(CloudNotFound):
            other.get_volume(DATA_VOL)

    def test_a_foreign_owner_is_not_visible_to_the_other_scoped_connection(self):
        other = target_backend(self.be)
        self.assertEqual(other.get_volume("fv-known").id, "fv-known")
        with self.assertRaises(CloudNotFound):
            other.get_volume("fv-unknown")
        with self.assertRaises(CloudNotFound):
            other.get_server("fsrv")

    def test_wildcard_project_is_refused_in_a_configuration(self):
        cfg = {"backend": "fake", "fake_root": self.be._root, "fake_project_id": "*"}
        with self.assertRaises(CloudConfigError):
            fake_backend.FakeBackend(cfg)
        with self.assertRaises(CloudConfigError):
            create_backend(cfg)

    def test_the_command_line_view_is_a_separate_argument(self):
        cfg = {"backend": "fake", "fake_root": self.be._root}
        everything = fake_backend.FakeBackend(cfg, all_projects=True)
        self.assertEqual(everything.get_volume("fv-unknown").id, "fv-unknown")
        self.assertEqual(everything.get_server("fsrv").id, "fsrv")



if __name__ == "__main__":
    unittest.main()
