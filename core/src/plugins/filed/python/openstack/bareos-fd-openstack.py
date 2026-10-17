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

# pylint: disable=invalid-name,too-many-lines,too-many-instance-attributes
"""This module provides the OpenStack FD plugin for Bareos"""

import os
import queue
import stat
import threading

from BareosFdWrapper import *  # pylint: disable=import-error,wildcard-import
import bareosfd  # pylint: disable=import-error
import BareosFdPluginBaseclass  # pylint: disable=import-error

from bareos_openstack import (
    __version__,
    chunks,
    cloud,
    metadata,
    naming,
)
from bareos_openstack.options import (
    Options,
    UnknownOptionError,
    redact_plugin_string,
)
from bareos_openstack.cloud_backup import BackupCloudSession
from bareos_openstack.cloud_restore import RestoreCloudSession

# Seconds to wait for the volume reader thread at the end of a job.
JOIN_TIMEOUT = 180

# FO_SPARSE is bit 5 of the file options (core/src/include/fileopts.h).
FO_SPARSE_BIT = 1 << 5


def fail(msg):
    """Report a fatal job message and return an error code"""
    bareosfd.JobMessage(bareosfd.M_FATAL, "openstack: " + msg + "\n")
    return bareosfd.bRC_Error


class LogQueue:
    """Thread-safe log sink that is flushed from the job's main thread"""

    def __init__(self):
        self._queue = queue.SimpleQueue()

    def __call__(self, level, message):
        try:
            self._queue.put((level, str(message)))
        except Exception:  # pylint: disable=broad-except
            pass

    def flush(self):
        """Send the queued messages to the core (main thread only)"""
        while True:
            try:
                level, message = self._queue.get_nowait()
            except queue.Empty:
                return
            text = "openstack: " + message.rstrip("\n") + "\n"
            if level == "debug":
                bareosfd.DebugMessage(100, text)
            elif level == "warning":
                bareosfd.JobMessage(bareosfd.M_WARNING, text)
            elif level == "error":
                bareosfd.JobMessage(bareosfd.M_ERROR, text)
            else:
                bareosfd.JobMessage(bareosfd.M_INFO, text)


