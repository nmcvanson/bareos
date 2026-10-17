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
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public
# License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

"""Integration tests for the OpenStack plugin and its file-backed fake."""

import base64
import contextlib
import copy
import io
import json
import os
import tempfile
import time
import unittest

import bareosfd_stub
from bareos_openstack import fake_backend
from bareos_openstack.cloud import (
    CloudCancelled,
    CloudNotFound,
    CloudTransferAccepted,
)

from core_harness import Catalog, run_backup, run_restore
from test_cloud_fake import (
    DATA_VOL,
    IMG_SERVER,
    PORT,
    ROOT_VOL,
    SERVER,
    base_spec,
    image_booted_spec,
    add_groups,
    batch_d_spec,
)

MIB = 1024 * 1024
PROJECT = "11111111-1111-4111-8111-111111111111"
PROXY = "22222222-2222-4222-8222-222222222222"


class FakePluginIntegrationTests(unittest.TestCase):
    """Exercise the plugin against a fresh fake cloud for each scenario."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.join(self.tmp.name, "cloud")
        self.spec = base_spec()
        self.spec["servers"][0]["volumes"][0]["size_bytes"] = 6 * MIB
        self.spec["servers"][0]["volumes"][0]["content"] = {
            "random_seed": 4,
            "zero_ranges": [[2 * MIB, MIB]],
        }
        self.spec["servers"][0]["volumes"][1]["size_bytes"] = 2 * MIB
        self.spec["servers"][0]["volumes"][1]["content"] = {
            "random_seed": 5,
            "zero_ranges": [[0, 2 * MIB]],
        }
        fake_backend.seed(self.root, self.spec)
        self.backend = fake_backend.FakeBackend(
            {"backend": "fake", "fake_root": self.root, "proxy_instance_id": PROXY}
        )
        self.backend.connect()
        self.catalog = Catalog()
        self.plugin_string = (
            "python3:module_name=bareos-fd-openstack:backend=fake:fake_root=%s:"
            "server_id=%s:chunk_size=1MiB" % (self.root, SERVER)
        )
        self.job_id = 1
        self.server_id = SERVER

    def tearDown(self):
        self.tmp.cleanup()

    def _backup(self, level="F", **kwargs):
        if level in "ID" and self.catalog.jobs:
            time.sleep(1.05)
        result = run_backup(
            self.catalog, self.plugin_string, level, self.job_id, **kwargs
        )
        self.job_id += 1
        return result

    def _use_image_server(self, volumes=True):
        """Replace the seeded cloud by one with an image-booted server."""
        self.spec = image_booted_spec(volumes=volumes)
        fake_backend.seed(self.root, self.spec, reset=True)
        self.server_id = IMG_SERVER
        self.plugin_string = self.plugin_string.replace(SERVER, IMG_SERVER)

    def _restored_server(self, name="img01"):
        return next(
            item
            for item in self._state()["servers"].values()
            if item["name"] == "%s-restore-%d" % (name, self.job_id + 100)
        )

    def _strip_wave1c_fields(self):
        """Rewrite stored documents in the Wave 1 shape (no root or image keys)."""
        for job in self.catalog.jobs:
            for obj in job["restore_objects"]:
                doc = json.loads(obj["object"])
                for key in ("boot_source", "root_restorable", "root_image"):
                    doc.pop(key, None)
                for volume in doc["volumes"]:
                    volume.pop("source", None)
                    volume.pop("image_metadata", None)
                obj["object"] = json.dumps(doc, sort_keys=True).encode("utf-8")

    def _write(self, volume_id, index, seed=None, zero=False, length=MIB):
        with open(self.backend._vol_file(volume_id), "r+b") as stream:
            stream.seek(index * MIB)
            stream.write(b"\0" * length if zero else bytes([seed or 91]) * length)

    def _fault(self, name, phase="before", sleep=0):
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            result = fake_backend.main(
                [
                    "fault",
                    "--root",
                    self.root,
                    "--op",
                    name,
                    "--phase",
                    phase,
                    "--sleep",
                    str(sleep),
                ]
            )
        self.assertEqual(result, 0)

    def _clean(self):
        state = self._state()
        leftovers = [
            volume
            for volume in state["volumes"].values()
            if volume["metadata"].get("bareos-role") == "backup-temp-volume"
        ]
        leftovers += [
            snapshot
            for snapshot in state["snapshots"].values()
            if snapshot["metadata"].get("bareos-role") == "backup-snapshot"
        ]
        leftovers += [
            image
            for image in state.get("images", {}).values()
            if image["properties"].get("bareos-role") == "backup-image"
        ]
        self.assertEqual(leftovers, [])
        self.assertEqual(self.backend.list_proxy_attachments(), [])

    def _state(self):
        with open(os.path.join(self.root, "state.json"), encoding="utf-8") as stream:
            return json.load(stream)

    def _save_state(self, state):
        with open(
            os.path.join(self.root, "state.json"), "w", encoding="utf-8"
        ) as stream:
            json.dump(state, stream)

    def _chunk_entries(self, job):
        return [
            entry for entry in job["files"] if entry["type"] == bareosfd_stub.FT_REG
        ]

    def _restore_local(self, upto_job=None, **kwargs):
        suffix = "latest" if upto_job is None else str(upto_job)
        target = os.path.join(self.tmp.name, "local-" + suffix)
        os.makedirs(target, exist_ok=True)
        result = run_restore(
            self.catalog,
            self.plugin_string,
            "python3:restore_target=local",
            target,
            self.job_id + 100,
            upto_job=upto_job,
            **kwargs
        )
        return result, os.path.join(target, self.server_id)

    def _restore_cloud(self, pluginoptions="python3:", upto_job=None, **kwargs):
        return run_restore(
            self.catalog,
            self.plugin_string,
            pluginoptions,
            "",
            self.job_id + 100,
            upto_job=upto_job,
            **kwargs
        )

    @staticmethod
    def _result_text(result):
        return "\n".join(str(message) for message in result.messages)

    def _prepare_cross_project_restore(self):
        self.spec = batch_d_spec(groups=False)
        fake_backend.seed(self.root, self.spec, reset=True)
        self.backend = fake_backend.FakeBackend(
            {"backend": "fake", "fake_root": self.root, "proxy_instance_id": PROXY}
        )
        self.backend.connect()
        self._backup()
        target_project = self.spec["projects"][0]["id"]
        credentials = os.path.join(self.tmp.name, "target-project.conf")
        with open(credentials, "w", encoding="utf-8") as stream:
            stream.write("backend=fake\nfake_project_id=%s\n" % target_project)
        return target_project, credentials

    def _run_cross_project_after_accept(self, error_type):
        target_project, credentials = self._prepare_cross_project_restore()
        original_accept = fake_backend.FakeBackend.accept_transfer
        accepted_keys = []
        injected = [False]

        def accept_then_raise(backend, transfer_id, auth_key, timeout):
            result = original_accept(backend, transfer_id, auth_key, timeout)
            if backend.project_id == target_project and not injected[0]:
                injected[0] = True
                accepted_keys.append(auth_key)
                raise error_type("simulated failure after accepted transfer")
            return result

        fake_backend.FakeBackend.accept_transfer = accept_then_raise
        try:
            result = self._restore_cloud(
                "python3:restore_project_credentials=" + credentials
            )
        finally:
            fake_backend.FakeBackend.accept_transfer = original_accept
        return target_project, result, accepted_keys

    def _assert_reconciled_transfer_recovery(self, target_project, result, keys):
        self.assertTrue(result.failed, result.messages)
        self.assertTrue(keys)
        state = self._state()
        restored = {
            volume_id: volume
            for volume_id, volume in state["volumes"].items()
            if volume["metadata"].get("bareos-role") == "restore-volume"
        }
        target_ids = {
            volume_id
            for volume_id, volume in restored.items()
            if volume.get("project_id") == target_project
        }
        source_ids = {
            volume_id
            for volume_id, volume in restored.items()
            if volume.get("project_id", PROJECT) == PROJECT
        }
        self.assertEqual(len(target_ids), 1, restored)
        self.assertEqual(len(source_ids), 1, restored)
        moved_id = next(iter(target_ids))
        not_moved_id = next(iter(source_ids))

        target = fake_backend.FakeBackend(
            {
                "backend": "fake",
                "fake_root": self.root,
                "fake_project_id": target_project,
                "proxy": False,
            }
        )
        target.connect()
        self.assertEqual(target.get_volume(moved_id).id, moved_id)
        with self.assertRaises(CloudNotFound):
            self.backend.get_volume(moved_id)
        self.assertEqual(self.backend.get_volume(not_moved_id).id, not_moved_id)
        with self.assertRaises(CloudNotFound):
            target.get_volume(not_moved_id)
        self.assertEqual(state.get("transfers", {}), {})

        text = self._result_text(result)
        self.assertTrue(all(key not in text for key in keys))
        recovery = [
            line
            for line in text.splitlines()
            if "volumes moved to the target project" in line
        ]
        self.assertTrue(recovery, text)
        for line in recovery:
            self.assertIn("volumes moved to the target project %s" % moved_id, line)
            self.assertIn("not moved %s" % not_moved_id, line)
            self.assertIn("pending transfers -", line)
            self.assertIn("ownership unknown -", line)
        aborted = [line for line in text.splitlines() if "restore aborted;" in line]
        self.assertTrue(aborted, text)
        self.assertTrue(
            any(
                "volumes moved to the target project %s" % moved_id in line
                and "not moved %s" % not_moved_id in line
                for line in aborted
            ),
            text,
        )
        target.close()
        return moved_id, not_moved_id

    def _volume_bytes(self, volume_id):
        with open(self.backend._vol_file(volume_id), "rb") as stream:
            return stream.read()

    def _path_for(self, entries, volume_id, index):
        return next(
            entry["fname"]
            for entry in entries
            if "/%s/%08d.chunk" % (volume_id, index) in entry["fname"]
        )

    def test_01_full_backup_clears_sparse_and_cleans(self):
        result = self._backup()
        self.assertFalse(result.failed, result.messages)
        chunks = self._chunk_entries(self.catalog.jobs[0])
        self.assertEqual(len(chunks), 8)
        self.assertEqual(sum(entry["no_read"] for entry in chunks), 3)
        self.assertEqual(len(result.restore_objects), 1)
        self.assertTrue(all(not (entry["flags"][0] & 32) for entry in chunks))
        self._clean()

    def test_02_incremental_saves_two_changed_chunks_and_document(self):
        self._backup()
        self._write(ROOT_VOL, 0, seed=44)
        self._write(ROOT_VOL, 4, seed=88)
        result = self._backup("I")
        self.assertFalse(result.failed, result.messages)
        self.assertEqual(len(self._chunk_entries(self.catalog.jobs[-1])), 2)
        self.assertEqual(len(result.restore_objects), 1)

    def test_03_differential_and_incremental_restore_snapshots(self):
        full = self._backup()
        first = self._volume_bytes(ROOT_VOL)
        self._write(ROOT_VOL, 1, seed=27)
        i1 = self._backup("I")
        after_i1 = self._volume_bytes(ROOT_VOL)
        self._write(ROOT_VOL, 5, seed=52)
        i2 = self._backup("I")
        after_i2 = self._volume_bytes(ROOT_VOL)
        diff = self._backup("D")
        changed = self._chunk_entries(self.catalog.jobs[-1])
        self.assertEqual(len(changed), 2)
        self.assertFalse(full.failed or i1.failed or i2.failed or diff.failed)
        for upto, expected in ((2, after_i1), (4, after_i2)):
            restored, folder = self._restore_local(upto_job=upto)
            self.assertFalse(restored.failed, restored.messages)
            with open(os.path.join(folder, ROOT_VOL + ".raw"), "rb") as stream:
                actual = stream.read()
            self.assertEqual(actual, expected)
        self.assertNotEqual(first, after_i1)

    def test_04_unchanged_incremental_contains_only_restore_object(self):
        full = self._backup()
        inc = self._backup("I")
        self.assertFalse(full.failed or inc.failed)
        self.assertEqual(len(self._chunk_entries(self.catalog.jobs[-1])), 0)
        self.assertEqual(len(inc.restore_objects), 1)
        restored, folder = self._restore_local()
        self.assertFalse(restored.failed, restored.messages)
        with open(os.path.join(folder, ROOT_VOL + ".raw"), "rb") as stream:
            self.assertEqual(stream.read(), self._volume_bytes(ROOT_VOL))

    def test_05_nonzero_to_zero_restores_local_and_cloud(self):
        self._backup()
        self._write(ROOT_VOL, 0, zero=True)
        inc = self._backup("I")
        entry = next(
            item
            for item in self._chunk_entries(self.catalog.jobs[-1])
            if "/%s/00000000.chunk" % ROOT_VOL in item["fname"]
        )
        self.assertTrue(entry["no_read"])
        local, folder = self._restore_local()
        self.assertFalse(local.failed, local.messages)
        with open(os.path.join(folder, ROOT_VOL + ".raw"), "rb") as stream:
            self.assertEqual(stream.read(MIB), b"\0" * MIB)
        cloud = self._restore_cloud()
        self.assertFalse(cloud.failed, cloud.messages)
        server = next(
            item
            for item in self._state()["servers"].values()
            if item["name"] == "web01-restore-%d" % (self.job_id + 100)
        )
        restored_id = server["attachments"][0]["volume_id"]
        self.assertEqual(self._volume_bytes(restored_id)[:MIB], b"\0" * MIB)
        self.assertFalse(inc.failed)

    def test_06_short_and_growing_last_chunk(self):
        state = self._state()
        state["volumes"][ROOT_VOL]["size_bytes"] = 6 * MIB + MIB // 2
        self._save_state(state)
        with open(self.backend._vol_file(ROOT_VOL), "r+b") as stream:
            stream.truncate(6 * MIB + MIB // 2)
        full = self._backup()
        state = self._state()
        state["volumes"][ROOT_VOL]["size_bytes"] = 7 * MIB + MIB // 4
        self._save_state(state)
        with open(self.backend._vol_file(ROOT_VOL), "r+b") as stream:
            stream.truncate(7 * MIB + MIB // 4)
            stream.seek(6 * MIB + MIB // 2)
            stream.write(b"G" * (3 * MIB // 4))
        incremental = self._backup("I")
        local, folder = self._restore_local()
        self.assertFalse(
            full.failed or incremental.failed or local.failed, local.messages
        )
        with open(os.path.join(folder, ROOT_VOL + ".raw"), "rb") as stream:
            self.assertEqual(stream.read(), self._volume_bytes(ROOT_VOL))

    def test_07_incremental_requires_accurate_and_supported_options(self):
        self._backup()
        inaccurate = self._backup("I", accurate=False)
        self.assertTrue(inaccurate.failed)
        self.assertIn(
            "Accurate mode is required", "\n".join(map(str, inaccurate.messages))
        )
        wrong = self._backup(
            "I", accurate_options=frozenset({bareosfd_stub.bAccurateOptionSize})
        )
        self.assertTrue(wrong.failed)
        self.assertIn("Accurate options", "\n".join(map(str, wrong.messages)))

    def test_08_incremental_rejects_chunk_size_or_hash_change(self):
        self._backup()
        changed_size = self.plugin_string.replace("chunk_size=1MiB", "chunk_size=2MiB")
        result = run_backup(self.catalog, changed_size, "I", self.job_id)
        self.assertTrue(result.failed)
        self.assertIn("chunk_size changed", "\n".join(map(str, result.messages)))
        changed_hash = self.plugin_string + ":hash=sha1"
        result = run_backup(self.catalog, changed_hash, "I", self.job_id)
        self.assertTrue(result.failed)
        self.assertIn("hash changed", "\n".join(map(str, result.messages)))

    def test_09_shuffled_local_restore_matches_both_files(self):
        self._backup()
        result, folder = self._restore_local(shuffle=True)
        self.assertFalse(result.failed, result.messages)
        for volume_id in (ROOT_VOL, DATA_VOL):
            with open(os.path.join(folder, volume_id + ".raw"), "rb") as stream:
                self.assertEqual(stream.read(), self._volume_bytes(volume_id))
        self.assertTrue(os.path.isfile(os.path.join(folder, "server.json")))

    def test_10_cloud_restore_creates_server_and_tagged_volumes(self):
        self._backup()
        before = self._state()
        result = self._restore_cloud()
        self.assertFalse(result.failed, result.messages)
        state = self._state()
        restored = next(
            item
            for item in state["servers"].values()
            if item["name"] == "web01-restore-%d" % (self.job_id + 100)
        )
        self.assertEqual(restored["attachments"][0]["boot_index"], 0)
        for old_id, attachment in zip((ROOT_VOL, DATA_VOL), restored["attachments"]):
            new_id = attachment["volume_id"]
            size_bytes = self.spec["servers"][0]["volumes"][
                0 if old_id == ROOT_VOL else 1
            ]["size_bytes"]
            self.assertEqual(
                self._volume_bytes(new_id)[:size_bytes],
                self._volume_bytes(old_id)[:size_bytes],
            )
            self.assertEqual(
                state["volumes"][new_id]["metadata"].get("bareos-role"),
                "restore-volume",
            )
        self.assertEqual(state["servers"][SERVER], before["servers"][SERVER])
        self.assertEqual(self.backend.list_proxy_attachments(), [])

    def test_11_missing_data_fails_and_force_leaves_original_untouched(self):
        self._backup()
        entry = next(
            item
            for item in self._chunk_entries(self.catalog.jobs[0])
            if item["data"] is not None
        )
        failed = self._restore_cloud(drop_data=(entry["fname"],))
        self.assertTrue(failed.failed)
        self.assertFalse(
            any(
                item["name"].startswith("web01-restore-")
                for item in self._state()["servers"].values()
            )
        )
        state = self._state()
        state["servers"][SERVER]["status"] = "SHUTOFF"
        self._save_state(state)
        failed = self._restore_cloud("python3:force=yes", drop_data=(entry["fname"],))
        self.assertTrue(failed.failed)
        self.assertIn(SERVER, self._state()["servers"])

    def test_12_missing_chunk_path_fails_local_and_cloud(self):
        self._backup()
        entry = self._chunk_entries(self.catalog.jobs[0])[0]
        cloud = self._restore_cloud(drop_paths=(entry["fname"],))
        self.assertTrue(cloud.failed)
        self.assertFalse(
            any(
                item["name"].startswith("web01-restore-")
                for item in self._state()["servers"].values()
            )
        )
        local, _ = self._restore_local(drop_paths=(entry["fname"],))
        self.assertTrue(local.failed)

    def test_13_force_active_fails_before_volume_creation(self):
        self._backup()
        old_count = len(self._state()["volumes"])
        result = self._restore_cloud("python3:force=yes")
        self.assertTrue(result.failed)
        self.assertIn("requires it to be SHUTOFF", "\n".join(map(str, result.messages)))
        self.assertEqual(len(self._state()["volumes"]), old_count)

    def test_14_force_shutoff_replaces_server_and_can_keep_old_volumes(self):
        self._backup()
        state = self._state()
        state["servers"][SERVER]["status"] = "SHUTOFF"
        self._save_state(state)
        result = self._restore_cloud("python3:force=yes")
        self.assertFalse(result.failed, result.messages)
        state = self._state()
        self.assertNotIn(SERVER, state["servers"])
        new_server = next(
            item
            for item in state["servers"].values()
            if item["name"].startswith("web01-restore-")
        )
        creates = [item for item in state["log"] if item["op"] == "create_server"]
        deletes = [item for item in state["log"] if item["op"] == "delete_server"]
        self.assertLess(creates[-1]["time"], deletes[-1]["time"])
        self.assertEqual(new_server["status"], "ACTIVE")

    def test_14b_force_keep_old_volumes(self):
        self._backup()
        state = self._state()
        state["servers"][SERVER]["status"] = "SHUTOFF"
        self._save_state(state)
        old_ids = {ROOT_VOL, DATA_VOL}
        result = self._restore_cloud("python3:force=yes:force_keep_old_volumes=yes")
        self.assertFalse(result.failed, result.messages)
        state = self._state()
        self.assertNotIn(SERVER, state["servers"])
        self.assertTrue(old_ids.issubset(set(state["volumes"])))

    def test_15_force_keep_addresses_server_fault_reports_recovery(self):
        self._backup()
        state = self._state()
        state["servers"][SERVER]["status"] = "SHUTOFF"
        self._save_state(state)
        self._fault("create_server", phase="after")
        result = self._restore_cloud("python3:force=yes:restore_keep_addresses=yes")
        text = "\n".join(map(str, result.messages))
        self.assertTrue(result.failed)
        self.assertIn("recovery:", text)
        state = self._state()
        self.assertNotIn(SERVER, state["servers"])
        self.assertFalse(any(item["device_id"] for item in state["ports"].values()))

    def test_16_keep_addresses_reuses_saved_ip_after_old_server_deleted(self):
        self._backup()
        state = self._state()
        state["servers"].pop(SERVER)
        for port in state["ports"].values():
            port["device_id"] = ""
        self._save_state(state)
        result = self._restore_cloud("python3:restore_keep_addresses=yes")
        self.assertFalse(result.failed, result.messages)
        state = self._state()
        restored = next(
            item
            for item in state["servers"].values()
            if item["name"].startswith("web01-restore-")
        )
        port = next(
            item
            for item in state["ports"].values()
            if item["device_id"] == restored["id"]
        )
        self.assertEqual(port["fixed_ips"][0]["ip_address"], "10.0.0.5")
        self.assertEqual(port["mac_address"], "fa:16:3e:00:00:01")

    def test_17_backup_faults_clean_resources(self):
        for op, phase in (
            ("attach_to_proxy", "before"),
            ("create_volume", "after"),
            ("attach_to_proxy", "after"),
        ):
            with self.subTest(op=op, phase=phase):
                fresh = tempfile.TemporaryDirectory()
                root = os.path.join(fresh.name, "cloud")
                fake_backend.seed(root, self.spec)
                self.root = root
                self.plugin_string = self.plugin_string.replace(
                    self.backend._root, root
                )
                self.backend = fake_backend.FakeBackend(
                    {"backend": "fake", "fake_root": root, "proxy_instance_id": PROXY}
                )
                self.backend.connect()
                self._fault(op, phase=phase)
                result = self._backup()
                self.assertTrue(result.failed)
                self._clean()
                fresh.cleanup()

    def test_18_long_backend_fault_can_be_cancelled_and_cleaned(self):
        self._fault("create_volume", phase="after", sleep=600)
        result = self._backup(cancel_at="start")
        self.assertTrue(result.failed)
        self.assertLess(result.elapsed, 15)
        self._clean()

    def test_19_credentials_are_redacted_from_messages_and_documents(self):
        plugin = self.plugin_string.replace("backend=fake", "backend=sdk")
        plugin += (
            ":proxy_instance_id=%s:auth_url=https://cloud.invalid:username=user:password=PlainSecret1"
            % PROXY
        )
        result = run_backup(self.catalog, plugin, "F", 1)
        text = "\n".join(map(str, result.messages))
        self.assertNotIn("PlainSecret1", text)
        encoded = base64.b85encode(b"HiddenSecret2").decode("ascii")
        defaults = os.path.join(self.tmp.name, "defaults.conf")
        with open(defaults, "w", encoding="utf-8") as stream:
            stream.write("application_credential_secret#enc=%s\n" % encoded)
        # The encoded secret is checked against stored RestoreObject bytes as well.
        result = run_backup(
            self.catalog,
            self.plugin_string + ":defaults_file=" + defaults,
            "F",
            2,
        )
        payloads = [obj["object"] for obj in result.restore_objects]
        self.assertFalse(
            any(
                b"HiddenSecret2" in item or encoded.encode() in item
                for item in payloads
            )
        )
        self.assertNotIn("HiddenSecret2", "\n".join(map(str, result.messages)))

    def test_21_encrypted_volume_fails_with_volume_name(self):
        self.spec["servers"][0]["volumes"][0]["encrypted"] = True
        root = os.path.join(self.tmp.name, "encrypted")
        fake_backend.seed(root, self.spec)
        plugin = self.plugin_string.replace(self.root, root)
        result = run_backup(Catalog(), plugin, "F", 1)
        self.assertTrue(result.failed)
        self.assertIn(ROOT_VOL, "\n".join(map(str, result.messages)))

    def test_22_stop_server_failure_rolls_back_new_server_and_keeps_volumes(self):
        state = self._state()
        state["servers"][SERVER]["status"] = "SHUTOFF"
        self._save_state(state)
        backup = self._backup()
        self.assertFalse(backup.failed, backup.messages)

        self._fault("stop_server", phase="after")
        restored = self._restore_cloud()

        self.assertTrue(restored.failed)
        state = self._state()
        self.assertEqual(set(state["servers"]), {SERVER, PROXY})
        self.assertEqual(state["servers"][SERVER]["status"], "SHUTOFF")
        restored_volumes = [
            volume
            for volume in state["volumes"].values()
            if volume["metadata"].get("bareos-role") == "restore-volume"
        ]
        self.assertEqual(len(restored_volumes), 2)
        self.assertTrue(
            all(volume["status"] == "available" for volume in restored_volumes)
        )
        self.assertTrue(
            all(not volume.get("attachments") for volume in restored_volumes)
        )

    def test_23_local_restore_refuses_symlinked_metadata(self):
        self._backup()
        target = os.path.join(self.tmp.name, "local-symlink")
        server_dir = os.path.join(target, SERVER)
        os.makedirs(server_dir)
        outside = os.path.join(self.tmp.name, "outside.json")
        original = b"leave this file untouched\n"
        with open(outside, "wb") as stream:
            stream.write(original)
        metadata = os.path.join(server_dir, "server.json")
        os.symlink(outside, metadata)

        result = run_restore(
            self.catalog,
            self.plugin_string,
            "python3:restore_target=local:restore_overwrite=yes",
            target,
            self.job_id + 100,
        )

        self.assertTrue(result.failed)
        self.assertTrue(os.path.islink(metadata))
        with open(outside, "rb") as stream:
            self.assertEqual(stream.read(), original)

    def test_25_wave1_documents_restore_as_before(self):
        self._backup()
        self._write(ROOT_VOL, 0, seed=44)
        self._backup("I")
        self._strip_wave1c_fields()
        restored = self._restore_cloud()
        self.assertFalse(restored.failed, restored.messages)
        server = next(
            item
            for item in self._state()["servers"].values()
            if item["name"] == "web01-restore-%d" % (self.job_id + 100)
        )
        self.assertEqual(server["attachments"][0]["boot_index"], 0)
        self.assertEqual(
            self._volume_bytes(server["attachments"][0]["volume_id"])[:MIB],
            self._volume_bytes(ROOT_VOL)[:MIB],
        )

    def test_28_restored_ports_carry_the_restore_tags(self):
        self._backup()
        before = set(self._state()["ports"])
        restored = self._restore_cloud()
        self.assertFalse(restored.failed, restored.messages)
        server = self._restored_server("web01")
        ports = [
            port
            for port_id, port in self._state()["ports"].items()
            if port_id not in before
        ]
        self.assertTrue(ports, "the restore created no port")
        for port in ports:
            self.assertEqual(port["device_id"], server["id"])
            self.assertIn("bareos-restore-job-%d" % (self.job_id + 100), port["tags"])
            self.assertIn("bareos-restored-from-%s" % SERVER, port["tags"])
        original = self._state()["ports"][PORT]
        self.assertFalse(
            [tag for tag in original.get("tags", []) if tag.startswith("bareos-")]
        )

    NO_DOCUMENT_TEXT = (
        "no server metadata (server.json) was received (VirtualFull and "
        "consolidated jobs carry none); restore the whole backup chain of one server"
    )

    def _drop_documents(self):
        for job in self.catalog.jobs:
            job["restore_objects"] = []

    def test_29_restore_without_document_fails_exactly_for_local_target(self):
        self._backup()
        self._drop_documents()
        before = self._state()
        result, folder = self._restore_local()
        self.assertTrue(result.failed)
        self.assertIn(self.NO_DOCUMENT_TEXT, "\n".join(map(str, result.messages)))
        self.assertFalse(os.path.exists(folder), "the local target was created")
        self.assertEqual(result.files, [])
        self.assertEqual(self._state()["volumes"], before["volumes"])
        self._clean()

    def test_30_restore_without_document_fails_exactly_for_cloud_target(self):
        self._backup()
        self._drop_documents()
        before = self._state()
        result = self._restore_cloud()
        self.assertTrue(result.failed)
        self.assertIn(self.NO_DOCUMENT_TEXT, "\n".join(map(str, result.messages)))
        after = self._state()
        self.assertEqual(sorted(after["servers"]), sorted(before["servers"]))
        self.assertEqual(sorted(after["volumes"]), sorted(before["volumes"]))
        self.assertEqual(sorted(after["ports"]), sorted(before["ports"]))
        self._clean()

    def test_33_cross_project_precheck_and_credentials_are_redacted(self):
        self.spec = batch_d_spec(groups=False)
        fake_backend.seed(self.root, self.spec, reset=True)
        self.backend = fake_backend.FakeBackend(
            {"backend": "fake", "fake_root": self.root, "proxy_instance_id": PROXY}
        )
        self.backend.connect()
        self._backup()
        target_project = self.spec["projects"][0]["id"]
        creds = os.path.join(self.tmp.name, "target-project.conf")
        secret = "TargetCredentialSecret-DoNotLog"
        with open(creds, "w", encoding="utf-8") as stream:
            stream.write(
                "backend=fake\nfake_project_id=%s\npassword=%s\n"
                % (target_project, secret)
            )
        same_project_creds = os.path.join(self.tmp.name, "same-project.conf")
        with open(same_project_creds, "w", encoding="utf-8") as stream:
            stream.write("backend=fake\nfake_project_id=%s\n" % PROJECT)
        same_project = self._restore_cloud(
            "python3:restore_project_credentials=" + same_project_creds
        )
        self.assertTrue(same_project.failed, same_project.messages)
        self.assertIn("normal restore", "\n".join(map(str, same_project.messages)))
        # A missing target flavor fails during precheck, before restored volumes exist.
        with open(os.path.join(self.root, "state.json"), encoding="utf-8") as stream:
            state = json.load(stream)
        state["projects"][target_project]["flavors"] = {}
        self._save_state(state)
        failed = self._restore_cloud("python3:restore_project_credentials=" + creds)
        self.assertTrue(failed.failed)
        self.assertIn("flavor", "\n".join(map(str, failed.messages)))
        self.assertNotIn(secret, "\n".join(map(str, failed.messages)))
        state = self._state()
        self.assertFalse(
            [
                v
                for v in state["volumes"].values()
                if v["metadata"].get("bareos-role") == "restore-volume"
            ]
        )

    def test_34_cross_project_accept_failure_deletes_transfer(self):
        self.spec = batch_d_spec(groups=False)
        fake_backend.seed(self.root, self.spec, reset=True)
        self.backend = fake_backend.FakeBackend(
            {"backend": "fake", "fake_root": self.root, "proxy_instance_id": PROXY}
        )
        self.backend.connect()
        self._backup()
        target_project = self.spec["projects"][0]["id"]
        creds = os.path.join(self.tmp.name, "target-project.conf")
        with open(creds, "w", encoding="utf-8") as stream:
            stream.write("backend=fake\nfake_project_id=%s\n" % target_project)
        self._fault("accept_transfer")
        result = self._restore_cloud("python3:restore_project_credentials=" + creds)
        self.assertTrue(result.failed, result.messages)
        state = self._state()
        self.assertEqual(state.get("transfers", {}), {})
        restored = [
            v
            for v in state["volumes"].values()
            if v["metadata"].get("bareos-role") == "restore-volume"
        ]
        self.assertEqual(len(restored), 2)
        self.assertTrue(all(v["status"] == "available" for v in restored))
        self.assertFalse(
            [
                s
                for s in state["servers"].values()
                if s["name"].endswith("-restore-%d" % (self.job_id + 100))
            ]
        )
        self.assertIn("recovery:", "\n".join(map(str, result.messages)))

    def test_35_group_backup_cleans_up_and_records_consistency(self):
        add_groups(self.spec, [ROOT_VOL, DATA_VOL])
        fake_backend.seed(self.root, self.spec, reset=True)
        self.backend = fake_backend.FakeBackend(
            {"backend": "fake", "fake_root": self.root, "proxy_instance_id": PROXY}
        )
        self.backend.connect()
        self.plugin_string = self.plugin_string + ":snapshot_mode=group"
        result = self._backup()
        self.assertFalse(result.failed, result.messages)
        doc = json.loads(self.catalog.jobs[-1]["restore_objects"][0]["object"])
        self.assertEqual(doc["consistency"], "group-crash-consistent")
        state = self._state()
        self.assertEqual(state["group_snapshots"], {})
        self.assertEqual(state["snapshots"], {})
        self._clean()

    def test_36_group_mode_needs_exact_members(self):
        add_groups(self.spec, [ROOT_VOL, DATA_VOL, "foreign-volume"])
        self.spec["volumes"].append(
            {"id": "foreign-volume", "name": "foreign", "size_gb": 1}
        )
        fake_backend.seed(self.root, self.spec, reset=True)
        self.backend = fake_backend.FakeBackend(
            {"backend": "fake", "fake_root": self.root, "proxy_instance_id": PROXY}
        )
        self.backend.connect()
        self.plugin_string = self.plugin_string + ":snapshot_mode=group"
        failed = self._backup()
        self.assertTrue(failed.failed)
        self.assertIn("volume", "\n".join(map(str, failed.messages)).lower())
        self.assertEqual(self._state().get("group_snapshots", {}), {})
        self.assertEqual(self._state()["snapshots"], {})

    def test_37_removed_target_and_cross_project_option_refusals_are_logged(self):
        self._backup()
        result = self._restore_cloud("python3:restore_target=image:force=yes")
        self.assertTrue(result.failed)
        self.assertIn("restore_target", "\n".join(map(str, result.messages)))
        local_cross = self._restore_cloud(
            "python3:restore_target=local:restore_project_cloud=other"
        )
        self.assertTrue(local_cross.failed)
        text = "\n".join(map(str, local_cross.messages))
        self.assertIn("restore_target=local", text)
        self.assertIn("restore_project_cloud", text)

    def test_37b_image_booted_server_is_refused_without_changes(self):
        self._use_image_server(volumes=True)
        self.backend = fake_backend.FakeBackend(
            {"backend": "fake", "fake_root": self.root, "proxy_instance_id": PROXY}
        )
        self.backend.connect()
        before = self._state()
        failed = self._backup()
        self.assertTrue(failed.failed)
        text = "\n".join(map(str, failed.messages))
        self.assertIn(
            "boots from an image; only servers that boot from a Cinder volume "
            "are supported",
            text,
        )
        after = self._state()
        for key in ("volumes", "snapshots"):
            self.assertEqual(after.get(key), before.get(key))

    def test_38_cross_project_security_group_override_and_target_rollback(self):
        self.spec = batch_d_spec(groups=False)
        fake_backend.seed(self.root, self.spec, reset=True)
        self.backend = fake_backend.FakeBackend(
            {"backend": "fake", "fake_root": self.root, "proxy_instance_id": PROXY}
        )
        self.backend.connect()
        self._backup()
        target_project = self.spec["projects"][0]["id"]
        creds = os.path.join(self.tmp.name, "target-project.conf")
        with open(creds, "w", encoding="utf-8") as stream:
            stream.write("backend=fake\nfake_project_id=%s\n" % target_project)
        state = self._state()
        state["security_groups"].pop("sg-1")
        self._save_state(state)
        mapped = self._restore_cloud("python3:restore_project_credentials=" + creds)
        self.assertTrue(mapped.failed, mapped.messages)
        self.assertIn("restore_security_groups", "\n".join(map(str, mapped.messages)))
        state = self._state()
        state["faults"]["create_server"] = {
            "remaining": 1,
            "message": "target server rejected",
        }
        self._save_state(state)
        failed = self._restore_cloud(
            "python3:restore_project_credentials=%s:restore_security_groups=default"
            % creds
        )
        self.assertTrue(failed.failed, failed.messages)
        state = self._state()
        self.assertEqual(state["transfers"], {})
        self.assertFalse(
            [
                s
                for s in state["servers"].values()
                if s["project_id"] == target_project
                and s["name"].endswith("-restore-%d" % (self.job_id + 100))
            ]
        )
        target_volumes = [
            v
            for v in state["volumes"].values()
            if v.get("project_id") == target_project
            and v["metadata"].get("bareos-role") == "restore-volume"
        ]
        self.assertEqual(len(target_volumes), 2)
        self.assertTrue(all(v["project_id"] == target_project for v in target_volumes))

    def test_39_group_snapshot_cleanup_after_cancel(self):
        add_groups(self.spec, [ROOT_VOL, DATA_VOL])
        fake_backend.seed(self.root, self.spec, reset=True)
        self.backend = fake_backend.FakeBackend(
            {"backend": "fake", "fake_root": self.root, "proxy_instance_id": PROXY}
        )
        self.backend.connect()
        self.plugin_string = self.plugin_string + ":snapshot_mode=group"
        self._fault("create_group_snapshot", phase="after", sleep=2)
        result = self._backup(cancel_at="start")
        self.assertTrue(result.failed, result.messages)
        state = self._state()
        self.assertEqual(state.get("group_snapshots", {}), {})
        self.assertEqual(state["snapshots"], {})
        self.assertFalse(
            [
                volume
                for volume in state["volumes"].values()
                if volume["metadata"].get("bareos-role") == "backup-temp-volume"
            ]
        )
        self.assertEqual(self.backend.list_proxy_attachments(), [])

    def test_41_post_accept_readiness_timeout_reconciles_after_abort(self):
        target_project, result, keys = self._run_cross_project_after_accept(
            CloudTransferAccepted
        )
        self._assert_reconciled_transfer_recovery(target_project, result, keys)

    def test_42_post_accept_cancel_reconciles_after_abort(self):
        target_project, result, keys = self._run_cross_project_after_accept(
            CloudCancelled
        )
        self._assert_reconciled_transfer_recovery(target_project, result, keys)

    def test_43_unknown_foreign_volume_is_refused_without_state_change(self):
        self._backup()
        original_create = fake_backend.FakeBackend.create_volume
        foreign_records = []

        def create_unknown_owner(backend, *args, **kwargs):
            volume = original_create(backend, *args, **kwargs)
            with backend._locked() as state:
                record = state["volumes"][volume.id]
                record["project_id"] = "unknown-foreign-project"
                foreign_records.append(copy.deepcopy(record))
            return volume

        fake_backend.FakeBackend.create_volume = create_unknown_owner
        try:
            result = self._restore_cloud()
        finally:
            fake_backend.FakeBackend.create_volume = original_create

        self.assertTrue(result.failed, result.messages)
        self.assertEqual(len(foreign_records), 1)
        foreign = foreign_records[0]
        foreign_id = foreign["id"]
        self.assertIn(foreign_id, self._state()["volumes"])
        self.assertEqual(self._state()["volumes"][foreign_id], foreign)
        self.assertNotIn(PROXY, foreign["attached_server_ids"])
        with self.assertRaises(CloudNotFound):
            self.backend.get_volume(foreign_id)
        self.assertNotIn(
            foreign_id, {volume.id for volume in self.backend.list_volumes({})}
        )


if __name__ == "__main__":
    unittest.main()
