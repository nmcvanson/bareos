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

"""Unit tests of the backup cloud session against the fake cloud."""

import os
import re
import tempfile
import threading
import unittest
from unittest import mock

from bareos_openstack import cloud_backup, fake_backend
from bareos_openstack.cloud import (
    CloudCancelled,
    CloudConfigError,
    CloudConflict,
    CloudError,
    CloudTimeout,
    bareos_tags,
    group_snapshot_description,
    group_snapshot_name,
)
from bareos_openstack.cloud_backup import BackupCloudSession

from test_cloud_fake import (
    DATA_VOL,
    IMG_DATA_VOL,
    IMG_SERVER,
    PROXY,
    ROOT_VOL,
    SERVER,
    add_groups,
    base_spec,
    batch_d_spec,
    image_booted_spec,
    make_cloud,
    read_state,
)

JOB = 50


class Collector:
    def __init__(self):
        self.lines = []

    def __call__(self, level, message):
        self.lines.append((level, message))

    def has(self, level, text):
        return any(lv == level and text in msg for lv, msg in self.lines)


def check_clean(be):
    state = read_state(be)
    left = [
        s["id"]
        for s in state["snapshots"].values()
        if s["metadata"].get("bareos-role") == "backup-snapshot"
    ]
    left += [
        v["id"]
        for v in state["volumes"].values()
        if v["metadata"].get("bareos-role") == "backup-temp-volume"
        or PROXY in v["attached_server_ids"]
    ]
    return left


class BackupSessionTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = self._tmp.name
        self.be, _ = make_cloud(self.tmp)
        self.log = Collector()

    def tearDown(self):
        self._tmp.cleanup()

    def session(self, **kw):
        args = dict(server_id=SERVER, job_id=JOB, poll_timeout=5, log=self.log)
        args.update(kw)
        job_id = args.pop("job_id")
        return BackupCloudSession(self.be, job_id, **args)

    def fault(self, op, **spec):
        with self.be._locked() as state:
            state["faults"][op] = spec

    # happy path -----------------------------------------------------------------

    def test_prepare_tags_and_order(self):
        sess = self.session()
        inv = sess.prepare()
        self.assertEqual([v.id for v in inv.volumes], [ROOT_VOL, DATA_VOL])
        self.assertEqual(inv.server.id, SERVER)
        self.assertEqual([p.name for p in inv.ports], ["web01-port"])
        snaps = read_state(self.be)["snapshots"].values()
        self.assertEqual(len(snaps), 2)
        for snap in snaps:
            self.assertEqual(
                snap["metadata"],
                bareos_tags(JOB, SERVER, "backup-snapshot", snap["volume_id"]),
            )
            self.assertEqual(
                snap["name"], "bareos-%d-snap-%s" % (JOB, snap["volume_id"])
            )
        self.assertEqual(sess.cleanup(), [])
        self.assertEqual(check_clean(self.be), [])

    def test_server_by_name_and_config_errors(self):
        self.assertEqual(
            self.session(server_id="", server_name="web01").prepare().server.id, SERVER
        )
        with self.assertRaises(CloudConfigError):
            self.session(server_id="", server_name="").prepare()
        with self.assertRaises(CloudConfigError):
            self.session(server_name="web01").prepare()

    def test_status_and_project_checks(self):
        with self.be._locked() as state:
            state["servers"][SERVER]["status"] = "ERROR"
        with self.assertRaises(CloudConflict):
            self.session().prepare()
        with self.be._locked() as state:
            state["servers"][SERVER]["status"] = "SHUTOFF"
        # the connection reports another project than the server's owner
        with mock.patch.object(
            fake_backend.FakeBackend,
            "project_id",
            new_callable=mock.PropertyMock,
            return_value="other",
        ):
            with self.assertRaises(CloudConflict):
                self.session().prepare()

    def test_open_volume_path_has_source_bytes_and_release(self):
        sess = self.session()
        sess.prepare()
        path = sess.open_volume(ROOT_VOL)
        with open(path, "rb") as fh:
            data = fh.read()
        with open(self.be._vol_file(ROOT_VOL), "rb") as fh:
            self.assertEqual(data, fh.read())
        self.assertNotEqual(path, self.be._vol_file(ROOT_VOL))
        temp_id = os.path.basename(path)[:-4]
        self.assertEqual(self.be.list_proxy_attachments(), [temp_id])
        temp = read_state(self.be)["volumes"][temp_id]
        self.assertEqual(temp["metadata"]["bareos-role"], "backup-temp-volume")
        self.assertEqual(temp["metadata"]["bareos-source-volume-id"], ROOT_VOL)
        sess.release_volume(ROOT_VOL)
        self.assertEqual(self.be.list_proxy_attachments(), [])
        left = check_clean(self.be)
        self.assertEqual(len(left), 1)  # the snapshot of the data volume remains
        sess.release_volume(DATA_VOL)
        self.assertEqual(check_clean(self.be), [])
        sess.release_volume("unknown")

    def test_every_step_logs_its_duration(self):
        sess = self.session()
        sess.prepare()
        sess.open_volume(ROOT_VOL)
        sess.release_volume(ROOT_VOL)
        sess.release_volume(DATA_VOL)
        infos = [m for lv, m in self.log.lines if lv == "info" and " took " in m]
        pattern = re.compile(r"^(?P<step>.+) of (?P<subject>.+) took (?P<sec>\d+\.\d{3}) s$")
        steps = {}
        for line in infos:
            match = pattern.match(line)
            self.assertIsNotNone(match, line)
            self.assertGreaterEqual(float(match.group("sec")), 0.0)
            steps.setdefault(match.group("step"), []).append(match.group("subject"))
        for step in ("snapshot", "temporary volume create"):
            self.assertIn("volume %s" % ROOT_VOL, steps[step])
        self.assertIn("attach until the device was found", steps)
        self.assertTrue([s for s in steps if s.startswith("detach")], steps)
        self.assertTrue([s for s in steps if s.startswith("delete of")], steps)
        text = " ".join(m for _, m in self.log.lines)
        self.assertNotIn("s3cr3t", text)

    # refusals ---------------------------------------------------------------------

    def _make_image_booted(self):
        with self.be._locked() as state:
            state["servers"][SERVER]["image_id"] = "img-1"
            for att in state["servers"][SERVER]["attachments"]:
                att["boot_index"] = None

    def test_image_booted_server_is_refused_with_only_read_calls(self):
        self._make_image_booted()
        before = list(read_state(self.be)["log"])
        with self.assertRaises(CloudConflict) as ctx:
            self.session().prepare()
        self.assertIn(
            "boots from an image; only servers that boot from a Cinder volume are "
            "supported",
            str(ctx.exception),
        )
        self.assertEqual(read_state(self.be)["log"], before)
        self.assertEqual(check_clean(self.be), [])
        self.assertEqual(self.session().cleanup(), [])

    def test_image_booted_spec_is_refused_without_a_change(self):
        with tempfile.TemporaryDirectory() as tmp:
            be, _ = make_cloud(tmp, image_booted_spec(volumes=True))
            sess = BackupCloudSession(be, JOB, server_id=IMG_SERVER, poll_timeout=5)
            with self.assertRaises(CloudConflict):
                sess.prepare()
            self.assertEqual(read_state(be)["log"], [])

    def test_boot_volume_keeps_its_image_metadata(self):
        spec = base_spec()
        spec["servers"][0]["volumes"][0]["image_metadata"] = {"os_type": "linux"}
        with tempfile.TemporaryDirectory() as tmp:
            be, _ = make_cloud(tmp, spec)
            inv = BackupCloudSession(be, JOB, server_id=SERVER, poll_timeout=5).prepare()
            self.assertEqual(inv.volumes[0].image_metadata, {"os_type": "linux"})
            self.assertEqual(inv.volumes[0].source, "volume")

    def test_encrypted_refused(self):
        with self.be._locked() as state:
            state["volumes"][DATA_VOL]["encrypted"] = True
        with self.assertRaises(CloudConflict) as ctx:
            self.session().prepare()
        self.assertIn(DATA_VOL, str(ctx.exception))
        self.assertEqual(read_state(self.be)["snapshots"], {})

    def test_no_volumes_refused(self):
        with self.be._locked() as state:
            state["servers"][SERVER]["attachments"] = []
        with self.assertRaises(CloudConflict):
            self.session().prepare()

    # leftovers --------------------------------------------------------------------

    def _add_leftovers(self):
        ids = {}
        ids["old_snap"] = self.be.create_snapshot(
            ROOT_VOL,
            "old",
            bareos_tags(40, SERVER, "backup-snapshot", ROOT_VOL),
            True,
            5,
        ).id
        ids["old_vol"] = self.be.create_volume(
            "oldtmp", 1, bareos_tags(40, SERVER, "backup-temp-volume", ROOT_VOL), 5
        ).id
        self.be.attach_to_proxy(ids["old_vol"], 5)
        ids["new_snap"] = self.be.create_snapshot(
            ROOT_VOL,
            "new",
            bareos_tags(99, SERVER, "backup-snapshot", ROOT_VOL),
            True,
            5,
        ).id
        ids["restore"] = self.be.create_volume(
            "res", 1, bareos_tags(30, SERVER, "restore-volume", ROOT_VOL), 5
        ).id
        ids["foreign"] = self.be.create_snapshot(
            ROOT_VOL, "f", {"bareos-role": "backup-snapshot"}, True, 5
        ).id
        return ids

    def test_leftovers_reported_by_default(self):
        ids = self._add_leftovers()
        self.session().prepare()
        self.assertTrue(self.log.has("warning", "set cleanup_leftovers=yes"))
        item = "snapshot %s (job 40)" % ids["old_snap"]
        self.assertTrue(self.log.has("warning", item))
        state = read_state(self.be)
        self.assertIn(ids["old_snap"], state["snapshots"])
        self.assertIn(ids["old_vol"], state["volumes"])

    def test_leftovers_removed_by_job_order(self):
        ids = self._add_leftovers()
        self.session(cleanup_leftovers=True).prepare()
        state = read_state(self.be)
        self.assertNotIn(ids["old_snap"], state["snapshots"])
        self.assertNotIn(ids["old_vol"], state["volumes"])
        self.assertIn(ids["new_snap"], state["snapshots"])
        self.assertIn(ids["restore"], state["volumes"])
        self.assertIn(ids["foreign"], state["snapshots"])
        self.assertTrue(self.log.has("warning", "no other job backs up"))
        self.assertEqual(self.be.list_proxy_attachments(), [])

    # failures and cleanup ----------------------------------------------------------

    def test_snapshot_fault_on_second_volume_then_cleanup(self):
        sess = self.session()
        calls = {"n": 0}
        real = self.be.create_snapshot

        def flaky(*args, **kwargs):
            calls["n"] += 1
            if calls["n"] == 2:
                raise CloudError("second snapshot failed")
            return real(*args, **kwargs)

        self.be.create_snapshot = flaky
        with self.assertRaises(CloudError):
            sess.prepare()
        self.assertEqual(len(read_state(self.be)["snapshots"]), 1)
        self.assertEqual(sess.cleanup(), [])
        self.assertEqual(check_clean(self.be), [])

    def test_fault_after_create_volume_then_cleanup(self):
        sess = self.session()
        sess.prepare()
        self.fault("create_volume", phase="after")
        with self.assertRaises(CloudTimeout):
            sess.open_volume(ROOT_VOL)
        self.assertEqual(len(check_clean(self.be)), 3)
        self.assertEqual(sess.cleanup(), [])
        self.assertEqual(check_clean(self.be), [])

    def test_fault_after_attach_then_cleanup(self):
        sess = self.session()
        sess.prepare()
        self.fault("attach_to_proxy", phase="after")
        with self.assertRaises(CloudTimeout):
            sess.open_volume(ROOT_VOL)
        self.assertEqual(len(self.be.list_proxy_attachments()), 1)
        self.assertEqual(sess.cleanup(), [])
        self.assertEqual(check_clean(self.be), [])

    def test_reconciliation_removes_untracked_resources(self):
        sess = self.session()
        sess.prepare()
        vol = self.be.create_volume(
            "untracked", 1, bareos_tags(JOB, SERVER, "backup-temp-volume", ROOT_VOL), 5
        )
        self.be.attach_to_proxy(vol.id, 5)
        snap = self.be.create_snapshot(
            DATA_VOL,
            "s",
            bareos_tags(JOB, SERVER, "backup-snapshot", DATA_VOL),
            True,
            5,
        )
        self.assertEqual(sess.cleanup(), [])
        state = read_state(self.be)
        self.assertNotIn(vol.id, state["volumes"])
        self.assertNotIn(snap.id, state["snapshots"])
        self.assertEqual(check_clean(self.be), [])

    def test_open_volume_after_close(self):
        sess = self.session()
        sess.prepare()
        sess.close()
        with self.assertRaises(CloudCancelled):
            sess.open_volume(ROOT_VOL)
        self.assertEqual(sess.cleanup(), [])

    def test_late_on_created_after_close_is_logged(self):
        sess = self.session()
        sess.prepare()
        sess.close()
        sess._record(sess.temp_volumes, ROOT_VOL, "volume")("late-id")
        self.assertEqual(sess.temp_volumes[ROOT_VOL], "late-id")
        self.assertTrue(self.log.has("warning", "created after cleanup started"))
        sess.temp_volumes.clear()
        self.assertEqual(sess.cleanup(), [])

    def test_cleanup_reports_failures_and_is_idempotent(self):
        sess = self.session()
        sess.prepare()
        path = sess.open_volume(ROOT_VOL)
        temp_id = os.path.basename(path)[:-4]
        self.fault("delete_volume", remaining=100)
        failures = sess.cleanup()
        self.assertEqual(failures, ["volume %s" % temp_id])
        self.assertEqual(self.be.list_proxy_attachments(), [])
        with self.be._locked() as state:
            state["faults"] = {}
        self.assertEqual(sess.cleanup(), [])
        self.assertEqual(check_clean(self.be), [])
        self.assertEqual(sess.cleanup(), [])

    def test_release_never_raises(self):
        sess = self.session()
        sess.prepare()
        sess.open_volume(ROOT_VOL)
        self.fault("detach_from_proxy", remaining=100)
        sess.release_volume(ROOT_VOL)
        self.assertTrue(self.log.has("warning", "could not detach"))
        self.assertIn(ROOT_VOL, sess.temp_volumes)
        with self.be._locked() as state:
            state["faults"] = {}
        sess.release_volume(ROOT_VOL)
        self.assertNotIn(ROOT_VOL, sess.temp_volumes)
        self.assertNotIn(ROOT_VOL, sess.snapshots)

    def test_cleanup_continues_when_cancelled_on_every_step(self):
        sess = self.session()
        sess.prepare()
        path = sess.open_volume(ROOT_VOL)
        temp_id = os.path.basename(path)[:-4]
        event = threading.Event()
        self.be.set_cancel_event(event)
        event.set()
        self.fault("delete_volume", remaining=100, sleep=0.01)
        self.fault("detach_from_proxy", remaining=100, sleep=0.01)
        failures = sess.cleanup()
        self.assertIn("attachment %s" % temp_id, failures)
        self.assertIn("volume %s" % temp_id, failures)
        self.assertEqual(read_state(self.be)["snapshots"], {})
        event.clear()
        with self.be._locked() as state:
            state["faults"] = {}
        self.assertEqual(sess.cleanup(), [])
        self.assertEqual(check_clean(self.be), [])



