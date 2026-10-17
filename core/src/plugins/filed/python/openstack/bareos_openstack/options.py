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

"""Typed plugin options of the OpenStack plugin (standard library only)"""

import base64
import hashlib

SIZE_MULTIPLIERS = {
    "k": 1_000,
    "m": 1_000_000,
    "g": 1_000_000_000,
    "t": 1_000_000_000_000,
    "ki": 1_024,
    "mi": 1_048_576,
    "gi": 1_073_741_824,
    "ti": 1_099_511_627_776,
}

TIME_MULTIPLIERS = {"s": 1, "min": 60, "h": 3_600, "d": 86_400}

HASHES = sorted(a for a in hashlib.algorithms_guaranteed if not a.startswith("shake_"))

SECRET_KEYS = ("application_credential_secret", "password")

MIB = 1024 * 1024
GIB = 1024 * MIB


def _str(value):
    """String option converter"""
    return value.strip()


def _bool(value):
    """Boolean option converter"""
    s = value.strip().lower()
    if s in ("yes", "y", "on", "true", "1"):
        return True
    if s in ("no", "n", "off", "false", "0"):
        return False
    raise ValueError("a valid boolean (yes or no)")


def _enum(*values):
    """Enum option converter (case insensitive)"""
    allowed = [v.lower() for v in values]

    def convert(value):
        s = value.strip().lower()
        if s in allowed:
            return s
        raise ValueError("one of " + ", ".join(allowed))

    return convert


def _size(value):
    """Size option converter (bytes)"""
    s = value.strip().lower()
    if s.endswith("b"):
        s = s[:-1]
    suffix = s[-2:]
    if suffix in ("ki", "mi", "gi", "ti"):
        s = s[:-2]
    else:
        suffix = s[-1:]
        if suffix in ("k", "m", "g", "t"):
            s = s[:-1]
        else:
            suffix = None
    try:
        number = int(s.strip())
    except ValueError as e:
        raise ValueError("a valid size in bytes, for example 64MiB") from e
    if number <= 0:
        raise ValueError("a positive size in bytes, for example 64MiB")
    return number * SIZE_MULTIPLIERS.get(suffix, 1)


def _int(value):
    """Integer option converter"""
    try:
        return int(value.strip())
    except ValueError as e:
        raise ValueError("a valid integer") from e


def _time(value):
    """Time option converter (seconds)"""
    s = value.strip().lower()
    if s.endswith("min"):
        suffix = "min"
        s = s[:-3]
    elif s[-1:] in ("s", "h", "d"):
        suffix = s[-1:]
        s = s[:-1]
    else:
        suffix = None
    try:
        number = int(s.strip())
    except ValueError as e:
        raise ValueError("a valid time, for example 15min") from e
    if number <= 0:
        raise ValueError("a positive time, for example 15min")
    return number * TIME_MULTIPLIERS.get(suffix, 1)


def _split_escaped(text):
    """Split on ':' that is not preceded by a backslash"""
    parts = []
    current = ""
    for part in text.split(":"):
        if current:
            current += ":" + part
        else:
            current = part
        if current.endswith("\\"):
            continue
        parts.append(current)
        current = ""
    if current:
        parts.append(current)
    return parts


def redact_plugin_string(text):
    """Return the plugin string with the values of secret options masked"""
    out = []
    for element in _split_escaped(text):
        key, sep, _ = element.partition("=")
        if sep and key.strip().partition("#")[0] in SECRET_KEYS:
            out.append(key + "=***")
        else:
            out.append(element)
    return ":".join(out)


class UnknownOptionError(Exception):
    """Raised for an option name the plugin does not know"""


