#!/usr/bin/env python3
# -*- coding: utf-8 -*-
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

"""Smoke test: the plugin class driven through an emulated File Daemon"""

import contextlib
import copy
import importlib.util
import io
import json
import os
import shutil
import tempfile
import threading
import unittest
from unittest import mock

import bareosfd_stub

HERE = os.path.dirname(os.path.abspath(__file__))
OPENSTACK_DIR = os.path.dirname(HERE)
PLUGIN_FILE = os.path.join(OPENSTACK_DIR, "bareos-fd-openstack.py")
REQUIRED = ("fake_backend.py", "cloud_backup.py", "cloud_restore.py", "metadata.py")
AVAILABLE = all(
    os.path.exists(os.path.join(OPENSTACK_DIR, "bareos_openstack", name))
    for name in REQUIRED
)

MIB = 1024 * 1024
PROJECT_ID = "11111111-1111-4111-8111-111111111111"
PROXY_ID = "22222222-2222-4222-8222-222222222222"
SERVER_ID = "33333333-3333-4333-8333-333333333333"
VOLUME_ID = "44444444-4444-4444-8444-444444444444"

SPEC = {
    "project_id": PROJECT_ID,
    "proxy_server_id": PROXY_ID,
    "flavors": [
        {"id": "f-small", "name": "m1.small", "vcpus": 1, "ram": 512, "disk": 1}
    ],
    "networks": [{"id": "net-1", "name": "net1", "subnet_ids": ["sub-1"]}],
    "security_groups": [{"id": "sg-1", "name": "default"}],
    "servers": [
        {
            "id": SERVER_ID,
            "name": "web01",
            "status": "ACTIVE",
            "flavor_id": "f-small",
            "image_id": "",
            "key_name": "k1",
            "availability_zone": "nova",
            "metadata": {},
            "tags": [],
            "security_groups": ["default"],
            "volumes": [
                {
                    "id": VOLUME_ID,
                    "name": "web01-root",
                    "size_gb": 1,
                    "size_bytes": 3 * MIB,
                    "volume_type": "",
                    "bootable": True,
                    "encrypted": False,
                    "availability_zone": "nova",
                    "metadata": {},
                    "device": "/dev/vda",
                    "boot_index": 0,
                    "delete_on_termination": False,
                    "content": {"random_seed": 5, "zero_ranges": [[MIB, MIB]]},
                }
            ],
            "ports": [],
        }
    ],
    "volumes": [],
}


