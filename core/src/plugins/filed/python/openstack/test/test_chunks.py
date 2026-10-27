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

"""Unit tests of bareos_openstack.chunks"""

import hashlib
import os
import queue
import re
import random
import shutil
import tempfile
import threading
import time
import unittest

import bareosfd_stub
from bareos_openstack import chunks

CHUNK = 65536


def collect(q, timeout=10):
    """Read queue items until ProducerEnd or ProducerFailure"""
    items = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        item = q.get(timeout=timeout)
        items.append(item)
        if isinstance(item, (chunks.ProducerEnd, chunks.ProducerFailure)):
            return items
    raise AssertionError("producer did not finish")


class TempDirCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="openstack-chunks-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def make_file(self, name, data):
        path = os.path.join(self.tmp, name)
        with open(path, "wb") as f:
            f.write(data)
        return path


class DigestTests(unittest.TestCase):
    def test_pack_digest_size_and_range(self):
        fields = chunks.pack_digest(hashlib.sha256(b"x").digest())
        self.assertEqual(tuple(fields), chunks.HIJACKED_FIELDS)
        for value in fields.values():
            self.assertTrue(0 <= value < 2**48)

    def test_pack_digest_reassembles_24_bytes(self):
        digest = hashlib.sha256(b"abc").digest()
        fields = chunks.pack_digest(digest)
        raw = b"".join(
            fields[name].to_bytes(6, "big") for name in chunks.HIJACKED_FIELDS
        )
        self.assertEqual(raw, digest[:24])

    def test_short_digest_is_padded(self):
        fields = chunks.pack_digest(b"\x01\x02")
        self.assertEqual(fields["st_mtime"], 0x010200000000)
        self.assertEqual(fields["st_ino"], 0)

    def test_apply_and_read_back(self):
        statp = bareosfd_stub.StatPacket()
        digest = hashlib.sha256(b"data").digest()
        chunks.apply_digest(statp, digest)
        self.assertEqual(chunks.digest_fields(statp), chunks.pack_digest(digest))
        self.assertEqual(statp.st_nlink, 0)

    def test_compute_and_zero_digest(self):
        self.assertEqual(
            chunks.compute_digest("sha256", b"abc"), hashlib.sha256(b"abc").digest()
        )
        self.assertEqual(
            chunks.zero_digest("sha256", 100), hashlib.sha256(bytes(100)).digest()
        )
        big = 3 * 1024 * 1024 + 5
        self.assertEqual(
            chunks.zero_digest("md5", big), hashlib.md5(bytes(big)).digest()
        )

    def test_is_zero(self):
        self.assertTrue(chunks.is_zero(bytearray(100)))
        self.assertTrue(chunks.is_zero(b""))
        self.assertFalse(chunks.is_zero(b"\x00\x01"))


class ReadExactTests(TempDirCase):
    def test_reads_and_raises_on_eof(self):
        path = self.make_file("f", b"abcdef")
        fd = os.open(path, os.O_RDONLY)
        try:
            self.assertEqual(chunks.read_exact(fd, 4), bytearray(b"abcd"))
            with self.assertRaises(chunks.ChunkError):
                chunks.read_exact(fd, 10)
        finally:
            os.close(fd)