class Options:
    """Plugin options with conversion, defaults and validation"""

    _TABLE = {
        "backend": (_enum("sdk", "fake"), "sdk"),
        "fake_root": (_str, ""),
        "fake_project_id": (_str, ""),
        "cloud": (_str, ""),
        "auth_url": (_str, ""),
        "application_credential_id": (_str, ""),
        "application_credential_secret": (_str, ""),
        "username": (_str, ""),
        "password": (_str, ""),
        "user_domain_name": (_str, ""),
        "project_name": (_str, ""),
        "project_id": (_str, ""),
        "project_domain_name": (_str, ""),
        "region_name": (_str, ""),
        "verify_ssl": (_bool, "yes"),
        "cacert": (_str, ""),
        "proxy_instance_id": (_str, ""),
        "server_id": (_str, ""),
        "server_name": (_str, ""),
        "chunk_size": (_size, "64MiB"),
        "hash": (_enum(*HASHES), "sha256"),
        "queue_depth": (_int, "4"),
        "poll_timeout": (_time, "15min"),
        "snapshot_force": (_bool, "yes"),
        "cleanup_leftovers": (_bool, "no"),
        "restore_target": (_enum("cloud", "local"), "cloud"),
        "restore_project_cloud": (_str, ""),
        "restore_project_credentials": (_str, ""),
        "restore_security_groups": (_str, ""),
        "snapshot_mode": (_enum("per-volume", "group"), "per-volume"),
        "restore_local_dir": (_str, ""),
        "restore_overwrite": (_bool, "no"),
        "restore_write_zeros": (_bool, "yes"),
        "restore_name": (_str, ""),
        "restore_flavor": (_str, ""),
        "restore_network": (_str, ""),
        "restore_az": (_str, ""),
        "restore_volume_type": (_str, ""),
        "restore_powerstate": (_enum("on", "off", "previous"), "previous"),
        "restore_keep_addresses": (_bool, "no"),
        "force": (_bool, "no"),
        "force_keep_old_volumes": (_bool, "no"),
    }

    _BACKEND_KEYS = (
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
    )

    def __init__(self):
        self._set_keys = set()
        self._values = {
            name: conv(default) for name, (conv, default) in self._TABLE.items()
        }

    def _check(self, key):
        if key not in self._TABLE:
            raise UnknownOptionError(key)

    def __contains__(self, key):
        self._check(key)
        return key in self._set_keys

    def is_set(self, key):
        """True when the option was given a value"""
        return key in self

    def __setitem__(self, key, text):
        self._check(key)
        try:
            self._values[key] = self._TABLE[key][0](text)
        except ValueError as e:
            if key in SECRET_KEYS:
                raise ValueError('Invalid value for option "%s"' % key) from None
            what = e.args[0] if e.args else "a valid value"
            raise ValueError(
                'Invalid value "%s" for option "%s". Expected %s.' % (text, key, what)
            ) from None
        self._set_keys.add(key)

    def __getitem__(self, key):
        self._check(key)
        return self._values[key]

    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        return self[name]

    @staticmethod
    def _config_errors(o, proxy_needed=True):
        """Errors of a backend configuration dictionary (names, no values)"""
        errors = []
        if o["backend"] == "fake":
            if not o["fake_root"]:
                errors.append("option fake_root is required with backend=fake")
            return errors
        if proxy_needed and not o["proxy_instance_id"]:
            errors.append("option proxy_instance_id is required with backend=sdk")
        if not o["cloud"]:
            has_app = o["application_credential_id"] and (
                o["application_credential_secret"]
            )
            has_user = o["username"] and o["password"]
            if not o["auth_url"] or not (has_app or has_user):
                errors.append(
                    "backend=sdk needs option cloud, or auth_url with "
                    "application_credential_id and application_credential_secret, "
                    "or auth_url with username and password"
                )
        return errors

    def _backend_errors(self):
        """Backend errors; the proxy is always needed with backend=sdk"""
        return self._config_errors(self._values)

    def validate_backup(self):
        """Return the list of option errors of a backup job (empty = valid)"""
        errors = []
        o = self._values
        if bool(o["server_id"]) == bool(o["server_name"]):
            errors.append("exactly one of server_id and server_name must be set")
        errors.extend(self._backend_errors())
        chunk_size = o["chunk_size"]
        if chunk_size % 65536 != 0 or not MIB <= chunk_size <= GIB:
            errors.append(
                "chunk_size must be a multiple of 64KiB between 1MiB and 1GiB"
            )
        if not 1 <= o["queue_depth"] <= 64:
            errors.append("queue_depth must be between 1 and 64")
        if o["poll_timeout"] < 10:
            errors.append("poll_timeout must be at least 10s")
        return errors

    def validate_restore(self, where):
        """Return the list of option errors of a restore job (empty = valid)"""
        errors = []
        o = self._values
        mode = o["restore_target"]
        cross = bool(o["restore_project_cloud"] or o["restore_project_credentials"])
        if o["restore_project_cloud"] and o["restore_project_credentials"]:
            errors.append(
                "restore_project_cloud and restore_project_credentials exclude "
                "each other"
            )
        if o["restore_security_groups"] and not cross:
            errors.append(
                "restore_security_groups needs restore_project_cloud or "
                "restore_project_credentials"
            )
        if mode == "local":
            target = o["restore_local_dir"] or where
            if target in ("", "/"):
                errors.append(
                    "restore_target=local needs restore_local_dir or a restore "
                    "Where other than / "
                )
            if cross:
                errors.append(
                    "restore_project_* cannot be used with restore_target=local"
                )
            return errors
        errors.extend(self._backend_errors())
        if cross:
            for key in ("force", "restore_keep_addresses"):
                if o[key]:
                    errors.append(
                        "%s cannot be used with restore_project_cloud or "
                        "restore_project_credentials" % key
                    )
        return errors

    def backend_config(self):
        """Configuration dictionary for cloud.create_backend()"""
        return {key: self._values[key] for key in self._BACKEND_KEYS}

    def target_backend_config(self, credentials_path=None):
        """Backend configuration of the target project; never proxy_instance_id

        Raises ValueError (no option values in the text) when it is unusable.
        """
        o = self._values
        config = {
            key: o[key] for key in ("backend", "fake_root", "cacert", "verify_ssl")
        }
        if o["restore_project_cloud"]:
            config["cloud"] = o["restore_project_cloud"]
        elif credentials_path:
            loaded = load_options_file(credentials_path)
            for key in self._BACKEND_KEYS + ("fake_project_id",):
                if key != "proxy_instance_id" and loaded.is_set(key):
                    config[key] = loaded[key]
        else:
            raise ValueError("no target project is configured")
        full = {key: "" for key in self._BACKEND_KEYS if key != "verify_ssl"}
        full["verify_ssl"] = True
        full.update(config)
        errors = self._config_errors(full, proxy_needed=False)
        if errors:
            raise ValueError("; ".join(errors))
        return full

    def restore_cloud_options(self):
        """Build the RestoreCloudOptions of a cloud restore"""
        # pylint: disable=import-outside-toplevel
        from bareos_openstack.cloud_restore import RestoreCloudOptions

        o = self._values
        return RestoreCloudOptions(
            name=o["restore_name"],
            flavor=o["restore_flavor"],
            network=o["restore_network"],
            availability_zone=o["restore_az"],
            volume_type=o["restore_volume_type"],
            powerstate=o["restore_powerstate"],
            force=o["force"],
            force_keep_old_volumes=o["force_keep_old_volumes"],
            keep_addresses=o["restore_keep_addresses"],
            target=o["restore_target"],
            security_groups=tuple(
                name.strip()
                for name in o["restore_security_groups"].split(",")
                if name.strip()
            ),
        )

    def redacted(self):
        """All options with their values; set secrets replaced by ***"""
        out = {}
        for key, value in self._values.items():
            if key in SECRET_KEYS and key in self._set_keys:
                value = "***"
            out[key] = value
        return out


