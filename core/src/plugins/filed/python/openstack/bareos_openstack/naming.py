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

"""Virtual file names of the OpenStack plugin (standard library only)"""

import re
from typing import NamedTuple, Optional

PLUGIN_ROOT = "@OPENSTACK"

_CHUNK_RE = re.compile(
    r"^@OPENSTACK/([^/]+)/([^/]+)/volumes/([^/]+)/([0-9]{8,})\.chunk\Z"
)
_DOCUMENT_RE = re.compile(r"^@OPENSTACK/[^/]+/[^/]+/server\.json\Z")


class ChunkPath(NamedTuple):
    """Components of a chunk file name"""

    project_id: str
    server_id: str
    volume_id: str
    index: int


def _component(value, what):
    if not isinstance(value, str) or not value:
        raise ValueError("%s must be a non-empty string" % what)
    if "/" in value or "\\" in value or "\0" in value or value in (".", ".."):
        raise ValueError("%s is not a valid path component" % what)
    return value


def server_root(project_id, server_id):
    """Directory of one server"""
    return "%s/%s/%s" % (
        PLUGIN_ROOT,
        _component(project_id, "project id"),
        _component(server_id, "server id"),
    )


def document_path(project_id, server_id):
    """Name of the server document (RestoreObject)"""
    return server_root(project_id, server_id) + "/server.json"


def chunk_path(project_id, server_id, volume_id, index):
    """Name of one chunk file"""
    _component(volume_id, "volume id")
    if not isinstance(index, int) or isinstance(index, bool) or index < 0:
        raise ValueError("chunk index must be an integer >= 0")
    return "%s/volumes/%s/%08d.chunk" % (
        server_root(project_id, server_id),
        volume_id,
        index,
    )


def parse_chunk_path(path) -> Optional[ChunkPath]:
    """Split a chunk file name; None when it is not a valid one"""
    match = _CHUNK_RE.match(path)
    if not match:
        return None
    project_id, server_id, volume_id, index = match.groups()
    try:
        for value in (project_id, server_id, volume_id):
            _component(value, "component")
    except ValueError:
        return None
    return ChunkPath(project_id, server_id, volume_id, int(index))


def is_document_path(path):
    """True when the name is the server document of a server"""
    return bool(_DOCUMENT_RE.match(path))