class ProducerTests(TempDirCase):
    def run_producer(self, paths, chunk_size=CHUNK, depth=64, release=None):
        q = queue.Queue(maxsize=depth)
        cancel = threading.Event()
        logs = []
        released = []

        def open_volume(vid):
            return paths[vid]

        def release_volume(vid):
            released.append(vid)
            if release:
                release(vid)

        producer = chunks.ChunkProducer(
            list(paths),
            open_volume,
            release_volume,
            chunk_size,
            "sha256",
            q,
            cancel,
            lambda level, msg: logs.append((level, msg)),
        )
        return producer, q, cancel, logs, released

    def test_two_volumes(self):
        rnd = random.Random(1)
        data_a = rnd.randbytes(2 * CHUNK + 100)
        data_b = rnd.randbytes(CHUNK) + bytes(CHUNK) + rnd.randbytes(10)
        paths = {"a": self.make_file("a", data_a), "b": self.make_file("b", data_b)}
        producer, q, _, logs, released = self.run_producer(paths)
        producer.start()
        items = collect(q)
        producer.join(5)
        self.assertIsInstance(items[-1], chunks.ProducerEnd)
        records = [i for i in items if isinstance(i, chunks.ChunkRecord)]
        done = [i for i in items if isinstance(i, chunks.VolumeDone)]
        self.assertEqual(
            [(r.volume_id, r.index) for r in records],
            [("a", 0), ("a", 1), ("a", 2), ("b", 0), ("b", 1), ("b", 2)],
        )
        self.assertEqual(
            [(d.volume_id, d.size_bytes, d.chunk_count) for d in done],
            [("a", len(data_a), 3), ("b", len(data_b), 3)],
        )
        by_key = {(r.volume_id, r.index): r for r in records}
        self.assertEqual(by_key[("a", 2)].length, 100)
        self.assertEqual(by_key[("a", 2)].offset, 2 * CHUNK)
        self.assertEqual(bytes(by_key[("a", 1)].data), data_a[CHUNK : 2 * CHUNK])
        self.assertEqual(
            by_key[("a", 0)].digest, hashlib.sha256(data_a[:CHUNK]).digest()
        )
        self.assertIsNone(by_key[("b", 1)].data)
        self.assertEqual(by_key[("b", 1)].digest, chunks.zero_digest("sha256", CHUNK))
        self.assertEqual(released, ["a", "b"])
        # volume done comes after the last chunk of its volume, end is last
        positions = {id(i): n for n, i in enumerate(items)}
        self.assertLess(positions[id(by_key[("a", 2)])], positions[id(done[0])])
        self.assertTrue(any("reading volume a" in m for _, m in logs))

    def test_volume_summary_line_has_numeric_timings(self):
        data = random.Random(2).randbytes(2 * CHUNK) + bytes(CHUNK)
        paths = {"a": self.make_file("a", data)}
        producer, q, _, logs, _ = self.run_producer(paths)
        producer.start()
        collect(q)
        producer.join(5)
        pattern = re.compile(
            r"^volume a: (\d+) bytes in (\d+\.\d{3}) s \((\d+\.\d) MiB/s\); "
            r"read (\d+\.\d{3}) s, hash (\d+\.\d{3}) s, zero test (\d+\.\d{3}) s$"
        )
        lines = [m for lv, m in logs if lv == "info" and pattern.match(m)]
        self.assertEqual(len(lines), 1, logs)
        numbers = pattern.match(lines[0]).groups()
        self.assertEqual(int(numbers[0]), len(data))
        for value in numbers[1:]:
            self.assertGreaterEqual(float(value), 0.0)

    def test_empty_volume(self):
        paths = {"e": self.make_file("e", b"")}
        producer, q, _, _, released = self.run_producer(paths)
        producer.start()
        items = collect(q)
        self.assertEqual(
            [type(i) for i in items], [chunks.VolumeDone, chunks.ProducerEnd]
        )
        self.assertEqual(items[0].chunk_count, 0)
        self.assertEqual(released, ["e"])

    def test_open_volume_failure(self):
        q = queue.Queue(maxsize=8)

        def open_volume(_vid):
            raise RuntimeError("no device")

        producer = chunks.ChunkProducer(
            ["x"],
            open_volume,
            lambda v: None,
            CHUNK,
            "sha256",
            q,
            threading.Event(),
            lambda *a: None,
        )
        producer.start()
        items = collect(q)
        self.assertIsInstance(items[-1], chunks.ProducerFailure)
        self.assertEqual(items[-1].message, "RuntimeError: no device")

    def test_short_read_failure(self):
        # a volume that shrinks while being read ends in a failure, not bad data
        path = self.make_file("s", b"x" * 100)
        q = queue.Queue(maxsize=8)
        real = chunks.read_exact

        def short(fd, length):
            raise chunks.ChunkError("short")

        chunks.read_exact = short
        try:
            producer = chunks.ChunkProducer(
                ["s"],
                lambda v: path,
                lambda v: None,
                CHUNK,
                "sha256",
                q,
                threading.Event(),
                lambda *a: None,
            )
            producer.start()
            items = collect(q)
        finally:
            chunks.read_exact = real
        self.assertIsInstance(items[-1], chunks.ProducerFailure)
        self.assertIn("ChunkError", items[-1].message)

    def test_cancel_while_queue_full(self):
        data = os.urandom(8 * CHUNK)
        paths = {"a": self.make_file("a", data)}
        producer, q, cancel, _, _ = self.run_producer(paths, depth=1)
        producer.start()
        time.sleep(0.3)
        cancel.set()
        producer.join(5)
        self.assertFalse(producer.is_alive())

    def test_cancel_before_start(self):
        paths = {"a": self.make_file("a", b"x" * 10)}
        producer, q, cancel, _, released = self.run_producer(paths)
        cancel.set()
        producer.start()
        items = collect(q)
        self.assertEqual(items[-1], chunks.ProducerFailure("cancelled"))
        self.assertEqual(released, [])


