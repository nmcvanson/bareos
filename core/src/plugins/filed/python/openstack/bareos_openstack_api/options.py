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

"""Plugin option parsing/validation.

Kept free of any `bareosfd` dependency so this whole module is
unit-testable with plain `unittest`, no live cloud and no bareosfd C
extension needed. `check_mandatory()` returns error strings rather than
calling `bareosfd.JobMessage()` directly for the same reason - the plugin
class turns each one into a JobMessage.
"""


class UnknownOptionException(Exception):
    """Raised when using an unknown option."""


class BareosFdOpenstackOptions:
    """Holds plugin options."""

    def __init__(self):
        self._options = {
            # Authentication: either "cloud" (clouds.yaml lookup) or an
            # explicit "auth_url" plus one of the two credential styles
            # below.
            "cloud": self.make_opt(str),
            "auth_url": self.make_opt(str),
            "application_credential_id": self.make_opt(str),
            "application_credential_secret": self.make_opt(str),
            "username": self.make_opt(str),
            "password": self.make_opt(str),
            "user_domain_name": self.make_opt(str, "Default"),
            "project_domain_name": self.make_opt(str, "Default"),
            "project_name": self.make_opt(str),
            "project_id": self.make_opt(str),
            "region_name": self.make_opt(str),
            "verify_ssl": self.make_opt(bool, True),
            # Job target / proxy
            "proxy_instance_id": self.make_opt(str),
            "instance": self.make_opt(str),
            "include_volumes": self.make_opt(bool, True),
            "include_images": self.make_opt(bool, False),
            "keep_snapshot": self.make_opt(bool, False),
            "poll_timeout": self.make_opt(int, 900),
            # Restore
            "restoretoimage": self.make_opt(bool, False),
            "force": self.make_opt(bool, False),
            "force_keep_old_volumes": self.make_opt(bool, False),
            "instance_name": self.make_opt(str),
            "target_project": self.make_opt(str),
            # Best-effort quiesce hooks
            "pre_snapshot_cmd": self.make_opt(str),
            "post_snapshot_cmd": self.make_opt(str),
        }

    @staticmethod
    def make_opt(type_, default=None):
        """Create a new option."""
        return {"type": type_, "value": default, "value_set": False}

    def check(self):
        """
        Per-option validation hook (types were already checked on
        assignment in __setitem__). Nothing extra to check per-option
        today; see check_mandatory() for the cross-option rules.
        """
        return True

    def check_mandatory(self):
        """
        Validate the one-of/conditional mandatory rules that a flat
        mandatory-options list can't express. Returns a list of error
        messages (empty if valid).
        """
        errors = []

        if "cloud" not in self and "auth_url" not in self:
            errors.append("One of 'cloud' or 'auth_url' must be set.")
        elif "auth_url" in self:
            has_appcred = (
                "application_credential_id" in self
                and "application_credential_secret" in self
            )
            has_password = "username" in self and "password" in self
            if not (has_appcred or has_password):
                errors.append(
                    "With 'auth_url', either 'application_credential_id' + "
                    "'application_credential_secret' or 'username' + "
                    "'password' must be set."
                )
            if "project_name" not in self and "project_id" not in self:
                errors.append("One of 'project_name' or 'project_id' must be set.")

        if "proxy_instance_id" not in self:
            errors.append("Mandatory option 'proxy_instance_id' not defined.")

        return errors

    def __contains__(self, key):
        try:
            return self._options[key]["value_set"]
        except KeyError as e:
            raise UnknownOptionException(key) from e

    def __setitem__(self, key, value):
        try:
            opt = self._options[key]
            if opt["type"] == str:
                opt["value"] = value
            elif opt["type"] == int:
                try:
                    opt["value"] = int(value, base=10)
                except ValueError as e:
                    raise ValueError(
                        f"invalid value '{value}' for integer option '{key}'"
                    ) from e
            elif opt["type"] == bool:
                tmp = value.strip().lower()
                if tmp in ("yes", "on", "true", "1"):
                    opt["value"] = True
                elif tmp in ("no", "off", "false", "0"):
                    opt["value"] = False
                else:
                    raise ValueError(
                        f"invalid value '{value}' for boolean option '{key}'"
                    )
            opt["value_set"] = True
        except KeyError as e:
            raise UnknownOptionException(key) from e

    def __getitem__(self, key):
        try:
            return self._options[key]["value"]
        except KeyError as e:
            raise UnknownOptionException(key) from e

    def __getattr__(self, key):
        try:
            return self._options[key]["value"]
        except KeyError as e:
            raise UnknownOptionException(key) from e
