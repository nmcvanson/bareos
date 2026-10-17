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

"""Unit tests of bareos_openstack.options"""

import base64
import os
import shutil
import tempfile
import unittest

from bareos_openstack import options as opt
from bareos_openstack.options import Options, UnknownOptionError, load_options_file

HERE = os.path.dirname(os.path.abspath(__file__))
CLOUD_RESTORE = os.path.join(
    os.path.dirname(HERE), "bareos_openstack", "cloud_restore.py"
)

BACKEND_KEYS = {
    "backend",
    "fake_root",
    "cloud",
    "auth_url",
    "application_credential_id",
    "application_credential_secret",
    "username",
    "password",
    "user_domain_name",
    "project_name",
    "project_id",
    "project_domain_name",
    "region_name",
    "cacert",
    "proxy_instance_id",
    "verify_ssl",
}


def valid_fake_backup_options():
    o = Options()
    o["backend"] = "fake"
    o["fake_root"] = "/tmp/fake"
    o["server_id"] = "abc"
    return o


class ConverterTests(unittest.TestCase):
    def test_str_strips(self):
        self.assertEqual(opt._str("  x "), "x")

    def test_bool(self):
        for text in ("yes", "Y", "on", "TRUE", "1"):
            self.assertIs(opt._bool(text), True)
        for text in ("no", "n", "off", "false", "0"):
            self.assertIs(opt._bool(text), False)
        with self.assertRaises(ValueError):
            opt._bool("maybe")

    def test_enum_lowercases(self):
        conv = opt._enum("Cloud", "local")
        self.assertEqual(conv(" LOCAL "), "local")
        self.assertEqual(conv("cloud"), "cloud")
        with self.assertRaises(ValueError):
            conv("other")

    def test_size(self):
        self.assertEqual(opt._size("64MiB"), 64 * 1024 * 1024)
        self.assertEqual(opt._size("1k"), 1000)
        self.assertEqual(opt._size("1kb"), 1000)
        self.assertEqual(opt._size("2gi"), 2 * 1024**3)
        self.assertEqual(opt._size("3t"), 3 * 10**12)
        self.assertEqual(opt._size("4096"), 4096)
        self.assertEqual(opt._size("1Ti"), 1024**4)
        for bad in ("0", "-1", "abc", "", "1x", "mib"):
            with self.assertRaises(ValueError):
                opt._size(bad)

    def test_int(self):
        self.assertEqual(opt._int(" 7 "), 7)
        with self.assertRaises(ValueError):
            opt._int("seven")

    def test_time(self):
        self.assertEqual(opt._time("30"), 30)
        self.assertEqual(opt._time("30s"), 30)
        self.assertEqual(opt._time("15min"), 900)
        self.assertEqual(opt._time("2h"), 7200)
        self.assertEqual(opt._time("1d"), 86400)
        for bad in ("0", "-5s", "x", "min", ""):
            with self.assertRaises(ValueError):
                opt._time(bad)

    def test_converter_error_describes_expected_value(self):
        with self.assertRaises(ValueError) as ctx:
            opt._size("zzz")
        self.assertIn("size", ctx.exception.args[0])


