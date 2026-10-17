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

"""Stand-in for the bareosfd C module, so the plugin runs without a Bareos build"""

import stat as _stat
import sys
import time

bRC_OK = 0
bRC_Stop = 1
bRC_Error = 2
bRC_More = 3
bRC_Term = 4
bRC_Seen = 5
bRC_Core = 6
bRC_Skip = 7
bRC_Cancel = 8

M_ABORT = 1
M_DEBUG = 2
M_FATAL = 3
M_ERROR = 4
M_WARNING = 5
M_INFO = 6

FT_LNKSAVED = 1
FT_REGE = 2
FT_REG = 3
FT_LNK = 4
FT_DIREND = 5
FT_SPEC = 6
FT_NOCHG = 10
FT_DIRNOCHG = 11
FT_DIRBEGIN = 18
FT_DELETED = 23
FT_RESTORE_FIRST = 25

CF_SKIP = 1
CF_ERROR = 2
CF_EXTRACT = 3
CF_CREATED = 4
CF_CORE = 5

IO_OPEN = 1
IO_READ = 2
IO_WRITE = 3
IO_CLOSE = 4
IO_SEEK = 5

iostat_error = -1
iostat_do_in_plugin = 0
iostat_do_in_core = 1

bVarJobId = 1
bVarFDName = 2
bVarLevel = 3
bVarType = 4
bVarClient = 5
bVarJobName = 6
bVarSinceTime = 8
bVarAccurate = 9
bVarWorkingDir = 12
bVarWhere = 13
bVarUsedConfig = 21
bVarAccurateOptions = 23

bEventJobStart = 1
bEventJobEnd = 2
bEventStartBackupJob = 3
bEventEndBackupJob = 4
bEventStartRestoreJob = 5
bEventEndRestoreJob = 6
bEventCancelCommand = 14
bEventRestoreObject = 15
bEventEndFileSet = 16
bEventPluginCommand = 17
bEventHandleBackupFile = 19
bEventNewPluginOptions = 20

bAccurateOptionInode = 1
bAccurateOptionPermissions = 2
bAccurateOptionNlink = 4
bAccurateOptionUid = 8
bAccurateOptionGid = 16
bAccurateOptionSize = 32
bAccurateOptionAtime = 64
bAccurateOptionMtime = 128
bAccurateOptionCtime = 256
bAccurateOptionSizeDecrease = 512
bAccurateOptionAlways = 1024
bAccurateOptionMD5 = 2048
bAccurateOptionSha1 = 4096


class StatPacket:
    """Defaults of the StatPacket of module/bareosfd.cc"""

    def __init__(self):
        now = int(time.time())
        self.st_dev = 0
        self.st_ino = 0
        self.st_mode = 0o700 | _stat.S_IFREG
        self.st_nlink = 0
        self.st_uid = 0
        self.st_gid = 0
        self.st_rdev = 0
        self.st_size = -1
        self.st_atime = now
        self.st_mtime = now
        self.st_ctime = now
        self.st_blksize = 4096
        self.st_blocks = 1


class SavePacket:
    """Save packet filled by start_backup_file"""

    def __init__(self):
        self.fname = ""
        self.link = ""
        self.statp = StatPacket()
        self.type = 0
        self.flags = bytearray(16)
        self.no_read = False
        self.portable = False
        self.object_name = ""
        self.object = b""
        self.object_len = 0
        self.object_index = 0


class RestorePacket:
    """Restore packet passed to create_file"""

    def __init__(self):
        self.ofname = ""
        self.olname = ""
        self.where = ""
        self.replace = 0
        self.create_status = 0
        self.statp = StatPacket()
        self.filedes = -1
        self.original_file_name = ""
        self.type = FT_REG
        self.file_index = 0


class IoPacket:
    """I/O packet passed to plugin_io"""

    def __init__(self):
        self.func = 0
        self.count = 0
        self.flags = 0
        self.mode = 0
        self.buf = bytearray()
        self.fname = ""
        self.status = 0
        self.io_errno = 0
        self.lerror = 0
        self.whence = 0
        self.offset = 0
        self.win32 = False
        self.filedes = -1


class RestoreObject:
    """Restore object passed to restore_object_data"""

    def __init__(self):
        self.object_name = ""
        self.object = b""
        self.plugin_name = ""
        self.object_len = 0
        self.object_full_len = 0
        self.object_index = 0
        self.jobid = 0


messages = []
debug_messages = []
registered_events = []
values = {}


def _default_values():
    return {
        bVarJobId: 1,
        bVarFDName: "test-fd",
        bVarLevel: ord("F"),
        bVarType: ord("B"),
        bVarClient: "test-fd",
        bVarJobName: "openstack-test.2026-10-12_10.00.00_01",
        bVarSinceTime: 0,
        bVarAccurate: 0,
        bVarWorkingDir: "/tmp",
        bVarWhere: "",
        bVarUsedConfig: "/nonexistent/*/*.conf",
        bVarAccurateOptions: frozenset({128, 256, 32}),
    }


def JobMessage(level, msg):  # pylint: disable=invalid-name
    """Record a job message"""
    messages.append((level, msg))


def DebugMessage(level, msg):  # pylint: disable=invalid-name
    """Record a debug message"""
    debug_messages.append((level, msg))


def RegisterEvents(events):  # pylint: disable=invalid-name
    """Record registered events"""
    registered_events.extend(events)


def GetValue(var):  # pylint: disable=invalid-name
    """Read a Bareos variable"""
    return values[var]


def SetValue(var, value):  # pylint: disable=invalid-name
    """Write a Bareos variable"""
    values[var] = value
    return bRC_OK


def reset(**overrides):
    """Restore the defaults and clear the recordings; keywords override values"""
    messages.clear()
    debug_messages.clear()
    registered_events.clear()
    values.clear()
    values.update(_default_values())
    for name, value in overrides.items():
        values[globals()[name]] = value


def install():
    """Make this module importable as bareosfd and return it"""
    module = sys.modules[__name__]
    sys.modules["bareosfd"] = module
    reset()
    return module
