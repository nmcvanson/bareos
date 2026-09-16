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

"""Collect enough about a Nova instance to recreate it on restore.

Kept free of any `bareosfd` dependency so this is unit-testable with plain
`unittest`.
"""

import json


def collect_instance_metadata(conn, instance, attachments=None):
    """
    Collect flavor, image, volume-attachment, and network-port info for a
    Nova instance. Pass a precomputed `attachments` list to avoid
    re-fetching it when the caller already has one.
    """
    # Nova microversion >=2.47 drops "id" from the embedded flavor dict
    # entirely, replacing it with "original_name" plus expanded fields.
    flavor_ref = (instance.flavor or {}).get("id") or (instance.flavor or {}).get(
        "original_name"
    )
    flavor = conn.compute.find_flavor(flavor_ref) if flavor_ref else None

    if attachments is None:
        attachments = conn.compute.volume_attachments(instance)
    volume_attachments = [
        {"volume_id": a.volume_id, "device": a.device} for a in attachments
    ]

    ports = [
        {
            "id": p.id,
            "network_id": p.network_id,
            "mac_address": p.mac_address,
            "fixed_ips": p.fixed_ips,
            "security_group_ids": p.security_group_ids,
        }
        for p in conn.network.ports(device_id=instance.id)
    ]

    return {
        "instance_id": instance.id,
        "instance_name": instance.name,
        "availability_zone": instance.availability_zone,
        "key_name": instance.key_name,
        "metadata": dict(instance.metadata or {}),
        "tags": list(instance.tags or []),
        "boot_from_volume": not bool(instance.image),
        "image_id": (instance.image or {}).get("id"),
        "flavor": (
            {
                "id": flavor.id,
                "name": flavor.name,
                "vcpus": flavor.vcpus,
                "ram": flavor.ram,
                "disk": flavor.disk,
            }
            if flavor
            else None
        ),
        "volume_attachments": volume_attachments,
        "ports": ports,
    }


def to_json_bytes(metadata):
    return json.dumps(metadata, indent=2).encode("utf-8")