class OptionsTests(unittest.TestCase):
    def test_defaults(self):
        o = Options()
        self.assertEqual(o.backend, "sdk")
        self.assertEqual(o.chunk_size, 64 * 1024 * 1024)
        self.assertEqual(o.hash, "sha256")
        self.assertEqual(o.queue_depth, 4)
        self.assertEqual(o.poll_timeout, 900)
        self.assertIs(o.verify_ssl, True)
        self.assertIs(o.force, False)
        self.assertEqual(o.restore_powerstate, "previous")
        self.assertEqual(o["restore_target"], "cloud")

    def test_contains_means_set(self):
        o = Options()
        self.assertNotIn("hash", o)
        self.assertFalse(o.is_set("hash"))
        o["hash"] = "SHA512"
        self.assertIn("hash", o)
        self.assertTrue(o.is_set("hash"))
        self.assertEqual(o.hash, "sha512")

    def test_unknown_key(self):
        o = Options()
        with self.assertRaises(UnknownOptionError):
            o["nope"] = "x"
        with self.assertRaises(UnknownOptionError):
            o["nope"]  # pylint: disable=pointless-statement
        with self.assertRaises(UnknownOptionError):
            "nope" in o  # pylint: disable=pointless-statement
        with self.assertRaises(UnknownOptionError):
            o.nope  # pylint: disable=pointless-statement

    def test_private_attribute_is_attribute_error(self):
        with self.assertRaises(AttributeError):
            Options()._missing  # pylint: disable=pointless-statement,protected-access

    def test_first_value_wins_by_caller_convention(self):
        o = Options()
        for key, value in (("chunk_size", "2MiB"), ("chunk_size", "4MiB")):
            if key not in o:
                o[key] = value
        self.assertEqual(o.chunk_size, 2 * 1024 * 1024)

    def test_bad_value_message(self):
        o = Options()
        with self.assertRaises(ValueError) as ctx:
            o["chunk_size"] = "lots"
        text = str(ctx.exception)
        self.assertIn('Invalid value "lots" for option "chunk_size"', text)
        self.assertIn("Expected", text)
        self.assertNotIn("chunk_size", o._set_keys)  # pylint: disable=protected-access

    def test_shake_rejected_for_hash(self):
        o = Options()
        with self.assertRaises(ValueError):
            o["hash"] = "shake_128"
        self.assertFalse([h for h in opt.HASHES if h.startswith("shake_")])

    def test_secret_never_in_error_or_redacted(self):
        o = Options()
        # secret keys accept any string; a failing converter must not echo it
        saved = o._TABLE["password"]  # pylint: disable=protected-access
        try:

            def failing(_value):
                raise ValueError("expected nothing")

            table = Options._TABLE  # pylint: disable=protected-access
            table["password"] = (failing, "")
            with self.assertRaises(ValueError) as ctx:
                o["password"] = "PlainSecret1"
            self.assertNotIn("PlainSecret1", str(ctx.exception))
            self.assertEqual(str(ctx.exception), 'Invalid value for option "password"')
        finally:
            Options._TABLE["password"] = saved  # pylint: disable=protected-access
        o["password"] = "PlainSecret1"
        o["application_credential_secret"] = "AppSecret2"
        red = o.redacted()
        self.assertEqual(red["password"], "***")
        self.assertEqual(red["application_credential_secret"], "***")
        self.assertNotIn("PlainSecret1", str(red))
        self.assertNotIn("AppSecret2", str(red))

    def test_unset_secret_is_empty_in_redacted(self):
        self.assertEqual(Options().redacted()["password"], "")


class RedactTests(unittest.TestCase):
    def test_plain_key(self):
        text = "a=1:password=Secret:b=2"
        self.assertEqual(opt.redact_plugin_string(text), "a=1:password=***:b=2")

    def test_enc_key(self):
        text = "application_credential_secret#enc=ABCDEF:x=y"
        self.assertEqual(
            opt.redact_plugin_string(text),
            "application_credential_secret#enc=***:x=y",
        )

    def test_escaped_colon_in_value(self):
        text = "password=ab\\:cd:other=1"
        out = opt.redact_plugin_string(text)
        self.assertEqual(out, "password=***:other=1")
        self.assertNotIn("cd", out)

    def test_non_secret_untouched(self):
        text = "server_id=abc:chunk_size=1MiB:flag"
        self.assertEqual(opt.redact_plugin_string(text), text)

    def test_element_without_equals(self):
        self.assertEqual(opt.redact_plugin_string("password"), "password")

    def test_value_with_equals(self):
        self.assertEqual(opt.redact_plugin_string("password=a=b=c"), "password=***")


