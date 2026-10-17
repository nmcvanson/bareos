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

"""Unit tests of the server document."""

import copy
import json
import unittest

from bareos_openstack import metadata
from bareos_openstack.cloud import PortInfo, ServerInfo, VolumeInfo
from bareos_openstack.cloud_backup import BackupInventory
from bareos_openstack.metadata import DocumentError

MIB = 1024 * 1024
CHUNK = 4 * MIB


def inventory():
    server = ServerInfo(
        id="srv-1",
        name="web01",
        project_id="proj-1",
        status="ACTIVE",
        flavor={"original_name": "m1.small"},
        image_id="",
    )
    volumes = [
        VolumeInfo(id="vol-a", name="root", size_gb=1, boot_index=0, device="/dev/vda"),
        VolumeInfo(id="vol-b", name="data", size_gb=1, device="/dev/vdb"),
    ]
    ports = [PortInfo(id="port-1", network_id="net-1", name="p")]
    return BackupInventory(server, volumes, ports)


STATES = {
    "vol-a": {"size_bytes": 10 * MIB, "chunk_count": 3},
    "vol-b": {"size_bytes": 4 * MIB, "chunk_count": 1},
}
JOB = {"job_id": 12, "level": "F", "backup_time": 1760000000, "plugin_version": "0.1.0"}
CHUNKING = {
    "chunk_size": CHUNK,
    "hash": "sha256",
    "hijacked_stat_fields": ["st_mtime", "st_ctime", "st_atime", "st_ino"],
}


def document():
    return metadata.build_document(inventory(), STATES, JOB, CHUNKING)


