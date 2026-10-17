.. _OpenStackPlugin:

OpenStack Plugin
~~~~~~~~~~~~~~~~

.. index::
   single: Plugin; OpenStack
   single: OpenStack Plugin

The OpenStack Plugin does agentless backups of OpenStack servers (Nova instances). One job backs up
one server: its Cinder volumes and its definition (flavor, volume attachments, ports, metadata).
Only a server that boots from a Cinder volume is supported. Full, Differential and Incremental
backups are supported; changed data is found by comparing chunk hashes.

A server can be restored as a new server in the cloud (the default) or as raw disk images in a
local directory of the |fd|.

The plugin does not run inside the server. Its |fd| runs on a *proxy*, which is itself a server in the
OpenStack project. For every volume of the backed up server the plugin takes a Cinder snapshot,
creates a temporary volume from it, attaches that volume to the proxy, reads the device and sends
the data to the Storage Daemon in chunks. Snapshots and temporary volumes are removed when the job
ends.

.. limitation:: The plugin is not validated against a real OpenStack cloud yet

   The plugin is tested against a file-backed fake of the OpenStack services and with mocks of the
   OpenStack SDK. Before relying on it, run a backup and a restore of a test server in your cloud
   (see :ref:`OpenStackPluginLimitations`).


Requirements
^^^^^^^^^^^^

Proxy
'''''

* The |fd| with the plugin runs on a Nova server of the same OpenStack project as the servers to be
  backed up. The Cinder service must be able to attach volumes to it. Every backup and every
  restore to the cloud needs the proxy; a restore to local files does not.
* The attached volumes must appear in the proxy as block devices below ``/dev/disk/by-id`` with the
  Cinder volume id as serial number (for example ``virtio-<first 20 characters of the volume id>``).
  The plugin finds a device only by this serial and refuses to use a device that is mounted or held
  by another process.
* The |fd| must be allowed to read and write block devices, which means running as ``root`` (the
  default of the |fd|).
* Python 3.9 or newer and the Python module ``openstacksdk``.
* Enough memory for ``queue_depth * chunk_size`` bytes (defaults ``4 * 64MiB``).

OpenStack project
'''''''''''''''''

* Credentials that may list and read servers, flavors, ports and volumes, create and delete volumes,
  snapshots and ports, attach and detach volumes, and create and delete servers.
* The cloud must support Cinder snapshots of in-use volumes (``snapshot_force=yes``).
* Encrypted volumes are refused.

Job and FileSet
'''''''''''''''

* Incremental and Differential backups need :config:option:`dir/job/accurate` set to ``yes``.
  Without it the job fails.
* Run at most one job per server at a time (``Maximum Concurrent Jobs = 1`` in the Job).


Installation
^^^^^^^^^^^^

Install the package **bareos-filedaemon-openstack-python-plugin** (RPM based systems: the package of
the same name) on the proxy. It installs the |fd|, the Python plugin and requires the package
``python3-openstacksdk``.


Configuration
^^^^^^^^^^^^^

Configure the |fd| to load the Python plugin
''''''''''''''''''''''''''''''''''''''''''''

.. code-block:: bareosconfig
   :caption: bareos-fd.d/client/myself.conf

   Client {
     ...
     Plugin Directory = "/usr/lib/bareos/plugins"
     Plugin Names = "python3"
     ...
   }

Restart the |fd| after the changes with :command:`systemctl restart bareos-fd`.


Credentials
'''''''''''

Prefer an entry of the ``clouds.yaml`` of the user running the |fd| (``cloud=<name>``) or a
``defaults_file``. The Plugin line of the FileSet is stored in the catalog and may appear in debug
output of the core; the plugin never logs secrets, but it cannot hide what the core does with the
line.

A ``defaults_file`` (path relative to the |fd| configuration directory) holds one ``key=value``
per line; the same keys as in the Plugin line are allowed:

.. code-block:: ini
   :caption: /etc/bareos/openstack-plugin.ini

   cloud=prod
   proxy_instance_id=2c3b1e0a-6d41-4b0e-9c57-0f1a5a1d9c11