class ValidateBackupTests(unittest.TestCase):
    def test_valid_fake(self):
        self.assertEqual(valid_fake_backup_options().validate_backup(), [])

    def test_exactly_one_server(self):
        o = valid_fake_backup_options()
        o["server_name"] = "web"
        self.assertTrue(o.validate_backup())
        o2 = Options()
        o2["backend"] = "fake"
        o2["fake_root"] = "/x"
        self.assertTrue(o2.validate_backup())

    def test_fake_needs_root(self):
        o = Options()
        o["backend"] = "fake"
        o["server_id"] = "abc"
        self.assertTrue(any("fake_root" in e for e in o.validate_backup()))

    def test_sdk_needs_proxy_and_credentials(self):
        o = Options()
        o["server_id"] = "abc"
        errors = o.validate_backup()
        self.assertTrue(any("proxy_instance_id" in e for e in errors))
        self.assertTrue(any("cloud" in e for e in errors))
        o["proxy_instance_id"] = "p"
        o["cloud"] = "prod"
        self.assertEqual(o.validate_backup(), [])

    def test_sdk_auth_variants(self):
        o = Options()
        o["server_id"] = "abc"
        o["proxy_instance_id"] = "p"
        o["auth_url"] = "http://keystone"
        self.assertTrue(o.validate_backup())
        o["application_credential_id"] = "id"
        self.assertTrue(o.validate_backup())
        o["application_credential_secret"] = "s"
        self.assertEqual(o.validate_backup(), [])
        o2 = Options()
        o2["server_id"] = "abc"
        o2["proxy_instance_id"] = "p"
        o2["auth_url"] = "http://keystone"
        o2["username"] = "u"
        self.assertTrue(o2.validate_backup())
        o2["password"] = "p"
        self.assertEqual(o2.validate_backup(), [])

    def test_chunk_size_rules(self):
        o = valid_fake_backup_options()
        o["chunk_size"] = "1MiB"
        self.assertEqual(o.validate_backup(), [])
        o["chunk_size"] = "1GiB"
        self.assertEqual(o.validate_backup(), [])
        o["chunk_size"] = "512KiB"
        self.assertTrue(o.validate_backup())
        o["chunk_size"] = "2GiB"
        self.assertTrue(o.validate_backup())
        o["chunk_size"] = "1500000"
        self.assertTrue(o.validate_backup())

    def test_queue_depth_rules(self):
        o = valid_fake_backup_options()
        for bad in ("0", "65"):
            o["queue_depth"] = bad
            self.assertTrue(o.validate_backup())
        o["queue_depth"] = "64"
        self.assertEqual(o.validate_backup(), [])

    def test_poll_timeout_rule(self):
        o = valid_fake_backup_options()
        o["poll_timeout"] = "5s"
        self.assertTrue(o.validate_backup())
        o["poll_timeout"] = "10s"
        self.assertEqual(o.validate_backup(), [])


class ValidateRestoreTests(unittest.TestCase):
    def test_local_needs_directory(self):
        o = Options()
        o["restore_target"] = "local"
        self.assertTrue(o.validate_restore(""))
        self.assertTrue(o.validate_restore("/"))
        self.assertEqual(o.validate_restore("/restore"), [])
        o["restore_local_dir"] = "/data"
        self.assertEqual(o.validate_restore("/"), [])

    def test_local_dir_slash_refused(self):
        o = Options()
        o["restore_target"] = "local"
        o["restore_local_dir"] = "/"
        self.assertTrue(o.validate_restore("/restore"))
        self.assertTrue(o.validate_restore(""))

    def test_cloud_uses_backend_rules_not_server_rule(self):
        o = Options()
        self.assertTrue(o.validate_restore(""))
        o["backend"] = "fake"
        o["fake_root"] = "/x"
        self.assertEqual(o.validate_restore(""), [])


