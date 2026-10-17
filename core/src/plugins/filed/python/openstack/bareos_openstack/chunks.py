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

"""Chunk data path of the OpenStack plugin (standard library only)"""

import dataclasses
import fcntl
import functools
import hashlib
import os
import queue
import stat
import struct
import threading
import time
from typing import Optional

MIB = 1024 * 1024

HIJACKED_FIELDS = ("st_mtime", "st_ctime", "st_atime", "st_ino")
FIELD_BYTES = 6
BLKZEROOUT = 0x127F
PIECE = 1024 * 1024


class ChunkError(Exception):
    """A chunk could not be read or written as required"""


def pack_digest(digest):
    """Pack a digest into four 48-bit values, one per hijacked stat field"""
    size = FIELD_BYTES * len(HIJACKED_FIELDS)
    padded = (bytes(digest) + bytes(size))[:size]
    return {
        name: int.from_bytes(padded[i * FIELD_BYTES : (i + 1) * FIELD_BYTES], "big")
        for i, name in enumerate(HIJACKED_FIELDS)
    }


def apply_digest(statp, digest):
    """Store the packed digest in the stat packet"""
    for name, value in pack_digest(digest).items():
        setattr(statp, name, value)


def digest_fields(statp):
    """Read the packed digest fields of a stat packet"""
    return {name: int(getattr(statp, name)) for name in HIJACKED_FIELDS}


def compute_digest(hash_name, data):
    """Digest of data with the named hash"""
    try:
        h = hashlib.new(hash_name, usedforsecurity=False)
    except TypeError:
        h = hashlib.new(hash_name)
    h.update(data)
    return h.digest()


@functools.lru_cache(maxsize=32)
def zero_digest(hash_name, length):
    """Digest of length zero bytes"""
    try:
        h = hashlib.new(hash_name, usedforsecurity=False)
    except TypeError:
        h = hashlib.new(hash_name)
    zeros = bytes(min(PIECE, max(length, 1)))
    left = length
    while left > 0:
        n = min(left, len(zeros))
        h.update(zeros[:n])
        left -= n
    return h.digest()


def is_zero(data):
    """True when every byte is zero"""
    return data.count(0) == len(data)


def read_exact(fd, length):
    """Read exactly length bytes from fd"""
    buf = bytearray(length)
    view = memoryview(buf)
    pos = 0
    while pos < length:
        n = os.readv(fd, [view[pos:]])
        if n == 0:
            raise ChunkError(
                "unexpected end of data after %d of %d bytes" % (pos, length)
            )
        pos += n
    return buf


def zero_fill(fd, offset, length, is_block):
    """Write zeros to [offset, offset+length) of fd"""
    if is_block:
        try:
            fcntl.ioctl(fd, BLKZEROOUT, struct.pack("QQ", offset, length))
            return
        except OSError:
            pass
    zeros = bytes(min(PIECE, max(length, 1)))
    pos = offset
    end = offset + length
    while pos < end:
        n = os.pwrite(fd, zeros[: min(len(zeros), end - pos)], pos)
        pos += n


@dataclasses.dataclass(frozen=True)
class ChunkRecord:
    """One chunk read from a volume (data None means all zero)"""

    volume_id: str
    index: int
    offset: int
    length: int
    digest: bytes
    data: Optional[bytearray]


@dataclasses.dataclass(frozen=True)
class VolumeDone:
    """All chunks of a volume were queued"""

    volume_id: str
    size_bytes: int
    chunk_count: int


@dataclasses.dataclass(frozen=True)
class ProducerFailure:
    """The producer stopped with an error"""

    message: str


@dataclasses.dataclass(frozen=True)
class ProducerEnd:
    """All volumes were queued"""


class ChunkProducer(threading.Thread):
    """Reads volume devices chunk by chunk and queues the chunks"""

    def __init__(
        self,
        volume_ids,
        open_volume,
        release_volume,
        chunk_size,
        hash_name,
        out_queue,
        cancel_event,
        log,
    ):
        super().__init__(name="openstack-chunk-producer", daemon=True)
        self._volume_ids = list(volume_ids)
        self._open_volume = open_volume
        self._release_volume = release_volume
        self._chunk_size = chunk_size
        self._hash_name = hash_name
        self._queue = out_queue
        self._cancel = cancel_event
        self._log = log

    def _put(self, item):
        """Queue an item; False when the job was cancelled before it fit"""
        while True:
            try:
                self._queue.put(item, timeout=1)
                return True
            except queue.Full:
                if self._cancel.is_set():
                    return False

    def run(self):
        try:
            for vid in self._volume_ids:
                if not self._read_volume(vid):
                    return
            self._put(ProducerEnd())
        except Exception as e:  # pylint: disable=broad-except
            self._put(ProducerFailure("%s: %s" % (type(e).__name__, e)))

    def _read_volume(self, vid):
        """Queue all chunks of one volume; False when the producer must stop"""
        if self._cancel.is_set():
            self._put(ProducerFailure("cancelled"))
            return False
        path = self._open_volume(vid)
        fd = os.open(path, os.O_RDONLY)
        index = 0
        try:
            size = os.lseek(fd, 0, os.SEEK_END)
            os.lseek(fd, 0, os.SEEK_SET)
            self._log("info", "reading volume %s (%d bytes)" % (vid, size))
            offset = 0
            reported = 0
            read_s = hash_s = zero_s = 0.0
            started = time.monotonic()
            while offset < size:
                if self._cancel.is_set():
                    self._put(ProducerFailure("cancelled"))
                    return False
                length = min(self._chunk_size, size - offset)
                t0 = time.monotonic()
                data = read_exact(fd, length)
                t1 = time.monotonic()
                digest = compute_digest(self._hash_name, data)
                t2 = time.monotonic()
                zero = is_zero(data)
                t3 = time.monotonic()
                read_s += t1 - t0
                hash_s += t2 - t1
                zero_s += t3 - t2
                record = ChunkRecord(
                    vid, index, offset, length, digest, None if zero else data
                )
                if not self._put(record):
                    return False
                index += 1
                offset += length
                percent = offset * 100 // size
                while reported < 100 and percent >= reported + 25:
                    reported += 25
                    self._log("info", "volume %s: %d%% read" % (vid, reported))
            elapsed = time.monotonic() - started
            rate = size / MIB / elapsed if elapsed > 0 else 0.0
            self._log(
                "info",
                "volume %s: %d bytes in %.3f s (%.1f MiB/s); read %.3f s, "
                "hash %.3f s, zero test %.3f s"
                % (vid, size, elapsed, rate, read_s, hash_s, zero_s),
            )
        finally:
            os.close(fd)
        if not self._put(VolumeDone(vid, size, index)):
            return False
        self._release_volume(vid)
        return True


