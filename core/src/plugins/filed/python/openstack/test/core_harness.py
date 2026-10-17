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
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public
# License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

"""Drive the OpenStack plugin through a small File Daemon and catalog model."""

import dataclasses
import importlib.util
import os
import random
import shutil
import sys
import tempfile
import threading
import time

import bareosfd_stub

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN_FILE = os.path.join(os.path.dirname(HERE), "bareos-fd-openstack.py")


@dataclasses.dataclass
class JobResult:
    """Observable result of one emulated File Daemon job."""

    return_code: int
    failed: bool
    files: list
    restore_objects: list
    messages: list
    elapsed: float


def load_plugin_module():
    """Install the bareosfd stand-in and load the plugin module."""
    bareosfd_stub.install()
    sys.modules.pop("bareos_fd_openstack_harness", None)
    spec = importlib.util.spec_from_file_location(
        "bareos_fd_openstack_harness", PLUGIN_FILE
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Catalog:
    """Keep successful job files, RestoreObjects and the current file tree."""

    def __init__(self):
        self.jobs = []
        self.tree = {}

    def accurate_list(self, level):
        if level == "I":
            result = dict(self.tree)
            for job in reversed(self.jobs):
                if job["restore_objects"]:
                    result[job["restore_objects"][-1]["name"]] = job["restore_objects"][
                        -1
                    ]
                    break
            return result
        if level == "D":
            full = next((j for j in reversed(self.jobs) if j["level"] == "F"), None)
            if not full:
                return {}
            result = dict(full["tree_after"])
            if full["restore_objects"]:
                result[full["restore_objects"][-1]["name"]] = full["restore_objects"][
                    -1
                ]
            return result
        return {}

    def add_success(self, level, job_id, files, restore_objects, seen_paths=()):
        current = dict(self.tree)
        for entry in files:
            if entry["type"] == bareosfd_stub.FT_DELETED:
                current.pop(entry["fname"], None)
            elif entry["type"] == bareosfd_stub.FT_REG:
                current[entry["fname"]] = entry
        if level in "ID":
            for path in list(current):
                if path not in seen_paths:
                    current.pop(path, None)
        self.tree = current
        self.jobs.append(
            {
                "level": level,
                "job_id": job_id,
                "files": list(files),
                "restore_objects": list(restore_objects),
                "tree_after": dict(current),
            }
        )

    def selected_chain(self, upto_job=None):
        jobs = [j for j in self.jobs if upto_job is None or j["job_id"] <= upto_job]
        full_index = next(
            (i for i in range(len(jobs) - 1, -1, -1) if jobs[i]["level"] == "F"),
            None,
        )
        if full_index is None:
            return []
        full = jobs[full_index]
        diff = next(
            (j for j in reversed(jobs[full_index + 1 :]) if j["level"] == "D"),
            None,
        )
        chain = [full] + ([diff] if diff else [])
        chain.extend(
            j
            for j in jobs[full_index + 1 :]
            if j["level"] == "I" and (diff is None or j["job_id"] > diff["job_id"])
        )
        return sorted(chain, key=lambda job: job["job_id"])

    def restore_tree(self, upto_job=None):
        chain = self.selected_chain(upto_job)
        return dict(chain[-1]["tree_after"]) if chain else {}


def _plugin_string(plugin_string):
    value = plugin_string
    if value.startswith("python3:"):
        value = value[len("python3:") :]
    parts = []
    for item in value.split(":"):
        if item.startswith("module_name=") or item.startswith("module_path="):
            continue
        parts.append(item)
    return ":".join(parts)


def _stat_equal(left, right):
    return all(
        getattr(left["statp"], name) == getattr(right["statp"], name)
        for name in ("st_mtime", "st_ctime", "st_size")
    )


def _message_text(messages):
    return "\n".join(str(message[1]) for message in messages)


def _read_backup_data(plugin, entry):
    stub = bareosfd_stub
    packet = stub.IoPacket()
    packet.func = stub.IO_OPEN
    packet.fname = entry["fname"]
    rc = plugin.plugin_io(packet)
    if rc != stub.bRC_OK or packet.status != stub.iostat_do_in_core:
        raise AssertionError("backup IO_OPEN did not return an in-core descriptor")
    chunks = []
    while True:
        part = os.read(packet.filedes, 1024 * 1024)
        if not part:
            break
        chunks.append(part)
    packet.func = stub.IO_CLOSE
    if plugin.plugin_io(packet) != stub.bRC_OK:
        raise AssertionError("backup IO_CLOSE failed")
    return b"".join(chunks)


def run_backup(
    catalog,
    plugin_string,
    level,
    job_id,
    accurate=True,
    cancel_at=None,
    accurate_options=None,
):
    """Run a complete backup job and update the catalog only on success."""
    module = load_plugin_module()
    stub = bareosfd_stub
    stub.reset(
        bVarLevel=ord(level),
        bVarJobId=job_id,
        bVarAccurate=int(accurate),
        bVarAccurateOptions=(
            accurate_options
            if accurate_options is not None
            else frozenset(
                {
                    stub.bAccurateOptionMtime,
                    stub.bAccurateOptionCtime,
                    stub.bAccurateOptionSize,
                }
            )
        ),
    )
    started = time.monotonic()
    files = []
    restore_objects = []
    seen_paths = []
    definition = _plugin_string(plugin_string)
    plugin = module.BareosFdOpenStack(definition)
    rc = plugin.parse_plugin_definition(definition)
    if rc == stub.bRC_OK and level in "ID":
        source = catalog.accurate_list(level)
        selected = catalog.jobs[-1:] if level == "I" else []
        if level == "D":
            selected = [j for j in catalog.jobs if j["level"] == "F"][-1:]
        for job in selected:
            for obj in job["restore_objects"]:
                packet = stub.RestoreObject()
                packet.object_name, packet.object = obj["name"], obj["object"]
                packet.jobid = job["job_id"]
                plugin.restore_object_data(packet)

    if rc == stub.bRC_OK:
        rc = plugin.handle_plugin_event(stub.bEventStartBackupJob)
    cancel_thread = None
    if cancel_at is not None and rc == stub.bRC_OK:
        if cancel_at == "start":
            cancel_thread = threading.Thread(
                target=lambda: (
                    time.sleep(1),
                    plugin.handle_plugin_event(stub.bEventCancelCommand),
                ),
                daemon=True,
            )
            cancel_thread.start()
    if rc == stub.bRC_OK:
        while True:
            packet = stub.SavePacket()
            packet.flags[0] |= 1 << 5
            next_rc = plugin.start_backup_file(packet)
            if next_rc == stub.bRC_Stop:
                rc = stub.bRC_OK
                break
            if next_rc != stub.bRC_OK:
                rc = next_rc
                break
            seen_paths.append(packet.fname)
            if level in "ID" and packet.fname in source:
                old = source[packet.fname]
                if _stat_equal({"statp": packet.statp}, old):
                    packet.type = stub.FT_NOCHG
                    plugin.end_backup_file()
                    continue
            entry = {
                "fname": packet.fname,
                "type": packet.type,
                "no_read": packet.no_read,
                "flags": bytes(packet.flags),
                "statp": packet.statp,
                "object": bytes(packet.object),
                "data": None,
                "job_id": job_id,
            }
            if packet.type == stub.FT_REG and not packet.no_read:
                entry["data"] = _read_backup_data(plugin, entry)
            files.append(entry)
            if packet.type == stub.FT_RESTORE_FIRST:
                restore_objects.append(
                    {
                        "name": packet.fname,
                        "object": bytes(packet.object),
                        "statp": packet.statp,
                        "fname": packet.fname,
                        "type": packet.type,
                        "job_id": job_id,
                    }
                )
            if plugin.end_backup_file() == stub.bRC_OK:
                break
    plugin.handle_plugin_event(stub.bEventEndBackupJob)
    if rc == stub.bRC_OK:
        plugin.handle_plugin_event(stub.bEventJobEnd)
        catalog.add_success(level, job_id, files, restore_objects, seen_paths)
    if cancel_thread is not None:
        cancel_thread.join(timeout=20)
    messages = list(stub.messages) + list(stub.debug_messages)
    failed = rc != stub.bRC_OK or any(
        level_value in (stub.M_FATAL, stub.M_ERROR) for level_value, _ in stub.messages
    )
    return JobResult(
        rc, failed, files, restore_objects, messages, time.monotonic() - started
    )


def run_restore(
    catalog,
    plugin_string,
    pluginoptions,
    where,
    job_id,
    upto_job=None,
    shuffle=False,
    drop_paths=(),
    drop_data=(),
):
    """Restore a catalog chain to local files or the fake cloud."""
    module = load_plugin_module()
    stub = bareosfd_stub
    stub.reset(bVarLevel=ord("R"), bVarJobId=job_id, bVarWhere=where)
    started = time.monotonic()
    definition = _plugin_string(plugin_string)
    plugin = module.BareosFdOpenStack(definition)
    options = _plugin_string(pluginoptions)
    combined = ":".join(part for part in (options, definition) if part)
    rc = plugin.parse_plugin_definition(combined)
    chain = catalog.selected_chain(upto_job)
    files = [entry for job in chain for entry in job["files"]]
    objects = [obj for job in chain for obj in job["restore_objects"]]
    for obj in objects:
        packet = stub.RestoreObject()
        packet.object_name, packet.object = obj["name"], obj["object"]
        packet.jobid = next(
            job["job_id"] for job in chain if obj in job["restore_objects"]
        )
        plugin.restore_object_data(packet)
    if rc == stub.bRC_OK:
        rc = plugin.handle_plugin_event(stub.bEventStartRestoreJob)
    if rc == stub.bRC_OK:
        rc = plugin.parse_plugin_definition(definition)
    output_files = []
    ordered = list(catalog.restore_tree(upto_job).values())
    if shuffle:
        random.Random(47).shuffle(ordered)
    if rc == stub.bRC_OK:
        for entry in ordered:
            if entry["fname"] in drop_paths:
                continue
            packet = stub.RestorePacket()
            packet.original_file_name = entry["fname"]
            packet.ofname = os.path.join(where, entry["fname"])
            packet.where = where
            packet.statp = entry["statp"]
            rc = plugin.create_file(packet)
            if rc != stub.bRC_OK:
                break
            if packet.create_status == stub.CF_EXTRACT:
                io_packet = stub.IoPacket()
                io_packet.func = stub.IO_OPEN
                io_packet.fname = entry["fname"]
                io_packet.flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
                rc = plugin.plugin_io(io_packet)
                if rc != stub.bRC_OK:
                    break
                data = entry["data"]
                if data is not None and entry["fname"] not in drop_data:
                    os.write(io_packet.filedes, data)
                io_packet.func = stub.IO_CLOSE
                rc = plugin.plugin_io(io_packet)
                output_files.append(entry)
                if rc != stub.bRC_OK:
                    break
                plugin.end_restore_file()
    end_rc = plugin.handle_plugin_event(stub.bEventEndRestoreJob)
    if rc == stub.bRC_OK:
        rc = end_rc
    plugin.handle_plugin_event(stub.bEventJobEnd)
    messages = list(stub.messages) + list(stub.debug_messages)
    failed = rc != stub.bRC_OK or any(
        level_value in (stub.M_FATAL, stub.M_ERROR) for level_value, _ in stub.messages
    )
    return JobResult(
        rc, failed, output_files, objects, messages, time.monotonic() - started
    )