Secrets (``password`` and ``application_credential_secret``) may be given base85 encoded with the
key suffix ``#enc``, for example ``password#enc=...``. This only avoids plain text, it is not
encryption.

Without ``clouds.yaml`` the plugin accepts ``auth_url`` plus either ``application_credential_id``
and ``application_credential_secret`` or ``username`` and ``password`` (with ``user_domain_name``,
``project_name`` or ``project_id``, ``project_domain_name``, ``region_name``, ``cacert``).


Configure jobs and filesets
'''''''''''''''''''''''''''

.. code-block:: bareosconfig
   :caption: bareos-dir.conf: OpenStack Plugin Job and FileSet definitions

   Job {
     Name = "openstack-web01"
     Client = "proxy-fd"
     FileSet = "openstack-web01"
     JobDefs = "DefaultJob"
     Accurate = yes
     Maximum Concurrent Jobs = 1
   }

   FileSet {
     Name = "openstack-web01"
     Description = "Backup of one OpenStack server"
     Include {
       Options {
         Signature = XXH128
       }
       Plugin = "python3"
                ":module_name=bareos-fd-openstack"
                ":defaults_file=openstack-plugin.ini"
                ":server_id=3fa85f64-5717-4562-b3fc-2c963f66afa6"
     }
   }

Exactly one of ``server_id`` and ``server_name`` selects the server. A FileSet backs up one server.

The chunk digests are stored in four fields of the file attributes. The plugin checks that the
Accurate options of the job compare at least one of them (modification, change or access time or
inode) and not only checksums; the default options of a FileSet with ``Accurate`` do. The plugin
clears the sparse flag of its chunks, so ``Sparse = yes`` in the Options is harmless, but nothing
may depend on it.


Backup
^^^^^^

.. code-block:: bconsole
   :caption: Backup of an OpenStack server

   *<input>run job=openstack-web01 level=Full yes</input>

A job does the following:

#. It checks the server and reads its definition.
#. For each volume it creates a snapshot, a temporary volume from it, attaches the temporary volume
   to the proxy and reads the device chunk by chunk.
#. Each chunk is one virtual file ``@OPENSTACK/<project>/<server>/volumes/<volume>/<index>.chunk``.
   A chunk that only contains zeros is sent without data. In Incremental and Differential jobs the
   core compares the stored digest and receives only chunks that changed.
#. After the data it sends the definition of the server as a restore object ``server.json``.
#. It detaches and deletes the temporary volumes and snapshots.

Changing ``chunk_size`` or ``hash`` needs a new Full backup; the job fails with a message
otherwise. A change of the server of a FileSet and a change of the root disk mode (see below)
also need a Full backup.

Consistency across the volumes
''''''''''''''''''''''''''''''

By default each volume gets its own snapshot, taken one after the other (``snapshot_mode=per-volume``).
Each snapshot is crash consistent; the snapshots of the volumes of one server are not taken at the
same moment.

With ``snapshot_mode=group`` the plugin takes one Cinder group snapshot. It does not create
groups: the volumes of the server must be exactly the members of one existing generic volume group
(no volume of another server), the group type must have ``consistent_group_snapshot_enabled`` set
and the block storage API must support microversion 3.14 or later. Otherwise the job fails with a
message that names the reason; it never falls back to ``per-volume``. The document records
``consistency = group-crash-consistent``. The result is crash consistent across the volumes of
the group, without application quiescing.

The group snapshot has no metadata. Its name is ``bareos-<jobid>-gsnap-<server id>`` and its
description holds the same keys as the other resources of a job; the plugin deletes it after the
last temporary volume of the job is gone.

Limits
''''''

* A server that boots from an image is refused before any change in the cloud; only servers that
  boot from a Cinder volume are supported.
* The local ephemeral and swap disks of the flavor are not backed up.
* A restore creates Cinder volumes and a server only; restoring to a Glance image is not available.
* The proxy must expose the Cinder volume ids as disk serials under ``/dev/disk/by-id``.
* Every backup reads every volume fully; an Incremental backup sends only the changed chunks.


Restore
^^^^^^^

Restore the files of the job (``select all``): the chunks and ``server.json`` belong together. The
restore selects the chain of Full, Differential and Incremental jobs as usual.

Restore to the cloud (default)
''''''''''''''''''''''''''''''

.. code-block:: bconsole
   :caption: Restore as a new server

   *<input>restore client=proxy-fd fileset=openstack-web01 select all done pluginoptions=python3:restore_name=web01-copy</input>

The plugin creates new volumes from the saved data, ports, and a new server with the saved flavor
(or ``restore_flavor``). Nothing existing is changed. Restored volumes get the name
``<name>-restore-<jobid>``; they, the ports and the server carry metadata with
``bareos-restored-from`` and ``bareos-restore-job-id``. A server is only created when every chunk of
every volume arrived.

Every port the restore creates is tagged in Neutron with ``bareos-restore-job-<jobid>`` and
``bareos-restored-from-<saved server id>``; ports that were reused are not tagged. If tagging fails
the job continues with a warning.

Restore options select a name, flavor, network, availability zone, volume type and the power
state (``restore_powerstate``: ``on``, ``off`` or ``previous``). By default new ports do not take
over the saved IP addresses (``restore_keep_addresses=no``), so the restored server does not
collide with a running original.

Replace an existing server (``force``)
''''''''''''''''''''''''''''''''''''''

With ``force=yes`` the restore replaces the server that still exists under the saved id. The old
server must be shut off. Nothing old is touched until every chunk has been written. The order is:

#. the new volumes are written and verified,
#. the new server is created and must become ACTIVE (a failure here deletes the new server and its
   new ports and leaves the old server untouched),
#. the old server is deleted, then its volumes (unless ``force_keep_old_volumes=yes``) and its
   unused ports.

With ``restore_keep_addresses=yes`` the old server and its ports are removed before the new ports
are created, because the addresses must be free first; that is the only case in which something old
disappears before the new server exists.

If a restore fails after it changed the cloud, the job log contains one line starting with
``recovery:`` that lists the new server, new ports, old server, old volumes, old ports and
restored volumes with their current state. Use it to clean up or to retry.

Restore to local files
''''''''''''''''''''''

