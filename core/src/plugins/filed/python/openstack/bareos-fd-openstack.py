#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# BAREOS - Backup Archiving REcovery Open Sourced
#
# Copyright (C) 2026 Bareos GmbH & Co. KG
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

# silence module-name warning
# pylint: disable=invalid-name
# pylint: enable=invalid-name

"""A Bareos plugin for OpenStack (agentless Nova/Cinder backup and restore).

Implements option parsing/validation and authentication + instance
resolution. Snapshot/attach/streaming and restore logic are not
implemented yet.
"""

import os
import time

from BareosFdWrapper import *  # noqa # pylint: disable=import-error,wildcard-import
import bareosfd  # pylint: disable=import-error
import BareosFdPluginBaseclass  # pylint: disable=import-error

from bareos_openstack_api.options import (  # pylint: disable=import-error
    BareosFdOpenstackOptions,
    UnknownOptionException,
)
from bareos_openstack_api.connection import connect  # pylint: disable=import-error
from bareos_openstack_api import volumes  # pylint: disable=import-error
from bareos_openstack_api import instance_metadata  # pylint: disable=import-error


@BareosPlugin  # pylint: disable=undefined-variable
class BareosFdOpenstack(BareosFdPluginBaseclass.BareosFdPluginBaseclass):
    """Plugin main class."""

    def __init__(self, plugindef):
        bareosfd.DebugMessage(
            100,
            f"Constructor called in module {__name__} with plugindef={plugindef}\n",
        )

        self.options = BareosFdOpenstackOptions()
        super().__init__(plugindef)

        self.conn = None
        self.instance = None
        self.proxy = None
        self.pending_volumes = []
        self.volume_devices = {}
        self.current_copy = None
        self._device_fd = None
        self.instance_metadata = None
        self.metadata_emitted = False
        self.object_index = 0

    def check_options(self, mandatory_options=None):
        """
        Validate option rules a flat mandatory-options list can't express
        (one-of auth style, proxy_instance_id always required).
        """
        del mandatory_options

        if not self.options.check():
            return False

        errors = self.options.check_mandatory()
        for msg in errors:
            bareosfd.JobMessage(bareosfd.M_FATAL, msg + "\n")
        return not errors

    def parse_plugin_definition(self, plugindef):
        """Parses the plugin arguments."""
        bareosfd.DebugMessage(
            100, f"parse_plugin_definition() was called in module {__name__}\n"
        )
        try:
            if not super().parse_plugin_definition(plugindef):
                return bareosfd.bRC_Error
        except UnknownOptionException as e:
            bareosfd.JobMessage(
                bareosfd.M_FATAL, f"Unknown option '{e}' passed to plugin\n"
            )
            return bareosfd.bRC_Error

        bareosfd.DebugMessage(100, f"self.options are: = {self.options}\n")

        # Full ("F") or Restore (" ") - incrementals not supported yet
        if chr(self.level) not in "F ":
            bareosfd.DebugMessage(10, f"unsupported level: {self.level}\n")
            bareosfd.JobMessage(
                bareosfd.M_FATAL, "Only Full backups are currently supported\n"
            )
            return bareosfd.bRC_Error

        if chr(self.level) == "F" and "instance" not in self.options:
            bareosfd.JobMessage(
                bareosfd.M_FATAL,
                "Mandatory option 'instance' not defined for backup\n",
            )
            return bareosfd.bRC_Error

        return bareosfd.bRC_OK

    def _connect(self):
        """
        Authenticate against OpenStack. Never pass secrets to JobMessage -
        only DebugMessage (handled generically by
        BareosFdPluginBaseclass._add_options()).
        """
        bareosfd.DebugMessage(100, "connecting to OpenStack\n")
        return connect(self.options)

    def start_backup_job(self):
        """
        Authenticate and resolve the target instance. Snapshot/attach/
        streaming are not implemented yet.
        """
        try:
            self.conn = self._connect()
        except Exception as e:  # pylint: disable=broad-except
            bareosfd.JobMessage(
                bareosfd.M_FATAL, f"OpenStack authentication failed: {e}\n"
            )
            return bareosfd.bRC_Error

        instance_ref = self.options.instance
        self.instance = self.conn.compute.find_server(instance_ref)
        if self.instance is None:
            bareosfd.JobMessage(
                bareosfd.M_FATAL, f"Instance '{instance_ref}' not found\n"
            )
            return bareosfd.bRC_Error

        bareosfd.JobMessage(
            bareosfd.M_INFO,
            f"Resolved instance '{self.instance.name}' ({self.instance.id})\n",
        )

        self.proxy = self.conn.compute.find_server(self.options.proxy_instance_id)
        if self.proxy is None:
            bareosfd.JobMessage(
                bareosfd.M_FATAL,
                f"Proxy instance '{self.options.proxy_instance_id}' not found\n",
            )
            return bareosfd.bRC_Error

        attachments = None
        if self.options.include_volumes:
            attachments = list(
                self.conn.compute.volume_attachments(self.instance)
            )
            self.volume_devices = {a.volume_id: a.device for a in attachments}
            self.pending_volumes = volumes.list_attached_volumes(
                self.conn, self.instance, attachments=attachments
            )

        try:
            self.instance_metadata = instance_metadata.collect_instance_metadata(
                self.conn, self.instance, attachments=attachments
            )
        except Exception as e:  # pylint: disable=broad-except
            bareosfd.JobMessage(
                bareosfd.M_FATAL, f"Failed to collect instance metadata: {e}\n"
            )
            return bareosfd.bRC_Error

        return bareosfd.bRC_OK

    def _emit_metadata_object(self, savepkt):
        """Emit the collected instance metadata as a restore object."""
        filename = f"@OPENSTACK/{self.instance.name}/{self.instance.id}/instance.json"
        object_data = bytearray(
            instance_metadata.to_json_bytes(self.instance_metadata)
        )

        savepkt.statp = bareosfd.StatPacket()
        savepkt.type = bareosfd.FT_RESTORE_FIRST
        savepkt.fname = filename
        savepkt.object_name = filename
        savepkt.object = object_data
        savepkt.object_len = len(object_data)
        self.object_index += 1
        savepkt.object_index = self.object_index

        bareosfd.JobMessage(
            bareosfd.M_INFO, f"Backing up instance metadata as {filename}\n"
        )
        return bareosfd.bRC_OK

    def start_backup_file(self, savepkt):
        """
        Snapshot the next pending volume, attach the copy to the proxy,
        and describe it as one virtual file per volume.
        """
        if not self.metadata_emitted:
            self.metadata_emitted = True
            return self._emit_metadata_object(savepkt)

        if not self.pending_volumes:
            return bareosfd.bRC_Stop

        volume = self.pending_volumes.pop(0)
        tags = {
            "created-by": "bareos",
            "bareos-job-id": str(self.jobId),
            "bareos-instance-uuid": self.instance.id,
        }
        try:
            self.current_copy = volumes.snapshot_and_attach(
                self.conn,
                volume,
                self.proxy,
                tags,
                timeout=self.options.poll_timeout,
            )
        except Exception as e:  # pylint: disable=broad-except
            bareosfd.JobMessage(
                bareosfd.M_FATAL,
                f"Failed to snapshot/attach volume {volume.id}: {e}\n",
            )
            return bareosfd.bRC_Error

        device_name = self.volume_devices.get(volume.id, volume.id).rsplit("/", 1)[-1]
        filename = (
            f"@OPENSTACK/{self.instance.name}/{self.instance.id}"
            f"/disk-{device_name}.raw"
        )

        savepkt.statp = bareosfd.StatPacket()
        savepkt.statp.st_size = volume.size * 1024**3
        savepkt.type = bareosfd.FT_REG
        savepkt.fname = filename

        bareosfd.JobMessage(
            bareosfd.M_INFO, f"Backing up volume '{volume.name}' as {filename}\n"
        )
        return bareosfd.bRC_OK

    def plugin_io_open(self, iop):
        """
        Open the attached device for reading - matches Proxmox's
        stream-a-real-fd approach rather than the base class's default,
        which assumes a real filesystem path (wrong for our virtual
        device filenames). Retries briefly since Cinder/Nova reporting the
        attachment as ready doesn't guarantee the device node has appeared
        on the proxy's OS yet.
        """
        device = self.current_copy.device
        deadline = time.monotonic() + self.options.poll_timeout
        while True:
            try:
                self._device_fd = os.open(device, os.O_RDONLY)
                break
            except FileNotFoundError:
                if time.monotonic() > deadline:
                    bareosfd.JobMessage(
                        bareosfd.M_FATAL,
                        f"Device {device} did not appear within "
                        f"{self.options.poll_timeout}s\n",
                    )
                    return bareosfd.bRC_Error
                time.sleep(1)
            except OSError as e:
                bareosfd.JobMessage(
                    bareosfd.M_FATAL, f"Failed to open device {device}: {e}\n"
                )
                return bareosfd.bRC_Error

        iop.filedes = self._device_fd
        iop.status = bareosfd.iostat_do_in_core
        return bareosfd.bRC_OK

    def plugin_io_close(self, iop):
        del iop
        if self._device_fd is not None:
            os.close(self._device_fd)
            self._device_fd = None
        return bareosfd.bRC_OK

    def end_backup_file(self):
        """Detach and delete the temp volume/snapshot for the file just backed up."""
        if self.current_copy is None:
            return bareosfd.bRC_OK
        warnings = volumes.detach_and_cleanup(
            self.conn,
            self.proxy,
            self.current_copy,
            keep_snapshot=self.options.keep_snapshot,
            timeout=self.options.poll_timeout,
        )
        for w in warnings:
            bareosfd.JobMessage(bareosfd.M_WARNING, w + "\n")
        self.current_copy = None
        return bareosfd.bRC_OK

    def create_file(self, restorepkt):
        """Not implemented yet."""
        del restorepkt
        bareosfd.JobMessage(bareosfd.M_FATAL, "restore not implemented yet\n")
        return bareosfd.bRC_Error


# vim: ts=4 tabstop=4 expandtab shiftwidth=4 softtabstop=4
