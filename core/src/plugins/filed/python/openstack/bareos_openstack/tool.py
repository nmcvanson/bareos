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

"""Operator tool: list servers, check one server, report and remove leftovers."""

import argparse
import re
import sys

from . import options
from .cloud import (
    ROLE_BACKUP_SNAPSHOT,
    ROLE_BACKUP_TEMP_VOLUME,
    TAG_CREATED_BY,
    TAG_CREATED_BY_VALUE,
    TAG_JOB_ID,
    TAG_ROLE,
    TAG_SERVER_ID,
    CloudConfigError,
    CloudConflict,
    CloudError,
    CloudNotFound,
    create_backend,
)
from .cloud_backup import USABLE_STATUSES

XFER_NAME = re.compile(r"^bareos-\d+-xfer-")

EXIT_OK = 0
EXIT_FINDINGS = 1
EXIT_ERROR = 2


class ToolError(Exception):
    """A usage or configuration problem; the tool exits with code 2."""


def _build_parser():
    parser = argparse.ArgumentParser(
        prog="bareos-openstack-tool",
        description="Inspect and clean up OpenStack resources of the Bareos plugin.",
    )
    parser.add_argument("--config", help="options file (defaults_file format)")
    parser.add_argument("--backend", choices=("sdk", "fake"))
    parser.add_argument("--fake-root", help="fake cloud directory (--backend fake)")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list-servers", help="list the servers of the project")

    p = sub.add_parser("preflight", help="check that a server can be backed up")
    group = p.add_mutually_exclusive_group(required=True)
    group.add_argument("--server-id")
    group.add_argument("--server-name")

    p = sub.add_parser("leftovers", help="report or remove backup leftovers")
    p.add_argument("--server-id")
    p.add_argument(
        "--cleanup",
        action="store_true",
        help="remove leftovers; no backup job may be running (job ids are global)",
    )
    p.add_argument("--older-than-job", type=int)
    p.add_argument("--yes", action="store_true")
    p.add_argument(
        "--detach",
        action="store_true",
        help="also detach and delete volumes attached to the proxy (they may "
        "be read by a running job; use only when no backup job is running)",
    )
    p.add_argument("--timeout", type=int, default=300)
    return parser


def _backend(args):
    if args.config:
        loader = getattr(options, "load_options_file", None)
        if loader is None:
            raise ToolError("reading an options file is not available")
        try:
            loaded = loader(args.config)
            config = loaded.backend_config()
        except (OSError, ValueError, options.UnknownOptionError) as exc:
            raise ToolError("cannot read %s: %s" % (args.config, exc)) from exc
    else:
        config = {"backend": args.backend or "sdk"}
    if args.backend:
        config["backend"] = args.backend
    if args.fake_root:
        config["fake_root"] = args.fake_root
    if config.get("backend") == "fake" and not config.get("fake_root"):
        raise ToolError("--backend fake needs --fake-root")
    if config.get("backend") != "fake" and not args.config:
        raise ToolError("--config is required for backend sdk")
    backend = create_backend(config, proxy=True)
    backend.connect()
    return backend


def _out(text):
    print(text)


def _list_servers(be, _args):
    for srv in sorted(be.list_servers(), key=lambda s: (s.name, s.id)):
        count = len(be.list_server_volumes(srv.id))
        _out(
            "%s  %s  %s  boot=%s  volumes=%d"
            % (
                srv.id,
                srv.name,
                srv.status,
                "volume" if srv.boot_from_volume else "image",
                count,
            )
        )
    return EXIT_OK


def _preflight(be, args):
    failed = []

    def report(ok, check, detail):
        _out("%-4s %s: %s" % ("OK" if ok else "FAIL", check, detail))
        if not ok:
            failed.append(check)

    try:
        server = (
            be.get_server(args.server_id)
            if args.server_id
            else be.find_server(args.server_name)
        )
    except CloudNotFound as exc:
        report(False, "server", str(exc))
        return EXIT_FINDINGS
    except CloudConflict as exc:
        report(False, "server", str(exc))
        return EXIT_FINDINGS
    report(True, "server", "%s (%s)" % (server.id, server.name))
    report(
        server.status in USABLE_STATUSES,
        "status",
        "%s (usable: %s)" % (server.status, ", ".join(USABLE_STATUSES)),
    )
    project = be.project_id
    same = not project or not server.project_id or project == server.project_id
    report(
        same,
        "project",
        "server in %s, connected to %s" % (server.project_id, project or "unknown"),
    )
    volumes = be.list_server_volumes(server.id)
    if server.boot_from_volume:
        report(True, "boot", "boots from a volume")
    else:
        report(
            False,
            "boot",
            "server %s boots from an image; only servers that boot from a "
            "Cinder volume are supported" % server.id,
        )
    encrypted = [v.id for v in volumes if v.encrypted]
    report(
        not encrypted,
        "encryption",
        "encrypted volumes: %s" % ", ".join(encrypted) if encrypted else "none",
    )
    report(
        bool(volumes),
        "volumes",
        "%d volume(s)" % len(volumes),
    )
    proxy = be.proxy_instance_id
    if not proxy or proxy == "auto":
        report(False, "proxy", "no proxy_instance_id is configured")
    else:
        try:
            be.get_server(proxy)
            report(True, "proxy", "server %s exists" % proxy)
        except CloudNotFound:
            report(False, "proxy", "server %s does not exist" % proxy)
    return EXIT_FINDINGS if failed else EXIT_OK


