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

"""Unit tests of the fake cloud command-line interface."""

import contextlib
import io
import json
import os
import random
import tempfile
import time
import unittest

from bareos_openstack import fake_backend
from bareos_openstack.cloud import CloudError, group_snapshot_description

from test_cloud_fake import (
    DATA_VOL,
    GIB_BYTES,
    PORT,
    ROOT_VOL,
    SERVER,
    base_spec,
    batch_d_spec,
)


def run(*argv):
    """Run main() in-process; return (exit code, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = fake_backend.main(list(argv))
    return code, out.getvalue(), err.getvalue()


class FakeCliTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = os.path.join(self._tmp.name, "cloud")
        self.spec = os.path.join(self._tmp.name, "spec.json")
        with open(self.spec, "w", encoding="utf-8") as fh:
            json.dump(base_spec(), fh)
        code, out, _ = run("init", "--root", self.root, "--spec", self.spec)
        self.assertEqual(code, 0)
        self.init_out = json.loads(out)

    def tearDown(self):
        self._tmp.cleanup()

    def volume_bytes(self, volume_id):
        code, out, _ = run("volume-path", "--root", self.root, "--volume", volume_id)
        self.assertEqual(code, 0)
        with open(out.strip(), "rb") as fh:
            return fh.read()

    def test_init_output_and_refusal(self):
        self.assertEqual(self.init_out["servers"], {"web01": SERVER})
        code, _, err = run("init", "--root", self.root, "--spec", self.spec)
        self.assertEqual(code, 2)
        self.assertIn("error:", err)
        code, _, _ = run("init", "--root", self.root, "--spec", self.spec, "--reset")
        self.assertEqual(code, 0)

    def test_servers_and_volume_path(self):
        code, out, _ = run("servers", "--root", self.root)
        self.assertEqual(code, 0)
        listing = json.loads(out)
        self.assertEqual(len(listing), 1)
        self.assertEqual(listing[0]["volume_ids"], [ROOT_VOL, DATA_VOL])
        self.assertEqual(listing[0]["port_ids"], [PORT])
        code, out, _ = run("servers", "--root", self.root, "--name", "nope")
        self.assertEqual(json.loads(out), [])
        path = run("volume-path", "--root", self.root, "--volume", ROOT_VOL)[1].strip()
        self.assertTrue(os.path.isabs(path))
        self.assertTrue(path.endswith(ROOT_VOL + ".img"))
        self.assertEqual(
            run("volume-path", "--root", self.root, "--volume", "nope")[0], 2
        )

    def test_write_seed_and_zero(self):
        self.assertEqual(
            run(
                "write",
                "--root",
                self.root,
                "--volume",
                DATA_VOL,
                "--offset",
                "100",
                "--length",
                "50",
                "--seed",
                "7",
            )[0],
            0,
        )
        data = self.volume_bytes(DATA_VOL)
        self.assertEqual(data[100:150], random.Random(7).randbytes(50))
        self.assertEqual(
            run(
                "write",
                "--root",
                self.root,
                "--volume",
                DATA_VOL,
                "--offset",
                "100",
                "--length",
                "50",
                "--zero",
            )[0],
            0,
        )
        self.assertEqual(self.volume_bytes(DATA_VOL)[100:150], b"\0" * 50)
        code, _, err = run(
            "write",
            "--root",
            self.root,
            "--volume",
            DATA_VOL,
            "--offset",
            str(4 * 1024 * 1024 - 10),
            "--length",
            "20",
            "--zero",
        )
        self.assertEqual(code, 2)
        self.assertIn("outside", err)

    def test_resize(self):
        size = 4 * 1024 * 1024
        self.assertEqual(
            run(
                "resize",
                "--root",
                self.root,
                "--volume",
                DATA_VOL,
                "--size-bytes",
                str(size + 4096),
            )[0],
            0,
        )
        self.assertEqual(len(self.volume_bytes(DATA_VOL)), size + 4096)
        code, _, err = run(
            "resize", "--root", self.root, "--volume", DATA_VOL, "--size-bytes", "1024"
        )
        self.assertEqual(code, 2)
        self.assertIn("error:", err)
        code, _, _ = run(
            "resize",
            "--root",
            self.root,
            "--volume",
            DATA_VOL,
            "--size-bytes",
            str(GIB_BYTES + 1),
        )
        self.assertEqual(code, 0)
        state = json.loads(run("show", "--root", self.root)[1])
        self.assertEqual(state["volumes"][DATA_VOL]["size_gb"], 2)

    def test_set_status_and_log(self):
        self.assertEqual(
            run(
                "set-status",
                "--root",
                self.root,
                "--server",
                SERVER,
                "--status",
                "SHUTOFF",
            )[0],
            0,
        )
        listing = json.loads(run("servers", "--root", self.root)[1])
        self.assertEqual(listing[0]["status"], "SHUTOFF")
        self.assertEqual(
            run("set-status", "--root", self.root, "--server", "x", "--status", "A")[0],
            2,
        )
        self.assertEqual(run("log", "--root", self.root)[1], "")

    def test_fault_delay_only_flag(self):
        code, _, _ = run(
            "fault",
            "--root",
            self.root,
            "--op",
            "create_volume",
            "--sleep",
            "0.2",
            "--phase",
            "after",
            "--delay-only",
        )
        self.assertEqual(code, 0)
        state = json.loads(run("show", "--root", self.root)[1])
        self.assertTrue(state["faults"]["create_volume"]["delay_only"])
        be = fake_backend.FakeBackend({"fake_root": self.root})
        started = time.monotonic()
        vol = be.create_volume("v", 1, {}, 5)
        self.assertGreaterEqual(time.monotonic() - started, 0.2)
        self.assertEqual(be.get_volume(vol.id).status, "available")

    def test_fault_phase_after_and_clear(self):
        code, _, _ = run(
            "fault",
            "--root",
            self.root,
            "--op",
            "create_volume",
            "--phase",
            "after",
            "--remaining",
            "3",
            "--message",
            "late",
        )
        self.assertEqual(code, 0)
        state = json.loads(run("show", "--root", self.root)[1])
        fault = state["faults"]["create_volume"]
        self.assertEqual(
            fault,
            {
                "remaining": 3,
                "message": "late",
                "sleep": 0.0,
                "phase": "after",
                "delay_only": False,
            },
        )
        run("fault", "--root", self.root, "--op", "get_server")
        state = json.loads(run("show", "--root", self.root)[1])
        self.assertEqual(
            state["faults"]["get_server"]["message"], "injected fault in get_server"
        )
        self.assertEqual(run("clear-faults", "--root", self.root)[0], 0)
        state = json.loads(run("show", "--root", self.root)[1])
        self.assertEqual(state["faults"], {})

    def test_check_clean(self):
        code, out, _ = run("check-clean", "--root", self.root)
        self.assertEqual((code, out.strip()), (0, "clean"))
        be = fake_backend.FakeBackend({"fake_root": self.root})
        snap = be.create_snapshot(
            ROOT_VOL, "s", {"bareos-role": "backup-snapshot"}, True, 5
        )
        code, out, _ = run("check-clean", "--root", self.root)
        self.assertEqual(code, 1)
        self.assertIn(snap.id, out)
        be.delete_snapshot(snap.id, 5)
        vol = be.create_volume("t", 1, {"bareos-role": "backup-temp-volume"}, 5)
        self.assertEqual(run("check-clean", "--root", self.root)[0], 1)
        be.attach_to_proxy(vol.id, 5)
        be.detach_from_proxy(vol.id, 5)
        be.delete_volume(vol.id, 5)
        self.assertEqual(run("check-clean", "--root", self.root)[0], 0)
        other = be.create_volume("plain", 1, {}, 5)
        be.attach_to_proxy(other.id, 5)
        code, out, _ = run("check-clean", "--root", self.root)
        self.assertEqual(code, 1)
        self.assertIn("attached to the proxy", out)

    def test_usage_error_exit_code(self):
        code, _, err = run("write", "--root", self.root)
        self.assertEqual(code, 2)
        self.assertIn("usage", err)
        self.assertEqual(run()[0], 2)
        self.assertEqual(
            run(
                "write",
                "--root",
                self.root,
                "--volume",
                ROOT_VOL,
                "--offset",
                "0",
                "--length",
                "1",
                "--seed",
                "1",
                "--zero",
            )[0],
            2,
        )

    def test_clouderror_is_exit_two(self):
        code, _, err = run("show", "--root", os.path.join(self._tmp.name, "none"))
        self.assertEqual(code, 2)
        self.assertTrue(err.startswith("error:"))
        self.assertTrue(issubclass(CloudError, Exception))

class FakeCliBatchDTest(unittest.TestCase):
    def test_check_clean_lists_group_snapshots_and_transfers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = os.path.join(tmp, "cloud")
            spec = batch_d_spec()
            spec_path = os.path.join(tmp, "spec.json")
            with open(spec_path, "w", encoding="utf-8") as fh:
                json.dump(spec, fh)
            self.assertEqual(run("init", "--root", root, "--spec", spec_path)[0], 0)
            be = fake_backend.FakeBackend(
                {"backend": "fake", "fake_root": root, "proxy_instance_id": ""}
            )
            be.connect()
            group = be.find_consistency_group([ROOT_VOL, DATA_VOL])
            gsnap = be.create_group_snapshot(
                group, "bareos-3-gsnap-s", group_snapshot_description(3, SERVER), 5
            )
            vol = be.create_volume("v", 1, {}, 5)
            tid, _ = be.create_transfer(vol.id, "bareos-3-xfer-x")
            code, out, _ = run("check-clean", "--root", root)
            self.assertEqual(code, 1)
            self.assertIn("group-snapshot", out)
            self.assertIn("transfer %s pending" % tid, out)
            self.assertEqual(len(gsnap), 2)


if __name__ == "__main__":
    unittest.main()
