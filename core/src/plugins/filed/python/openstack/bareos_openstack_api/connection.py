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

"""OpenStack SDK connection setup.

Kept free of any `bareosfd` dependency so `build_connect_kwargs()` is
unit-testable with plain `unittest` - no live cloud, no bareosfd C
extension needed. Never put secrets in exception messages that might get
logged via JobMessage; the plugin class is responsible for only ever
surfacing connection errors, not the kwargs used to make them.
"""

import openstack  # pylint: disable=import-error


def build_connect_kwargs(options):
    """
    Turn plugin options into openstack.connect() kwargs. Returns
    {"cloud": <name>} for the clouds.yaml path, or an explicit kwargs dict
    for the auth_url path. Assumes options.check_mandatory() has already
    been called and returned no errors.
    """
    if "cloud" in options:
        return {"cloud": options.cloud}

    kwargs = {"auth_url": options.auth_url}
    if options.region_name:
        kwargs["region_name"] = options.region_name
    if not options.verify_ssl:
        kwargs["verify"] = False
    if options.project_name:
        kwargs["project_name"] = options.project_name
    if options.project_id:
        kwargs["project_id"] = options.project_id

    if "application_credential_id" in options:
        kwargs["auth_type"] = "v3applicationcredential"
        kwargs["application_credential_id"] = options.application_credential_id
        kwargs["application_credential_secret"] = (
            options.application_credential_secret
        )
    else:
        kwargs["username"] = options.username
        kwargs["password"] = options.password
        kwargs["user_domain_name"] = options.user_domain_name
        kwargs["project_domain_name"] = options.project_domain_name

    return kwargs


def connect(options):
    """Authenticate against OpenStack using the given plugin options."""
    return openstack.connect(**build_connect_kwargs(options))