def _collect(be, server_id):
    """Return (kind, resource, tags) of every backup-role resource."""
    wanted = {TAG_CREATED_BY: TAG_CREATED_BY_VALUE}
    if server_id:
        wanted[TAG_SERVER_ID] = server_id
    found = []
    for vol in be.list_volumes(dict(wanted, **{TAG_ROLE: ROLE_BACKUP_TEMP_VOLUME})):
        found.append(("volume", vol, vol.metadata))
    for snap in be.list_snapshots(dict(wanted, **{TAG_ROLE: ROLE_BACKUP_SNAPSHOT})):
        found.append(("snapshot", snap, snap.metadata))
    try:
        for gsnap in be.list_group_snapshots("bareos-"):
            tags = gsnap.tags
            if tags and (not server_id or tags.get(TAG_SERVER_ID) == server_id):
                found.append(("group snapshot", gsnap, tags))
    except CloudConfigError:
        pass  # the cloud has no volume groups, so no group snapshots
    except CloudError as exc:
        print("warning: group snapshots not listed: %s" % exc, file=sys.stderr)
    return found


def _report_only(be, server_id):
    """Lines for pending Bareos transfers."""
    lines = []
    try:
        for tr in be.list_transfers("bareos-"):
            if XFER_NAME.match(tr.name):
                lines.append(
                    "transfer %s name=%s volume=%s (pending; not removed by the "
                    "tool)" % (tr.id, tr.name, tr.volume_id)
                )
    except CloudError as exc:
        print("warning: transfers not listed: %s" % exc, file=sys.stderr)
    return lines


def _line(kind, res, tags):
    return "%s %s job=%s server=%s" % (
        kind,
        res.id,
        tags.get(TAG_JOB_ID, "?"),
        tags.get(TAG_SERVER_ID, "?"),
    )


def _leftovers(be, args):
    if args.cleanup:
        return _cleanup(be, args)
    found = _collect(be, args.server_id)
    listed = {res.id for kind, res, _ in found if kind == "volume"}
    attached = [v for v in be.list_proxy_attachments() if v in listed]
    for kind, res, tags in found:
        _out(_line(kind, res, tags))
    for volume_id in attached:
        _out("attachment %s is attached to the proxy" % volume_id)
    pending = _report_only(be, args.server_id)
    for line in pending:
        _out(line)
    if not found and not attached and not pending:
        _out("no leftovers")
        return EXIT_OK
    return EXIT_FINDINGS


def _cleanup(be, args):
    if args.older_than_job is None or not args.yes:
        raise ToolError("--cleanup needs --older-than-job N and --yes")
    timeout = args.timeout
    attached = set(be.list_proxy_attachments())
    failures = 0
    collected = _collect(be, args.server_id)
    ordered = sorted(
        collected,
        key=lambda item: ("volume", "snapshot", "group snapshot").index(item[0]),
    )
    for kind, res, tags in ordered:
        job = tags.get(TAG_JOB_ID, "")
        if not job.isdigit() or int(job) >= args.older_than_job:
            _out(
                "kept    %s (job id not lower than %d)"
                % (_line(kind, res, tags), args.older_than_job)
            )
            continue
        if kind == "volume" and res.id in attached and not args.detach:
            _out(
                "kept    %s (attached to the proxy; a job may be reading it)"
                % _line(kind, res, tags)
            )
            continue
        try:
            if kind == "volume":
                if res.id in attached:
                    be.detach_from_proxy(res.id, timeout)
                be.delete_volume(res.id, timeout)
            elif kind == "snapshot":
                be.delete_snapshot(res.id, timeout)
            else:
                be.delete_group_snapshot(res.id, timeout)
            _out("removed %s" % _line(kind, res, tags))
        except CloudError as exc:
            failures += 1
            _out("FAILED  %s: %s" % (_line(kind, res, tags), exc))
    for line in _report_only(be, args.server_id):
        _out("kept    %s" % line)
    return EXIT_FINDINGS if failures else EXIT_OK


def main(argv=None):
    """Entry point; returns the exit code."""
    parser = _build_parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return exc.code if isinstance(exc.code, int) else EXIT_ERROR
    handlers = {
        "list-servers": _list_servers,
        "preflight": _preflight,
        "leftovers": _leftovers,
    }
    backend = None
    try:
        backend = _backend(args)
        return handlers[args.command](backend, args)
    except (ToolError, CloudError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return EXIT_ERROR
    finally:
        if backend is not None:
            backend.close()


if __name__ == "__main__":
    sys.exit(main())
