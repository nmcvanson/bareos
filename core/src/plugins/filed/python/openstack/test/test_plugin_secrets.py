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

"""No credential may appear in any message, exception or document"""

import base64
import contextlib
import io
import os
import shutil
import tempfile
import unittest

import bareosfd_stub
import test_plugin_smoke as smoke

PLAIN = "PlainSecret1"
ENC = "EncSecret2"
FILE_PLAIN = "FileSecret3"
FILE_ENC = "FileEnc4"


def b85(text):
    return base64.b85encode(text.encode("utf-8")).decode("ascii")


@unittest.skipUnless(smoke.AVAILABLE, "the cloud package is not complete yet")
class PluginSecretsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = smoke.load_plugin_module()

    def setUp(self):
        # pylint: disable-next=import-outside-toplevel
        from bareos_openstack import fake_backend

        self.tmp = tempfile.mkdtemp(prefix="openstack-secrets-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.root = os.path.join(self.tmp, "fake")
        fake_backend.seed(self.root, smoke.SPEC)
        self.daemon = smoke.FileDaemon(self.module)
        self.texts = []

    def start(self, plugindef):
        """Create and parse a plugin with the config directory in the temp dir"""
        stub = bareosfd_stub
        stub.reset()
        stub.values[stub.bVarUsedConfig] = os.path.join(self.tmp, "*", "*.conf")
        plugin = self.module.BareosFdOpenStack(plugindef)
        return plugin, plugin.parse_plugin_definition(plugindef)

    def base_def(self, **extra):
        parts = {
            "backend": "fake",
            "fake_root": self.root,
            "server_id": smoke.SERVER_ID,
            "chunk_size": "1MiB",
        }
        parts.update(extra)
        return ":".join("%s=%s" % kv for kv in parts.items())

    def assert_no_secret(self, secrets, extra_text=()):
        stub = bareosfd_stub
        haystack = [m for _, m in stub.messages] + [m for _, m in stub.debug_messages]
        haystack += list(extra_text) + self.texts
        forms = []
        for secret in secrets:
            forms += [secret, b85(secret)]
        for text in haystack:
            for form in forms:
                self.assertNotIn(form, str(text))

    def run_backup(self, plugin):
        try:
            rc, files = self.daemon.backup(plugin)
        except Exception as e:  # pylint: disable=broad-except
            self.texts.append(repr(e))
            self.fail("backup raised %r" % e)
        return rc, files

    def test_inline_secrets(self):
        stub = bareosfd_stub
        plugindef = self.base_def(
            password=PLAIN, **{"application_credential_secret#enc": b85(ENC)}
        )
        plugin, rc = self.start(plugindef)
        self.assertEqual(rc, stub.bRC_OK)
        self.assertEqual(plugin.options.password, PLAIN)
        self.assertEqual(plugin.options.application_credential_secret, ENC)
        rc, files = self.run_backup(plugin)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        documents = [f["object"] for f in files if f["type"] == stub.FT_RESTORE_FIRST]
        self.assertEqual(len(documents), 1)
        self.assert_no_secret(
            [PLAIN, ENC], [d.decode("utf-8") for d in documents] + [str(plugin)]
        )

    def test_secrets_from_defaults_file(self):
        stub = bareosfd_stub
        with open(os.path.join(self.tmp, "openstack.ini"), "w") as f:
            f.write("password=%s\n" % FILE_PLAIN)
            f.write("application_credential_secret#enc=%s\n" % b85(FILE_ENC))
        plugin, rc = self.start(self.base_def(defaults_file="openstack.ini"))
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        self.assertEqual(plugin.options.password, FILE_PLAIN)
        self.assertEqual(plugin.options.application_credential_secret, FILE_ENC)
        rc, files = self.run_backup(plugin)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        documents = [f["object"] for f in files if f["type"] == stub.FT_RESTORE_FIRST]
        self.assert_no_secret(
            [FILE_PLAIN, FILE_ENC], [d.decode("utf-8") for d in documents]
        )

    def test_sdk_backend_failure_does_not_leak(self):
        stub = bareosfd_stub
        plugindef = ":".join(
            [
                "backend=sdk",
                "cloud=none",
                "proxy_instance_id=" + smoke.PROXY_ID,
                "server_id=" + smoke.SERVER_ID,
                "password=" + PLAIN,
                "application_credential_secret#enc=" + b85(ENC),
            ]
        )
        plugin, rc = self.start(plugindef)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        rc, _ = self.run_backup(plugin)
        self.assertEqual(rc, stub.bRC_Error)
        self.assert_no_secret([PLAIN, ENC])

    def test_bad_value_and_unknown_option_do_not_leak(self):
        stub = bareosfd_stub
        plugin, rc = self.start(self.base_def(password=PLAIN, chunk_size="not-a-size"))
        self.assertEqual(rc, stub.bRC_Error)
        self.assert_no_secret([PLAIN])
        plugin, rc = self.start(self.base_def(password=PLAIN, bogus="1"))
        self.assertEqual(rc, stub.bRC_Error)
        self.assert_no_secret([PLAIN])
        plugin, rc = self.start(
            self.base_def(**{"password#enc": "not valid base85 \x00"})
        )
        self.assertEqual(rc, stub.bRC_Error)
        self.assertIsNotNone(plugin)

    def test_undecodable_secret_value_is_not_echoed(self):
        stub = bareosfd_stub
        bad = '"""bad"""'
        _, rc = self.start(self.base_def(**{"password#enc": bad}))
        self.assertEqual(rc, stub.bRC_Error)
        self.assert_no_secret([bad])

    # Credentials of the target project (restore_project_credentials)

    def target_file(self, text, name="target.ini", mode=0o600):
        path = os.path.join(self.tmp, name)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        os.chmod(path, mode)
        return name

    def restore_with(self, **extra):
        """Cloud restore of a fresh backup; returns the restore result code"""
        stub = bareosfd_stub
        plugin, rc = self.start(self.base_def())
        self.assertEqual(rc, stub.bRC_OK)
        rc, files = self.run_backup(plugin)
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        rplugin, rc = self.start(self.base_def(**extra))
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        try:
            return self.daemon.restore(rplugin, files)
        except Exception as e:  # pylint: disable=broad-except
            self.texts.append(repr(e))
            self.fail("restore raised %r" % e)
        return None

    def test_target_credentials_plain_and_enc_do_not_leak(self):
        stub = bareosfd_stub
        name = self.target_file(
            "backend=sdk\n"
            "auth_url=https://127.0.0.1:1/v3\n"
            "application_credential_id=tid\n"
            "application_credential_secret=%s\n"
            "password#enc=%s\n" % (FILE_PLAIN, b85(FILE_ENC))
        )
        rc = self.restore_with(restore_project_credentials=name, password=PLAIN)
        self.assertEqual(rc, stub.bRC_Error)
        self.assert_no_secret([PLAIN, FILE_PLAIN, FILE_ENC])

    def test_inline_form_carries_only_entry_name_or_path(self):
        stub = bareosfd_stub
        for key, value in (
            ("restore_project_credentials", "target.ini"),
            ("restore_project_cloud", "other-project"),
        ):
            plugin, rc = self.start(self.base_def(password=PLAIN, **{key: value}))
            self.assertEqual(rc, stub.bRC_OK, stub.messages)
            shown = plugin.options.redacted()
            self.assertEqual(shown[key], value)
            self.assertEqual(shown["password"], "***")
            self.assert_no_secret([PLAIN], [str(shown)])

    def test_unreadable_target_file_is_reported_without_content(self):
        stub = bareosfd_stub
        os.mkdir(os.path.join(self.tmp, "dir.ini"))
        for name in ("dir.ini", "missing.ini"):
            rc = self.restore_with(restore_project_credentials=name)
            self.assertEqual(rc, stub.bRC_Error)
            text = " ".join(m for _, m in stub.messages)
            self.assertIn(name, text)
            self.assert_no_secret([PLAIN])

    def test_bad_enc_value_in_target_file_is_not_echoed(self):
        stub = bareosfd_stub
        bad = '"""bad"""'
        name = self.target_file(
            "backend=sdk\n"
            "application_credential_secret=%s\n"
            "password#enc=%s\n" % (FILE_PLAIN, bad)
        )
        rc = self.restore_with(restore_project_credentials=name)
        self.assertEqual(rc, stub.bRC_Error)
        self.assert_no_secret([bad, FILE_PLAIN])

    def test_unknown_option_in_target_file_is_not_echoed(self):
        stub = bareosfd_stub
        name = self.target_file(
            "backend=sdk\nbogus=%s\napplication_credential_secret=%s\n"
            % (FILE_ENC, FILE_PLAIN)
        )
        rc = self.restore_with(restore_project_credentials=name)
        self.assertEqual(rc, stub.bRC_Error)
        self.assert_no_secret([FILE_ENC, FILE_PLAIN])

    def test_wildcard_project_in_target_file_fails_the_restore_clearly(self):
        stub = bareosfd_stub
        name = self.target_file(
            "backend=fake"
            + "\n"
            + "fake_project_id=*"
            + "\n"
            + "password=%s" % FILE_PLAIN
            + "\n"
        )
        rc = self.restore_with(restore_project_credentials=name)
        self.assertEqual(rc, stub.bRC_Error)
        text = " ".join(m for _, m in stub.messages)
        self.assertIn("fake_project_id must name a project", text)
        self.assert_no_secret([FILE_PLAIN])

    def test_target_file_open_to_others_gets_a_warning_without_content(self):
        stub = bareosfd_stub
        name = self.target_file(
            "backend=sdk\napplication_credential_secret=%s\n" % FILE_PLAIN,
            mode=0o644,
        )
        if not os.stat(os.path.join(self.tmp, name)).st_mode & 0o044:
            self.skipTest("file modes are not supported here")
        self.restore_with(restore_project_credentials=name)
        warnings = [m for lvl, m in stub.messages if lvl == stub.M_WARNING]
        self.assertTrue([w for w in warnings if "group or others" in w], warnings)
        self.assertTrue([w for w in warnings if name in w], warnings)
        self.assert_no_secret([FILE_PLAIN])

    def test_target_file_with_only_cloud_and_project_name_works(self):
        stub = bareosfd_stub
        name = self.target_file("cloud=bareos-lab\nproject_name=bareos-lab-target\n")
        plugin, rc = self.start(self.base_def(password=PLAIN))
        self.assertEqual(rc, stub.bRC_OK, stub.messages)
        plugin.options["restore_project_credentials"] = name
        plugin.configdir = self.tmp
        config = plugin._target_backend_config()  # pylint: disable=protected-access
        self.assertEqual(config["cloud"], "bareos-lab")
        self.assertEqual(config["project_name"], "bareos-lab-target")
        self.assertEqual(config["password"], "")
        self.assertEqual(config["proxy_instance_id"], "")
        self.assert_no_secret([PLAIN], [str(config)])

    def test_removed_option_message_carries_no_secret(self):
        stub = bareosfd_stub
        _, rc = self.start(self.base_def(password=PLAIN, transfer_mode="glance"))
        self.assertEqual(rc, stub.bRC_Error)
        self.assertTrue([m for _, m in stub.messages if "transfer_mode" in m])
        self.assert_no_secret([PLAIN])

    def test_tool_output_carries_no_secret(self):
        # pylint: disable-next=import-outside-toplevel
        from bareos_openstack import tool

        conf = os.path.join(self.tmp, "tool.conf")
        with open(conf, "w", encoding="utf-8") as f:
            f.write(
                "backend=fake\nfake_root=%s\npassword=%s\n"
                "application_credential_secret#enc=%s\n"
                % (self.root, PLAIN, b85(ENC))
            )
        out = []
        for argv in (
            ["leftovers"],
            ["leftovers", "--cleanup", "--older-than-job", "9", "--yes"],
            ["leftovers", "--transfer-dir", "relative"],
        ):
            buf, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
                tool.main(["--config", conf] + argv)
            out += [buf.getvalue(), err.getvalue()]
        self.assert_no_secret([PLAIN, ENC], out)


if __name__ == "__main__":
    unittest.main()