class GroupSnapshotBackupTest(unittest.TestCase):
    """snapshot_mode=group (plan C.10.1 B-3)."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.be, _ = make_cloud(self._tmp.name, batch_d_spec())
        self.log = Collector()

    def tearDown(self):
        self._tmp.cleanup()

    def session(self, job=JOB, server_id=SERVER, **kw):
        kw.setdefault("snapshot_mode", "group")
        return BackupCloudSession(
            self.be, job, server_id=server_id, poll_timeout=5, log=self.log, **kw
        )

    def state(self):
        return read_state(self.be)

    def ops(self):
        return [e["op"] for e in self.state()["log"]]

    def test_prepare_creates_one_identified_group_snapshot(self):
        sess = self.session()
        inv = sess.prepare()
        self.assertEqual(inv.consistency, "group-crash-consistent")
        state = self.state()
        (gsnap,) = state["group_snapshots"].values()
        self.assertEqual(gsnap["name"], "bareos-50-gsnap-%s" % SERVER)
        self.assertEqual(
            gsnap["description"],
            "created-by=bareos bareos-job-id=50 bareos-server-id=%s "
            "bareos-role=backup-group-snapshot" % SERVER,
        )
        self.assertEqual(set(gsnap["snapshot_ids"]), {ROOT_VOL, DATA_VOL})
        self.assertEqual(sess.group_snapshot_id, gsnap["id"])
        self.assertEqual(sess.snapshots, {})
        self.assertNotIn("create_snapshot", self.ops())
        self.assertEqual(sess.cleanup(), [])
        self.assertEqual(self.state()["group_snapshots"], {})
        self.assertEqual(self.state()["snapshots"], {})

    def test_temp_volumes_come_from_the_members_and_release_order(self):
        sess = self.session()
        sess.prepare()
        members = next(iter(self.state()["group_snapshots"].values()))["snapshot_ids"]
        sess.open_volume(ROOT_VOL)
        sess.open_volume(DATA_VOL)
        state = self.state()
        sources = {
            v["source_snapshot_id"]
            for v in state["volumes"].values()
            if v["metadata"].get("bareos-role") == "backup-temp-volume"
        }
        self.assertEqual(sources, set(members.values()))
        sess.release_volume(ROOT_VOL)
        self.assertEqual(len(self.state()["group_snapshots"]), 1)
        sess.release_volume(DATA_VOL)
        self.assertEqual(self.state()["group_snapshots"], {})
        self.assertEqual(self.state()["snapshots"], {})
        ops = self.ops()
        self.assertNotIn("delete_snapshot", ops)
        self.assertEqual(ops.count("delete_group_snapshot"), 1)
        self.assertGreater(
            ops.index("delete_group_snapshot"),
            len(ops) - 1 - ops[::-1].index("delete_volume"),
        )
        self.assertEqual(sess.cleanup(), [])

    def test_group_snapshot_survives_until_every_member_is_released(self):
        sess = self.session()
        sess.prepare()
        sess.open_volume(ROOT_VOL)
        sess.release_volume(ROOT_VOL)
        self.assertEqual(len(self.state()["group_snapshots"]), 1)
        sess.open_volume(DATA_VOL)
        sess.release_volume(DATA_VOL)
        self.assertEqual(self.state()["group_snapshots"], {})

    def test_cleanup_deletes_temp_volumes_before_the_group_snapshot(self):
        sess = self.session()
        sess.prepare()
        sess.open_volume(ROOT_VOL)
        self.assertEqual(sess.cleanup(), [])
        ops = self.ops()
        self.assertLess(
            len(ops) - 1 - ops[::-1].index("delete_volume"),
            ops.index("delete_group_snapshot"),
        )
        self.assertEqual(self.state()["group_snapshots"], {})

    def test_cleanup_reports_a_group_snapshot_with_a_stuck_volume(self):
        sess = self.session()
        sess.prepare()
        sess.open_volume(ROOT_VOL)
        with self.be._locked() as state:
            state["faults"]["delete_volume"] = {"remaining": 9, "message": "busy"}
        failed = sess.cleanup()
        self.assertTrue(any(f.startswith("group snapshot ") for f in failed))
        self.assertEqual(len(self.state()["group_snapshots"]), 1)

    def test_cleanup_finds_an_untracked_group_snapshot_by_description(self):
        sess = self.session()
        with self.be._locked() as state:
            state["faults"]["create_group_snapshot"] = {
                "remaining": 1,
                "message": "lost",
                "phase": "after",
            }
        with self.assertRaises(CloudError):
            sess.prepare()
        self.assertEqual(len(self.state()["group_snapshots"]), 1)
        sess2 = self.session()
        self.assertEqual(sess2.cleanup(), [])
        self.assertEqual(self.state()["group_snapshots"], {})

    def test_refusals_create_nothing(self):
        for case in ("no-group", "extra", "type", "version", "image-root"):
            with tempfile.TemporaryDirectory() as tmp:
                sp = batch_d_spec(groups=False)
                server = SERVER
                if case == "extra":
                    add_groups(sp, [ROOT_VOL, DATA_VOL, "other"])
                    sp["volumes"] = [{"id": "other", "name": "o", "size_gb": 1}]
                elif case == "type":
                    add_groups(sp, [ROOT_VOL, DATA_VOL], group_type="plain")
                elif case == "version":
                    add_groups(sp, [ROOT_VOL, DATA_VOL])
                    sp["block_storage_microversion"] = "3.11"
                elif case == "image-root":
                    sp = image_booted_spec(volumes=True)
                    add_groups(sp, [IMG_DATA_VOL])
                    server = IMG_SERVER
                be, _ = make_cloud(tmp, sp)
                sess = BackupCloudSession(
                    be, 50, server_id=server, poll_timeout=5, snapshot_mode="group"
                )
                with self.assertRaises((CloudConflict, CloudConfigError), msg=case):
                    sess.prepare()
                state = read_state(be)
                self.assertEqual(state["snapshots"], {}, case)
                self.assertEqual(state.get("group_snapshots", {}), {}, case)

    def test_group_leftovers_follow_the_job_id_rules(self):
        group = self.be.find_consistency_group([ROOT_VOL, DATA_VOL])
        for job in (40, 60):
            self.be.create_group_snapshot(
                group,
                group_snapshot_name(job, SERVER),
                group_snapshot_description(job, SERVER),
                5,
            )
        self.be.create_group_snapshot(
            group,
            group_snapshot_name(30, "other-server"),
            group_snapshot_description(30, "other-server"),
            5,
        )
        self.be.create_group_snapshot(group, "bareos-manual", "created by hand", 5)
        sess = self.session()
        sess.prepare()
        self.assertTrue(self.log.has("warning", "group snapshot"))
        names = {g["name"] for g in self.state()["group_snapshots"].values()}
        self.assertEqual(len(names), 5)
        sess.cleanup()
        self.log.lines.clear()
        sess = self.session(cleanup_leftovers=True)
        sess.prepare()
        names = {g["name"] for g in self.state()["group_snapshots"].values()}
        self.assertNotIn(group_snapshot_name(40, SERVER), names)
        self.assertIn(group_snapshot_name(60, SERVER), names)
        self.assertIn(group_snapshot_name(30, "other-server"), names)
        self.assertIn("bareos-manual", names)
        self.assertTrue(self.log.has("info", "removed leftover group snapshot"))
        self.assertTrue(self.log.has("warning", "not removed"))

    def test_per_volume_mode_never_touches_groups(self):
        with tempfile.TemporaryDirectory() as tmp:
            be, _ = make_cloud(
                tmp, dict(base_spec(), block_storage_microversion="3.10")
            )
            sess = BackupCloudSession(
                be, 50, server_id=SERVER, poll_timeout=5, cleanup_leftovers=True
            )
            inv = sess.prepare()
            self.assertEqual(inv.consistency, "per-volume")
            self.assertEqual(sess.cleanup(), [])
            ops = {e["op"] for e in read_state(be)["log"]}
            self.assertFalse({"create_group_snapshot", "delete_group_snapshot"} & ops)

    def test_unknown_mode_is_a_config_error(self):
        with self.assertRaises(CloudConfigError):
            self.session(snapshot_mode="cluster")


if __name__ == "__main__":
    unittest.main()
