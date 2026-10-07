.. _SdBackendDplcompat:

Dplcompat Storage Backend
-------------------------

.. index::
   single: Backend; Dplcompat
   single: Backend; Dplcompat; S3
   single: Backend; S3|see {Backend; Dplcompat}

The **bareos-storage-dplcompat** backend (:sinceVersion:`24.0.0: Dplcompat`) can
be used to access Object Storage through external wrapper programs.
For information on bundled wrappers, see :ref:`section-DplcompatWrappers`.
To learn how to implement your own wrapper, see :ref:`WritingDplcompatWrappers`.

Installation
~~~~~~~~~~~~

Install the package **bareos-storage-dplcompat**.

Configuration
~~~~~~~~~~~~~

The droplet backend requires a |dir| :ref:`DirectorResourceStorage`, a |sd|
:ref:`StorageResourceDevice` as well as a wrapper script that facilitates access
to the object storage.
This wrapper script may need additional configuration options, that are passed
using :config:option:`sd/device/DeviceOptions`.

Storage Daemon
^^^^^^^^^^^^^^
The following :config:option:`sd/device/DeviceOptions`\  settings configure
dplcompat itself:

chunksize
   Size of Volume Chunks in Bytes (default: 10 MB).

iothreads
   Number of IO-threads to use for uploads (if not set, blocking uploads are
   used)

ioslots
   Number of IO-slots per IO-thread (0-255, default 10). Set this to values
   greater than 1 for cached and to 0 for direct writing.

retries
   Number of upload tries of a chunk after which the device refuses further
   writes (0-255, default: 0 = no limit). The chunk is not discarded: it stays
   queued and is retried until it is uploaded. Jobs writing to the device in
   the meantime fail with a message naming the chunk. Once every queued chunk
   is uploaded, the device takes writes again.

flush_timeout
   Seconds without a successful chunk upload after which a job that waits for
   its uploads at its end fails with "Failed to flush device", and a job that
   waits for a free upload slot fails with "no upload progress" (default: 1800,
   minimum 1). It must be longer than the upload of one chunk takes. The
   chunks still queued are kept and retried. After a refused write the volume
   is marked Full and the device refuses writes until the chunks are uploaded;
   after a failed wait at the end of a job, the next append to the volume
   compares its size with the catalog. While a device holds chunks it cannot
   upload, every later job writing to it fails by its own deadline. A failed
   upload is tried again after 5 seconds, then after a pause that doubles with
   each try up to 60 seconds, but at most a quarter of ``flush_timeout``. A
   job canceled while it waits for its uploads ends within a few seconds; a
   job that only reads from the device does not wait for them.

transport
   How the device reaches the object storage. The only value is ``program``
   (default): every operation runs the wrapper program. Other values are
   refused.

program
   The wrapper program to use. Either an absolute path or the name of a program
   in :config:option:`sd/storage/ScriptsDirectory`.

program_timeout
   Timeout in seconds after which the wrapper program is presumed dead if it
   does not respond to I/O operations (default: 30). Output of the program
   counts as a response, so a ``list`` that keeps printing is not killed.


.. tip::
   Due to the nature of Dplcompat, it benefits from large chunksizes, because
   that reduces the number of wrapper processes the plugin spawns.


.. warning::
   The SD will allocate up to :math:`iothreads * ioslots * chunksize` bytes of
   memory for the device. With larger chunksize settings, this escalates pretty
   quickly. Make sure your have enough memory or reduce the number of ioslots.

.. note::
   Memory: each concurrent backup job on a dplcompat device holds up to the
   value of the Storage Daemon directive Maximum Append Queue Size (bytes;
   0 = no bound) of client data in the Storage Daemon, plus
   :math:`(iothreads * ioslots + 1) * chunksize` bytes of chunk buffers on the
   device.

