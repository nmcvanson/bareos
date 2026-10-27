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

"""Server document (server.json) of an OpenStack backup: build, check, select."""

import copy
import hashlib
import json

from .cloud import CONSISTENCY_GROUP, CONSISTENCY_PER_VOLUME, GIB

FORMAT_VERSION = 1
DOCUMENT_KIND = "bareos-openstack-server"
SUPPORTED_HASHES = frozenset(
    name for name in hashlib.algorithms_guaranteed if not name.startswith("shake_")
)
MIN_CHUNK_SIZE = 1024 * 1024
MAX_CHUNK_SIZE = 1024**3
CHUNK_ALIGN = 65536
OLD_VERSION_TEXT = "made by an older plugin version; not supported"
CONSISTENCY_VALUES = (CONSISTENCY_PER_VOLUME, CONSISTENCY_GROUP)


class DocumentError(ValueError):
    """The server document is malformed or inconsistent."""


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def _check_component(value, what):
    if (
        not isinstance(value, str)
        or not value
        or len(value) > 255
        or "/" in value
        or "\\" in value
        or "\0" in value
        or value in (".", "..")
    ):
        raise DocumentError("%s is not a valid identifier: %r" % (what, value))


def _section(doc, key):
    value = doc.get(key)
    if not isinstance(value, dict):
        raise DocumentError("%s is missing or not an object" % key)
    return value