def load_options_file(path):
    """Read a defaults_file into Options; values never appear in error messages"""
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    result = Options()
    seen = set()
    pos = 0
    while pos < len(lines):
        number = pos + 1
        raw_key, _, value = lines[pos].partition("=")
        pos += 1
        key = raw_key.strip()
        value = value.strip()
        if not key and not value:
            continue
        if key.startswith(("#", ";", "[")):
            continue
        if not key or not value:
            raise ValueError("line %d: no key or no value" % number)
        while value.endswith("\\") and pos < len(lines):
            value = value[:-1] + lines[pos].strip()
            pos += 1
        name, _, transform = key.partition("#")
        if name not in Options._TABLE:  # pylint: disable=protected-access
            raise UnknownOptionError(name)
        if name in seen:
            continue
        seen.add(name)
        if transform:
            if transform != "enc":
                raise ValueError(
                    'line %d: unknown transformation of "%s"' % (number, name)
                )
            try:
                value = base64.b85decode(value).decode("utf-8")
            except Exception:  # pylint: disable=broad-except
                raise ValueError(
                    'line %d: cannot decode the value of option "%s"' % (number, name)
                ) from None
        try:
            result[name] = value
        except ValueError:
            raise ValueError(
                'line %d: invalid value for option "%s"' % (number, name)
            ) from None
    return result