.. code-block:: bconsole
   :caption: Restore the raw disks to a directory of the proxy

   *<input>restore client=proxy-fd fileset=openstack-web01 select all done where=/restore pluginoptions=python3:restore_target=local</input>

The plugin writes ``<dir>/<server_id>/<volume_id>.raw`` for every volume and ``server.json``. ``<dir>`` is ``restore_local_dir`` or, if empty, the
``where`` of the restore. Existing files are refused unless ``restore_overwrite=yes``; symbolic
links are never followed.

Restore to another project
''''''''''''''''''''''''''

.. code-block:: bconsole
   :caption: Restore a server into another project

   *<input>restore client=proxy-fd fileset=openstack-web01 select all done pluginoptions=python3:restore_project_cloud=project-b</input>

With ``restore_project_cloud=<entry of clouds.yaml>`` or ``restore_project_credentials=<file>`` the
restored server is created in another project. The volumes are still written through the proxy, in
the project of the plugin credentials, because only there they can be attached. They are then moved
to the target project with a Cinder volume transfer, and the ports and the server are created with
the credentials of the target project. The transfer key is never logged or stored.

* ``restore_project_credentials`` is a file in the format of ``defaults_file`` that holds the
  credential options of the target project (``auth_url`` with ``application_credential_id`` and
  ``application_credential_secret``, or ``username`` and ``password``, or ``cloud``); values in the
  ``#enc`` form are accepted. A relative path is relative to the |fd| configuration directory. The
  plugin warns when the file is readable by group or others. ``proxy_instance_id`` in that file is
  ignored. The values of the file are never logged.
* A file that only names ``cloud`` and ``project_name`` (and optionally ``project_domain_name`` or
  ``project_id``) uses that ``clouds.yaml`` entry with the given project instead of the project
  of the entry; ``project_id`` replaces a ``project_name`` of the entry.
