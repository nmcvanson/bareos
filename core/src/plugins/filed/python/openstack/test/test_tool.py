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

"""Unit tests of the operator tool against the fake cloud."""

import contextlib
import io
import json
import os
import tempfile
import unittest
from unittest import mock

from bareos_openstack import fake_backend, options, tool
from bareos_openstack.cloud import (
    bareos_tags,
    group_snapshot_description,
    group_snapshot_name,
)

from test_cloud_fake import (
    DATA_VOL,
    IMG_SERVER,
    PROXY,
    ROOT_VOL,
    SERVER,
    base_spec,
    image_booted_spec,
    make_cloud,
    read_state,
)


def run(*argv):
    """Run tool.main in-process; return (exit code, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = tool.main(list(argv))
    return code, out.getvalue(), err.getvalue()


class ToolTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.be = None

    def tearDown(self):
        self._tmp.cleanup()

    def cloud(self, spec=None):
        self.be, _ = make_cloud(self._tmp.name, spec or image_booted_spec())
        self.args = ["--backend", "fake", "--fake-root", self.be._root]

    def tool(self, *argv):
        return run(*(self.args + list(argv)))

    def snapshot_of_state(self):
        state = read_state(self.be)
        state["log"] = len(state["log"])
        return json.dumps(state, sort_keys=True)

    # list-servers ---------------------------------------------------------

    def test_list_servers(self):
        self.cloud()
        before = self.snapshot_of_state()
        code, out, _ = self.tool("list-servers")
        self.assertEqual(code, 0)
        lines = {line.split()[0]: line for line in out.splitlines()}
        self.assertNotIn(PROXY, lines)
        self.assertIn("web01", lines[SERVER])
        self.assertIn("boot=volume", lines[SERVER])
        self.assertIn("volumes=2", lines[SERVER])
        self.assertIn("boot=image", lines[IMG_SERVER])
        self.assertIn("volumes=0", lines[IMG_SERVER])
        self.assertEqual(self.snapshot_of_state(), before)

    # preflight ------------------------------------------------------------

    def test_preflight_ok_is_read_only(self):
        self.cloud()
        before = self.snapshot_of_state()
        code, out, _ = self.tool("preflight", "--server-id", SERVER)
        self.assertEqual(code, 0, out)
        self.assertNotIn("FAIL", out)
        self.assertEqual(len(out.splitlines()), 7)
        self.assertEqual(self.snapshot_of_state(), before)
        code, _, _ = self.tool("preflight", "--server-name", "web01")
        self.assertEqual(code, 0)

    def test_preflight_image_server_is_refused(self):
        self.cloud()
        before = self.snapshot_of_state()
        code, out, _ = self.tool("preflight", "--server-id", IMG_SERVER)
        self.assertEqual(code, 1)
        self.assertIn("FAIL boot", out)
        self.assertIn(
            "boots from an image; only servers that boot from a Cinder volume "
            "are supported",
            out,
        )
        self.assertNotIn("root image", out)
        self.assertEqual(self.snapshot_of_state(), before)

    def test_removed_preflight_flags_are_usage_errors(self):
        self.cloud()
        for flag in ("--skip-ephemeral", "--ignore-ephemeral-disk"):
            code, _, _ = self.tool("preflight", "--server-id", IMG_SERVER, flag)
            self.assertEqual(code, 2)
        code, _, _ = self.tool("leftovers", "--transfer-dir", "/tmp/x")
        self.assertEqual(code, 2)

    def test_preflight_findings(self):
        self.cloud(base_spec())
        with self.be._locked() as state:
            state["servers"][SERVER]["status"] = "ERROR"
            state["volumes"][ROOT_VOL]["encrypted"] = True
        with mock.patch.object(
            fake_backend.FakeBackend,
            "project_id",
            new_callable=mock.PropertyMock,
            return_value="other",
        ):
            code, out, _ = self.tool("preflight", "--server-id", SERVER)
        self.assertEqual(code, 1)
        for check in ("status", "project", "encryption"):
            self.assertIn("FAIL %s" % check, out)

    def test_preflight_missing_server_and_proxy(self):
        self.cloud(base_spec())
        code, out, _ = self.tool("preflight", "--server-id", "nope")
        self.assertEqual(code, 1)
        self.assertIn("FAIL server", out)
        code, out, _ = self.tool("preflight", "--server-name", "nope")
        self.assertEqual(code, 1)
        with self.be._locked() as state:
            del state["servers"][PROXY]
        code, out, _ = run(
            "--backend",
            "fake",
            "--fake-root",
            self.be._root,
            "preflight",
            "--server-id",
            SERVER,
        )
        self.assertEqual(code, 1)
        self.assertIn("FAIL proxy", out)

    def test_usage_errors(self):
        self.cloud()
        self.assertEqual(self.tool("preflight")[0], 2)
        self.assertEqual(run()[0], 2)
        code, _, err = run("list-servers")
        self.assertEqual(code, 2)
        self.assertIn("--config", err)
        code, _, err = run("--backend", "fake", "list-servers")
        self.assertEqual(code, 2)
        code, _, err = run(
            "--backend",
            "fake",
            "--fake-root",
            os.path.join(self._tmp.name, "none"),
            "list-servers",
        )
        self.assertEqual(code, 2)
        self.assertTrue(err.startswith("error:"))

    # leftovers ------------------------------------------------------------

    def leftovers_fixture(self):
        self.cloud(base_spec())
        be = self.be
        self.ids = {}
        for job in (40, 60):
            tags = lambda role, src="": bareos_tags(job, SERVER, role, src)
            self.ids["snap%d" % job] = be.create_snapshot(
                ROOT_VOL, "s", tags("backup-snapshot", ROOT_VOL), True, 5
            ).id
            self.ids["vol%d" % job] = be.create_volume(
                "t", 1, tags("backup-temp-volume", ROOT_VOL), 5
            ).id
        be.attach_to_proxy(self.ids["vol40"], 5)
        self.ids["foreign"] = be.create_volume("proxy-disk", 1, {}, 5).id
        be.attach_to_proxy(self.ids["foreign"], 5)
        self.ids["restore"] = be.create_volume(
            "r", 1, bareos_tags(40, SERVER, "restore-volume", ROOT_VOL), 5
        ).id
        self.ids["plain"] = be.create_volume("p", 1, {}, 5).id

    def test_leftovers_report_is_read_only(self):
        self.leftovers_fixture()
        before = self.snapshot_of_state()
        code, out, _ = self.tool("leftovers")
        self.assertEqual(code, 1)
        for key in ("snap40", "vol40", "snap60", "vol60"):
            self.assertIn(self.ids[key], out)
        self.assertIn("job=40", out)
        self.assertIn("attachment %s" % self.ids["vol40"], out)
        self.assertNotIn(self.ids["restore"], out)
        self.assertNotIn(self.ids["plain"], out)
        self.assertNotIn(self.ids["foreign"], out)
        self.assertEqual(self.snapshot_of_state(), before)
        code, out, _ = self.tool("leftovers", "--server-id", "other-server")
        self.assertEqual(code, 0)
        self.assertIn("no leftovers", out)

    def test_foreign_proxy_volume_is_not_a_finding(self):
        self.cloud(base_spec())
        vol = self.be.create_volume("proxy-disk", 1, {}, 5)
        self.be.attach_to_proxy(vol.id, 5)
        code, out, _ = self.tool("leftovers")
        self.assertEqual((code, out.strip()), (0, "no leftovers"))

    def test_cleanup_needs_confirmation(self):
        self.leftovers_fixture()
        before = self.snapshot_of_state()
        for extra in (
            ["--cleanup"],
            ["--cleanup", "--older-than-job", "50"],
            ["--cleanup", "--yes"],
        ):
            self.assertEqual(self.tool("leftovers", *extra)[0], 2)
        self.assertEqual(self.snapshot_of_state(), before)

    def test_cleanup_removes_only_lower_job_backup_resources(self):
        self.leftovers_fixture()
        code, out, _ = self.tool(
            "leftovers", "--cleanup", "--older-than-job", "50", "--yes"
        )
        self.assertEqual(code, 0, out)
        state = read_state(self.be)
        self.assertIn(self.ids["vol40"], state["volumes"])
        self.assertIn("a job may be reading it", out)
        self.assertEqual(self.be.list_proxy_attachments().count(self.ids["vol40"]), 1)
        self.assertNotIn(self.ids["snap40"], state["snapshots"])
        for key in ("vol60", "restore", "plain"):
            self.assertIn(self.ids[key], state["volumes"])
        self.assertIn(self.ids["snap60"], state["snapshots"])
        self.assertIn(self.ids["foreign"], self.be.list_proxy_attachments())

    def test_cleanup_detach_removes_attached_temporary_volumes(self):
        self.leftovers_fixture()
        code, out, _ = self.tool(
            "leftovers", "--cleanup", "--older-than-job", "50", "--yes", "--detach"
        )
        self.assertEqual(code, 0, out)
        self.assertNotIn(self.ids["vol40"], read_state(self.be)["volumes"])
        self.assertEqual(self.be.list_proxy_attachments(), [self.ids["foreign"]])
        self.assertIn(self.ids["vol60"], read_state(self.be)["volumes"])

    def test_cleanup_failure_exits_one(self):
        self.leftovers_fixture()
        with self.be._locked() as state:
            state["faults"]["delete_snapshot"] = {"remaining": 5, "message": "busy"}
        code, out, _ = self.tool(
            "leftovers", "--cleanup", "--older-than-job", "50", "--yes", "--detach"
        )
        self.assertEqual(code, 1)
        self.assertIn("FAILED", out)
        self.assertIn(self.ids["snap40"], read_state(self.be)["snapshots"])

    # configuration --------------------------------------------------------

    def test_config_file_is_read_with_load_options_file(self):
        self.cloud()
        loaded = mock.Mock()
        loaded.backend_config.return_value = {
            "backend": "fake",
            "fake_root": self.be._root,
            "proxy_instance_id": PROXY,
        }
        with mock.patch.object(
            options, "load_options_file", create=True, return_value=loaded
        ) as loader:
            code, out, _ = run("--config", "/etc/x.conf", "list-servers")
        loader.assert_called_once_with("/etc/x.conf")
        self.assertEqual(code, 0)
        self.assertIn("web01", out)

    def test_unreadable_config_is_exit_two(self):
        with mock.patch.object(
            options, "load_options_file", create=True, side_effect=OSError("no file")
        ):
            code, _, err = run("--config", "/missing", "list-servers")
        self.assertEqual(code, 2)
        self.assertIn("/missing", err)

    def test_config_with_a_removed_option_is_exit_two(self):
        path = os.path.join(self._tmp.name, "old.ini")
        with open(path, "w", encoding="utf-8") as f:
            f.write("backend=fake\nfake_root=%s\ntransfer_mode=glance\n" % path)
        code, _, err = run("--config", path, "list-servers")
        self.assertEqual(code, 2)
        self.assertIn("transfer_mode", err)


class BatchDToolTest(unittest.TestCase):
    setUp = ToolTest.setUp
    tearDown = ToolTest.tearDown
    cloud = ToolTest.cloud
    tool = ToolTest.tool
    snapshot_of_state = ToolTest.snapshot_of_state

    def fixture(self):
        spec = base_spec()
        spec["group_types"] = {"gt": {"consistent_group_snapshot_enabled": True}}
        spec["groups"] = [
            {"name": "g", "group_type": "gt", "volumes": [ROOT_VOL, DATA_VOL]}
        ]
        self.cloud(spec)
        group = self.be.find_consistency_group([ROOT_VOL, DATA_VOL])
        self.ids = {}
        for job in (40, 60):
            self.ids[job] = self.be.create_group_snapshot(
                group,
                group_snapshot_name(job, SERVER),
                group_snapshot_description(job, SERVER),
                5,
            )
        self.be.create_group_snapshot(group, "bareos-1-gsnap-x", "by hand", 5)

    def group_ids(self):
        return {g.id: g.name for g in self.be.list_group_snapshots("bareos-")}

    def test_leftovers_list_group_snapshots_without_changes(self):
        self.fixture()
        before = self.snapshot_of_state()
        code, out, _ = self.tool("leftovers")
        self.assertEqual(code, 1)
        self.assertIn("group snapshot", out)
        self.assertIn("job=40", out)
        self.assertIn("job=60", out)
        self.assertEqual(out.count("group snapshot"), 2)
        self.assertEqual(self.snapshot_of_state(), before)

    def test_cleanup_removes_only_older_group_snapshots(self):
        self.fixture()
        code, out, _ = self.tool(
            "leftovers", "--cleanup", "--older-than-job", "50", "--yes"
        )
        self.assertEqual(code, 0, out)
        names = set(self.group_ids().values())
        self.assertNotIn(group_snapshot_name(40, SERVER), names)
        self.assertIn(group_snapshot_name(60, SERVER), names)
        self.assertIn("bareos-1-gsnap-x", names)

    def test_cleanup_reports_a_refused_group_snapshot(self):
        self.fixture()
        snap = read_state(self.be)["group_snapshots"]
        member = next(
            s
            for g in snap.values()
            if g["name"].startswith("bareos-40-")
            for s in g["snapshot_ids"].values()
        )
        self.be.create_volume("dep", 1, {}, 5, snapshot_id=member)
        code, out, _ = self.tool(
            "leftovers", "--cleanup", "--older-than-job", "50", "--yes"
        )
        self.assertEqual(code, 1)
        self.assertIn("FAILED", out)
        self.assertIn("dependent", out)

    def test_pending_transfers_are_reported_only(self):
        self.cloud(base_spec())
        vol = self.be.create_volume("r", 1, {}, 5)
        tid, _ = self.be.create_transfer(vol.id, "bareos-70-xfer-%s" % ROOT_VOL)
        other = self.be.create_volume("o", 1, {}, 5)
        self.be.create_transfer(other.id, "someone-elses")
        before = self.snapshot_of_state()
        code, out, _ = self.tool("leftovers")
        self.assertEqual(code, 1)
        self.assertIn("transfer %s" % tid, out)
        self.assertNotIn("someone-elses", out)
        self.assertEqual(self.snapshot_of_state(), before)
        code, out, _ = self.tool(
            "leftovers", "--cleanup", "--older-than-job", "99", "--yes"
        )
        self.assertEqual(code, 0, out)
        self.assertIn("kept    transfer %s" % tid, out)
        state = read_state(self.be)
        self.assertIn(tid, state["transfers"])

    def test_a_cloud_without_groups_lists_nothing_extra(self):
        spec = base_spec()
        spec["block_storage_microversion"] = "3.10"
        self.cloud(spec)
        code, out, err = self.tool("leftovers")
        self.assertEqual((code, out.strip(), err), (0, "no leftovers", ""))


if __name__ == "__main__":
    unittest.main()