@BareosPlugin  # pylint: disable=undefined-variable
class BareosFdOpenStack(BareosFdPluginBaseclass.BareosFdPluginBaseclass):
    """Backs up and restores one OpenStack server per job"""

    def __init__(self, plugindef):
        self.options = Options()
        # The base class only logs this string, so it must not hold secrets.
        super().__init__(redact_plugin_string(plugindef))
        bareosfd.RegisterEvents(
            [
                bareosfd.bEventStartRestoreJob,
                bareosfd.bEventEndRestoreJob,
                bareosfd.bEventCancelCommand,
            ]
        )
        self.log = LogQueue()
        self.cancel_event = threading.Event()
        self.backend = None
        self.target_backend = None
        self.backup_session = None
        self.producer = None
        self.queue = None
        self.current = None
        self.feeder = None
        self.inventory = None
        self.volume_states = {}
        self.level_char = ""
        self.document_emitted = False
        self.backup_started = False
        self.accurate_checked = False
        self.estimate_reported = False
        self.documents = []
        self.plugin_line_server = None
        self.restore_started = False
        self.restore_prepared = False
        self.restore_session = None
        self.restore_doc = None
        self.targets = {}
        self.restore_current = None
        self.restore_fd = None
        self.restore_failed = False
        self.local_dir = None

    # ----------------------------------------------------------------- options

    def parse_plugin_definition(self, plugindef, multi_value_options=None):
        """Parse the plugin string without logging any option value"""
        raw = BareosFdPluginBaseclass.parse_plugindef_string(plugindef, [])

        if chr(self.level) in "FID":
            pair = (raw.get("server_id", ""), raw.get("server_name", ""))
            if any(pair):
                if self.plugin_line_server is None:
                    self.plugin_line_server = pair
                elif self.plugin_line_server != pair:
                    return fail(
                        "only one Plugin line per FileSet is supported "
                        "(one server per job)"
                    )

        files = {}
        for key in ("defaults_file", "overrides_file"):
            name = raw.pop(key, None)
            if name is None:
                files[key] = {}
                continue
            path = os.path.join(self.configdir, name)
            try:
                files[key] = BareosFdPluginBaseclass.parse_options_file(path, [])
            except OSError as e:
                return fail(
                    "cannot read options file %s: %s" % (name, e.strerror or "error")
                )

        effective = {}
        effective.update(files["defaults_file"])
        effective.update(raw)
        effective.update(files["overrides_file"])

        for key, value in effective.items():
            name, _, transform = key.partition("#")
            if transform:
                try:
                    value = BareosFdPluginBaseclass.transform_value(value, transform)
                except Exception:  # pylint: disable=broad-except
                    return fail('cannot decode the value of option "%s"' % name)
            try:
                if name not in self.options:
                    self.options[name] = value
            except UnknownOptionError:
                return fail("unknown option '%s'" % name)
            except ValueError as e:
                return fail(str(e))

        bareosfd.DebugMessage(
            100, "openstack: options " + str(self.options.redacted()) + "\n"
        )
        return bareosfd.bRC_OK

    def check_options(self, mandatory_options=None):
        """Options are validated at the start of the job"""
        return bareosfd.bRC_OK

    # ------------------------------------------------------------------ events

    def handle_plugin_event(self, event):
        """Dispatch core events; the cancel event only sets a flag"""
        if event == bareosfd.bEventCancelCommand:
            self.cancel_event.set()
            self.log("info", "cancel requested")
            return bareosfd.bRC_OK
        if event == bareosfd.bEventStartBackupJob:
            return self.start_backup_job()
        if event == bareosfd.bEventEndBackupJob:
            return self.end_backup_job()
        if event == bareosfd.bEventStartRestoreJob:
            return self.start_restore_job()
        if event == bareosfd.bEventEndRestoreJob:
            return self.end_restore_job()
        if event == bareosfd.bEventJobEnd:
            return self.end_job()
        return bareosfd.bRC_OK

    def check_file(self, fname):
        """Chunks that Accurate did not see are deleted by the core"""
        return bareosfd.bRC_OK

    def restore_object_data(self, ROP):
        """Collect the server document of every job of the chain"""
        name = str(ROP.object_name)
        if not naming.is_document_path(name):
            bareosfd.DebugMessage(100, "openstack: ignoring restore object\n")
            return bareosfd.bRC_OK
        try:
            doc = metadata.load_document(ROP.object)
        except metadata.DocumentError as e:
            return fail("invalid server metadata in %s: %s" % (name, e))
        jobid = int(getattr(ROP, "jobid", 0) or doc["job"]["job_id"])
        self.documents.append((jobid, doc))
        return bareosfd.bRC_OK

    # ------------------------------------------------------------------ backup

    def _info(self, text):
        bareosfd.JobMessage(bareosfd.M_INFO, "openstack: " + text + "\n")

    def _warning(self, text):
        bareosfd.JobMessage(bareosfd.M_WARNING, "openstack: " + text + "\n")

    def _error(self, text):
        bareosfd.JobMessage(bareosfd.M_ERROR, "openstack: " + text + "\n")

    def start_backup_job(self):
        """Validate the options, prepare the cloud and start the reader"""
        self.backup_started = True
        errors = self.options.validate_backup()
        if errors:
            for error in errors:
                fail(error)
            return bareosfd.bRC_Error

        level = chr(bareosfd.GetValue(bareosfd.bVarLevel))
        if level not in "FID":
            return fail("backup level %s is not supported" % level)
        if level in "ID" and not bareosfd.GetValue(bareosfd.bVarAccurate):
            return fail(
                "Accurate mode is required for Incremental and Differential "
                "backups; set Accurate = yes in the Job"
            )
        self.level_char = level

        chunk_size = self.options.chunk_size
        hash_name = self.options.hash
        prev = None
        if level in "ID":
            if self.documents:
                try:
                    prev = metadata.select_chain_document(self.documents)
                except metadata.DocumentError as e:
                    return fail(str(e))
                old = prev["chunking"]
                if old["chunk_size"] != chunk_size:
                    return fail(
                        "chunk_size changed from %d to %d since the previous "
                        "backup; run a Full backup" % (old["chunk_size"], chunk_size)
                    )
                if old["hash"] != hash_name:
                    return fail(
                        "hash changed from %s to %s since the previous backup; "
                        "run a Full backup" % (old["hash"], hash_name)
                    )
            else:
                self._warning("no metadata of the previous backup received")

        o = self.options
        try:
            self.backend = cloud.create_backend(o.backend_config(), proxy=True)
            self.backend.set_cancel_event(self.cancel_event)
            self.backend.connect()
            self.backup_session = BackupCloudSession(
                self.backend,
                int(self.jobId),
                server_id=o.server_id,
                server_name=o.server_name,
                poll_timeout=o.poll_timeout,
                snapshot_force=o.snapshot_force,
                snapshot_mode=o.snapshot_mode,
                cleanup_leftovers=o.cleanup_leftovers,
                log=self.log,
            )
            self.inventory = self.backup_session.prepare()
        except Exception as e:  # pylint: disable=broad-except
            self.log.flush()
            return fail("backup preparation failed: %s" % e)
        self.log.flush()

        server = self.inventory.server
        if prev is not None and prev["server"]["id"] != server.id:
            return fail(
                "the server of this FileSet changed from %s to %s; run a Full backup"
                % (prev["server"]["id"], server.id)
            )
        if prev is not None:
            old_mode = metadata.root_mode(prev)
            new_mode = metadata.inventory_root_mode(self.inventory)
            if old_mode != new_mode:
                return fail(
                    "the root disk mode changed from %s to %s since the previous "
                    "backup; run a Full backup" % (old_mode, new_mode)
                )

        self.queue = queue.Queue(maxsize=o.queue_depth)
        self.producer = chunks.ChunkProducer(
            [v.id for v in self.inventory.volumes],
            self.backup_session.open_volume,
            self.backup_session.release_volume,
            chunk_size,
            hash_name,
            self.queue,
            self.cancel_event,
            self.log,
        )
        self.producer.start()
        self._info(
            "backing up server %s (%s), %d volume(s), chunk size %d, level %s"
            % (
                server.name,
                server.id,
                len(self.inventory.volumes),
                chunk_size,
                level,
            )
        )
        return bareosfd.bRC_OK

    def _check_accurate_options(self):
        """The Accurate options must compare a field that carries the digest"""
        opts = bareosfd.GetValue(bareosfd.bVarAccurateOptions)
        wanted = (
            bareosfd.bAccurateOptionMtime,
            bareosfd.bAccurateOptionCtime,
            bareosfd.bAccurateOptionAtime,
            bareosfd.bAccurateOptionInode,
        )
        unwanted = (bareosfd.bAccurateOptionMD5, bareosfd.bAccurateOptionSha1)
        if not any(w in opts for w in wanted) or any(u in opts for u in unwanted):
            return fail(
                "the FileSet Accurate options must compare mtime, ctime, atime "
                "or inode and must not use md5 or sha1 (recommended: mcs)"
            )
        self.accurate_checked = True
        return bareosfd.bRC_OK

    def start_backup_file(self, savepkt):
        """Hand the next chunk file, or at the end the server document, to the core"""
        if self.producer is None:
            if self.backup_started:
                return bareosfd.bRC_Error
            if not self.estimate_reported:
                self.estimate_reported = True
                self._info("estimate is not supported; nothing to list")
            return bareosfd.bRC_Stop
        if self.document_emitted:
            return bareosfd.bRC_Stop
        if self.level_char in "ID" and not self.accurate_checked:
            if self._check_accurate_options() != bareosfd.bRC_OK:
                return bareosfd.bRC_Error

        self.current = None
        while True:
            self.log.flush()
            if self.cancel_event.is_set():
                return fail("job cancelled")
            try:
                item = self.queue.get(timeout=5)
            except queue.Empty:
                if not self.producer.is_alive() and self.queue.empty():
                    return fail("chunk producer stopped unexpectedly")
                continue

            if isinstance(item, chunks.ChunkRecord):
                return self._chunk_savepacket(savepkt, item)
            if isinstance(item, chunks.VolumeDone):
                self.volume_states[item.volume_id] = {
                    "size_bytes": item.size_bytes,
                    "chunk_count": item.chunk_count,
                }
                self._info("volume %s: %d chunks" % (item.volume_id, item.chunk_count))
                continue
            if isinstance(item, chunks.ProducerFailure):
                self.log.flush()
                return fail("reading volumes failed: %s" % item.message)
            if isinstance(item, chunks.ProducerEnd):
                return self._document_savepacket(savepkt)

    def _chunk_savepacket(self, savepkt, item):
        server = self.inventory.server
        savepkt.fname = naming.chunk_path(
            server.project_id, server.id, item.volume_id, item.index
        )
        savepkt.type = bareosfd.FT_REG
        statp = bareosfd.StatPacket()
        statp.st_mode = stat.S_IFREG | 0o600
        statp.st_size = item.length
        chunks.apply_digest(statp, item.digest)
        savepkt.statp = statp
        savepkt.no_read = item.data is None
        flags = bytearray(savepkt.flags)
        flags[0] &= 0xFF ^ FO_SPARSE_BIT
        savepkt.flags = flags
        self.current = item
        return bareosfd.bRC_OK

    def _document_savepacket(self, savepkt):
        server = self.inventory.server
        try:
            doc = metadata.build_document(
                self.inventory,
                self.volume_states,
                {
                    "job_id": int(self.jobId),
                    "level": self.level_char,
                    "backup_time": int(self.startTime),
                    "plugin_version": __version__,
                },
                {
                    "chunk_size": self.options.chunk_size,
                    "hash": self.options.hash,
                    "hijacked_stat_fields": list(chunks.HIJACKED_FIELDS),
                },
            )
            data = metadata.dump_document(doc)
            path = naming.document_path(server.project_id, server.id)
        except (metadata.DocumentError, ValueError) as e:
            return fail("cannot build the server metadata: %s" % e)
        savepkt.statp = bareosfd.StatPacket()
        savepkt.type = bareosfd.FT_RESTORE_FIRST
        savepkt.fname = path
        savepkt.object_name = path
        savepkt.object = bytearray(data)
        savepkt.object_len = len(data)
        savepkt.object_index = 1
        self.document_emitted = True
        return bareosfd.bRC_OK

    def end_backup_file(self):
        """More files follow until the server document was emitted"""
        self.log.flush()
        return bareosfd.bRC_OK if self.document_emitted else bareosfd.bRC_More

    # ---------------------------------------------------------------------- io

    def plugin_io_open(self, IOP):
        if self.restore_started:
            return self._restore_io_open(IOP)
        if self.current is None or self.current.data is None:
            IOP.status = bareosfd.iostat_error
            self._error("no chunk data to open")
            return bareosfd.bRC_Error
        self.feeder = chunks.PipeFeeder(self.current.data)
        IOP.filedes = self.feeder.start()
        IOP.status = bareosfd.iostat_do_in_core
        return bareosfd.bRC_OK

    def plugin_io_close(self, IOP):
        if self.restore_started:
            return self._restore_io_close(IOP)
        if self.feeder is not None:
            err = self.feeder.close()
            self.feeder = None
            self.current = None
            if err:
                self._error("chunk data could not be delivered: %s" % err)
                return bareosfd.bRC_Error
        return bareosfd.bRC_OK

    def _unexpected_io(self, IOP):
        IOP.status = bareosfd.iostat_error
        self._error("unexpected read, write or seek request")
        return bareosfd.bRC_Error

    def plugin_io_read(self, IOP):
        return self._unexpected_io(IOP)

    def plugin_io_write(self, IOP):
        return self._unexpected_io(IOP)

    def plugin_io_seek(self, IOP):
        return self._unexpected_io(IOP)

    # -------------------------------------------------------------- backup end

    def _drain(self):
        if self.queue is None:
            return
        while True:
            try:
                self.queue.get_nowait()
            except queue.Empty:
                return

    def _release_cancel(self):
        """Let cleanup and abort run their waits after a cancel"""
        for backend in (self.backend, self.target_backend):
            if backend is not None:
                backend.set_cancel_event(None)

    def _close_backend(self):
        for name in ("backend", "target_backend"):
            backend = getattr(self, name)
            if backend is not None:
                try:
                    backend.close()
                except Exception:  # pylint: disable=broad-except
                    pass
                setattr(self, name, None)

    def end_backup_job(self):
        """Stop the reader, remove every resource of the job and close the cloud"""
        if self.feeder is not None:
            self.feeder.close()
            self.feeder = None
        alive = False
        if self.producer is not None and self.producer.is_alive():
            self.cancel_event.set()
            self._drain()
            self.producer.join(timeout=JOIN_TIMEOUT)
            self._drain()
            alive = self.producer.is_alive()
        if self.backup_session is not None:
            # A reader that is still running keeps the cancel event: cleanup
            # then only does what a cancelled backend allows and reports the rest.
            if not alive:
                self._release_cancel()
            failures = self.backup_session.cleanup()
            self.log.flush()
            if failures:
                self._warning("resources left behind: " + ", ".join(failures))
        if alive:
            self._warning(
                "the volume reader did not stop within %d s; cleanup ran with the "
                "cancellation still active, so resources of this job may remain "
                "and are reported as leftovers by the next backup" % JOIN_TIMEOUT
            )
        else:
            self._close_backend()
        self.log.flush()
        return bareosfd.bRC_OK

    def end_job(self):
        """Close the cloud connection if nothing is using it any more"""
        if self.producer is None or not self.producer.is_alive():
            self._close_backend()
        self.log.flush()
        return bareosfd.bRC_OK

    # ----------------------------------------------------------------- restore

    def start_restore_job(self):
        """Select the document of the chain and prepare the restore targets"""
        if self.restore_started:
            return bareosfd.bRC_OK
        self.restore_started = True
        o = self.options

        where = bareosfd.GetValue(bareosfd.bVarWhere) or ""
        errors = o.validate_restore(where)
        if errors:
            for error in errors:
                fail(error)
            return bareosfd.bRC_Error

        try:
            doc = metadata.select_chain_document(self.documents)
        except metadata.DocumentError as e:
            return fail(str(e) + "; restore the whole backup chain of one server")
        self.restore_doc = doc
        chunk_size = doc["chunking"]["chunk_size"]

        if o.restore_target == "local":
            result = self._prepare_local(doc, chunk_size, where)
        else:
            result = self._prepare_cloud(doc, chunk_size)
        if result != bareosfd.bRC_OK:
            return result
        self.restore_prepared = True
        return bareosfd.bRC_OK

    def _prepare_local(self, doc, chunk_size, where):
        o = self.options
        base = os.path.realpath(o.restore_local_dir or where)
        dirpath = os.path.realpath(os.path.join(base, doc["server"]["id"]))
        if os.path.dirname(dirpath) != base:
            return fail("the restore directory %s is not inside %s" % (dirpath, base))
        try:
            os.makedirs(dirpath, exist_ok=True)
            for vol in doc["volumes"]:
                self.targets[vol["id"]] = chunks.RestoreTarget(
                    os.path.join(dirpath, vol["id"] + ".raw"),
                    vol["size_bytes"],
                    chunk_size,
                    vol["chunk_count"],
                    "if_written",
                    create=True,
                    overwrite=o.restore_overwrite,
                )
            flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW
            flags |= 0 if o.restore_overwrite else os.O_EXCL
            fd = os.open(os.path.join(dirpath, "server.json"), flags, 0o600)
            with os.fdopen(fd, "wb") as f:
                f.write(metadata.dump_document(doc))
        except (OSError, ValueError) as e:
            return fail("local restore preparation failed: %s" % e)
        self.local_dir = dirpath
        self._info("restoring %d volume(s) to %s" % (len(doc["volumes"]), dirpath))
        return bareosfd.bRC_OK

    def _target_backend_config(self):
        """Configuration of the target project connection, or None"""
        o = self.options
        if not (o.restore_project_cloud or o.restore_project_credentials):
            return None
        path = None
        name = o.restore_project_credentials
        if name:
            path = os.path.join(self.configdir, name)
            try:
                if os.stat(path).st_mode & 0o077:
                    self._warning(
                        "the file %s of restore_project_credentials is readable "
                        "by group or others" % name
                    )
            except OSError:
                pass
        try:
            return o.target_backend_config(path)
        except UnknownOptionError as e:
            raise ValueError("unknown option '%s' in %s" % (e, name)) from None
        except OSError as e:
            raise ValueError(
                "cannot read restore_project_credentials %s: %s"
                % (name, e.strerror or "error")
            ) from None
        except ValueError as e:
            if name:
                raise ValueError(
                    "restore_project_credentials %s: %s" % (name, e)
                ) from None
            raise

    def _prepare_cloud(self, doc, chunk_size):
        o = self.options
        try:
            target_config = self._target_backend_config()
            self.backend = cloud.create_backend(o.backend_config(), proxy=True)
            self.backend.set_cancel_event(self.cancel_event)
            self.backend.connect()
            if target_config is not None:
                self.target_backend = cloud.create_backend(target_config, proxy=False)
                self.target_backend.set_cancel_event(self.cancel_event)
                self.target_backend.connect()
            self.restore_session = RestoreCloudSession(
                self.backend,
                doc,
                int(self.jobId),
                o.restore_cloud_options(),
                poll_timeout=o.poll_timeout,
                target_backend=self.target_backend,
                log=self.log,
            )
            self.restore_session.precheck()
            paths = self.restore_session.prepare_volumes()
            mode = "always" if o.restore_write_zeros else "if_written"
            for vol in doc["volumes"]:
                self.targets[vol["id"]] = chunks.RestoreTarget(
                    paths[vol["id"]],
                    vol["size_bytes"],
                    chunk_size,
                    vol["chunk_count"],
                    mode,
                    create=False,
                )
        except (cloud.CloudError, OSError, ValueError, KeyError) as e:
            self.log.flush()
            if self.restore_session is not None:
                self._release_cancel()
                self.restore_session.abort()
                self.log.flush()
            return fail("restore preparation failed: %s" % e)
        self.log.flush()
        self._info("restoring %d volume(s) to the cloud" % len(doc["volumes"]))
        return bareosfd.bRC_OK

    def start_restore_file(self, cmd):
        return bareosfd.bRC_OK

    def end_restore_file(self):
        return bareosfd.bRC_OK

    def _skip_chunk(self, restorepkt, text):
        self._error(text)
        self.restore_failed = True
        restorepkt.create_status = bareosfd.CF_SKIP
        return bareosfd.bRC_OK

    def create_file(self, restorepkt):
        """Map a chunk file to its restore target"""
        if not self.restore_started:
            result = self.start_restore_job()
            if result != bareosfd.bRC_OK:
                restorepkt.create_status = bareosfd.CF_ERROR
                return result
        if not self.restore_prepared:
            restorepkt.create_status = bareosfd.CF_SKIP
            return bareosfd.bRC_OK

        path = getattr(restorepkt, "original_file_name", "") or restorepkt.ofname
        cp = naming.parse_chunk_path(path)
        if cp is None:
            self._warning("skipping unexpected file %s" % path)
            restorepkt.create_status = bareosfd.CF_SKIP
            return bareosfd.bRC_OK

        server = self.restore_doc["server"]
        if cp.project_id != server["project_id"] or cp.server_id != server["id"]:
            return self._skip_chunk(
                restorepkt, "chunk %s does not belong to the restored server" % path
            )
        target = self.targets.get(cp.volume_id)
        if target is None:
            return self._skip_chunk(
                restorepkt, "chunk %s belongs to an unknown volume" % path
            )
        try:
            expected = target.chunk_length(cp.index)
        except IndexError:
            return self._skip_chunk(restorepkt, "chunk %s is outside its volume" % path)
        if restorepkt.statp.st_size != expected:
            return self._skip_chunk(
                restorepkt,
                "chunk %s has size %d, expected %d"
                % (path, restorepkt.statp.st_size, expected),
            )

        self.restore_current = (cp, expected, chunks.digest_fields(restorepkt.statp))
        restorepkt.create_status = bareosfd.CF_EXTRACT
        return bareosfd.bRC_OK

    def _restore_io_open(self, IOP):
        if self.restore_current is None:
            IOP.status = bareosfd.iostat_error
            self._error("no chunk selected for writing")
            return bareosfd.bRC_Error
        cp = self.restore_current[0]
        try:
            fd = self.targets[cp.volume_id].open_chunk(cp.index)
        except OSError as e:
            self.restore_failed = True
            IOP.status = bareosfd.iostat_error
            self._error("cannot open volume %s: %s" % (cp.volume_id, e))
            return bareosfd.bRC_Error
        self.restore_fd = fd
        IOP.filedes = fd
        IOP.status = bareosfd.iostat_do_in_core
        return bareosfd.bRC_OK

    def _restore_io_close(self, IOP):
        if self.restore_current is None or self.restore_fd is None:
            return bareosfd.bRC_OK
        cp, expected, saved = self.restore_current
        fd = self.restore_fd
        target = self.targets[cp.volume_id]
        try:
            try:
                position = os.lseek(fd, 0, os.SEEK_CUR)
            except OSError:
                os.close(fd)
                raise
            written = position - cp.index * target.chunk_size
            zero = chunks.pack_digest(
                chunks.zero_digest(self.restore_doc["chunking"]["hash"], expected)
            )
            if written == 0 and saved != zero:
                os.close(fd)
                self._error(
                    "chunk %d of volume %s arrived without data but is not an "
                    "all-zero chunk" % (cp.index, cp.volume_id)
                )
                self.restore_failed = True
                return bareosfd.bRC_Error
            target.close_chunk(fd, cp.index)
            return bareosfd.bRC_OK
        except (chunks.ChunkError, OSError) as e:
            self._error("restore of volume %s failed: %s" % (cp.volume_id, e))
            self.restore_failed = True
            return bareosfd.bRC_Error
        finally:
            self.restore_current = None
            self.restore_fd = None

    def end_restore_job(self):
        """Verify every volume, then create the server or report the result"""
        if not self.restore_started or not self.restore_prepared:
            self.log.flush()
            return bareosfd.bRC_OK
        self.log.flush()
        cancelled = self.cancel_event.is_set()
        missing_any = False
        for vid, target in self.targets.items():
            try:
                target.finalize()
            except OSError as e:
                self._error("cannot flush volume %s: %s" % (vid, e))
                self.restore_failed = True
            missing = target.missing()
            if missing:
                missing_any = True
                self._error(
                    "volume %s: %d chunk(s) missing (first: %s)"
                    % (vid, len(missing), ", ".join(str(i) for i in missing[:5]))
                )
        complete = not self.restore_failed and not cancelled and not missing_any

        if self.options.restore_target == "local":
            if complete:
                self._info(
                    "restored %d volume(s) to %s" % (len(self.targets), self.local_dir)
                )
                return bareosfd.bRC_OK
            return fail("local restore incomplete")

        if cancelled:
            self._release_cancel()
            self.restore_session.abort()
            self.log.flush()
            return fail("restore cancelled; restored volumes were kept")
        try:
            result = self.restore_session.finish(complete)
        except cloud.CloudError as e:
            self._release_cancel()
            self.restore_session.abort()
            self.log.flush()
            return fail("restore failed: %s" % e)
        self.log.flush()
        if not complete:
            return fail("restore incomplete; restored volumes were kept")
        self._info(
            "restored server %s with volume(s) %s"
            % (
                result.server_id or "no server created",
                ", ".join(result.volume_ids.values()),
            )
        )
        self._close_backend()
        return bareosfd.bRC_OK