* Before any volume is created the plugin checks in the target project the flavor, every network, the
  volume type and the availability zone. The saved security groups are looked up **by name** in the
  target project; a group that does not exist there fails the restore. For a disaster recovery
  where the groups of the source project are gone, ``restore_security_groups=<name>,<name>`` names
  the groups of the target project to use instead.
* If a transfer cannot be accepted, the plugin deletes it so that the volume does not stay
  ``awaiting-transfer``, keeps the volume in the source project and writes the ``recovery:`` line
  that says which volumes moved.
* If a transfer step fails or is cancelled, the plugin checks again through both connections in
  which project each restored volume is. The ``recovery:`` line then names the volumes
  ``moved to the target project``, the ones ``not moved`` (still in the source project), the
  ``pending transfers`` and the volumes with ``ownership unknown``. A volume is unknown when
  the check failed (for example because the cloud did not answer) or when both connections or
  neither of them can show it. Check such a
  volume with ``openstack volume show <id>`` using the credentials of each project and with
  ``openstack volume transfer request list``: a volume can already belong to the target project
  although the job log reports an error, because the plugin waits for the volume to become
  available after the transfer was accepted. Do not delete a volume of the recovery line before
  you know its project.
* ``force`` and ``restore_keep_addresses`` are refused, as are both ``restore_project_*`` options
  together, ``restore_target=local`` and a target project that is the project of the plugin
  credentials.


Validated environments
^^^^^^^^^^^^^^^^^^^^^^

Filled in after the real-lab validation.