class ValidateRestoreBatchDTests(unittest.TestCase):
    @staticmethod
    def fake(**extra):
        o = Options()
        o["backend"] = "fake"
        o["fake_root"] = "/x"
        o["proxy_instance_id"] = "proxy"
        for key, value in extra.items():
            o[key] = value
        return o

    def refused(self, **extra):
        errors = self.fake(**extra).validate_restore("/restore")
        self.assertTrue(errors, extra)
        return " ".join(errors)

    def test_defaults_unchanged(self):
        o = self.fake()
        self.assertEqual(o["restore_target"], "cloud")
        self.assertEqual(o["snapshot_mode"], "per-volume")
        self.assertEqual(o.validate_restore("/restore"), [])

    def test_image_target_is_refused(self):
        with self.assertRaises(ValueError) as ctx:
            Options()["restore_target"] = "image"
        self.assertIn("restore_target", str(ctx.exception))

    def test_both_project_options_refused(self):
        text = self.refused(
            restore_project_cloud="other", restore_project_credentials="t.ini"
        )
        self.assertIn("exclude each other", text)

    def test_local_with_project_option_refused(self):
        for key, value in (
            ("restore_project_cloud", "other"),
            ("restore_project_credentials", "t.ini"),
        ):
            o = self.fake(restore_target="local", **{key: value})
            self.assertTrue(o.validate_restore("/restore"))

    def test_force_and_keep_addresses_refused_across_projects(self):
        for extra in ({"force": "yes"}, {"restore_keep_addresses": "yes"}):
            self.refused(restore_project_cloud="other", **extra)
            self.refused(restore_project_credentials="t.ini", **extra)

    def test_security_groups_need_a_project_option(self):
        text = self.refused(restore_security_groups="default,web")
        self.assertIn("restore_security_groups", text)
        o = self.fake(restore_project_cloud="other")
        o["restore_security_groups"] = "default, web"
        self.assertEqual(o.validate_restore("/restore"), [])

    def test_cross_project_alone_is_valid(self):
        self.assertEqual(
            self.fake(restore_project_cloud="other").validate_restore("/r"), []
        )
        self.assertEqual(
            self.fake(restore_project_credentials="t.ini").validate_restore("/r"),
            [],
        )

    def test_snapshot_mode_values(self):
        self.assertEqual(self.fake(snapshot_mode="GROUP")["snapshot_mode"], "group")
        with self.assertRaises(ValueError):
            self.fake(snapshot_mode="pool")


class TargetBackendConfigTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="openstack-target-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def write(self, text):
        path = os.path.join(self.tmp, "target.ini")
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        return path

    @staticmethod
    def source():
        o = Options()
        o["backend"] = "sdk"
        o["proxy_instance_id"] = "proxy-1"
        o["cloud"] = "source-cloud"
        o["password"] = "SourceSecret"
        o["verify_ssl"] = "no"
        return o

    def test_clouds_entry(self):
        o = self.source()
        o["restore_project_cloud"] = "target-cloud"
        cfg = o.target_backend_config()
        self.assertEqual(cfg["cloud"], "target-cloud")
        self.assertEqual(cfg["proxy_instance_id"], "")
        self.assertEqual(cfg["password"], "")
        self.assertIs(cfg["verify_ssl"], False)
        self.assertEqual(set(cfg), BACKEND_KEYS)

    def test_credentials_file_plain_and_enc(self):
        path = self.write(
            "auth_url=https://keystone.example/v3\n"
            "application_credential_id=abc\n"
            "application_credential_secret#enc=%s\n"
            "proxy_instance_id=must-be-ignored\n"
            % base64.b85encode(b"TargetSecret").decode("ascii")
        )
        o = self.source()
        o["restore_project_credentials"] = "target.ini"
        cfg = o.target_backend_config(path)
        self.assertEqual(cfg["application_credential_secret"], "TargetSecret")
        self.assertEqual(cfg["auth_url"], "https://keystone.example/v3")
        self.assertEqual(cfg["cloud"], "")
        self.assertEqual(cfg["password"], "")
        self.assertEqual(cfg["proxy_instance_id"], "")

    def test_fake_project_id_from_file(self):
        path = self.write("backend=fake\nfake_project_id=proj-b\n")
        o = Options()
        o["backend"] = "fake"
        o["fake_root"] = "/r"
        cfg = o.target_backend_config(path)
        self.assertEqual(cfg["fake_project_id"], "proj-b")
        self.assertEqual(cfg["fake_root"], "/r")

    def test_incomplete_file_is_refused_without_values(self):
        path = self.write("username=alice\npassword=TargetPw9\n")
        o = self.source()
        with self.assertRaises(ValueError) as ctx:
            o.target_backend_config(path)
        self.assertNotIn("TargetPw9", str(ctx.exception))
        self.assertNotIn("alice", str(ctx.exception))

    def test_file_errors_never_carry_values(self):
        o = self.source()
        for text in ("password\n", "password#enc=%%%\n", "verify_ssl=maybe\n"):
            with self.assertRaises(ValueError) as ctx:
                o.target_backend_config(self.write(text))
            self.assertNotIn("maybe", str(ctx.exception))
        with self.assertRaises(OSError):
            o.target_backend_config(os.path.join(self.tmp, "missing.ini"))

    def test_no_target_is_refused(self):
        with self.assertRaises(ValueError):
            self.source().target_backend_config()


