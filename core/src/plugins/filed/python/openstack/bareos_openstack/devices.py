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

"""Identify the local block device of a Cinder volume attached to the proxy."""

import os
import re
import stat
import time

from .cloud import CloudConflict, CloudTimeout

_PARTITION_SUFFIX = re.compile(r"^p?[0-9]+$")


def _candidates(by_id_dir, key):
    """Return the distinct existing targets of by-id entries naming the volume."""
    try:
        names = os.listdir(by_id_dir)
    except OSError:
        return []
    found = []
    for name in sorted(names):
        if key not in name or "-part" in name:
            continue
        target = os.path.realpath(os.path.join(by_id_dir, name))
        if os.path.exists(target) and target not in found:
            found.append(target)
    return found


def _mounted(device, mounts_file):
    """Return True when the device or one of its partitions is mounted."""
    try:
        with open(mounts_file, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()
    except OSError as exc:
        raise CloudConflict("cannot read %s: %s" % (mounts_file, exc)) from exc
    for line in lines:
        fields = line.split()
        if not fields or not fields[0].startswith("/"):
            continue
        source = os.path.realpath(fields[0])
        if source == device:
            return True
        if source.startswith(device) and _PARTITION_SUFFIX.match(source[len(device) :]):
            return True
    return False


def _verify(device, expected_min_size, require_block, mounts_file, sys_block_dir):
    mode = os.stat(device).st_mode
    if require_block and not stat.S_ISBLK(mode):
        raise CloudConflict("%s is not a block device" % device)
    if not require_block and not (stat.S_ISBLK(mode) or stat.S_ISREG(mode)):
        raise CloudConflict("%s is not a block device or file" % device)
    size = device_size(device)
    if size < expected_min_size:
        raise CloudConflict(
            "device %s has %d bytes, expected at least %d"
            % (device, size, expected_min_size)
        )
    if _mounted(device, mounts_file):
        raise CloudConflict("device %s is mounted; refusing to use it" % device)
    held = _holders(device, stat.S_ISBLK(mode), sys_block_dir)
    if held:
        raise CloudConflict(
            "device %s is held by %s; refusing to use it" % (device, ", ".join(held))
        )


def _partitions(name, sys_block_dir, is_block):
    """List sysfs entries of the partitions of `name`; fail closed for block devices."""
    try:
        siblings = os.listdir(sys_block_dir)
    except OSError:
        if is_block:
            raise
        return []
    return sorted(
        s
        for s in siblings
        if s.startswith(name) and _PARTITION_SUFFIX.match(s[len(name) :])
    )


def _holders(device, is_block, sys_block_dir):
    """Return the holders of the device and of its partitions.

    For block devices any missing or unreadable information (partition list,
    holders of the disk or of a listed partition that did not just disappear) is
    an error; regular files (tests) may have no sysfs entry.
    """
    name = os.path.basename(device)

    def unverified(reason):
        return CloudConflict(
            "cannot verify that device %s is not held: %s" % (device, reason)
        )

    try:
        partitions = _partitions(name, sys_block_dir, is_block)
    except OSError as exc:
        raise unverified("cannot list %s: %s" % (sys_block_dir, exc)) from exc
    held = []
    for entry in [name] + partitions:
        path = os.path.join(sys_block_dir, entry, "holders")
        try:
            held += ["%s (%s)" % (h, entry) for h in os.listdir(path)]
        except FileNotFoundError as exc:
            if not is_block:
                continue
            if entry != name:
                try:
                    still = entry in _partitions(name, sys_block_dir, True)
                except OSError:
                    still = True
                if not still:
                    continue
            raise unverified("%s missing" % path) from exc
        except OSError as exc:
            raise unverified(exc) from exc
    return held


def find_volume_device(
    volume_id,
    timeout,
    by_id_dir="/dev/disk/by-id",
    poll_interval=1.0,
    expected_min_size=0,
    require_block=True,
    mounts_file="/proc/mounts",
    sys_block_dir="/sys/class/block",
    cancel_check=None,
):
    """Wait for the by-id entry of the volume and return its verified device path."""
    key = volume_id[:20]
    deadline = time.monotonic() + timeout
    while True:
        if cancel_check is not None:
            cancel_check()
        devices = _candidates(by_id_dir, key)
        if len(devices) > 1:
            raise CloudConflict(
                "volume %s matches several devices: %s"
                % (volume_id, ", ".join(devices))
            )
        if devices:
            _verify(
                devices[0], expected_min_size, require_block, mounts_file, sys_block_dir
            )
            return devices[0]
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise CloudTimeout(
                "no device for volume %s appeared under %s within %d seconds; "
                "the proxy must expose Cinder volume serials under /dev/disk/by-id"
                % (volume_id, by_id_dir, timeout)
            )
        time.sleep(min(poll_interval, remaining))


def device_size(path):
    """Return the size in bytes of a block device or file."""
    fd = os.open(path, os.O_RDONLY)
    try:
        return os.lseek(fd, 0, os.SEEK_END)
    finally:
        os.close(fd)