def load_plugin_module():
    """Install the stub and import the plugin module from its file"""
    bareosfd_stub.install()
    spec = importlib.util.spec_from_file_location("bareos_fd_openstack", PLUGIN_FILE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FileDaemon:
    """Drives one plugin instance the way the File Daemon does"""

    def __init__(self, module):
        self.module = module
        self.stub = bareosfd_stub

    def new_plugin(self, plugindef, level="F", jobid=1, accurate=0, where=""):
        self.stub.reset(
            bVarLevel=ord(level),
            bVarJobId=jobid,
            bVarAccurate=accurate,
            bVarWhere=where,
        )
        plugin = self.module.BareosFdOpenStack(plugindef)
        rc = plugin.parse_plugin_definition(plugindef)
        return plugin, rc

    def backup(self, plugin):
        """Run the backup hooks; returns (result code, list of file dicts)"""
        stub = self.stub
        rc = plugin.handle_plugin_event(stub.bEventStartBackupJob)
        files = []
        if rc != stub.bRC_OK:
            plugin.handle_plugin_event(stub.bEventEndBackupJob)
            plugin.handle_plugin_event(stub.bEventJobEnd)
            return rc, files
        while True:
            pkt = stub.SavePacket()
            pkt.flags = bytearray(b"\xff" * 16)
            rc = plugin.start_backup_file(pkt)
            if rc != stub.bRC_OK:
                break
            entry = {
                "fname": pkt.fname,
                "type": pkt.type,
                "no_read": pkt.no_read,
                "flags": bytes(pkt.flags),
                "statp": pkt.statp,
                "object": bytes(pkt.object),
                "data": None,
            }
            if pkt.type == stub.FT_REG and not pkt.no_read:
                entry["data"] = self._read_file(plugin, pkt.fname)
            files.append(entry)
            if plugin.end_backup_file() == stub.bRC_OK:
                rc = stub.bRC_OK
                break
        plugin.handle_plugin_event(stub.bEventEndBackupJob)
        plugin.handle_plugin_event(stub.bEventJobEnd)
        return rc, files

    def _read_file(self, plugin, fname):
        stub = self.stub
        iop = stub.IoPacket()
        iop.func = stub.IO_OPEN
        iop.fname = fname
        assert plugin.plugin_io(iop) == stub.bRC_OK
        assert iop.status == stub.iostat_do_in_core
        data = bytearray()
        while True:
            part = os.read(iop.filedes, 1024 * 1024)
            if not part:
                break
            data += part
        iop2 = stub.IoPacket()
        iop2.func = stub.IO_CLOSE
        assert plugin.plugin_io(iop2) == stub.bRC_OK
        return bytes(data)

    def restore(self, plugin, files, drop=()):
        """Deliver the files of a backup; chunk names in drop get no data"""
        stub = self.stub
        for entry in files:
            if entry["type"] == stub.FT_RESTORE_FIRST:
                rop = stub.RestoreObject()
                rop.object_name = entry["fname"]
                rop.object = entry["object"]
                rop.object_len = len(entry["object"])
                rop.jobid = 1
                assert plugin.restore_object_data(rop) == stub.bRC_OK
        rc = plugin.handle_plugin_event(stub.bEventStartRestoreJob)
        if rc != stub.bRC_OK:
            return rc
        for entry in files:
            if entry["type"] != stub.FT_REG:
                continue
            pkt = stub.RestorePacket()
            pkt.original_file_name = entry["fname"]
            pkt.ofname = entry["fname"]
            pkt.statp = entry["statp"]
            assert plugin.create_file(pkt) == stub.bRC_OK
            if pkt.create_status != stub.CF_EXTRACT:
                continue
            iop = stub.IoPacket()
            iop.func = stub.IO_OPEN
            iop.fname = entry["fname"]
            iop.flags = os.O_WRONLY
            assert plugin.plugin_io(iop) == stub.bRC_OK
            if entry["data"] is not None and entry["fname"] not in drop:
                os.write(iop.filedes, entry["data"])
            iop2 = stub.IoPacket()
            iop2.func = stub.IO_CLOSE
            plugin.plugin_io(iop2)
            plugin.end_restore_file()
        rc = plugin.handle_plugin_event(stub.bEventEndRestoreJob)
        plugin.handle_plugin_event(stub.bEventJobEnd)
        return rc


@unittest.skipUnless(AVAILABLE, "the cloud package of coder-2 is not complete yet")
class PluginSmokeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_plugin_module()

    def setUp(self):
        # pylint: disable-next=import-outside-toplevel
        from bareos_openstack import fake_backend

        self.fake_backend = fake_backend
        self.tmp = tempfile.mkdtemp(prefix="openstack-smoke-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.root = os.path.join(self.tmp, "fake")
        fake_backend.seed(self.root, SPEC)
        self.daemon = FileDaemon(self.module)

    def plugindef(self, **extra):
        parts = {
            "backend": "fake",
            "fake_root": self.root,
            "server_id": SERVER_ID,
            "chunk_size": "1MiB",
        }
        parts.update(extra)
        return ":".join("%s=%s" % kv for kv in parts.items())

    def check_clean(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = self.fake_backend.main(["check-clean", "--root", self.root])
        self.assertEqual(rc, 0, out.getvalue())

    def volume_bytes(self):
        path = os.path.join(self.root, "volumes", VOLUME_ID + ".img")
        with open(path, "rb") as f:
            return f.read()

    def test_full_backup_then_local_restore(self):
        stub = bareosfd_stub
        plugin, rc = self.daemon.new_plugin(self.plugindef())
        self.assertEqual(rc, stub.bRC_OK)
        rc, files = self.daemon.backup(plugin)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)

        chunk_files = [f for f in files if f["type"] == stub.FT_REG]
        documents = [f for f in files if f["type"] == stub.FT_RESTORE_FIRST]
        self.assertEqual(len(chunk_files), 3)
        self.assertEqual(len(documents), 1)
        self.assertEqual(files[-1]["type"], stub.FT_RESTORE_FIRST)
        self.assertEqual([f["no_read"] for f in chunk_files], [False, True, False])
        for entry in chunk_files:
            self.assertEqual(entry["flags"][0] & (1 << 5), 0)
            self.assertEqual(entry["statp"].st_size, MIB)
            self.assertEqual(entry["statp"].st_nlink, 0)
        self.assertEqual(
            [f["fname"] for f in chunk_files],
            [
                "@OPENSTACK/%s/%s/volumes/%s/%08d.chunk"
                % (PROJECT_ID, SERVER_ID, VOLUME_ID, i)
                for i in range(3)
            ],
        )
        self.assertEqual(
            documents[0]["fname"],
            "@OPENSTACK/%s/%s/server.json" % (PROJECT_ID, SERVER_ID),
        )
        source = self.volume_bytes()
        self.assertEqual(chunk_files[0]["data"], source[:MIB])
        self.assertEqual(chunk_files[2]["data"], source[2 * MIB :])
        self.check_clean()

        target = os.path.join(self.tmp, "restore")
        os.makedirs(target)
        rplugin, rc = self.daemon.new_plugin(
            self.plugindef(restore_target="local", restore_local_dir=target)
        )
        self.assertEqual(rc, stub.bRC_OK)
        rc = self.daemon.restore(rplugin, files)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        raw = os.path.join(target, SERVER_ID, VOLUME_ID + ".raw")
        with open(raw, "rb") as f:
            self.assertEqual(f.read(), source)
        self.assertTrue(os.path.exists(os.path.join(target, SERVER_ID, "server.json")))

    def test_restore_target_image_is_refused(self):
        stub = bareosfd_stub
        _, rc = self.daemon.new_plugin(self.plugindef(restore_target="image"))
        self.assertEqual(rc, stub.bRC_Error)
        self.assertTrue(
            any("restore_target" in m for _, m in stub.messages), stub.messages
        )

    def test_removed_options_are_refused(self):
        stub = bareosfd_stub
        for name in (
            "transfer_mode",
            "transfer_format",
            "transfer_dir",
            "qemu_img",
            "skip_ephemeral",
            "ignore_ephemeral_disk",
            "restore_image_keep_volumes",
        ):
            _, rc = self.daemon.new_plugin(self.plugindef() + ":%s=yes" % name)
            self.assertEqual(rc, stub.bRC_Error, name)
            self.assertTrue(
                any("unknown option '%s'" % name in m for _, m in stub.messages),
                (name, stub.messages),
            )

    def test_restore_to_other_project_from_credentials_file(self):
        stub = bareosfd_stub
        spec = copy.deepcopy(SPEC)
        spec["projects"] = [
            {
                "id": "proj-b",
                "flavors": copy.deepcopy(SPEC["flavors"]),
                "networks": copy.deepcopy(SPEC["networks"]),
                "security_groups": [{"id": "sg-b", "name": "default"}],
            }
        ]
        root = os.path.join(self.tmp, "fake-two-projects")
        self.fake_backend.seed(root, spec)
        creds = os.path.join(self.tmp, "target.ini")
        with open(creds, "w", encoding="utf-8") as f:
            f.write("backend=fake\nfake_project_id=proj-b\n")
        plugin, _ = self.daemon.new_plugin(self.plugindef(fake_root=root))
        rc, files = self.daemon.backup(plugin)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        rplugin, rc = self.daemon.new_plugin(
            self.plugindef(fake_root=root, restore_project_credentials=creds)
        )
        self.assertEqual(rc, stub.bRC_OK)
        rc = self.daemon.restore(rplugin, files)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        self.assertTrue([m for _, m in stub.messages if "restored server" in m])

    def transfer_cloud(self):
        """Two-project fake cloud: the proxy lives in proj-b; returns the creds file."""
        spec = copy.deepcopy(SPEC)
        spec["proxy_project_id"] = "proj-b"
        spec["projects"] = [
            {
                "id": "proj-b",
                "flavors": copy.deepcopy(SPEC["flavors"]),
                "networks": copy.deepcopy(SPEC["networks"]),
                "security_groups": [{"id": "sg-b", "name": "default"}],
            }
        ]
        root = os.path.join(self.tmp, "fake-transfer")
        self.fake_backend.seed(root, spec)
        creds = os.path.join(self.tmp, "provider.ini")
        with open(creds, "w", encoding="utf-8") as f:
            f.write(
                "backend=fake\nfake_project_id=proj-b\nproxy_instance_id=%s\n"
                % PROXY_ID
            )
        self.root = root
        return creds

    def test_transfer_mode_backup_then_restore_reverses_the_transfer(self):
        stub = bareosfd_stub
        creds = self.transfer_cloud()
        source = self.volume_bytes()
        defn = self.plugindef(proxy_mode="transfer", proxy_project_credentials=creds)
        plugin, rc = self.daemon.new_plugin(defn)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        rc, files = self.daemon.backup(plugin)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        chunks = [f for f in files if f["type"] == stub.FT_REG]
        self.assertEqual(len(chunks), 3)
        self.assertEqual(chunks[0]["data"], source[:MIB])
        self.assertEqual(chunks[2]["data"], source[2 * MIB :])
        self.check_clean()
        rplugin, rc = self.daemon.new_plugin(defn)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        rc = self.daemon.restore(rplugin, files)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        self.assertTrue([m for _, m in stub.messages if "restored server" in m])
        self.check_clean()
        with open(os.path.join(self.root, "state.json"), encoding="utf-8") as f:
            state = json.load(f)
        restored = [
            v for v in state["volumes"].values() if v["name"].endswith("-restore-1")
        ]
        self.assertEqual(len(restored), 1)
        self.assertNotEqual(restored[0].get("project_id"), "proj-b")
        with open(os.path.join(self.root, "volumes", restored[0]["id"] + ".img"), "rb") as f:
            self.assertEqual(f.read(len(source)), source)

    def test_transfer_mode_without_credentials_is_refused(self):
        stub = bareosfd_stub
        plugin, rc = self.daemon.new_plugin(self.plugindef(proxy_mode="transfer"))
        self.assertEqual(rc, stub.bRC_OK)
        rc, _ = self.daemon.backup(plugin)
        self.assertEqual(rc, stub.bRC_Error)
        self.assertTrue(
            [m for _, m in stub.messages if "proxy_project_credentials" in m],
            stub.messages,
        )

    def test_cross_project_with_force_is_refused_before_the_cloud(self):
        stub = bareosfd_stub
        plugin, _ = self.daemon.new_plugin(self.plugindef())
        rc, files = self.daemon.backup(plugin)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        rplugin, rc = self.daemon.new_plugin(
            self.plugindef(restore_project_cloud="other", force="yes")
        )
        self.assertEqual(rc, stub.bRC_OK)
        self.assertEqual(self.daemon.restore(rplugin, files), stub.bRC_Error)
        self.assertTrue(
            [m for _, m in stub.messages if "force cannot be used" in m], stub.messages
        )

    def test_group_mode_without_a_group_fails_and_leaves_nothing(self):
        stub = bareosfd_stub
        plugin, rc = self.daemon.new_plugin(self.plugindef(snapshot_mode="group"))
        self.assertEqual(rc, stub.bRC_OK)
        rc, _ = self.daemon.backup(plugin)
        self.assertEqual(rc, stub.bRC_Error)
        self.assertTrue([m for _, m in stub.messages if "group" in m], stub.messages)
        self.check_clean()

    def test_restore_without_data_of_a_data_chunk_fails(self):
        stub = bareosfd_stub
        plugin, _ = self.daemon.new_plugin(self.plugindef())
        rc, files = self.daemon.backup(plugin)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        chunk_files = [f for f in files if f["type"] == stub.FT_REG]

        target = os.path.join(self.tmp, "restore")
        os.makedirs(target)
        rplugin, _ = self.daemon.new_plugin(
            self.plugindef(restore_target="local", restore_local_dir=target)
        )
        rc = self.daemon.restore(rplugin, files, drop={chunk_files[0]["fname"]})
        self.assertNotEqual(rc, stub.bRC_OK)
        levels = {level for level, _ in stub.messages}
        self.assertTrue(levels & {stub.M_ERROR, stub.M_FATAL})

    def test_incremental_with_accurate_and_previous_document(self):
        stub = bareosfd_stub
        plugin, _ = self.daemon.new_plugin(self.plugindef())
        rc, full = self.daemon.backup(plugin)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        document = [f for f in full if f["type"] == stub.FT_RESTORE_FIRST][0]

        iplugin, rc = self.daemon.new_plugin(
            self.plugindef(), level="I", jobid=2, accurate=1
        )
        self.assertEqual(rc, stub.bRC_OK)
        rop = stub.RestoreObject()
        rop.object_name = document["fname"]
        rop.object = document["object"]
        rop.jobid = 1
        self.assertEqual(iplugin.restore_object_data(rop), stub.bRC_OK)
        rc, files = self.daemon.backup(iplugin)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        self.assertEqual(len([f for f in files if f["type"] == stub.FT_REG]), 3)
        self.assertEqual(files[-1]["type"], stub.FT_RESTORE_FIRST)
        self.check_clean()

    def test_incremental_with_changed_chunk_size_fails(self):
        stub = bareosfd_stub
        plugin, _ = self.daemon.new_plugin(self.plugindef())
        _, full = self.daemon.backup(plugin)
        document = [f for f in full if f["type"] == stub.FT_RESTORE_FIRST][0]
        iplugin, _ = self.daemon.new_plugin(
            self.plugindef(chunk_size="2MiB"), level="I", jobid=2, accurate=1
        )
        rop = stub.RestoreObject()
        rop.object_name = document["fname"]
        rop.object = document["object"]
        rop.jobid = 1
        iplugin.restore_object_data(rop)
        rc, _ = self.daemon.backup(iplugin)
        self.assertEqual(rc, stub.bRC_Error)
        self.assertTrue(any("chunk_size changed" in m for _, m in stub.messages))

    def previous_document(self, plugin, files, jobid=1):
        stub = bareosfd_stub
        document = [f for f in files if f["type"] == stub.FT_RESTORE_FIRST][0]
        rop = stub.RestoreObject()
        rop.object_name = document["fname"]
        rop.object = document["object"]
        rop.jobid = jobid
        self.assertEqual(plugin.restore_object_data(rop), stub.bRC_OK)
        return document

    def test_image_booted_server_is_refused_before_any_change(self):
        stub = bareosfd_stub
        # pylint: disable-next=import-outside-toplevel
        import test_cloud_fake as tcf

        root = os.path.join(self.tmp, "image-cloud")
        self.fake_backend.seed(root, tcf.image_booted_spec())
        definition = "backend=fake:fake_root=%s:server_id=%s:chunk_size=1MiB" % (
            root,
            tcf.IMG_SERVER,
        )
        plugin, rc = self.daemon.new_plugin(definition)
        self.assertEqual(rc, stub.bRC_OK)
        rc, files = self.daemon.backup(plugin)
        self.assertEqual(rc, stub.bRC_Error)
        self.assertEqual(files, [])
        self.assertTrue(
            any(
                "boots from an image; only servers that boot from a Cinder volume "
                "are supported" in m
                for _, m in stub.messages
            ),
            stub.messages,
        )

    def test_incremental_fails_when_root_disk_mode_changed(self):
        stub = bareosfd_stub
        definition = self.plugindef()
        plugin, _ = self.daemon.new_plugin(definition)
        rc, files = self.daemon.backup(plugin)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        iplugin, _ = self.daemon.new_plugin(definition, level="I", jobid=2, accurate=1)
        self.previous_document(iplugin, files)
        with mock.patch.object(
            self.module.metadata, "inventory_root_mode", return_value="no-root"
        ):
            rc, _ = self.daemon.backup(iplugin)
        self.assertEqual(rc, stub.bRC_Error)
        self.assertTrue(
            any(
                "root disk mode changed from volume-root to no-root" in m
                and "run a Full backup" in m
                for _, m in stub.messages
            ),
            stub.messages,
        )

    def test_cancel_then_cleanup_removes_everything(self):
        # pylint: disable=import-outside-toplevel
        from unittest import mock
        from bareos_openstack import cloud

        class CancelAwareFake(self.fake_backend.FakeBackend):
            """Fails like a real backend wait when the cancel event is set"""

            def detach_from_proxy(self, volume_id, timeout):
                self.check_cancel()
                return super().detach_from_proxy(volume_id, timeout)

            def delete_volume(self, volume_id, timeout):
                self.check_cancel()
                return super().delete_volume(volume_id, timeout)

            def delete_snapshot(self, snapshot_id, timeout):
                self.check_cancel()
                return super().delete_snapshot(snapshot_id, timeout)

        stub = bareosfd_stub
        plugin, rc = self.daemon.new_plugin(self.plugindef())
        self.assertEqual(rc, stub.bRC_OK)
        with mock.patch.object(
            cloud,
            "create_backend",
            side_effect=lambda cfg, proxy=True: CancelAwareFake(cfg),
        ):
            rc = plugin.handle_plugin_event(stub.bEventStartBackupJob)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        plugin.handle_plugin_event(stub.bEventCancelCommand)
        self.assertEqual(
            plugin.handle_plugin_event(stub.bEventEndBackupJob), stub.bRC_OK
        )
        plugin.handle_plugin_event(stub.bEventJobEnd)
        self.check_clean()

    def _backup_files(self):
        plugin, _ = self.daemon.new_plugin(self.plugindef())
        rc, files = self.daemon.backup(plugin)
        self.assertEqual(rc, bareosfd_stub.bRC_OK, bareosfd_stub.messages)
        return files

    def test_local_restore_refuses_symlinked_metadata(self):
        stub = bareosfd_stub
        files = self._backup_files()
        target = os.path.join(self.tmp, "restore")
        server_dir = os.path.join(target, SERVER_ID)
        os.makedirs(server_dir)
        outside = os.path.join(self.tmp, "unrelated.txt")
        with open(outside, "w") as f:
            f.write("KEEP ME")
        os.symlink(outside, os.path.join(server_dir, "server.json"))
        rplugin, _ = self.daemon.new_plugin(
            self.plugindef(
                restore_target="local",
                restore_local_dir=target,
                restore_overwrite="yes",
            )
        )
        rc = self.daemon.restore(rplugin, files)
        self.assertNotEqual(rc, stub.bRC_OK)
        with open(outside) as f:
            self.assertEqual(f.read(), "KEEP ME")

    def test_local_restore_overwrites_regular_files(self):
        stub = bareosfd_stub
        files = self._backup_files()
        target = os.path.join(self.tmp, "restore")
        server_dir = os.path.join(target, SERVER_ID)
        os.makedirs(server_dir)
        for name in ("server.json", VOLUME_ID + ".raw"):
            with open(os.path.join(server_dir, name), "w") as f:
                f.write("old")
        rplugin, _ = self.daemon.new_plugin(
            self.plugindef(
                restore_target="local",
                restore_local_dir=target,
                restore_overwrite="yes",
            )
        )
        rc = self.daemon.restore(rplugin, files)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        with open(os.path.join(server_dir, VOLUME_ID + ".raw"), "rb") as f:
            self.assertEqual(f.read(), self.volume_bytes())
        with open(os.path.join(server_dir, "server.json"), "rb") as f:
            self.assertIn(b"bareos-openstack-server", f.read())

    def test_cancel_event_queues_exact_log_line(self):
        stub = bareosfd_stub
        plugin, rc = self.daemon.new_plugin(self.plugindef())
        self.assertEqual(rc, stub.bRC_OK)
        before = len(stub.messages)
        self.assertEqual(
            plugin.handle_plugin_event(stub.bEventCancelCommand), stub.bRC_OK
        )
        self.assertTrue(plugin.cancel_event.is_set())
        self.assertEqual(len(stub.messages), before)
        plugin.log.flush()
        self.assertEqual(
            stub.messages[before:],
            [(stub.M_INFO, "openstack: cancel requested\n")],
        )

    def test_cancel_stays_attached_while_reader_is_alive(self):
        # pylint: disable=import-outside-toplevel
        from unittest import mock
        from bareos_openstack import cloud

        allocated = threading.Event()
        release = threading.Event()
        cancel_calls = []

        class BlockingFake(self.fake_backend.FakeBackend):
            """Blocks the reader right after a temporary volume was allocated"""

            def create_volume(self, *args, **kwargs):
                volume = super().create_volume(*args, **kwargs)
                allocated.set()
                release.wait(30)
                return volume

            def set_cancel_event(self, event):
                cancel_calls.append(event)
                super().set_cancel_event(event)

        stub = bareosfd_stub
        plugin, _ = self.daemon.new_plugin(self.plugindef())
        with mock.patch.object(
            cloud,
            "create_backend",
            side_effect=lambda cfg, proxy=True: BlockingFake(cfg),
        ):
            rc = plugin.handle_plugin_event(stub.bEventStartBackupJob)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        self.assertTrue(allocated.wait(10), "the reader never allocated a volume")

        session = plugin.backup_session
        real_cleanup = session.cleanup
        seen = []

        def cleanup():
            seen.append(plugin.cancel_event.is_set())
            return real_cleanup()

        session.cleanup = cleanup
        producer = plugin.producer
        old = self.module.JOIN_TIMEOUT
        self.module.JOIN_TIMEOUT = 0.3
        try:
            rc = plugin.handle_plugin_event(stub.bEventEndBackupJob)
            self.assertTrue(producer.is_alive())
            self.assertIsNotNone(plugin.backend)
        finally:
            self.module.JOIN_TIMEOUT = old
            release.set()
            producer.join(10)
        self.assertEqual(rc, stub.bRC_OK)
        self.assertFalse(producer.is_alive())
        self.assertEqual(seen, [True])
        self.assertNotIn(None, cancel_calls)
        self.assertTrue(any("did not stop" in m for _, m in stub.messages))
        # the released reader found the session closed and attached nothing
        state_path = os.path.join(self.root, "state.json")
        with open(state_path) as f:
            ops = [entry["op"] for entry in json.load(f)["log"]]
        self.assertNotIn("attach_to_proxy", ops)
        self.check_clean()

    def test_reader_blocked_in_attach_is_reported_after_join_timeout(self):
        # pylint: disable=import-outside-toplevel
        from unittest import mock
        from bareos_openstack import cloud

        in_attach = threading.Event()
        barrier = threading.Event()
        cancel_calls = []

        class AttachBarrierFake(self.fake_backend.FakeBackend):
            """Blocks attach_to_proxy (not cancel-aware); cleanup waits are aware"""

            def attach_to_proxy(self, volume_id, timeout):
                in_attach.set()
                barrier.wait(30)
                return super().attach_to_proxy(volume_id, timeout)

            def detach_from_proxy(self, volume_id, timeout):
                self.check_cancel()
                return super().detach_from_proxy(volume_id, timeout)

            def delete_volume(self, volume_id, timeout):
                self.check_cancel()
                return super().delete_volume(volume_id, timeout)

            def delete_snapshot(self, snapshot_id, timeout):
                self.check_cancel()
                return super().delete_snapshot(snapshot_id, timeout)

            def set_cancel_event(self, event):
                cancel_calls.append(event)
                super().set_cancel_event(event)

        stub = bareosfd_stub
        plugin, _ = self.daemon.new_plugin(self.plugindef(), jobid=1)
        with mock.patch.object(
            cloud,
            "create_backend",
            side_effect=lambda cfg, proxy=True: AttachBarrierFake(cfg),
        ):
            rc = plugin.handle_plugin_event(stub.bEventStartBackupJob)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        self.assertTrue(in_attach.wait(10), "the reader never reached attach")

        producer = plugin.producer
        session = plugin.backup_session
        old = self.module.JOIN_TIMEOUT
        self.module.JOIN_TIMEOUT = 0.2
        try:
            plugin.handle_plugin_event(stub.bEventCancelCommand)
            rc = plugin.handle_plugin_event(stub.bEventEndBackupJob)
            self.assertEqual(rc, stub.bRC_OK)
            self.assertTrue(producer.is_alive())
            self.assertTrue(plugin.cancel_event.is_set())
            self.assertNotIn(None, cancel_calls)
            self.assertTrue(session.closed)
            with self.assertRaises(cloud.CloudCancelled):
                session.open_volume("any")
            texts = [m for _, m in stub.messages]
            self.assertTrue(any("did not stop" in m for m in texts))
            left = [m for m in texts if "resources left behind" in m]
            self.assertTrue(left, texts)
        finally:
            self.module.JOIN_TIMEOUT = old
            barrier.set()
            producer.join(5)
        self.assertFalse(producer.is_alive())

        state_path = os.path.join(self.root, "state.json")
        with open(state_path) as f:
            state = json.load(f)
        ops = [entry["op"] for entry in state["log"]]
        self.assertEqual(ops.count("create_volume"), 1)
        temp_ids = [
            v["id"]
            for v in state["volumes"].values()
            if v["metadata"].get("bareos-role") == "backup-temp-volume"
        ]
        self.assertEqual(len(temp_ids), 1)
        self.assertIn(temp_ids[0], left[0])

        # the next job reports the late volume as a leftover
        stub.reset()
        plugin2, _ = self.daemon.new_plugin(self.plugindef(), jobid=2)
        plugin2.handle_plugin_event(stub.bEventStartBackupJob)
        reported = [m for _, m in stub.messages]
        plugin2.handle_plugin_event(stub.bEventEndBackupJob)
        plugin2.handle_plugin_event(stub.bEventJobEnd)
        self.assertTrue(any(temp_ids[0] in m for m in reported), reported)

    def test_incremental_without_accurate_fails(self):
        stub = bareosfd_stub
        plugin, _ = self.daemon.new_plugin(self.plugindef(), level="I", accurate=0)
        rc, _ = self.daemon.backup(plugin)
        self.assertEqual(rc, stub.bRC_Error)
        self.assertTrue(any("Accurate" in m for _, m in stub.messages))
        self.check_clean()


@unittest.skipUnless(AVAILABLE, "the cloud package of coder-2 is not complete yet")
class PluginVolumePathTests(unittest.TestCase):
    """Attach mode wiring of the plugin class"""

    @classmethod
    def setUpClass(cls):
        cls.module = load_plugin_module()

    def setUp(self):
        # pylint: disable-next=import-outside-toplevel
        from bareos_openstack import cloud, fake_backend

        self.cloud = cloud
        self.fake_backend = fake_backend
        self.tmp = tempfile.mkdtemp(prefix="openstack-volume-path-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.root = os.path.join(self.tmp, "fake")
        fake_backend.seed(self.root, SPEC)
        self.daemon = FileDaemon(self.module)

    def plugindef(self, **extra):
        parts = {
            "backend": "fake",
            "fake_root": self.root,
            "server_id": SERVER_ID,
            "chunk_size": "1MiB",
        }
        parts.update(extra)
        return ":".join("%s=%s" % kv for kv in parts.items())

    def new(self, **extra):
        """A parsed plugin whose working directory is the temporary directory"""
        plugin, rc = self.daemon.new_plugin(self.plugindef(**extra))
        self.assertEqual(rc, bareosfd_stub.bRC_OK, bareosfd_stub.messages)
        plugin.workingdir = self.tmp
        return plugin

    def start_backup(self, plugin):
        stub = bareosfd_stub
        rc = plugin.handle_plugin_event(stub.bEventStartBackupJob)
        plugin.handle_plugin_event(stub.bEventEndBackupJob)
        plugin.handle_plugin_event(stub.bEventJobEnd)
        return rc

    def attach_backup(self):
        plugin = self.new()
        rc, files = self.daemon.backup(plugin)
        self.assertEqual(rc, bareosfd_stub.bRC_OK, bareosfd_stub.messages)
        return files

    def start_restore(self, plugin, files):
        """Deliver the documents and run only the start of a restore"""
        stub = bareosfd_stub
        for entry in files:
            if entry["type"] == stub.FT_RESTORE_FIRST:
                rop = stub.RestoreObject()
                rop.object_name = entry["fname"]
                rop.object = entry["object"]
                rop.object_len = len(entry["object"])
                rop.jobid = 1
                self.assertEqual(plugin.restore_object_data(rop), stub.bRC_OK)
        return plugin.handle_plugin_event(stub.bEventStartRestoreJob)

    def texts(self):
        return [m for _, m in bareosfd_stub.messages]

    def test_backup_backend_always_uses_the_proxy(self):
        stub = bareosfd_stub
        plugin = self.new()
        with mock.patch.object(
            self.module.cloud,
            "create_backend",
            side_effect=self.cloud.CloudError("stop"),
        ) as create:
            rc = self.start_backup(plugin)
        self.assertEqual(rc, stub.bRC_Error)
        create.assert_called_once()
        self.assertIs(create.call_args.kwargs["proxy"], True)

    def test_sessions_get_no_transfer_keywords(self):
        stub = bareosfd_stub
        plugin = self.new()
        with mock.patch.object(self.module, "BackupCloudSession") as session:
            session.return_value.prepare.side_effect = self.cloud.CloudError("stop")
            self.assertEqual(self.start_backup(plugin), stub.bRC_Error)
        removed = {
            "transfer_mode",
            "transfer_format",
            "transfer_dir",
            "qemu_img",
            "skip_ephemeral",
            "ignore_ephemeral_disk",
        }
        self.assertFalse(removed & set(session.call_args.kwargs))

    def test_restore_session_gets_no_transfer_keywords(self):
        files = self.attach_backup()
        plugin = self.new()
        with mock.patch.object(self.module, "RestoreCloudSession") as session:
            session.return_value.prepare_volumes.return_value = {
                VOLUME_ID: os.path.join(self.tmp, "x.raw")
            }
            self.start_restore(plugin, files)
        removed = {"transfer_mode", "transfer_format", "transfer_dir", "qemu_img"}
        self.assertFalse(removed & set(session.call_args.kwargs))

    def test_attach_restore_keeps_the_write_mode_of_the_option(self):
        files = self.attach_backup()
        for zeros, mode in (("yes", "always"), ("no", "if_written")):
            plugin = self.new(restore_write_zeros=zeros)
            path = os.path.join(self.tmp, "t-%s.raw" % zeros)
            with mock.patch.object(
                self.module, "RestoreCloudSession"
            ) as session, mock.patch.object(
                self.module.chunks, "RestoreTarget"
            ) as target:
                session.return_value.prepare_volumes.return_value = {VOLUME_ID: path}
                rc = self.start_restore(plugin, files)
            self.assertEqual(rc, bareosfd_stub.bRC_OK, bareosfd_stub.messages)
            target.assert_called_once_with(path, 3 * MIB, MIB, 3, mode, create=False)

    def test_cloud_restore_creates_the_backend_with_proxy(self):
        files = self.attach_backup()
        plugin = self.new()
        with mock.patch.object(
            self.module.cloud, "create_backend", wraps=self.cloud.create_backend
        ) as create, mock.patch.object(self.module, "RestoreCloudSession") as session:
            session.return_value.prepare_volumes.return_value = {
                VOLUME_ID: os.path.join(self.tmp, "x.raw")
            }
            self.start_restore(plugin, files)
        self.assertIs(create.call_args.kwargs["proxy"], True)

    def test_local_restore_creates_no_backend(self):
        stub = bareosfd_stub
        files = self.attach_backup()
        target = os.path.join(self.tmp, "restore")
        os.makedirs(target)
        plugin = self.new(restore_target="local", restore_local_dir=target)
        with mock.patch.object(self.module.cloud, "create_backend") as create:
            rc = self.daemon.restore(plugin, files)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        create.assert_not_called()


if __name__ == "__main__":
    unittest.main()