class DocumentTest(unittest.TestCase):
    def test_round_trip(self):
        doc = document()
        data = metadata.dump_document(doc)
        self.assertIsInstance(data, bytes)
        self.assertEqual(metadata.load_document(data), doc)
        self.assertEqual(metadata.load_document(bytearray(data)), doc)
        self.assertEqual(metadata.load_document(data.decode("utf-8")), doc)
        self.assertEqual(doc["kind"], metadata.DOCUMENT_KIND)
        self.assertEqual(doc["volumes"][0]["chunk_count"], 3)
        self.assertEqual(doc["volumes"][0]["size_bytes"], 10 * MIB)
        self.assertTrue(doc["boot_from_volume"])
        self.assertEqual(metadata.volume_entry(doc, "vol-b")["name"], "data")

    def test_missing_volume_state(self):
        with self.assertRaises(DocumentError):
            partial = {"vol-a": STATES["vol-a"]}
            metadata.build_document(inventory(), partial, JOB, CHUNKING)

    def test_supported_hashes(self):
        self.assertIn("sha256", metadata.SUPPORTED_HASHES)
        shake = [h for h in metadata.SUPPORTED_HASHES if h.startswith("shake")]
        self.assertFalse(shake)

    def check_invalid(self, mutate):
        doc = document()
        mutate(doc)
        with self.assertRaises(DocumentError):
            metadata.validate_document(doc)
        with self.assertRaises(DocumentError):
            metadata.load_document(json.dumps(doc))

    def test_bad_components(self):
        for bad in ("../x", "a/b", "", "a\\b", ".", "..", "a\0b", "x" * 256):
            self.check_invalid(lambda d, b=bad: d["server"].update(id=b))
            self.check_invalid(lambda d, b=bad: d["server"].update(project_id=b))
            self.check_invalid(lambda d, b=bad: d["volumes"][0].update(id=b))

    def test_duplicate_volume_ids(self):
        self.check_invalid(lambda d: d["volumes"][1].update(id="vol-a"))

    def test_size_and_count(self):
        gib = 1024**3
        self.check_invalid(lambda d: d["volumes"][0].update(size_bytes=gib + 1))
        self.check_invalid(lambda d: d["volumes"][0].update(chunk_count=4))
        self.check_invalid(lambda d: d["volumes"][0].update(size_gb=0))
        self.check_invalid(lambda d: d["volumes"][0].update(size_bytes=-1))
        self.check_invalid(lambda d: d["volumes"][0].update(size_bytes="10"))

    def test_chunking(self):
        self.check_invalid(lambda d: d["chunking"].update(hash="shake_128"))
        self.check_invalid(lambda d: d["chunking"].update(hash="nope"))
        self.check_invalid(lambda d: d["chunking"].update(chunk_size=MIB // 2))
        self.check_invalid(lambda d: d["chunking"].update(chunk_size=2 * 1024**3))
        self.check_invalid(lambda d: d["chunking"].update(chunk_size=MIB + 1))

    def test_boot_rules(self):
        self.check_invalid(lambda d: d["volumes"][1].update(boot_index=0))
        self.check_invalid(lambda d: d["volumes"][0].update(boot_index=None))

    def test_header_rules(self):
        self.check_invalid(lambda d: d.update(kind="other"))
        self.check_invalid(lambda d: d.update(format=2))
        self.check_invalid(lambda d: d["job"].update(job_id=-1))
        self.check_invalid(lambda d: d.update(volumes=[]))
        self.check_invalid(lambda d: d.update(boot_from_volume="yes"))
        self.check_invalid(lambda d: d["ports"].append({"id": "p"}))

    def test_load_rejects_garbage(self):
        for data in (b"{", b"[]", b"\xff\xfe", "null"):
            with self.assertRaises(DocumentError):
                metadata.load_document(data)


class ChainTest(unittest.TestCase):
    def doc(self, jobid):
        doc = copy.deepcopy(document())
        doc["job"]["job_id"] = jobid
        return doc

    def test_picks_highest_job(self):
        docs = [(5, self.doc(5)), (9, self.doc(9)), (7, self.doc(7))]
        self.assertEqual(metadata.select_chain_document(docs)["job"]["job_id"], 9)

    def test_empty(self):
        with self.assertRaises(DocumentError) as ctx:
            metadata.select_chain_document([])
        self.assertEqual(
            str(ctx.exception),
            "no server metadata (server.json) was received "
            "(VirtualFull and consolidated jobs carry none)",
        )

    def test_mismatches(self):
        def other(mutate):
            doc = self.doc(9)
            mutate(doc)
            metadata.validate_document(doc)
            with self.assertRaises(DocumentError) as ctx:
                metadata.select_chain_document([(5, self.doc(5)), (9, doc)])
            self.assertIn("differs", str(ctx.exception))

        other(lambda d: d["server"].update(id="srv-2"))
        other(lambda d: d["server"].update(project_id="proj-2"))

        def bigger_chunks(d):
            d["chunking"]["chunk_size"] = 2 * CHUNK
            d["volumes"][0]["chunk_count"] = 2

        other(bigger_chunks)
        other(lambda d: d["chunking"].update(hash="sha512"))


class RootModeTest(unittest.TestCase):
    def test_volume_root_document_fields(self):
        doc = document()
        self.assertTrue(doc["root_restorable"])
        for key in ("boot_source", "root_image", "ephemeral_root_skipped"):
            self.assertNotIn(key, doc)
        self.assertEqual(metadata.root_mode(doc), "volume-root")
        self.assertEqual(metadata.inventory_root_mode(inventory()), "volume-root")

    def test_old_document_gets_defaults(self):
        doc = document()
        doc.pop("root_restorable")
        for vol in doc["volumes"]:
            vol.pop("source")
            vol.pop("image_metadata")
        metadata.validate_document(doc)
        self.assertTrue(doc["root_restorable"])
        self.assertEqual(
            [(v["source"], v["image_metadata"]) for v in doc["volumes"]],
            [("volume", {}), ("volume", {})],
        )
        self.assertEqual(metadata.root_mode(doc), "volume-root")

    def refused(self, mutate, text="made by an older plugin version; not supported"):
        doc = document()
        mutate(doc)
        with self.assertRaises(DocumentError) as ctx:
            metadata.validate_document(doc)
        self.assertIn(text, str(ctx.exception))

    def test_image_root_volume_is_refused(self):
        self.refused(lambda d: d["volumes"][0].update(source="image-root", id="root"))

    def test_image_root_document_fields_are_refused(self):
        self.refused(lambda d: d.update(root_image={"snapshot_image_id": "img-9"}))

    def test_image_booted_document_is_refused(self):
        def mutate(d):
            d["boot_from_volume"] = False
            d["boot_source"] = "image"
            d["root_restorable"] = False
            d["volumes"][0]["boot_index"] = None

        self.refused(mutate)
        self.refused(lambda d: d.update(boot_from_volume=False))
        self.refused(lambda d: d.update(boot_source="image"))

    def test_validation_failures(self):
        doc = document()
        doc["volumes"][1]["source"] = "weird"
        with self.assertRaises(DocumentError):
            metadata.validate_document(doc)
        for mutate in (
            lambda d: d.update(root_restorable=False),
            lambda d: d.update(root_restorable="yes"),
            lambda d: d["volumes"][1].update(image_metadata={"a": 1}),
            lambda d: d["volumes"][1].update(image_metadata=["x"]),
            lambda d: d["volumes"][1].update(image_metadata={1: "a"}),
        ):
            doc = document()
            mutate(doc)
            with self.assertRaises(DocumentError):
                metadata.validate_document(doc)


class ConsistencyFieldTest(unittest.TestCase):
    def test_group_inventory_records_the_consistency(self):
        inv = inventory()
        inv.consistency = "group-crash-consistent"
        doc = metadata.build_document(inv, STATES, JOB, CHUNKING)
        self.assertEqual(doc["consistency"], "group-crash-consistent")
        self.assertEqual(
            metadata.load_document(metadata.dump_document(doc))["consistency"],
            "group-crash-consistent",
        )

    def test_default_document_has_no_consistency_field(self):
        doc = document()
        self.assertNotIn("consistency", doc)
        metadata.validate_document(doc)

    def test_values_are_checked(self):
        doc = document()
        doc["consistency"] = "per-volume"
        metadata.validate_document(doc)
        doc["consistency"] = "application"
        with self.assertRaises(DocumentError):
            metadata.validate_document(doc)


if __name__ == "__main__":
    unittest.main()
