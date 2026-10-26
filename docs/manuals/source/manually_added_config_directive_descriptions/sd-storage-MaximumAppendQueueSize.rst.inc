The maximum number of bytes of client data that one backup job may hold in the Storage Daemon while the data waits to be written to the volume. When a job holds this many bytes, the Storage Daemon stops reading from the File Daemon until the device has taken some of them, so the File Daemon slows down instead of the Storage Daemon filling its memory. The default value 0 sets no bound, only the limit of 500 queued messages applies.

Set this when a device writes slower than the clients send, for example a dplcompat device on a slow link, and many jobs run at the same time. The memory that one job needs for its queue is then about this value plus one message of at most :config:option:`sd/storage/MaximumNetworkBufferSize`\ .

A value below the size of one message does not stop the job: it holds one message at a time.