def validate_document(doc):
    """Raise DocumentError unless `doc` is a consistent server document."""
    if not isinstance(doc, dict):
        raise DocumentError("the document is not an object")
    if doc.get("kind") != DOCUMENT_KIND:
        raise DocumentError("unexpected document kind %r" % (doc.get("kind"),))
    if doc.get("format") != FORMAT_VERSION or isinstance(doc.get("format"), bool):
        raise DocumentError(
            "unsupported document format %r (supported: %d)"
            % (doc.get("format"), FORMAT_VERSION)
        )
    job = _section(doc, "job")
    if not _is_int(job.get("job_id")) or job["job_id"] < 0:
        raise DocumentError("job.job_id must be a non-negative integer")
    chunking = _section(doc, "chunking")
    chunk_size = chunking.get("chunk_size")
    if (
        not _is_int(chunk_size)
        or chunk_size % CHUNK_ALIGN
        or not MIN_CHUNK_SIZE <= chunk_size <= MAX_CHUNK_SIZE
    ):
        raise DocumentError(
            "chunking.chunk_size must be a multiple of 64 KiB from 1 MiB to 1 GiB"
        )
    if chunking.get("hash") not in SUPPORTED_HASHES:
        raise DocumentError("unsupported hash %r" % (chunking.get("hash"),))
    server = _section(doc, "server")
    _check_component(server.get("id"), "server.id")
    _check_component(server.get("project_id"), "server.project_id")
    if not isinstance(server.get("name"), str):
        raise DocumentError("server.name must be a string")
    if not isinstance(doc.get("boot_from_volume"), bool):
        raise DocumentError("boot_from_volume must be a boolean")
    if not doc["boot_from_volume"] or doc.get("boot_source", "volume") != "volume":
        raise DocumentError("the server boots from an image: " + OLD_VERSION_TEXT)
    if doc.get("consistency", CONSISTENCY_PER_VOLUME) not in CONSISTENCY_VALUES:
        raise DocumentError(
            "consistency must be one of %s" % ", ".join(CONSISTENCY_VALUES)
        )
    volumes = doc.get("volumes")
    if not isinstance(volumes, list) or not volumes:
        raise DocumentError("volumes must be a non-empty list")
    seen = set()
    boot_volumes = 0
    for vol in volumes:
        if not isinstance(vol, dict):
            raise DocumentError("a volume entry is not an object")
        _check_component(vol.get("id"), "volume id")
        if vol["id"] in seen:
            raise DocumentError("duplicate volume id %s" % vol["id"])
        seen.add(vol["id"])
        source = vol.setdefault("source", "volume")
        if source == "image-root" or doc.get("root_image"):
            raise DocumentError(
                "volume %s is an image root disk: %s" % (vol["id"], OLD_VERSION_TEXT)
            )
        if source != "volume":
            raise DocumentError("volume %s: unknown source %r" % (vol["id"], source))
        metadata = vol.setdefault("image_metadata", {})
        if not isinstance(metadata, dict) or any(
            not isinstance(k, str) or not isinstance(v, str)
            for k, v in metadata.items()
        ):
            raise DocumentError(
                "volume %s: image_metadata must map strings to strings" % vol["id"]
            )
        size_gb = vol.get("size_gb")
        if not _is_int(size_gb) or size_gb < 1:
            raise DocumentError("volume %s: size_gb must be at least 1" % vol["id"])
        size_bytes = vol.get("size_bytes")
        if not _is_int(size_bytes) or not 0 <= size_bytes <= size_gb * GIB:
            raise DocumentError(
                "volume %s: size_bytes must be between 0 and size_gb * 1 GiB"
                % vol["id"]
            )
        count = vol.get("chunk_count")
        if not _is_int(count) or count != -(-size_bytes // chunk_size):
            raise DocumentError(
                "volume %s: chunk_count does not match size_bytes and chunk_size"
                % vol["id"]
            )
        boot_index = vol.get("boot_index")
        if boot_index is not None and not _is_int(boot_index):
            raise DocumentError("volume %s: boot_index is not an integer" % vol["id"])
        if boot_index == 0:
            boot_volumes += 1
    if boot_volumes > 1:
        raise DocumentError("more than one volume has boot_index 0")
    if doc["boot_from_volume"] and boot_volumes != 1:
        raise DocumentError("a server that boots from a volume needs one boot volume")
    if "root_restorable" in doc:
        if not isinstance(doc["root_restorable"], bool):
            raise DocumentError("root_restorable must be a boolean")
        if doc["root_restorable"] != (boot_volumes == 1):
            raise DocumentError(
                "root_restorable does not agree with the volume boot indexes"
            )
    else:
        doc["root_restorable"] = doc["boot_from_volume"]
    ports = doc.get("ports")
    if not isinstance(ports, list):
        raise DocumentError("ports must be a list")
    for port in ports:
        if (
            not isinstance(port, dict)
            or not isinstance(port.get("id"), str)
            or not isinstance(port.get("network_id"), str)
        ):
            raise DocumentError("a port entry needs string id and network_id")


def build_document(inventory, volume_states, job, chunking):
    """Return the validated server document of a backup inventory."""
    volumes = []
    for vol in inventory.volumes:
        state = volume_states.get(vol.id)
        if state is None:
            raise DocumentError("no state recorded for volume %s" % vol.id)
        entry = vol.to_dict()
        entry["size_bytes"] = state["size_bytes"]
        entry["chunk_count"] = state["chunk_count"]
        volumes.append(entry)
    doc = {
        "kind": DOCUMENT_KIND,
        "format": FORMAT_VERSION,
        "job": copy.deepcopy(dict(job)),
        "chunking": copy.deepcopy(dict(chunking)),
        "server": inventory.server.to_dict(),
        "boot_from_volume": inventory.server.boot_from_volume,
        "root_restorable": sum(1 for v in inventory.volumes if v.boot_index == 0) == 1,
        "volumes": volumes,
        "ports": [p.to_dict() for p in inventory.ports],
    }
    consistency = getattr(inventory, "consistency", CONSISTENCY_PER_VOLUME)
    if consistency != CONSISTENCY_PER_VOLUME:
        doc["consistency"] = consistency
    validate_document(doc)
    return doc


def dump_document(doc):
    """Serialize a document to UTF-8 JSON bytes."""
    return json.dumps(doc, sort_keys=True).encode("utf-8")


def load_document(data):
    """Parse and validate a document given as bytes, bytearray or str."""
    try:
        if isinstance(data, (bytes, bytearray)):
            data = bytes(data).decode("utf-8")
        doc = json.loads(data)
    except (UnicodeDecodeError, ValueError) as exc:
        raise DocumentError("the document is not valid JSON: %s" % exc) from exc
    validate_document(doc)
    return doc


def root_mode(doc):
    """Return volume-root or no-root for a validated document."""
    if doc.get("root_restorable", doc.get("boot_from_volume", False)):
        return "volume-root"
    return "no-root"


def inventory_root_mode(inventory):
    """Return the root mode a backup inventory will be stored with."""
    if sum(1 for v in inventory.volumes if v.boot_index == 0) == 1:
        return "volume-root"
    return "no-root"


def select_chain_document(documents):
    """Return the document of the highest job id after checking they belong together."""
    if not documents:
        raise DocumentError(
            "no server metadata (server.json) was received "
            "(VirtualFull and consolidated jobs carry none)"
        )
    first = documents[0][1]
    for _jobid, doc in documents:
        validate_document(doc)
    checks = (
        ("server id", lambda d: d["server"]["id"]),
        ("project id", lambda d: d["server"]["project_id"]),
        ("format", lambda d: d["format"]),
        ("chunk size", lambda d: d["chunking"]["chunk_size"]),
        ("hash", lambda d: d["chunking"]["hash"]),
        ("root mode", root_mode),
    )
    for jobid, doc in documents[1:]:
        for label, getter in checks:
            if getter(doc) != getter(first):
                hint = "; run a Full backup" if label == "root mode" else ""
                raise DocumentError(
                    "job %s differs from the other jobs of the chain in its %s: %r "
                    "instead of %r%s" % (jobid, label, getter(doc), getter(first), hint)
                )
    return max(documents, key=lambda item: item[0])[1]


def volume_entry(doc, volume_id):
    """Return the volume entry with the given id."""
    for vol in doc.get("volumes", []):
        if vol.get("id") == volume_id:
            return vol
    raise DocumentError("volume %s is not part of the document" % volume_id)
