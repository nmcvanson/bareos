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

"""Unit tests of the device lookup."""

import os
import tempfile
import time
import unittest
from unittest import mock

from bareos_openstack import devices
from bareos_openstack.cloud import CloudCancelled, CloudConflict, CloudTimeout

VOLUME = "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d"
KEY = VOLUME[:20]


class DeviceTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = self._tmp.name
        self.by_id = os.path.join(self.tmp, "by-id")
        self.sys_block = os.path.join(self.tmp, "sys-block")
        self.mounts = os.path.join(self.tmp, "mounts")
        os.makedirs(self.by_id)
        os.makedirs(self.sys_block)
        with open(self.mounts, "w", encoding="utf-8") as fh:
            fh.write("proc /proc proc rw 0 0\n")

    def tearDown(self):
        self._tmp.cleanup()

    def disk(self, name, size=4096):
        path = os.path.join(self.tmp, name)
        with open(path, "wb") as fh:
            fh.truncate(size)
        return path

    def link(self, name, target):
        os.symlink(target, os.path.join(self.by_id, name))

    def find(self, **kw):
        args = dict(
            by_id_dir=self.by_id,
            poll_interval=0.01,
            require_block=False,
            mounts_file=self.mounts,
            sys_block_dir=self.sys_block,
        )
        args.update(kw)
        return devices.find_volume_device(VOLUME, 0.3, **args)

    def test_one_match(self):
        disk = self.disk("vdb")
        self.link("virtio-" + KEY, disk)
        self.assertEqual(self.find(), os.path.realpath(disk))

    def test_no_match_times_out(self):
        self.link("virtio-other-volume-id-xx", self.disk("vdc"))
        started = time.monotonic()
        with self.assertRaises(CloudTimeout) as ctx:
            self.find()
        self.assertLess(time.monotonic() - started, 2)
        self.assertIn("/dev/disk/by-id", str(ctx.exception))

    def test_missing_directory_is_no_entry(self):
        with self.assertRaises(CloudTimeout):
            self.find(by_id_dir=os.path.join(self.tmp, "absent"))

    def test_two_targets_conflict(self):
        self.link("virtio-" + KEY, self.disk("vdb"))
        self.link("scsi-" + KEY, self.disk("vdc"))
        with self.assertRaises(CloudConflict):
            self.find()

    def test_same_target_twice_is_one(self):
        disk = self.disk("vdb")
        self.link("virtio-" + KEY, disk)
        self.link("scsi-" + KEY, disk)
        self.assertEqual(self.find(), os.path.realpath(disk))

    def test_partition_entries_ignored(self):
        disk = self.disk("vdb")
        self.link("virtio-" + KEY, disk)
        self.link("virtio-%s-part1" % KEY, self.disk("vdb1"))
        self.assertEqual(self.find(), os.path.realpath(disk))

    def test_unrelated_device_never_returned(self):
        self.disk("big", size=10 * 1024 * 1024)
        with self.assertRaises(CloudTimeout):
            self.find()

    def test_size_too_small(self):
        self.link("virtio-" + KEY, self.disk("vdb", size=1000))
        with self.assertRaises(CloudConflict):
            self.find(expected_min_size=4096)
        self.assertTrue(self.find(expected_min_size=1000))

    def test_mounted_device_and_partition_refused(self):
        disk = os.path.realpath(self.disk("vdb"))
        self.link("virtio-" + KEY, disk)
        for source in (disk, disk + "1", disk + "p2"):
            with open(self.mounts, "w", encoding="utf-8") as fh:
                fh.write("%s /mnt ext4 rw 0 0\n" % source)
            with self.assertRaises(CloudConflict):
                self.find()
        with open(self.mounts, "w", encoding="utf-8") as fh:
            fh.write("%sa /mnt ext4 rw 0 0\n" % disk)
        self.assertEqual(self.find(), disk)

    def test_holders_refused(self):
        disk = self.disk("vdb")
        self.link("virtio-" + KEY, disk)
        holders = os.path.join(self.sys_block, "vdb", "holders")
        os.makedirs(holders)
        self.assertTrue(self.find())
        open(os.path.join(holders, "dm-0"), "w").close()
        with self.assertRaises(CloudConflict):
            self.find()

    def test_cancel_check_stops_poll(self):
        calls = []

        def cancel():
            calls.append(1)
            if len(calls) >= 3:
                raise CloudCancelled("operation cancelled")

        with self.assertRaises(CloudCancelled):
            devices.find_volume_device(
                VOLUME,
                30,
                by_id_dir=self.by_id,
                poll_interval=0.01,
                cancel_check=cancel,
            )
        self.assertEqual(len(calls), 3)

    def test_regular_file_refused_when_block_required(self):
        self.link("virtio-" + KEY, self.disk("vdb"))
        with self.assertRaises(CloudConflict):
            self.find(require_block=True)

    def test_device_size(self):
        self.assertEqual(devices.device_size(self.disk("x", size=12345)), 12345)

    def as_block(self):
        patcher = mock.patch.object(devices.stat, "S_ISBLK", return_value=True)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_block_device_without_holders_info_is_refused(self):
        self.link("virtio-" + KEY, self.disk("vdb"))
        self.as_block()
        with self.assertRaises(CloudConflict) as ctx:
            self.find()
        self.assertIn("cannot verify", str(ctx.exception))

    def test_block_device_with_empty_holders_is_accepted(self):
        disk = self.disk("vdb")
        self.link("virtio-" + KEY, disk)
        self.as_block()
        os.makedirs(os.path.join(self.sys_block, "vdb", "holders"))
        self.assertEqual(self.find(), os.path.realpath(disk))

    def test_unreadable_holders_is_refused(self):
        self.link("virtio-" + KEY, self.disk("vdb"))
        os.makedirs(os.path.join(self.sys_block, "vdb", "holders"))
        real = os.listdir

        def listdir(path):
            if str(path).endswith("holders"):
                raise PermissionError("denied")
            return real(path)

        with mock.patch.object(devices.os, "listdir", side_effect=listdir):
            with self.assertRaises(CloudConflict) as ctx:
                self.find()
        self.assertIn("cannot verify", str(ctx.exception))

    def test_held_partition_is_refused(self):
        self.link("virtio-" + KEY, self.disk("vdb"))
        self.as_block()
        os.makedirs(os.path.join(self.sys_block, "vdb", "holders"))
        part = os.path.join(self.sys_block, "vdb1", "holders")
        os.makedirs(part)
        self.assertTrue(self.find())
        open(os.path.join(part, "dm-3"), "w").close()
        with self.assertRaises(CloudConflict) as ctx:
            self.find()
        self.assertIn("vdb1", str(ctx.exception))

    def test_unreadable_sys_block_dir_is_refused(self):
        self.link("virtio-" + KEY, self.disk("vdb"))
        self.as_block()
        os.makedirs(os.path.join(self.sys_block, "vdb", "holders"))
        real = os.listdir

        def listdir(path):
            if str(path) == self.sys_block:
                raise PermissionError("denied")
            return real(path)

        with mock.patch.object(devices.os, "listdir", side_effect=listdir):
            with self.assertRaises(CloudConflict) as ctx:
                self.find()
        self.assertIn("cannot verify", str(ctx.exception))

    def test_listed_partition_without_holders_info_is_refused(self):
        self.link("virtio-" + KEY, self.disk("vdb"))
        self.as_block()
        os.makedirs(os.path.join(self.sys_block, "vdb", "holders"))
        os.makedirs(os.path.join(self.sys_block, "vdb1"))
        with self.assertRaises(CloudConflict) as ctx:
            self.find()
        self.assertIn("vdb1", str(ctx.exception))

    def test_partition_that_disappears_is_accepted(self):
        disk = self.disk("vdb")
        self.link("virtio-" + KEY, disk)
        self.as_block()
        os.makedirs(os.path.join(self.sys_block, "vdb", "holders"))
        os.makedirs(os.path.join(self.sys_block, "vdb1"))
        real = os.listdir
        calls = {"n": 0}

        def listdir(path):
            result = real(path)
            if str(path) == self.sys_block:
                calls["n"] += 1
                if calls["n"] > 1:
                    return [n for n in result if n != "vdb1"]
            return result

        with mock.patch.object(devices.os, "listdir", side_effect=listdir):
            self.assertEqual(self.find(), os.path.realpath(disk))


if __name__ == "__main__":
    unittest.main()
