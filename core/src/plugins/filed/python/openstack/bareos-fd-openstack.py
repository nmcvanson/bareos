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

from BareosFdWrapper import *  # noqa # pylint: disable=import-error,wildcard-import
import bareosfd  # pylint: disable=import-error
import BareosFdPluginBaseclass  # pylint: disable=import-error

from bareos_openstack_api.options import (  # pylint: disable=import-error
    BareosFdOpenstackOptions,
    UnknownOptionException,
)
from bareos_openstack_api.connection import connect  # pylint: disable=import-error


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
        return bareosfd.bRC_OK

    def start_backup_file(self, savepkt):
        """Not implemented yet."""
        del savepkt
        bareosfd.JobMessage(
            bareosfd.M_FATAL, "backup file enumeration not implemented yet\n"
        )
        return bareosfd.bRC_Error

    def create_file(self, restorepkt):
        """Not implemented yet."""
        del restorepkt
        bareosfd.JobMessage(bareosfd.M_FATAL, "restore not implemented yet\n")
        return bareosfd.bRC_Error


# vim: ts=4 tabstop=4 expandtab shiftwidth=4 softtabstop=4
