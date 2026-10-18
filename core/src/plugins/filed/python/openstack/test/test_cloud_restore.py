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

"""Unit tests of the restore cloud session against the fake cloud."""

import contextlib
import copy
import io
import json
import tempfile
import unittest
from unittest import mock

from bareos_openstack import fake_backend, metadata, tool
from bareos_openstack.cloud import (
    CloudCancelled,
    CloudError,
    CloudNotFound,
    CloudTimeout,
    CloudTransferAccepted,
    VolumeInfo,
)
from bareos_openstack.cloud_backup import BackupCloudSession
from bareos_openstack.cloud_restore import (
    RestoreCloudOptions,
    RestoreCloudSession,
    RestoreError,
)

from test_cloud_fake import (
    DATA_VOL,
    PORT,
    PROJECT,
    PROXY,
    ROOT_VOL,
    SERVER,
    TARGET,
    TARGET_SG,
    batch_d_spec,
    make_cloud,
    read_state,
    target_backend,
)

MIB = 1024 * 1024
JOB = 77


class Collector:
    def __init__(self):
        self.lines = []

    def __call__(self, level, message):
        self.lines.append((level, message))

    def has(self, level, text):
        return any(lv == level and text in msg for lv, msg in self.lines)

    def find(self, level, text):
        return [m for lv, m in self.lines if lv == level and text in m]


class RestoreSessionTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.be, _ = make_cloud(self._tmp.name)
        self.log = Collector()
        backup = BackupCloudSession(self.be, 5, server_id=SERVER, poll_timeout=5)
        inv = backup.prepare()
        backup.cleanup()
        states = {
            ROOT_VOL: {"size_bytes": 8 * MIB, "chunk_count": 2},
            DATA_VOL: {"size_bytes": 4 * MIB, "chunk_count": 1},
        }
        job = {"job_id": 5, "level": "F", "backup_time": 1, "plugin_version": "0.1.0"}
        chunking = {"chunk_size": 4 * MIB, "hash": "sha256", "hijacked_stat_fields": []}
        self.doc = metadata.build_document(inv, states, job, chunking)

    def tearDown(self):
        self._tmp.cleanup()

    # helpers ---------------------------------------------------------------------

    def session(self, doc=None, **opts):
        return RestoreCloudSession(
            self.be,
            doc or self.doc,
            JOB,
            RestoreCloudOptions(**opts),
            poll_timeout=5,
            log=self.log,
        )

    def restore(self, doc=None, complete=True, **opts):
        sess = self.session(doc, **opts)
        sess.precheck()
        paths = sess.prepare_volumes()
        return sess, paths, sess.finish(complete)

    def fault(self, op, **spec):
        with self.be._locked() as state:
            state["faults"][op] = spec

    def state(self):
        return read_state(self.be)

    def new_servers(self):
        servers = self.state()["servers"].values()
        return [s for s in servers if s["name"].endswith("-restore-77")]

    def stop_original(self):
        self.be.stop_server(SERVER, 5)

    # precheck ----------------------------------------------------------------------

    def test_precheck_missing_flavor(self):
        with self.assertRaises(RestoreError) as ctx:
            self.session(flavor="huge").precheck()
        self.assertIn("restore_flavor", str(ctx.exception))

    def test_precheck_missing_network(self):
        doc = copy.deepcopy(self.doc)
        doc["ports"][0]["network_id"] = "net-9"
        with self.assertRaises(RestoreError) as ctx:
            self.session(doc).precheck()
        self.assertIn("restore_network", str(ctx.exception))
        with self.assertRaises(RestoreError):
            self.session(network="net-9").precheck()
        self.session(doc, network="net-1").precheck()

    def test_precheck_force_rules(self):
        with self.assertRaises(RestoreError) as ctx:
            self.session(force=True).precheck()
        self.assertIn("SHUTOFF", str(ctx.exception))
        self.stop_original()
        sess = self.session(force=True)
        sess.precheck()
        self.assertEqual(sess.old_server.id, SERVER)
        real = self.be.get_server

        def foreign(server_id):
            info = real(server_id)
            info.project_id = "other"
            return info

        # the server reports an owner other than the saved project
        with mock.patch.object(self.be, "get_server", side_effect=foreign):
            with self.assertRaises(RestoreError):
                self.session(force=True).precheck()
        self.be.delete_server(SERVER, 5)
        sess = self.session(force=True)
        sess.precheck()
        self.assertIsNone(sess.old_server)
        self.assertTrue(self.log.has("warning", "nothing to replace"))

    def test_precheck_force_needs_boot_volume_document(self):
        doc = copy.deepcopy(self.doc)
        doc["boot_from_volume"] = False
        del doc["root_restorable"]
        doc["volumes"][0]["boot_index"] = None
        with self.assertRaises(RestoreError):
            self.session(doc, force=True).precheck()
        self.session(doc).precheck()
        self.assertTrue(self.log.has("warning", "without a server"))

    def test_keep_old_volumes_without_force_warns(self):
        self.session(force_keep_old_volumes=True).precheck()
        self.assertTrue(self.log.has("warning", "no effect without force"))

    # volumes -----------------------------------------------------------------------

    def test_prepare_volumes(self):
        sess = self.session(volume_type="hdd", availability_zone="az2")
        sess.precheck()
        paths = sess.prepare_volumes()
        self.assertEqual(set(paths), {ROOT_VOL, DATA_VOL})
        for saved_id, path in paths.items():
            with open(path, "r+b") as fh:
                fh.write(b"x")
            rec = self.state()["volumes"][sess.restored[saved_id]]
            self.assertEqual(rec["metadata"]["bareos-role"], "restore-volume")
            self.assertEqual(rec["metadata"]["bareos-source-volume-id"], saved_id)
            self.assertEqual(rec["metadata"]["bareos-server-id"], SERVER)
            self.assertEqual(rec["volume_type"], "hdd")
            self.assertEqual(rec["availability_zone"], "az2")
            self.assertEqual(rec["status"], "in-use")
        self.assertTrue(
            self.state()["volumes"][sess.restored[ROOT_VOL]]["name"].endswith(
                "web01-root-restore-77"
            )
        )

    def test_prepare_volumes_fault_after_leaves_volume_for_abort(self):
        sess = self.session()
        sess.precheck()
        self.fault("create_volume", phase="after")
        with self.assertRaises(CloudTimeout):
            sess.prepare_volumes()
        self.assertEqual(len(sess.restored), 1)
        new_id = next(iter(sess.restored.values()))
        self.assertIn(new_id, self.state()["volumes"])
        self.assertEqual(sess.abort(), [new_id])
        self.assertIn(new_id, self.state()["volumes"])

    def test_abort_detaches_and_deletes_created_ports(self):
        sess = self.session()
        sess.precheck()
        sess.prepare_volumes()
        self.assertEqual(len(self.be.list_proxy_attachments()), 2)
        port = self.be.create_port("net-1", "tmp", [])
        sess.created_ports.append(port.id)
        ids = sess.abort()
        self.assertEqual(len(ids), 2)
        self.assertEqual(self.be.list_proxy_attachments(), [])
        self.assertIsNone(self.be.get_port(port.id))
        self.assertTrue(self.log.find("warning", "recovery:"))

    # finish: new server --------------------------------------------------------------

    def test_finish_creates_server(self):
        sess, paths, result = self.restore()
        self.assertTrue(result.server_id)
        self.assertEqual(self.be.list_proxy_attachments(), [])
        srv = self.state()["servers"][result.server_id]
        self.assertEqual(srv["name"], "web01-restore-77")
        self.assertEqual(srv["status"], "ACTIVE")
        self.assertEqual(srv["flavor_id"], "f-small")
        self.assertEqual(srv["key_name"], "k1")
        self.assertEqual(srv["availability_zone"], "nova")
        self.assertEqual(srv["metadata"]["role"], "web")
        self.assertEqual(srv["metadata"]["bareos-restored-from"], SERVER)
        self.assertEqual(srv["metadata"]["bareos-restore-job-id"], "77")
        first = srv["attachments"][0]
        self.assertEqual(first["boot_index"], 0)
        self.assertEqual(first["volume_id"], result.volume_ids[ROOT_VOL])
        self.assertEqual(first["device"], "/dev/vda")
        volumes = self.state()["volumes"]
        self.assertTrue(volumes[result.volume_ids[ROOT_VOL]]["bootable"])
        self.assertFalse(volumes[result.volume_ids[DATA_VOL]]["bootable"])
        port = self.state()["ports"][result.port_ids[0]]
        self.assertEqual(port["name"], "web01-port-restore-77")
        self.assertEqual(port["fixed_ips"], [])
        self.assertNotEqual(port["mac_address"], "fa:16:3e:00:00:01")
        self.assertEqual(port["device_id"], result.server_id)
        self.assertIn(SERVER, self.state()["servers"])
        self.assertEqual(result.deleted_old_volume_ids, [])

    def test_options_name_flavor_network(self):
        with self.be._locked() as state:
            state["flavors"]["f-big"] = {
                "id": "f-big",
                "name": "m1.big",
                "vcpus": 4,
                "ram": 1,
                "disk": 1,
                "extra_specs": {},
            }
            state["networks"]["net-2"] = {"id": "net-2", "name": "n2", "subnet_ids": []}
        _, _, result = self.restore(
            name="copy", flavor="m1.big", network="net-2", availability_zone="az9"
        )
        srv = self.state()["servers"][result.server_id]
        self.assertEqual(srv["name"], "copy")
        self.assertEqual(srv["flavor_id"], "f-big")
        self.assertEqual(srv["availability_zone"], "az9")
        port = self.state()["ports"][result.port_ids[0]]
        self.assertEqual(port["network_id"], "net-2")

    def test_powerstate(self):
        _, _, result = self.restore(powerstate="off")
        self.assertEqual(self.state()["servers"][result.server_id]["status"], "SHUTOFF")
        doc = copy.deepcopy(self.doc)
        doc["server"]["status"] = "SHUTOFF"
        _, _, result = self.restore(doc, powerstate="previous")
        self.assertEqual(self.state()["servers"][result.server_id]["status"], "SHUTOFF")
        _, _, result = self.restore(doc, powerstate="on")
        self.assertEqual(self.state()["servers"][result.server_id]["status"], "ACTIVE")
        _, _, result = self.restore(powerstate="previous")
        self.assertEqual(self.state()["servers"][result.server_id]["status"], "ACTIVE")

    def test_incomplete_restore_creates_nothing(self):
        sess, _, result = self.restore(complete=False)
        self.assertEqual(result.server_id, "")
        self.assertEqual(len(result.volume_ids), 2)
        self.assertEqual(self.new_servers(), [])
        self.assertEqual(self.be.list_proxy_attachments(), [])
        for new_id in result.volume_ids.values():
            self.assertEqual(self.state()["volumes"][new_id]["status"], "available")
        self.assertTrue(self.log.has("error", "restore incomplete"))
        self.assertTrue(self.log.has("error", "recovery:"))

    def test_document_without_boot_volume_restores_volumes_only(self):
        doc = copy.deepcopy(self.doc)
        doc["boot_from_volume"] = False
        del doc["root_restorable"]
        doc["volumes"][0]["boot_index"] = None
        _, _, result = self.restore(doc)
        self.assertEqual(result.server_id, "")
        self.assertEqual(len(result.volume_ids), 2)
        self.assertEqual(self.new_servers(), [])

    # keep_addresses -------------------------------------------------------------------

    def test_keep_addresses_bound_port_gets_new_port(self):
        _, _, result = self.restore(keep_addresses=True)
        self.assertNotEqual(result.port_ids[0], PORT)
        self.assertTrue(self.log.has("warning", "still bound to %s" % SERVER))
        self.assertEqual(self.state()["ports"][result.port_ids[0]]["fixed_ips"], [])
        self.assertEqual(self.state()["ports"][PORT]["device_id"], SERVER)

    def test_keep_addresses_missing_port_is_recreated_with_addresses(self):
        self.be.delete_server(SERVER, 5)
        self.be.delete_port(PORT)
        _, _, result = self.restore(keep_addresses=True)
        port = self.state()["ports"][result.port_ids[0]]
        self.assertEqual(
            port["fixed_ips"], [{"subnet_id": "sub-1", "ip_address": "10.0.0.5"}]
        )
        self.assertEqual(port["mac_address"], "fa:16:3e:00:00:01")

    def test_keep_addresses_unbound_port_is_reused(self):
        self.be.delete_server(SERVER, 5)
        _, _, result = self.restore(keep_addresses=True)
        self.assertEqual(result.port_ids, [PORT])
        self.assertEqual(self.state()["ports"][PORT]["device_id"], result.server_id)

    # force ------------------------------------------------------------------

    def ops(self):
        return [e["op"] for e in self.state()["log"]]

    def test_force_without_keep_addresses_replaces_after_new_server(self):
        self.stop_original()
        _, _, result = self.restore(force=True)
        ops = self.ops()
        self.assertLess(ops.index("create_server"), ops.index("delete_server"))
        state = self.state()
        self.assertNotIn(SERVER, state["servers"])
        self.assertEqual(state["servers"][result.server_id]["status"], "ACTIVE")
        self.assertNotIn(ROOT_VOL, state["volumes"])
        self.assertNotIn(DATA_VOL, state["volumes"])
        self.assertEqual(
            sorted(result.deleted_old_volume_ids), sorted([ROOT_VOL, DATA_VOL])
        )
        self.assertNotIn(PORT, state["ports"])
        self.assertIn(result.port_ids[0], state["ports"])

    def test_force_keep_old_volumes(self):
        self.stop_original()
        _, _, result = self.restore(force=True, force_keep_old_volumes=True)
        state = self.state()
        self.assertNotIn(SERVER, state["servers"])
        self.assertEqual(state["volumes"][ROOT_VOL]["status"], "available")
        self.assertIn(DATA_VOL, state["volumes"])
        self.assertEqual(
            sorted(result.kept_old_volume_ids), sorted([ROOT_VOL, DATA_VOL])
        )
        self.assertEqual(result.deleted_old_volume_ids, [])

    def test_force_old_volume_conflict_keeps_volume(self):
        self.stop_original()
        self.fault("delete_volume", remaining=1, message="busy")
        _, _, result = self.restore(force=True)
        self.assertEqual(len(result.kept_old_volume_ids), 1)
        self.assertEqual(len(result.deleted_old_volume_ids), 1)
        self.assertTrue(self.log.has("warning", "kept"))

    def test_force_with_keep_addresses_deletes_old_server_first(self):
        self.stop_original()
        _, _, result = self.restore(force=True, keep_addresses=True)
        ops = self.ops()
        self.assertLess(ops.index("delete_server"), ops.index("create_server"))
        self.assertEqual(result.port_ids, [PORT])
        state = self.state()
        self.assertIn(PORT, state["ports"])
        self.assertEqual(state["ports"][PORT]["device_id"], result.server_id)
        self.assertNotIn(ROOT_VOL, state["volumes"])

    def test_force_keep_addresses_server_failure_keeps_everything(self):
        self.stop_original()
        sess = self.session(force=True, keep_addresses=True)
        sess.precheck()
        sess.prepare_volumes()
        self.fault("create_server", remaining=1, message="no capacity")
        with self.assertRaises(RestoreError):
            sess.finish(True)
        state = self.state()
        self.assertNotIn(SERVER, state["servers"])
        self.assertIn(ROOT_VOL, state["volumes"])
        self.assertIn(DATA_VOL, state["volumes"])
        self.assertIn(PORT, state["ports"])
        self.assertEqual(state["ports"][PORT]["device_id"], "")
        for new_id in sess.restored.values():
            self.assertIn(new_id, state["volumes"])
        line = self.log.find("error", "recovery:")[-1]
        self.assertIn("old server %s (deleted)" % SERVER, line)
        self.assertIn(ROOT_VOL, line)
        self.assertIn(PORT, line)
        for new_id in sess.restored.values():
            self.assertIn(new_id, line)
        self.assertEqual(self.new_servers(), [])

    def test_port_failure_deletes_created_but_not_reused(self):
        self.stop_original()
        doc = copy.deepcopy(self.doc)
        second = {
            "id": "gone-port",
            "network_id": "net-1",
            "mac_address": "fa:16:3e:00:00:09",
            "fixed_ips": [{"subnet_id": "sub-1", "ip_address": "10.0.0.9"}],
            "security_group_ids": [],
            "name": "second",
            "device_id": "",
        }
        third = dict(second, id="gone-port-2", name="third")
        third["fixed_ips"] = [{"subnet_id": "sub-1", "ip_address": "10.0.0.10"}]
        doc["ports"] += [second, third]
        sess = self.session(doc, force=True, keep_addresses=True)
        sess.precheck()
        sess.prepare_volumes()
        with self.assertRaises(RestoreError):
            sess.finish(True)
        state = self.state()
        self.assertIn(PORT, state["ports"])
        self.assertFalse(
            [p for p in state["ports"].values() if p["name"].endswith("-restore-77")]
            or [p for p in state["ports"].values() if p["name"] == "second"]
        )
        self.assertEqual(sess.created_ports, [])
        self.assertEqual(sess.reused_ports, [PORT])
        self.assertTrue(self.log.has("error", "recovery:"))

    def test_server_fault_after_deletes_server_keeps_volumes(self):
        sess = self.session()
        sess.precheck()
        sess.prepare_volumes()
        self.fault("create_server", phase="after", message="timed out")
        with self.assertRaises(RestoreError):
            sess.finish(True)
        state = self.state()
        self.assertEqual(self.new_servers(), [])
        for new_id in sess.restored.values():
            self.assertEqual(state["volumes"][new_id]["status"], "available")
        ports = state["ports"].values()
        self.assertEqual([p for p in ports if p["name"].endswith("-restore-77")], [])
        self.assertIn(SERVER, state["servers"])
        self.assertTrue(self.log.has("error", "recovery:"))

    def test_force_requires_old_server_still_shutoff_at_finish(self):
        self.stop_original()
        sess = self.session(force=True)
        sess.precheck()
        sess.prepare_volumes()
        self.be.start_server(SERVER, 5)
        with self.assertRaises(RestoreError):
            sess.finish(True)
        self.assertIn(SERVER, self.state()["servers"])
        self.assertIn(ROOT_VOL, self.state()["volumes"])
        self.assertEqual(self.new_servers(), [])

    def test_old_server_delete_failure_names_both(self):
        self.stop_original()
        sess = self.session(force=True)
        sess.precheck()
        sess.prepare_volumes()
        self.fault("delete_server", remaining=1, message="locked")
        with self.assertRaises(RestoreError):
            sess.finish(True)
        self.assertEqual(len(self.new_servers()), 1)
        self.assertIn(SERVER, self.state()["servers"])
        self.assertTrue(self.log.has("error", self.new_servers()[0]["id"]))

    # lifecycle of the new server ---------------------------------------------------

    def _cancel_after_create(self):
        original = self.be.create_server

        def create(*args, **kwargs):
            original(*args, **kwargs)
            raise CloudCancelled("cancelled after allocation")

        self.be.create_server = create

    def _prepared(self, **opts):
        sess = self.session(**opts)
        sess.precheck()
        sess.prepare_volumes()
        return sess

    def test_cancel_after_allocation_rolls_back_on_abort(self):
        sess = self._prepared()
        self._cancel_after_create()
        with self.assertRaises(CloudCancelled):
            sess.finish(True)
        self.assertEqual(len(self.new_servers()), 1)
        ids = sess.abort()
        self.assertEqual(self.new_servers(), [])
        state = self.state()
        self.assertEqual(
            [p for p in state["ports"].values() if p["name"].endswith("-restore-77")],
            [],
        )
        for new_id in ids:
            self.assertEqual(state["volumes"][new_id]["status"], "available")
        self.assertIn(SERVER, state["servers"])
        text = "new server %s (deleted)" % sess.new_server_id
        self.assertTrue(self.log.has("warning", text))

    def test_stop_server_failure_rolls_back(self):
        sess = self._prepared(powerstate="off")
        self.fault("stop_server", remaining=1, message="power failure")
        with self.assertRaises(RestoreError):
            sess.finish(True)
        self.assertEqual(self.new_servers(), [])
        state = self.state()
        self.assertEqual(
            [p for p in state["ports"].values() if p["name"].endswith("-restore-77")],
            [],
        )
        self.assertEqual(len(sess.abort()), 2)
        line = self.log.find("error", "recovery:")[-1]
        self.assertIn("recovery: new server %s (deleted)" % sess.new_server_id, line)
        self.assertIn("new ports - still existing (kept)", line)

    def test_stop_failure_after_old_server_deleted_keeps_new_server(self):
        self.stop_original()
        sess = self._prepared(force=True, keep_addresses=True, powerstate="off")
        self.fault("stop_server", remaining=1, message="power failure")
        with self.assertRaises(RestoreError):
            sess.finish(True)
        sess.abort()
        state = self.state()
        new_id = sess.new_server_id
        self.assertIn(new_id, state["servers"])
        self.assertEqual(state["ports"][PORT]["device_id"], new_id)
        line = self.log.find("error", "recovery:")[-1]
        self.assertIn("new server %s (kept)" % new_id, line)
        self.assertIn(ROOT_VOL, state["volumes"])

    def test_old_server_delete_failure_keeps_new_server_and_ports(self):
        self.stop_original()
        sess = self._prepared(force=True)
        self.fault("delete_server", remaining=1, message="locked")
        with self.assertRaises(RestoreError):
            sess.finish(True)
        sess.abort()
        state = self.state()
        new_id = sess.new_server_id
        self.assertIn(new_id, state["servers"])
        self.assertIn(SERVER, state["servers"])
        bound = [p for p in state["ports"].values() if p["device_id"] == new_id]
        self.assertEqual(len(bound), 1)
        line = self.log.find("error", "recovery:")[-1]
        self.assertIn("new server %s (kept)" % new_id, line)

    def test_server_delete_failure_during_rollback_is_reported(self):
        sess = self._prepared()
        self._cancel_after_create()
        with self.assertRaises(CloudCancelled):
            sess.finish(True)
        self.fault("delete_server", remaining=1, message="stuck")
        sess.abort()
        state = self.state()
        self.assertIn(sess.new_server_id, state["servers"])
        new_id = sess.new_server_id
        bound = [p for p in state["ports"].values() if p["device_id"] == new_id]
        self.assertEqual(len(bound), 1)
        self.assertTrue(self.log.has("warning", "could not delete new server"))
        self.assertIn("new server %s (kept)" % new_id, self.log.lines[-1][1])
        sess.abort()
        self.assertNotIn(sess.new_server_id, self.state()["servers"])

    def test_boot_volume_image_metadata_is_set_on_the_restored_root(self):
        doc = copy.deepcopy(self.doc)
        doc["volumes"][0]["image_metadata"] = {"hw_firmware_type": "uefi"}
        _, _, result = self.restore(doc)
        self.assertTrue(result.server_id)
        state = self.state()
        self.assertEqual(
            state["volumes"][result.volume_ids[ROOT_VOL]]["image_metadata"],
            {"hw_firmware_type": "uefi"},
        )
        self.assertEqual(
            state["volumes"][result.volume_ids[DATA_VOL]]["image_metadata"], {}
        )

    def test_created_ports_are_tagged_and_reused_ports_are_not(self):
        _, _, result = self.restore()
        tags = self.state()["ports"][result.port_ids[0]]["tags"]
        self.assertEqual(
            tags, ["bareos-restore-job-77", "bareos-restored-from-%s" % SERVER]
        )
        self.be.delete_server(SERVER, 5)
        _, _, result = self.restore(keep_addresses=True)
        self.assertEqual(result.port_ids, [PORT])
        self.assertEqual(self.state()["ports"][PORT].get("tags", []), [])