The operator tool bareos-openstack-tool
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The package installs :command:`bareos-openstack-tool` to ``/usr/sbin``. It uses the same
credentials as the plugin and runs on the proxy; ``--config FILE`` reads a ``defaults_file`` (the
format and the ``#enc`` form of the plugin options).

.. code-block:: sh
   :caption: Examples of the operator tool

   root@proxy:~#<input>bareos-openstack-tool --config /etc/bareos/openstack-plugin.ini list-servers</input>
   root@proxy:~#<input>bareos-openstack-tool --config /etc/bareos/openstack-plugin.ini preflight --server-id <id></input>
   root@proxy:~#<input>bareos-openstack-tool --config /etc/bareos/openstack-plugin.ini leftovers</input>

list-servers
   Lists the servers of the project: id, name, status, boot source and number of volumes.

preflight (--server-id ID | --server-name NAME)
   Runs the read-only checks of a backup without taking snapshots: server status, project, boot
   type (a server that boots from an image fails), encrypted volumes and the existence of the proxy. One ``OK`` or
   ``FAIL`` line per check.

leftovers [--server-id ID]
   Lists the snapshots and temporary volumes with a backup role that a job left behind and
   the Bareos temporary volumes that are still attached to the proxy, with their job ids. Group
   snapshots of ``snapshot_mode=group`` are listed too. Other volumes attached to the proxy (its own
   disks) are not listed. Two kinds of resources are only reported and never deleted by the tool:
   pending Bareos volume transfers of a cross-project restore (a crash between creating and
   accepting a transfer leaves the volume ``awaiting-transfer``).

leftovers --cleanup --older-than-job N --yes [--detach]
   Deletes the resources of the previous command that belong to jobs with an id lower than ``N``.
   Job ids are global: no backup job may be running while this command runs, because the temporary
   volume of a running job is read by its |fd|. Temporary volumes that are attached to the proxy are
   kept and reported unless ``--detach`` is given, which detaches them first. Volumes with the
   role ``restore-volume`` are never deleted.

Exit codes: ``0`` all checks passed or the command did what was asked, ``1`` a check failed or
leftovers were found, ``2`` wrong usage or a cloud error. The read-only commands never change the
cloud.


Description of all Plugin Options
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Options are given in the Plugin line, in ``defaults_file`` and ``overrides_file`` (paths relative
to the |fd| configuration directory) or, at restore time, in ``pluginoptions``. The first value
found wins, so options given at restore time override the stored ones. Sizes accept ``k``, ``m``,
``g``, ``t`` and the binary forms ``ki``, ``mi``, ``gi``, ``ti`` with an optional ``b``; times
accept ``s``, ``min``, ``h`` and ``d``; booleans accept ``yes``/``no``, ``y``/``n``, ``on``/``off``,
``true``/``false`` and ``1``/``0``.

server_id, server_name (one of them mandatory on backup)
   The server to back up.

cloud (optional)
   An entry of ``clouds.yaml``. Without it ``auth_url`` and credentials are required.

auth_url, application_credential_id, application_credential_secret, username, password, user_domain_name, project_name, project_id, project_domain_name, region_name, cacert (optional)
   Credentials and scope when ``cloud`` is not used. ``password`` and
   ``application_credential_secret`` are never logged and accept the ``#enc`` form.

verify_ssl (optional)
   Verify the TLS certificates of the services. Defaults to ``yes``.

proxy_instance_id (mandatory for backup and for restore to the cloud)
   The id of the Nova server running the |fd|, or ``auto``. With ``auto`` the plugin reads the
   ``uuid`` of ``http://169.254.169.254/openstack/latest/meta_data.json`` (5 seconds timeout) and,
   if that fails, the file ``/var/lib/cloud/data/instance-id``. The id found must be a server of the
   project of the credentials, otherwise the job fails and names the sources that were tried. The
   source used is written to the job log.

chunk_size (optional)
   Size of one chunk: multiple of ``64KiB``, from ``1MiB`` to ``1GiB``. Changing it requires a Full
   backup. Defaults to ``64MiB``.

hash (optional)
   Hash algorithm of the chunk digest (an algorithm of Python ``hashlib`` without the ``shake``
   family). Only the first 24 bytes of the digest are stored. Changing it requires a Full backup.
   Defaults to ``sha256``.

queue_depth (optional)
   Maximum number of chunks held in memory (1 to 64). Defaults to ``4``.

poll_timeout (optional)
   How long to wait for a snapshot, volume, attachment or server to reach its state (at
   least ``10s``). Defaults to ``15min``.

snapshot_force (optional)
   Allow snapshots of volumes that are attached to a running server. Defaults to ``yes``.

cleanup_leftovers (optional)
   Delete the resources a previous job of this plugin left behind (see Troubleshooting). Defaults
   to ``no``.

snapshot_mode (optional)
   ``per-volume`` or ``group``. ``group`` takes one Cinder group snapshot of an existing volume
   group that holds exactly the volumes of the server (see Consistency across the volumes).
   Defaults to ``per-volume``.

restore_target (optional)
   ``cloud`` or ``local``. Defaults to ``cloud``.

restore_project_cloud, restore_project_credentials (optional, at most one)
   Restore into another project: an entry of ``clouds.yaml``, or a file in the format of
   ``defaults_file`` with the credentials of the target project (see Restore to another
   project). Not combined with ``force``, ``restore_keep_addresses`` or
   ``restore_target=local``.

restore_security_groups (optional)
   Comma separated names of security groups of the target project that replace the saved ones.
   Only with ``restore_project_cloud`` or ``restore_project_credentials``.

restore_local_dir (optional)
   Directory for ``restore_target=local``. Defaults to the ``where`` of the restore. ``/`` is
   refused.

restore_overwrite (optional)
   Overwrite existing raw files on a local restore. Defaults to ``no``.

restore_write_zeros (optional)
   Write all-zero chunks to the restored volume. Defaults to ``yes``; ``no`` leaves the new volume
   as the cloud created it.

restore_name, restore_flavor, restore_network, restore_az, restore_volume_type (optional)
   Name, flavor, network, availability zone and volume type of the restored server and volumes.
   Without them the saved values are used.

restore_powerstate (optional)
   ``on``, ``off`` or ``previous``. Defaults to ``previous``.

restore_keep_addresses (optional)
   Give the restored server the saved fixed addresses. Defaults to ``no``.

force (optional)
   Replace the existing server of the same id. Defaults to ``no``.

force_keep_old_volumes (optional)
   With ``force=yes``, keep the old volumes. Defaults to ``no``.

.. warning::

   Colons (**:**) need to be escaped with a backslash (**\\**) when passing them as plugin
   parameters. This is required because the colon is the delimiter between plugin parameters.


.. _OpenStackPluginLimitations:

Limitations
^^^^^^^^^^^

.. limitation:: OpenStack Plugin: not validated against a real cloud

   Outstanding real-cloud checks: boot and firmware of a restored server, the behaviour of the SDK
   in a Python plugin of the |fd|, and Python 3.9 as runtime. The restore to another project and
   ``snapshot_mode=group`` were tested against a simulated cloud only. Outstanding: Cinder volume
   transfers between real projects, and support for consistent group snapshots by the Cinder
   backends.

.. limitation:: OpenStack Plugin: crash consistency, and no application quiescing

   Snapshots are crash consistent. With the default ``snapshot_mode=per-volume`` the volumes of a
   server are not snapshotted at the same moment. ``snapshot_mode=group`` gives one crash
   consistent point across the volumes of an existing volume group; it needs a group that holds
   exactly the volumes of the server. Neither mode quiesces applications.

.. limitation:: OpenStack Plugin: no partial restore

   A restore of a server needs all its chunks. Single files inside a volume cannot be restored;
   restore a volume to a local raw file and mount it instead.

.. limitation:: OpenStack Plugin: restore within one cloud

   A restore writes into the cloud and the project of the plugin credentials, or, with
   ``restore_project_cloud`` or ``restore_project_credentials``, into another project of the same
   cloud (the volumes move with a Cinder transfer, so both projects must be on the same Cinder).
   Restoring on another cloud is not supported; restore to
   local files and import the raw disks there instead.

* The ephemeral disk and swap of the flavor are not backed up.
* Encrypted volumes are refused.
* Neutron resources other than the ports of the server (networks, subnets, security groups,
  floating IPs) are not backed up; the restore needs them to exist.
* VirtualFull and Always Incremental consolidation are not supported. The Director copies only file
  records into a consolidated job, so the server document (``server.json``, stored as a restore
  object) is not part of it. A restore that selects only consolidated jobs fails with the message
  ``no server metadata``. In a test, a restore that also selected the consolidated Incremental
  job, while that job still existed, received its document and restored correctly; this is not
  supported. Use Full, Incremental and Differential jobs without consolidation.
* With the gRPC bridge (``Plugin = "grpc:...:python3"``) a cancel takes effect only when the plugin
  call in progress returns, because the bridge runs all plugin calls on one thread. The delay is
  bounded by ``poll_timeout`` (default ``15min``) for waits for the cloud and by 60 seconds for
  single API calls; use a lower ``poll_timeout`` with the bridge. The cleanup then runs as in
  native mode.
* The job needs free quota for the snapshots and temporary volumes of the volume it is reading.


Troubleshooting
^^^^^^^^^^^^^^^

A backup job removes the snapshots and temporary volumes it created. If the job was
killed or a cloud call did not return in time, the job log names what could not be removed
(``resources left behind``). All of them carry the keys ``created-by=bareos``,
``bareos-job-id`` and ``bareos-role`` (Cinder metadata). The next job of the same server reports such leftovers at its start. With
``cleanup_leftovers=yes`` it deletes resources of earlier job ids that have a backup role and
belong to the same server; resources with the role ``restore-volume`` are never deleted.
The plugin cannot tell whether a resource of an earlier job id is still in use by a job that is
running: run at most one job per server at a time (``Maximum Concurrent Jobs = 1``) and do not
use ``cleanup_leftovers=yes`` while another job of the same server is running.

A volume that cannot be found as a device on the proxy fails the job instead of guessing: check
that the volume is attached and that ``/dev/disk/by-id`` shows its serial (see Requirements).

A cancelled job stops its reader and runs the same cleanup; if the reader does not stop within
three minutes the job ends with a warning and lists the temporary volume, which the next job can
remove with ``cleanup_leftovers=yes``.

The ``recovery:`` line of a failed restore is described in the section about ``force``. Debug
messages of the plugin start with ``openstack:``; set a debug level in the |fd| with
``setdebug level=100 client=proxy-fd trace=1``.