class BackendConfigTests(unittest.TestCase):
    def test_keys_and_values(self):
        o = Options()
        o["backend"] = "fake"
        o["fake_root"] = "/r"
        o["verify_ssl"] = "no"
        cfg = o.backend_config()
        self.assertEqual(set(cfg), BACKEND_KEYS)
        self.assertEqual(cfg["backend"], "fake")
        self.assertEqual(cfg["fake_root"], "/r")
        self.assertIs(cfg["verify_ssl"], False)
        self.assertEqual(cfg["cloud"], "")

    @unittest.skipUnless(
        os.path.exists(CLOUD_RESTORE), "cloud_restore.py is not available yet"
    )
    def test_restore_cloud_options(self):
        o = Options()
        o["restore_name"] = "n"
        o["restore_flavor"] = "f"
        o["restore_network"] = "net"
        o["restore_az"] = "az"
        o["restore_volume_type"] = "ssd"
        o["restore_powerstate"] = "off"
        o["force"] = "yes"
        o["force_keep_old_volumes"] = "yes"
        o["restore_keep_addresses"] = "yes"
        r = o.restore_cloud_options()
        self.assertEqual(
            (r.name, r.flavor, r.network, r.availability_zone, r.volume_type),
            ("n", "f", "net", "az", "ssd"),
        )
        self.assertEqual(r.powerstate, "off")
        self.assertTrue(r.force and r.force_keep_old_volumes and r.keep_addresses)
        self.assertEqual(r.target, "cloud")
        self.assertEqual(r.security_groups, ())

    @unittest.skipUnless(
        os.path.exists(CLOUD_RESTORE), "cloud_restore.py is not available yet"
    )
    def test_restore_cloud_options_batch_d_fields(self):
        o = Options()
        o["restore_target"] = "local"
        o["restore_security_groups"] = " default , web,, "
        r = o.restore_cloud_options()
        self.assertEqual(r.target, "local")
        self.assertEqual(r.security_groups, ("default", "web"))


