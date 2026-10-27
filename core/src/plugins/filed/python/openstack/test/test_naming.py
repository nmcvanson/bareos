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

"""Unit tests of bareos_openstack.naming"""

import unittest

from bareos_openstack import naming

P = "11111111-1111-4111-8111-111111111111"
S = "33333333-3333-4333-8333-333333333333"
V = "44444444-4444-4444-8444-444444444444"


class NamingTests(unittest.TestCase):
    def test_paths(self):
        self.assertEqual(naming.server_root(P, S), "@OPENSTACK/%s/%s" % (P, S))
        self.assertEqual(
            naming.document_path(P, S), "@OPENSTACK/%s/%s/server.json" % (P, S)
        )
        self.assertEqual(
            naming.chunk_path(P, S, V, 7),
            "@OPENSTACK/%s/%s/volumes/%s/00000007.chunk" % (P, S, V),
        )

    def test_round_trip(self):
        for index in (0, 1, 12345678, 123456789, 10**12):
            path = naming.chunk_path(P, S, V, index)
            self.assertEqual(naming.parse_chunk_path(path), (P, S, V, index))

    def test_index_padding_and_more_than_eight_digits(self):
        self.assertTrue(naming.chunk_path(P, S, V, 5).endswith("/00000005.chunk"))
        path = naming.chunk_path(P, S, V, 123456789)
        self.assertTrue(path.endswith("/123456789.chunk"))

    def test_invalid_components(self):
        bad = ["..", ".", "a/b", "a\\b", "", "a\0b"]
        for value in bad:
            with self.assertRaises(ValueError):
                naming.server_root(value, S)
            with self.assertRaises(ValueError):
                naming.server_root(P, value)
            with self.assertRaises(ValueError):
                naming.document_path(value, S)
            with self.assertRaises(ValueError):
                naming.chunk_path(P, S, value, 0)

    def test_invalid_index(self):
        for index in (-1, "1", 1.5, None):
            with self.assertRaises(ValueError):
                naming.chunk_path(P, S, V, index)

    def test_parse_rejects_other_names(self):
        for path in (
            "@OPENSTACK/%s/%s/server.json" % (P, S),
            "@OPENSTACK/%s/%s/volumes/%s/1.chunk" % (P, S, V),
            "@OPENSTACK/%s/%s/volumes/%s/00000001.chunk/" % (P, S, V),
            "OPENSTACK/%s/%s/volumes/%s/00000001.chunk" % (P, S, V),
            "@OPENSTACK/%s/%s/volumes/%s/00000001.chunk\n" % (P, S, V),
            "@OPENSTACK/%s/%s/volumes//00000001.chunk" % (P, S),
            "",
        ):
            self.assertIsNone(naming.parse_chunk_path(path), path)

    def test_parse_rejects_invalid_component(self):
        self.assertIsNone(
            naming.parse_chunk_path(
                "@OPENSTACK/../%s/volumes/%s/00000001.chunk" % (S, V)
            )
        )
        self.assertIsNone(
            naming.parse_chunk_path(
                "@OPENSTACK/%s/%s/volumes/./00000001.chunk" % (P, S)
            )
        )
        self.assertIsNone(
            naming.parse_chunk_path(
                "@OPENSTACK/a\\b/%s/volumes/%s/00000001.chunk" % (S, V)
            )
        )

    def test_is_document_path(self):
        self.assertTrue(naming.is_document_path(naming.document_path(P, S)))
        self.assertFalse(naming.is_document_path(naming.chunk_path(P, S, V, 0)))
        self.assertFalse(naming.is_document_path("@OPENSTACK/%s/server.json" % P))
        self.assertFalse(naming.is_document_path(naming.document_path(P, S) + "\n"))
        self.assertFalse(naming.is_document_path("other/%s/%s/server.json" % (P, S)))

    def test_document_never_parses_as_chunk(self):
        self.assertIsNone(naming.parse_chunk_path(naming.document_path(P, S)))


if __name__ == "__main__":
    unittest.main()