.. note::
   With ``iothreads`` set to 0 each chunk is uploaded while the job writes. If
   that upload fails, the job fails with "could not upload chunk N of volume V"
   and the volume is marked Full. The chunk is kept in the storage daemon's
   memory and the device refuses writes: every job writing to it fails with
   "refuses writes", also one that wants to mount another volume. The kept
   chunk is tried again when a job that waits for its uploads at its end does
   so (within ``flush_timeout``, pauses as above), and once whenever the device
   is opened or closed. When it is uploaded, the device takes writes again. A
   job ends OK only after every kept chunk of the device is uploaded. At most
   two chunks are kept per device (a warning names more); they are lost only
   if the storage daemon stops, which it then logs. Canceling a job that
   retries kept chunks takes up to two times ``program_timeout`` plus one
   second when the backend hangs instead of failing. With ``Maximum
   Concurrent Jobs`` above 1 on the device, a job may fail and the device
   refuse writes while another job uploads the same chunk at its end; no data
   is lost. Use ``iothreads`` of 1 or more for concurrent jobs on one device.

.. note::
   A device that refuses writes, for any volume, also fails a job whose own
   chunks are all uploaded when it ends ("refuses writes"), since the device
   cannot tell whose data the refused chunks hold.

.. note::
   While a device refuses writes, each backup job that tries to open it marks
   the volume it was given in Error ("not on device", "Marking Volume ... in
   Error"), also a healthy volume. Restores read Error volumes as usual. Once
   the storage daemon logs that the device "takes writes again", compare the
   sizes of each such volume's objects in the bucket with the catalog's
   VolBytes (``s3cmd ls s3://<bucket>/<volume>/`` against
   ``list volume=<volume>``). If the bucket holds at least VolBytes:

   - a volume named in a job's "could not upload chunk N of volume V" or
     "refuses writes: chunk N of volume V" message held a kept chunk:
     ``update volume=<volume> volstatus=Used``, so nothing is appended to it;
   - a volume named only in "not on device" or "Marking Volume ... in Error"
     is healthy: ``update volume=<volume> volstatus=Append``.

   A volume whose bucket holds less than VolBytes stays in Error.

.. warning::
   Do not purge, truncate or relabel a dplcompat volume while chunks of it may
   still wait for upload in a storage daemon, that is after a job that wrote
   to it was canceled or ended with "Failed to flush device", "made no
   progress", "no upload progress", "could not upload chunk", "could not
   queue chunk" or "refuses writes". Wait until the sizes of the volume's
   objects in the bucket add up to at least the catalog's VolBytes
   (``s3cmd ls s3://<bucket>/<volume>/`` against ``list volume=<volume>``), or
   until every storage daemon that wrote to it has been restarted (its queue
   is then gone, with any data not yet uploaded). Only the device that holds
   the chunks refuses the truncate ("cannot be truncated before they are
   uploaded"); another device or another storage daemon does not know about
   them. When a backup job recycles such a volume, the refused truncate fails
   that job ("Truncate error") and marks the volume Error; once its chunks
   are uploaded, ``update volume=<volume> volstatus=Recycle`` lets it be
   recycled again. If a storage daemon stops while a failed job's chunks are
   still queued, that job's Full volume stays shorter than the catalog says.

.. note::
   Versions without the ``flush_timeout`` option behave differently: a backend
   outage keeps a job and its device waiting without limit, also a job that
   only reads; a failed upload is retried only every 300 seconds; ``retries``
   greater than zero discards a chunk after its last try; a write refused by
   the device, or a failed upload with ``iothreads`` set to 0, lets the job go
   on with another volume without that chunk's data; a ``list`` that keeps
   printing longer than ``program_timeout`` is killed; an empty ``list``
   output is an error; a failed ``list`` of the volume is read as a volume
   size of 0, so the volume is marked Error for a size mismatch instead of for
   the failed listing; and a volume can be truncated while its chunks wait for
   upload. Such a version refuses a device configured with ``flush_timeout``.

Example
'''''''
We want to configure a device **ObjectStore**:

* Upload up to four chunks of 256 MB in parallel.
* Limit to at most 2 chunks per upload-thread.
* Use the wrapper program ``acme-wrapper``.
* Pass options ``fruit`` with value ``banana`` and ``vegetable`` with value
  ``carrot`` to the wrapper program.

.. code-block:: bareosconfig
   :caption: bareos-sd.d/device/ObjectStore.conf

   Device {
     Name = ObjectStore
     Media Type = Objects
     Archive Device = Object Storage # currently unused, but required
     Device Options = "iothreads=4"
                      ",ioslots=2"
                      ",chunksize=262144000"
                      ",program=acme-wrapper"
                      ",fruit=banana"
                      ",vegetable=carrot"
     Device Type = dplcompat
     LabelMedia = yes
     Random Access = yes
     AutomaticMount = yes
     RemovableMedia = no
     AlwaysOpen = no
   }

.. note::
   For improved readability the :config:option:`sd/device/DeviceOptions` was
   split into multiple lines.
   This is still a single value containing a comma-separated list of key-value
   pairs.

Director
^^^^^^^^
To connect the director to the storage, a :ref:`DirectorResourceStorage` must be
configured.

Example
'''''''
Based on the example above, a matching :ref:`DirectorResourceStorage` would be

.. code-block:: bareosconfig
   :caption: bareos-dir.d/storage/ObjectStore.conf

   Storage {
     Name = ObjectStore
     Media Type = Objects
     Address  = "bareos-sd.example.com"
     Password = "secret"
     Device = ObjectStore
   }

.. _section-DplcompatWrappers:

Bundled Wrapper Programs
~~~~~~~~~~~~~~~~~~~~~~~~
The scripts in this section are bundled with Dplcompat.

s3cmd-wrapper.sh
^^^^^^^^^^^^^^^^
This program wraps ``s3cmd``. It connects Dplcompat to S3 compatible storages.

Prerequisites
'''''''''''''
This wrapper program requires a properly configured ``s3cmd``.

You can install ``s3cmd`` with your system's package manager or using ``pip``
with or without a ``venv``.

Configuring ``s3cmd`` can be done interactively using ``s3cmd --configure``.
The resulting configuration file (usually ``$HOME/.s3cfg``) should be copied
into the Bareos configuration directory and its owner should be set to the user
that runs the |sd|.

When running ``s3cmd`` as the user that also runs the |sd| and providing the
configuration file, you should be able to access your S3 bucket.
If the command ``s3cmd --config /etc/bareos/s3cmd.cfg info s3://your-bucket``
returns successfully, the configuration should be fine.
The bucket must allow read/write access to the access key or token that is used,
of course.

Configuration
'''''''''''''
s3cmd_prog
   Configure the full path to the s3cmd program to use (default: autodetected)

s3cfg
   Full path to the s3cfg file to read the configuration from (required).

bucket
   Name of the S3 bucket to use (default: "backup")

base_url 
   Override the base URL used to generate the object URLs from. When this is
   set, the bucket option is ignored. (no default)

storage_class
   Defines the storage class of newly uploaded objects. When this is not set,
   the bucket's policy applies.

Example
'''''''
The following example configures a :ref:`StorageResourceDevice` as follows:

* use ``s3cmd-wrapper.sh``
* read ``s3cmd`` configuration from ``/etc/bareos/s3cmd.cfg``
* use the bucket named "bareos"
* set the storage class to "ONEZONE_IA"

.. code-block:: bareosconfig
   :caption: bareos-sd.d/device/S3Cmd-Example.conf

   Device {
     Name = S3Cmd-Example
     Media Type = S3
     Archive Device = Object Storage # currently unused, but required
     Device Options = "iothreads=4"
                      ",ioslots=2"
                      ",chunksize=262144000"
                      ",program=s3cmd-wrapper.sh"
                      ",s3cfg=/etc/bareos/s3cmd.cfg"
                      ",bucket=bareos"
                      ",storage_class=ONEZONE_IA"
     Device Type = dplcompat
     LabelMedia = yes
     Random Access = yes
     AutomaticMount = yes
     RemovableMedia = no
     AlwaysOpen = no
   }