class PipeFeeder:
    """Hands a buffer to the core through a pipe fed by a writer thread"""

    def __init__(self, data):
        self._data = data
        self._read_fd = None
        self._thread = None
        self._error = None

    def start(self):
        """Create the pipe and the writer; returns the read end"""
        read_fd, write_fd = os.pipe()
        try:
            with open("/proc/sys/fs/pipe-max-size") as f:
                max_size = int(f.read())
            fcntl.fcntl(write_fd, getattr(fcntl, "F_SETPIPE_SZ", 1031), max_size)
        except (OSError, ValueError):
            pass
        self._read_fd = read_fd
        self._thread = threading.Thread(
            target=self._write,
            args=(write_fd,),
            name="openstack-pipe-feeder",
            daemon=True,
        )
        self._thread.start()
        return read_fd

    def _write(self, write_fd):
        try:
            view = memoryview(self._data)
            pos = 0
            while pos < len(view):
                pos += os.write(write_fd, view[pos:])
        except OSError as e:
            self._error = "%s: %s" % (type(e).__name__, e)
        finally:
            os.close(write_fd)

    def close(self, timeout=60.0):
        """Close the read end and join the writer; error text or None"""
        if self._read_fd is not None:
            try:
                os.close(self._read_fd)
            except OSError:
                pass
            self._read_fd = None
        if self._thread is not None:
            self._thread.join(timeout)
            if self._thread.is_alive():
                return "pipe writer did not finish"
            self._thread = None
        return self._error


class RestoreTarget:
    """A raw volume file or device that is written chunk by chunk"""

    def __init__(
        self,
        path,
        size_bytes,
        chunk_size,
        chunk_count,
        zero_fill_mode,
        create=False,
        overwrite=False,
    ):
        self.path = path
        self.size_bytes = size_bytes
        self.chunk_size = chunk_size
        self.chunk_count = chunk_count
        self.zero_fill_mode = zero_fill_mode
        self.received = set()
        self._written = set()
        if create:
            flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
            flags |= getattr(os, "O_NOFOLLOW", 0)
            if not overwrite:
                flags |= os.O_EXCL
            fd = os.open(path, flags, 0o600)
            try:
                os.ftruncate(fd, size_bytes)
            finally:
                os.close(fd)
        else:
            fd = os.open(path, os.O_RDONLY)
            try:
                actual = os.lseek(fd, 0, os.SEEK_END)
            finally:
                os.close(fd)
            if actual < size_bytes:
                raise ValueError(
                    "%s is %d bytes, smaller than the %d bytes to restore"
                    % (path, actual, size_bytes)
                )
        info = os.stat(path)
        self.is_block = stat.S_ISBLK(info.st_mode)
        self._identity = (info.st_dev, info.st_ino)
        self._nofollow = getattr(os, "O_NOFOLLOW", 0) if create else 0

    def _open(self):
        """Open the target for writing; it must still be the file seen at start"""
        fd = os.open(self.path, os.O_WRONLY | self._nofollow)
        info = os.fstat(fd)
        if (info.st_dev, info.st_ino) != self._identity:
            os.close(fd)
            raise OSError("%s was replaced while it was being restored" % self.path)
        return fd

    def chunk_length(self, index):
        """Expected length of a chunk"""
        if not 0 <= index < self.chunk_count:
            raise IndexError(
                "chunk index %d is outside 0..%d" % (index, self.chunk_count - 1)
            )
        return min(self.chunk_size, self.size_bytes - index * self.chunk_size)

    def open_chunk(self, index):
        """Open the target positioned at the start of the chunk"""
        self.chunk_length(index)
        fd = self._open()
        try:
            os.lseek(fd, index * self.chunk_size, os.SEEK_SET)
        except OSError:
            os.close(fd)
            raise
        return fd

    def close_chunk(self, fd, index):
        """Check what was written, zero-fill if needed and close; returns bytes"""
        try:
            offset = index * self.chunk_size
            length = self.chunk_length(index)
            written = os.lseek(fd, 0, os.SEEK_CUR) - offset
            if written == length:
                self._written.add(index)
            elif written == 0:
                if self.zero_fill_mode == "always" or index in self._written:
                    zero_fill(fd, offset, length, self.is_block)
                self._written.discard(index)
            else:
                raise ChunkError(
                    "chunk %d of %s: %d bytes written, expected %d or 0"
                    % (index, self.path, written, length)
                )
            self.received.add(index)
            return written
        finally:
            os.close(fd)

    def missing(self):
        """Sorted indexes of chunks not received yet"""
        return [i for i in range(self.chunk_count) if i not in self.received]

    def finalize(self):
        """Flush the target to stable storage"""
        fd = self._open()
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