class ProxyOptionTests(unittest.TestCase):
    @staticmethod
    def sdk_backup():
        o = Options()
        o["server_id"] = "abc"
        o["cloud"] = "prod"
        return o

    def test_removed_options_are_unknown(self):
        o = Options()
        for key in (
            "transfer_mode",
            "transfer_format",
            "transfer_dir",
            "qemu_img",
            "skip_ephemeral",
            "ignore_ephemeral_disk",
            "restore_image_keep_volumes",
        ):
            with self.assertRaises(UnknownOptionError) as ctx:
                o[key] = "yes"
            self.assertIn(key, str(ctx.exception))
        with self.assertRaises(UnknownOptionError):
            o.transfer_config  # pylint: disable=pointless-statement

    def test_removed_option_in_an_options_file(self):
        tmp = tempfile.mkdtemp(prefix="openstack-removed-")
        self.addCleanup(shutil.rmtree, tmp, True)
        path = os.path.join(tmp, "old.ini")
        with open(path, "w", encoding="utf-8") as f:
            f.write("transfer_mode=glance\n")
        with self.assertRaises(UnknownOptionError) as ctx:
            load_options_file(path)
        self.assertIn("transfer_mode", str(ctx.exception))

    def test_proxy_needed_for_backup_and_cloud_restore(self):
        o = self.sdk_backup()
        self.assertTrue(any("proxy_instance_id" in e for e in o.validate_backup()))
        self.assertTrue(any("proxy_instance_id" in e for e in o.validate_restore("")))
        o["proxy_instance_id"] = "proxy-1"
        self.assertEqual(o.validate_backup(), [])
        self.assertEqual(o.validate_restore(""), [])

    def test_local_restore_needs_no_proxy(self):
        o = Options()
        o["restore_target"] = "local"
        self.assertEqual(o.validate_restore("/restore"), [])

    def test_target_config_never_needs_proxy(self):
        o = Options()
        o["restore_project_cloud"] = "target"
        cfg = o.target_backend_config()
        self.assertEqual(cfg["cloud"], "target")
        self.assertEqual(cfg["proxy_instance_id"], "")

    def test_project_file_with_cloud_and_project_name(self):
        tmp = tempfile.mkdtemp(prefix="openstack-target-")
        self.addCleanup(shutil.rmtree, tmp, True)
        path = os.path.join(tmp, "target.ini")
        with open(path, "w", encoding="utf-8") as f:
            f.write("cloud=bareos-lab\nproject_name=bareos-lab-target\n")
        o = Options()
        o["restore_project_credentials"] = "target.ini"
        cfg = o.target_backend_config(path)
        self.assertEqual(cfg["cloud"], "bareos-lab")
        self.assertEqual(cfg["project_name"], "bareos-lab-target")
        self.assertEqual(cfg["proxy_instance_id"], "")


class LoadOptionsFileTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="openstack-optfile-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def write(self, text):
        path = os.path.join(self.tmp, "tool.ini")
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        return path

    def test_format(self):
        secret = base64.b85encode(b"S3cret").decode("ascii")
        text = "\n".join(
            [
                "# comment",
                "",
                "; other",
                "[section]",
                "cloud = prod",
                "proxy_instance_id=abc" + chr(92),
                "def",
                "password#enc=" + secret,
                "queue_depth=8",
                "cloud=second",
                "",
            ]
        )
        o = opt.load_options_file(self.write(text))
        self.assertEqual(o.cloud, "prod")
        self.assertEqual(o.proxy_instance_id, "abcdef")
        self.assertEqual(o.password, "S3cret")
        self.assertEqual(o.queue_depth, 8)
        self.assertIn("password", o)
        self.assertNotIn("server_id", o)

    def test_unknown_key(self):
        with self.assertRaises(UnknownOptionError) as ctx:
            opt.load_options_file(self.write("bogus=SECRETVALUE\n"))
        self.assertNotIn("SECRETVALUE", str(ctx.exception))

    def test_errors_never_contain_values(self):
        for text in (
            "password=\nqueue_depth=SECRETVALUE\n",
            "queue_depth=SECRETVALUE\n",
            "password#enc=SECRETVALUE" + chr(0) + "\n",
            "password#zip=SECRETVALUE\n",
            "SECRETVALUE\n",
        ):
            with self.assertRaises(ValueError) as ctx:
                opt.load_options_file(self.write(text))
            self.assertNotIn("SECRETVALUE", str(ctx.exception))

    def test_missing_file_is_oserror(self):
        with self.assertRaises(OSError):
            opt.load_options_file(os.path.join(self.tmp, "missing.ini"))


if __name__ == "__main__":
    unittest.main()