class BatchDRestoreTest(unittest.TestCase):
    """Restore to images and into another project (plan C.10.1)."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.log = Collector()
        self.make(batch_d_spec())

    def tearDown(self):
        self._tmp.cleanup()

    def make(self, spec):
        self._tmp.cleanup()
        self._tmp = tempfile.TemporaryDirectory()
        self.be, _ = make_cloud(self._tmp.name, spec)
        self.tbe = target_backend(self.be)
        backup = BackupCloudSession(self.be, 5, server_id=SERVER, poll_timeout=5)
        inv = backup.prepare()
        backup.cleanup()
        states = {
            ROOT_VOL: {"size_bytes": 8 * MIB, "chunk_count": 2},
            DATA_VOL: {"size_bytes": 4 * MIB, "chunk_count": 1},
        }
        job = {"job_id": 5, "level": "F", "backup_time": 1, "plugin_version": "0.1.0"}
        chunking = {"chunk_size": 4 * MIB, "hash": "sha256", "hijacked_stat_fields": []}
        self.doc = metadata.build_document(inv, states, job, chunking)

    def session(self, cross=False, tgt=None, **opts):
        return RestoreCloudSession(
            self.be,
            self.doc,
            JOB,
            RestoreCloudOptions(**opts),
            poll_timeout=5,
            log=self.log,
            target_backend=(tgt or self.tbe) if cross else None,
        )

    def run_restore(self, cross=False, **opts):
        sess = self.session(cross, **opts)
        sess.precheck()
        sess.prepare_volumes()
        return sess, sess.finish(True)

    def state(self):
        return read_state(self.be)

    def originals(self):
        return {ROOT_VOL, DATA_VOL}

    # restore_target=image -----------------------------------------------------

    # cross-project ------------------------------------------------------------

    def test_cross_project_restore_builds_the_server_in_the_target(self):
        keys = []

        def spy(volume_id, name, on_created=None):
            tid, key = self.be.__class__.create_transfer(
                self.be, volume_id, name, on_created
            )
            keys.append((name, key))
            return tid, key

        with mock.patch.object(self.be, "create_transfer", side_effect=spy):
            _, result = self.run_restore(cross=True)
        state = self.state()
        server = state["servers"][result.server_id]
        self.assertEqual(server["project_id"], TARGET)
        self.assertNotIn(result.server_id, [s.id for s in self.be.list_servers()])
        for new_id in result.volume_ids.values():
            self.assertEqual(state["volumes"][new_id]["project_id"], TARGET)
            self.assertEqual(state["volumes"][new_id]["status"], "in-use")
        self.assertEqual(
            sorted(name for name, _ in keys),
            sorted("bareos-77-xfer-%s" % v for v in (ROOT_VOL, DATA_VOL)),
        )
        self.assertEqual(state.get("transfers", {}), {})
        port = state["ports"][result.port_ids[0]]
        self.assertEqual(port["project_id"], TARGET)
        self.assertEqual(port["security_group_ids"], [TARGET_SG])
        self.assertEqual(
            port["tags"], ["bareos-restore-job-77", "bareos-restored-from-%s" % SERVER]
        )
        everything = json.dumps(state["log"]) + json.dumps(self.log.lines)
        for _, key in keys:
            self.assertNotIn(key, everything)
        self.assertEqual(len(self.tbe.list_proxy_attachments()), 0)

    def test_cross_project_precheck_runs_before_any_volume_exists(self):
        before = set(self.state()["volumes"])
        with self.assertRaises(RestoreError):
            self.session(cross=True, tgt=target_backend(self.be, PROJECT)).precheck()
        with self.assertRaises(RestoreError) as ctx:
            self.session(cross=True, volume_type="gold").precheck()
        self.assertIn("volume type gold", str(ctx.exception))
        with self.assertRaises(RestoreError) as ctx:
            self.session(cross=True, availability_zone="zone-x").precheck()
        self.assertIn("availability zone zone-x", str(ctx.exception))
        with self.assertRaises(RestoreError):
            self.session(cross=True, force=True).precheck()
        with self.assertRaises(RestoreError):
            self.session(cross=True, keep_addresses=True).precheck()
        with self.assertRaises(RestoreError):
            self.session(cross=True, flavor="huge").precheck()
        self.assertEqual(set(self.state()["volumes"]), before)

    def test_same_project_is_refused(self):
        same = target_backend(self.be, PROJECT)
        with self.assertRaises(RestoreError) as ctx:
            self.session(cross=True, tgt=same).precheck()
        self.assertIn("normal restore", str(ctx.exception))

    def test_security_groups_are_mapped_by_name(self):
        with self.be._locked() as state:
            del state["projects"][TARGET]["security_groups"][TARGET_SG]
        with self.assertRaises(RestoreError) as ctx:
            self.session(cross=True).precheck()
        self.assertIn("security group default", str(ctx.exception))
        with self.be._locked() as state:
            state["projects"][TARGET]["security_groups"]["tsg-2"] = {
                "id": "tsg-2",
                "name": "dr",
            }
        with self.assertRaises(RestoreError):
            self.session(cross=True).precheck()
        with self.assertRaises(RestoreError):
            self.session(cross=True, security_groups=("nope",)).precheck()
        _, result = self.run_restore(cross=True, security_groups=("dr",))
        port = self.state()["ports"][result.port_ids[0]]
        self.assertEqual(port["security_group_ids"], ["tsg-2"])

    def test_ambiguous_target_group_fails_the_precheck(self):
        with self.be._locked() as state:
            state["projects"][TARGET]["security_groups"]["tsg-2"] = {
                "id": "tsg-2",
                "name": "default",
            }
        with self.assertRaises(RestoreError):
            self.session(cross=True).precheck()

    def test_missing_source_group_needs_the_override(self):
        with self.be._locked() as state:
            del state["security_groups"]["sg-1"]
        with self.assertRaises(RestoreError) as ctx:
            self.session(cross=True).precheck()
        self.assertIn("restore_security_groups", str(ctx.exception))
        sess = self.session(cross=True, security_groups=("default",))
        sess.precheck()

    def test_accept_failure_deletes_the_transfer_and_keeps_the_volume(self):
        with self.be._locked() as state:
            state["faults"]["accept_transfer"] = {"remaining": 1, "message": "no"}
        sess = self.session(cross=True)
        sess.precheck()
        sess.prepare_volumes()
        with self.assertRaises(RestoreError) as ctx:
            sess.finish(True)
        self.assertIn("transferring volume", str(ctx.exception))
        state = self.state()
        self.assertEqual(state.get("transfers", {}), {})
        restored = set(state["volumes"]) - self.originals()
        self.assertEqual(len(restored), 2)
        for vol_id in restored:
            self.assertNotIn("project_id", state["volumes"][vol_id])
            self.assertEqual(state["volumes"][vol_id]["status"], "available")
        self.assertEqual(self.new_servers(), [])
        self.assertTrue(self.log.has("error", "not moved"))

    def new_servers(self):
        return [
            s
            for s in self.state()["servers"].values()
            if s["name"].endswith("-restore-77")
        ]

    def test_partial_transfer_reports_which_volumes_moved(self):
        real = self.tbe.accept_transfer
        calls = []

        def accept(transfer_id, key, timeout):
            calls.append(transfer_id)
            if len(calls) == 2:
                raise CloudError("quota")
            return real(transfer_id, key, timeout)

        sess = self.session(cross=True)
        sess.precheck()
        sess.prepare_volumes()
        with mock.patch.object(self.tbe, "accept_transfer", side_effect=accept):
            with self.assertRaises(RestoreError):
                sess.finish(True)
        self.assertEqual(len(sess.moved), 1)
        line = sess._recovery_line()
        self.assertIn("moved to the target project %s" % sess.moved[0], line)
        self.assertEqual(self.state().get("transfers", {}), {})
        self.assertEqual(self.tbe.get_volume(sess.moved[0]).status, "available")

    def test_cancel_during_accept_is_rolled_back_by_abort(self):
        sess = self.session(cross=True)
        sess.precheck()
        sess.prepare_volumes()
        with mock.patch.object(
            self.tbe, "accept_transfer", side_effect=CloudCancelled("stop")
        ):
            with self.assertRaises(CloudCancelled):
                sess.finish(True)
        self.assertEqual(len(self.state()["transfers"]), 1)
        self.assertTrue(self.log.has("error", "pending transfers"))
        sess.abort()
        state = self.state()
        self.assertEqual(state["transfers"], {})
        for vol_id in set(state["volumes"]) - self.originals():
            self.assertEqual(state["volumes"][vol_id]["status"], "available")

    def test_server_failure_in_the_target_rolls_back_ports_only(self):
        with self.be._locked() as state:
            state["faults"]["create_server"] = {"remaining": 1, "message": "boom"}
        sess = self.session(cross=True)
        sess.precheck()
        sess.prepare_volumes()
        with self.assertRaises(RestoreError):
            sess.finish(True)
        state = self.state()
        self.assertEqual(self.new_servers(), [])
        created = [p for p in state["ports"].values() if p.get("project_id") == TARGET]
        self.assertEqual(created, [])
        moved = set(state["volumes"]) - self.originals()
        self.assertEqual(len(moved), 2)
        for vol_id in moved:
            self.assertEqual(state["volumes"][vol_id]["project_id"], TARGET)
        self.assertEqual(len(sess.moved), 2)
        self.assertTrue(self.log.has("error", "recovery:"))

    # Round 11 ---------------------------------------------------------------

    def tool_output(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        args = ["--backend", "fake", "--fake-root", self.be._root] + list(argv)
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = tool.main(args)
        return code, out.getvalue()

    # ownership after a transfer step ----------------------------------------

    def accept_then(self, exc):
        real = self.tbe.accept_transfer

        def call(transfer_id, key, timeout):
            real(transfer_id, key, timeout)
            raise exc

        return call

    def transfer_session(self):
        sess = self.session(cross=True)
        sess.precheck()
        sess.prepare_volumes()
        return sess

    def line_of(self, sess):
        return sess._recovery_line()

    def test_accept_then_wait_timeout_is_reported_as_moved(self):
        for exc in (CloudTimeout("late"), CloudTransferAccepted("late")):
            self.make(batch_d_spec())
            sess = self.transfer_session()
            with mock.patch.object(
                self.tbe, "accept_transfer", side_effect=self.accept_then(exc)
            ):
                with self.assertRaises(RestoreError):
                    sess.finish(True)
            self.assertEqual(len(sess.moved), 1)
            moved = sess.moved[0]
            line = self.line_of(sess)
            self.assertIn("moved to the target project %s," % moved, line)
            other = [v for v in sess.restored.values() if v != moved][0]
            self.assertIn("not moved %s," % other, line)
            self.assertIn("ownership unknown -", line)
            self.assertEqual(self.tbe.get_volume(moved).id, moved)
            self.assertEqual(self.state().get("transfers", {}), {})

    def test_accept_then_cancel_keeps_the_moved_volume_after_abort(self):
        sess = self.transfer_session()
        with mock.patch.object(
            self.tbe,
            "accept_transfer",
            side_effect=self.accept_then(CloudCancelled("stop")),
        ):
            with self.assertRaises(CloudCancelled):
                sess.finish(True)
        moved = list(sess.moved)
        self.assertEqual(len(moved), 1)
        sess.abort()
        self.assertEqual(sess.moved, moved)
        line = self.line_of(sess)
        self.assertIn("moved to the target project %s," % moved[0], line)
        self.assertEqual(self.tbe.get_volume(moved[0]).id, moved[0])
        self.assertEqual(self.state().get("transfers", {}), {})

    def test_rejection_before_acceptance_is_not_moved(self):
        with self.be._locked() as state:
            state["faults"]["accept_transfer"] = {"remaining": 1, "message": "no"}
        sess = self.transfer_session()
        with self.assertRaises(RestoreError):
            sess.finish(True)
        self.assertEqual(sess.moved, [])
        line = self.line_of(sess)
        self.assertIn("moved to the target project -,", line)
        self.assertIn("ownership unknown -", line)
        self.assertEqual(self.state().get("transfers", {}), {})

    def test_failed_probe_is_unknown_not_moved(self):
        with self.be._locked() as state:
            state["faults"]["accept_transfer"] = {"remaining": 1, "message": "no"}
        sess = self.transfer_session()
        with mock.patch.object(
            self.tbe, "get_volume", side_effect=CloudError("api down")
        ):
            with self.assertRaises(RestoreError):
                sess.finish(True)
        self.assertEqual(sess.moved, [])
        first = list(sess.restored.values())[0]
        self.assertIn(first, sess.unknown_owner)
        line = self.line_of(sess)
        self.assertIn("ownership unknown", line)
        self.assertNotIn("not moved %s" % first, line)

    def test_contradictory_probes_are_unknown(self):
        present = VolumeInfo(id="x")
        cases = {
            "both": (lambda v: present, lambda v: present),
            "neither": (CloudNotFound("gone"), CloudNotFound("gone")),
        }
        for name, (target_effect, source_effect) in cases.items():
            self.make(batch_d_spec())
            with self.be._locked() as state:
                state["faults"]["accept_transfer"] = {"remaining": 1}
            sess = self.transfer_session()
            with mock.patch.object(
                self.tbe, "get_volume", side_effect=target_effect
            ), mock.patch.object(self.be, "get_volume", side_effect=source_effect):
                with self.assertRaises(RestoreError):
                    sess.finish(True)
            line = self.line_of(sess)
            ids = list(sess.restored.values())
            self.assertEqual(sess.moved, [], name)
            self.assertEqual(sorted(sess.unknown_owner), sorted(ids), name)
            self.assertIn("not moved -,", line, name)

    def test_unfinished_ownership_never_leaks_the_auth_key(self):
        keys = []
        real = self.be.create_transfer

        def spy(volume_id, name, on_created=None):
            tid, key = real(volume_id, name, on_created)
            keys.append(key)
            return tid, key

        sess = self.transfer_session()
        with mock.patch.object(self.be, "create_transfer", side_effect=spy):
            with mock.patch.object(
                self.tbe,
                "accept_transfer",
                side_effect=self.accept_then(CloudTimeout("late")),
            ):
                with self.assertRaises(RestoreError) as ctx:
                    sess.finish(True)
        text = str(ctx.exception) + json.dumps(self.log.lines) + self.line_of(sess)
        text += json.dumps(self.state()["log"])
        self.assertTrue(keys)
        for key in keys:
            self.assertNotIn(key, text)

class TransferModeRestoreTest(unittest.TestCase):
    """B2 restore: the provider writes and attaches, the tenant receives and boots."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.log = Collector()
        spec = batch_d_spec(proxy_project_id=TARGET)
        self.tbe, _ = make_cloud(
            self._tmp.name, spec, proxy_instance_id="", proxy=False
        )
        self.pbe = fake_backend.FakeBackend(
            {
                "backend": "fake",
                "fake_root": self.tbe._root,
                "fake_project_id": TARGET,
                "proxy_instance_id": PROXY,
            }
        )
        self.pbe.connect()
        backup = BackupCloudSession(self.tbe, 5, server_id=SERVER, poll_timeout=5)
        inv = backup.prepare()
        backup.cleanup()
        states = {
            ROOT_VOL: {"size_bytes": 8 * MIB, "chunk_count": 2},
            DATA_VOL: {"size_bytes": 4 * MIB, "chunk_count": 1},
        }
        job = {"job_id": 5, "level": "F", "backup_time": 1, "plugin_version": "0.1.0"}
        chunking = {"chunk_size": 4 * MIB, "hash": "sha256", "hijacked_stat_fields": []}
        self.doc = metadata.build_document(inv, states, job, chunking)

    def tearDown(self):
        self._tmp.cleanup()

    def session(self, groups_in_target=True, **opts):
        return RestoreCloudSession(
            self.pbe,
            self.doc,
            JOB,
            RestoreCloudOptions(**opts),
            poll_timeout=5,
            log=self.log,
            target_backend=self.tbe,
            groups_in_target=groups_in_target,
        )

    def state(self):
        return read_state(self.tbe)

    def test_restore_reverses_the_transfer_direction(self):
        sess = self.session()
        sess.precheck()
        sess.prepare_volumes()
        for new_id in sess.restored.values():
            self.assertEqual(self.state()["volumes"][new_id]["project_id"], TARGET)
        result = sess.finish(True)
        state = self.state()
        self.assertEqual(state["servers"][result.server_id]["project_id"], PROJECT)
        for new_id in result.volume_ids.values():
            self.assertEqual(state["volumes"][new_id].get("project_id", PROJECT), PROJECT)
            self.assertEqual(state["volumes"][new_id]["status"], "in-use")
        port = state["ports"][result.port_ids[0]]
        self.assertEqual(port["security_group_ids"], ["sg-1"])
        self.assertEqual(state.get("transfers", {}), {})

    def test_saved_groups_are_read_through_the_tenant(self):
        with self.assertRaises(RestoreError) as ctx:
            self.session(groups_in_target=False).precheck()
        self.assertIn("restore_security_groups", str(ctx.exception))
        self.session(groups_in_target=True).precheck()

    def test_tenant_quota_refusal_keeps_the_volumes_in_the_provider(self):
        with self.tbe._locked() as state:
            state["quota"] = {state["project_id"]: {"volumes": 0}}
        sess = self.session()
        sess.precheck()
        sess.prepare_volumes()
        with self.assertRaises(RestoreError) as ctx:
            sess.finish(True)
        self.assertIn("transferring volume", str(ctx.exception))
        state = self.state()
        self.assertEqual(state.get("transfers", {}), {})
        for new_id in sess.restored.values():
            self.assertEqual(state["volumes"][new_id]["project_id"], TARGET)
        kept = sess.abort()
        self.assertEqual(sorted(kept), sorted(sess.restored.values()))
        for new_id in sess.restored.values():
            self.assertEqual(self.state()["volumes"][new_id]["project_id"], TARGET)
            self.assertEqual(self.state()["volumes"][new_id]["status"], "available")
        self.assertIn("moved to the target project -,", sess._recovery_line())


if __name__ == "__main__":
    unittest.main()