class PipeFeederTests(unittest.TestCase):
    def test_delivers_everything(self):
        data = os.urandom(3 * 1024 * 1024 + 17)
        feeder = chunks.PipeFeeder(data)
        fd = feeder.start()
        received = bytearray()
        while True:
            part = os.read(fd, 1024 * 1024)
            if not part:
                break
            received += part
        self.assertEqual(bytes(received), data)
        self.assertIsNone(feeder.close())

    def test_early_close_reports_error(self):
        data = os.urandom(64 * 1024 * 1024)
        feeder = chunks.PipeFeeder(data)
        feeder.start()
        time.sleep(0.2)
        err = feeder.close()
        self.assertIsInstance(err, str)
        self.assertTrue(err)


class RestoreTargetTests(TempDirCase):
    def target(self, name="v.raw", size=3 * CHUNK + 10, mode="always", **kw):
        path = os.path.join(self.tmp, name)
        count = -(-size // CHUNK)
        return chunks.RestoreTarget(path, size, CHUNK, count, mode, **kw)

    def write_chunk(self, target, index, data):
        fd = target.open_chunk(index)
        os.write(fd, data)
        return target.close_chunk(fd, index)

    def test_create_refuses_existing(self):
        self.target(create=True)
        with self.assertRaises(FileExistsError):
            self.target(create=True)

    def test_create_overwrite_truncates(self):
        t = self.target(create=True)
        self.write_chunk(t, 0, b"A" * CHUNK)
        self.target(create=True, overwrite=True)
        with open(os.path.join(self.tmp, "v.raw"), "rb") as f:
            self.assertEqual(f.read(16), bytes(16))

    def test_create_size_and_mode(self):
        self.target(create=True)
        path = os.path.join(self.tmp, "v.raw")
        self.assertEqual(os.path.getsize(path), 3 * CHUNK + 10)
        self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)

    def test_existing_file_size_check(self):
        path = self.make_file("small.raw", b"x" * 100)
        with self.assertRaises(ValueError):
            chunks.RestoreTarget(path, CHUNK, CHUNK, 1, "always")
        big = self.make_file("big.raw", bytes(CHUNK))
        t = chunks.RestoreTarget(big, CHUNK, CHUNK, 1, "always", create=False)
        self.assertEqual(t.chunk_length(0), CHUNK)

    def test_chunk_length(self):
        t = self.target(create=True)
        self.assertEqual(t.chunk_length(0), CHUNK)
        self.assertEqual(t.chunk_length(3), 10)
        with self.assertRaises(IndexError):
            t.chunk_length(4)
        with self.assertRaises(IndexError):
            t.chunk_length(-1)

    def test_offset_writes_in_random_order(self):
        rnd = random.Random(7)
        pieces = [rnd.randbytes(CHUNK) for _ in range(3)] + [rnd.randbytes(10)]
        t = self.target(create=True)
        order = [2, 0, 3, 1]
        for index in order:
            n = self.write_chunk(t, index, pieces[index])
            self.assertEqual(n, len(pieces[index]))
        self.assertEqual(t.missing(), [])
        t.finalize()
        with open(os.path.join(self.tmp, "v.raw"), "rb") as f:
            self.assertEqual(f.read(), b"".join(pieces))

    def test_missing(self):
        t = self.target(create=True)
        self.write_chunk(t, 1, b"x" * CHUNK)
        self.assertEqual(t.missing(), [0, 2, 3])

    def test_zero_chunk_always_mode_zero_fills(self):
        path = self.make_file("v.raw", b"\xff" * (2 * CHUNK))
        t = chunks.RestoreTarget(path, 2 * CHUNK, CHUNK, 2, "always")
        self.write_chunk(t, 0, b"")
        with open(path, "rb") as f:
            data = f.read()
        self.assertEqual(data[:CHUNK], bytes(CHUNK))
        self.assertEqual(data[CHUNK:], b"\xff" * CHUNK)
        self.assertEqual(t.missing(), [1])

    def test_zero_chunk_if_written_mode_leaves_data(self):
        path = self.make_file("v.raw", b"\xff" * (2 * CHUNK))
        t = chunks.RestoreTarget(path, 2 * CHUNK, CHUNK, 2, "if_written")
        self.write_chunk(t, 0, b"")
        with open(path, "rb") as f:
            self.assertEqual(f.read(CHUNK), b"\xff" * CHUNK)

    def test_zero_chunk_if_written_after_data_is_zeroed(self):
        path = self.make_file("v.raw", bytes(2 * CHUNK))
        t = chunks.RestoreTarget(path, 2 * CHUNK, CHUNK, 2, "if_written")
        self.write_chunk(t, 0, b"\x07" * CHUNK)
        self.write_chunk(t, 0, b"")
        with open(path, "rb") as f:
            self.assertEqual(f.read(CHUNK), bytes(CHUNK))

    def test_wrong_length_raises(self):
        t = self.target(create=True)
        fd = t.open_chunk(0)
        os.write(fd, b"x" * 5)
        with self.assertRaises(chunks.ChunkError):
            t.close_chunk(fd, 0)
        self.assertEqual(t.missing(), [0, 1, 2, 3])
        with self.assertRaises(OSError):
            os.fstat(fd)  # the descriptor was closed

    def test_swapped_target_is_refused(self):
        t = self.target(create=True)
        path = os.path.join(self.tmp, "v.raw")
        other = self.make_file("other", bytes(3 * CHUNK + 10))
        os.unlink(path)
        os.symlink(other, path)
        with self.assertRaises(OSError):
            t.open_chunk(0)
        with self.assertRaises(OSError):
            t.finalize()
        with open(other, "rb") as f:
            self.assertEqual(f.read(), bytes(3 * CHUNK + 10))

    def test_create_refuses_symlink_with_overwrite(self):
        other = self.make_file("other", b"KEEP")
        os.symlink(other, os.path.join(self.tmp, "v.raw"))
        with self.assertRaises(OSError):
            self.target(create=True, overwrite=True)
        with open(other, "rb") as f:
            self.assertEqual(f.read(), b"KEEP")

    def test_last_short_chunk_length_is_exact(self):
        t = self.target(create=True)
        fd = t.open_chunk(3)
        os.write(fd, b"y" * 11)
        with self.assertRaises(chunks.ChunkError):
            t.close_chunk(fd, 3)


if __name__ == "__main__":
    unittest.main()
