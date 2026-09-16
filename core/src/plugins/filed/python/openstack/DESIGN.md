# Bareos File Daemon Plugin: OpenStack (agentless)

Status: Design — ready for implementation
Author: (nmc.vanson.pt@gmail.com)

See [`ROADMAP.md`](ROADMAP.md) for how this document relates to the other
four in this directory, the full cross-track milestone timeline, and the
pull-request plan for landing all of it in `bareos/core`.

## Summary

Add a new Bareos FD Python plugin, `bareos-fd-openstack`, that backs up
OpenStack Nova instances and their Cinder volumes agentlessly — no Bareos
component runs inside the guest OS. The plugin talks only to the OpenStack
control plane (Keystone/Nova/Cinder/Glance), snapshots the instance, pulls
the resulting bytes into Bareos-managed storage, and can recreate the
instance on restore.

Key design points:
- The plugin runs on a **proxy host that is itself a Nova instance**, able
  to attach Cinder volumes to itself — not an arbitrary external host with
  just API access (§3.5, §4).
- Backup reads volume data by attaching a temp volume (cloned from a Cinder
  snapshot) directly to the proxy and streaming the raw block device — no
  Glance round-trip for Cinder-backed disks (§5).
- Restore, by default, **creates a new instance with new volumes,
  non-destructively** — this needs no opt-in flag, since it can never
  delete or replace anything. Replacing an existing instance
  (`force=yes`) and stopping at a Glance image only (`restoretoimage=yes`)
  are both separate, explicit opt-ins (§6).
- Incrementals (v2) use backend-agnostic content-hashed chunking, the same
  primitive the Incus plugin in this tree already implements, rather than
  a storage-backend-specific diff mechanism (§11).
- Target platform floor: OpenStack 2024.1 (Caracal) and later.
- Fleet-scale operator tooling (proxy-pool coordinator, multi-tenant
  onboarding, garbage collection, billing) ships as **companion tooling**
  in this same directory — `TEAMING.md`/`TEAMING_PLAN.md` — built on top
  of this plugin's options and behavior as a stable contract (§10), rather
  than mixed into the plugin's own code. This document (`DESIGN.md`)
  covers just the plugin itself: `bareos-fd-openstack.py`, a
  self-contained contribution matching the shape of the existing
  Proxmox/Incus/VMware/Libcloud plugins in this tree.
- Deploying more than one proxy — teaming pools, project-scoped teams,
  region-scoped teams, and their HA/performance tradeoffs — is covered
  separately in `PROXY_TOPOLOGY.md` (§10.3).

## 1. Goal

Add a new Bareos FD Python plugin, `bareos-fd-openstack`, that backs up
OpenStack resources **agentlessly** — i.e. without a Bareos component running
inside the guest OS of any Nova instance. Conceptually this is Bareos's
equivalent of Commvault's OpenStack integration, Bacula Enterprise's
OpenStack Plugin, or TrilioVault: talk only to the OpenStack control plane
(Keystone/Nova/Cinder/Glance/Neutron REST APIs), snapshot resources, pull the
resulting bytes into Bareos-managed storage, and be able to recreate the
resource on restore.

In scope:
- Nova **instances** (compute VMs) — full disk + metadata backup/restore.
- Cinder **volumes** attached to those instances (boot-from-volume and
  data volumes).

Explicitly out of scope for this plugin:
- Application-consistent quiescing (requires in-guest cooperation).
- Byte-perfect incrementals on first release (v2, see §11).
- Neutron topology backup beyond "enough metadata to recreate the ports".
- Swift object storage — already coverable by the existing
  `bareos-fd-libcloud` plugin (Apache Libcloud ships an `OPENSTACK_SWIFT`
  storage driver); extending that plugin's Keystone auth support is a much
  smaller, separate task (§10.4) rather than reinventing object storage
  backup here.
- Glance image catalog backup (images are just Glance objects; could reuse
  libcloud-style chunked download if ever needed, not core to "protect
  running workloads").
- Acting as a Cinder backup **driver** installed on OpenStack controller
  nodes (the architecture Bacula Enterprise calls its "openstack-vm"/Cinder
  plugin, see §3.2) — much higher integration cost than an FD plugin, not a
  goal here.
- **Fleet-scale operator tooling** (proxy-pool coordinator, multi-tenant
  onboarding, GC, billing) — ships as companion tooling alongside this
  plugin; see `TEAMING.md` in this directory, not this file.

## 2. Prior art already in this tree

The existing agentless/hypervisor-adjacent FD plugins in this repo set the
conventions this design reuses rather than inventing new ones:

| Plugin | File | Pattern it demonstrates |
|---|---|---|
| Proxmox | `core/src/plugins/filed/python/proxmox/bareos-fd-proxmox.py` | Simplest agentless-VM model: one guest per Job, shell out to a platform-native dump tool (`vzdump`), stream its stdout via a pipe (`LogPipe`), one virtual file per job, `restoretodisk` fallback mode, `force` to overwrite on restore. **Runs co-located on the hypervisor** (needs local CLI + root). |
| Incus | `core/src/plugins/filed/python/incus/bareos-fd-incus.py` | Same one-instance-per-job idea, but the payload is a **stream of many chunked files**, produced by a background thread and consumed through a bounded `queue.Queue`, with `start_backup_file()` called repeatedly (`bRC_OK` ... `bRC_OK` ... `bRC_Stop`) to enumerate them. It **fixed-size-chunks** large disk images (`chunk_size`, default 64MiB), computes a content **hash per chunk** (`hash` option, default sha256), and "hijacks" spare stat-packet fields (`hijacked_stat_fields`: `st_ino`/`st_atime`/`st_mtime`/`st_ctime`) to carry that hash — plus tracks all-zero chunks separately (`zero_digest`) to avoid storing sparse regions. This is a ready-made, in-tree template for backend-agnostic block-level incrementals (see §11). Also **runs co-located**, shells out to the `incus` CLI. |
| VMware | `core/src/plugins/filed/python/vmware/bareos-fd-vmware.py` | The closest analogue to what we need: talks to a **remote control-plane API** (vCenter), not a local CLI — the FD does **not** need to run on the hypervisor. Snapshots the VM, downloads disk data via VDDK, and serializes VM config (flavor/network/disks) as JSON, stored as a **restore object** (`FT_RESTORE_FIRST`, via `create_restoreobject_savepacket`/`restore_object_data`) so a full VM can be *recreated* (`create_vm()`) on restore, not just have its disk bytes replayed. Supports CBT-based incrementals. |
| Libcloud | `core/src/plugins/filed/python/libcloud/` | Generic multi-cloud **object storage** backup via Apache Libcloud drivers (`bucket_explorer.py`, `streamer.py`, `worker.py` show the chunked-parallel-download pattern). Its driver factory (`get_libcloud_driver.py`) only forwards `secret/key/host/port/secure/timeout` today — no Keystone/OpenStack auth params — which is why it can't currently be pointed at OpenStack Swift, even though Libcloud itself supports it. |
| `BareosFdPluginBaseclass` | `core/src/plugins/filed/python/pyfiles/BareosFdPluginBaseclass.py` | The hook contract: `parse_plugin_definition`, `start_backup_job`, `start_backup_file` (called repeatedly to enumerate objects; return `bRC_Stop` when done), `plugin_io_open/read/write/close`, `end_backup_file`, `restore_object_data`, `create_file`. Also provides the `key#enc=<base85>` option-value transform used to avoid plaintext secrets in `Plugin =` lines. |

Packaging/docs/test conventions this plan follows:
- `debian/control.<name>` + `debian/bareos-filedaemon-<name>-python-plugin.install.in`
  (e.g. `control.proxmox`, `bareos-filedaemon-proxmox-python-plugin.install.in`).
- `core/platforms/packaging/bareos.spec`: one `%package filedaemon-<name>-python-plugin`
  block (`Requires: bareos-filedaemon`, `bareos-filedaemon-python-plugin`).
- Docs: `docs/manuals/source/TasksAndConcepts/Plugins/FileDaemonPlugins/<Name>Plugin.rst.inc`,
  linked from `FileDaemonPlugins.rst.inc`.
- System tests: `systemtests/tests/py3plug-fd-<name>/` with `etc/bareos/{bareos-dir,bareos-fd,bareos-sd}.d`,
  a `CMakeLists.txt`, and a `testrunner-*` script.
- `docs/adr/0005-plugin-virtual-filenames.md`: virtual restore-tree filenames
  should be prefixed `@<PLUGIN-NAME>/`.

## 3. How other backup products integrate with OpenStack

Before finalizing the design, I looked at how Commvault, Bacula Enterprise
(the closest analogue — same codebase lineage as Bareos), TrilioVault, and
Veeam approach this, to build on validated architecture rather than guess.

### 3.1 Commvault

Commvault backs up OpenStack by creating a **Nova snapshot for the instance
and a Cinder snapshot for each attached volume**; both are explicitly
documented as **crash-consistent** (no in-guest quiescing). After the first
full, it uses **CRC comparison to identify changed data for incrementals**.
Notably, **Glance image backups are always full** — Commvault's own docs
state incrementals don't apply to images, only to instance/volume data. A
"hypervisor" in Commvault maps to a Keystone endpoint, and volume attachment
to the access node (their term for a backup proxy) is constrained by the
guest's disk controller type (virtio vs SCSI), which is the same category of
constraint this plugin's proxy-attach design (§5) hits.
[Backup Process for OpenStack Images and Instances](https://documentation.commvault.com/v11/commcell-console/backup_process_for_openstack_images_and_instances.html),
[Cloud Feature Support for OpenStack](https://documentation.commvault.com/11.20/cloud_feature_support_for_openstack.html)

### 3.2 Bacula Enterprise — the most directly relevant prior art

Bacula Enterprise (closed-source, built on the same lineage as Bareos) ships
**two architecturally different** OpenStack integrations, which is itself
useful signal about how much scope a single plugin should carry:

**(a) The "OpenStack Plugin" (proxy-VM method)** — an FD plugin, exactly
the shape this design follows:
- Runs on a **proxy server**: Linux host with the Bacula File Daemon + this
  plugin, network access to the OpenStack REST APIs, and enough OpenStack
  permission to **attach/detach volumes to itself**. Documented minimum
  sizing: 2 vCPU / 4 GB RAM per running job, +~500 MB RAM and +1 vCPU per
  2-3 additional concurrent jobs, plus a small (~500 KB/job) working-file
  area for block-map/checksum files.
- **Backup steps** (confirms the flow this plugin follows):
  1. Export Nova server metadata/configuration (for restore).
  2. For each attached volume, snapshot it, create a temp volume from the
     snapshot, and **attach that temp volume to the proxy server itself**.
  3. Read the raw block device from the proxy and stream it to Bacula
     storage — no Glance round-trip for volume data.
  4. Detach + delete the temp volume and snapshot.
- **Virtual filename layout** (confirms the `@PLUGIN-NAME/` ADR convention
  independently): `@Openstack/nova/server/<uuid>_<name>.name`,
  `<uuid>/<uuid>.conf` (metadata), `<uuid>/<volUuid>.bmp` +
  `<uuid>/<volUuid>.bmpsha` (block map + per-block checksum file — see
  §11), `<uuid>/<volName>_<volUuid>.bvmdk` (raw data), plus an
  `OpenstackServerProjectMap.proj` file for cross-project bookkeeping.
- **Incrementals**: implemented via those `.bmp`/`.sha` working files —
  Bacula recomputes block checksums on each run and only transfers blocks
  whose hash changed since the file recorded in the previous job; deleting
  those working files forces a full re-transfer. This is a
  **storage-backend-agnostic** scheme (works identically on LVM, Ceph,
  NFS-backed Cinder — it operates purely on bytes read from the attached
  device), unlike a Ceph-RBD-diff approach. It maps almost exactly onto the
  Incus plugin's existing chunk+hash mechanism already in this tree (§2,
  §11).
- **Plugin option syntax observed**: `openstack: proxy_server=<uuid>
  server=<instance-name> verbose=1 glance_image_backup=1 nova_server_backup=0`
  — independent toggles for "back up the Nova server" vs "back up its
  Glance image", a useful precedent for this plugin's own option design
  (§10).
- **Restore**: create volume(s) from the backed-up data, attach to the
  proxy, write the data back, detach, and — for cross-project restores —
  attach to the proxy first and then transfer/re-own the volume into the
  target project, then reconstruct the Nova server with the saved flavor
  and volume attachments.
- Two Keystone auth modes supported: password (`OS_*` vars / RC file) or
  Application Credential (id + secret) — matches §4 of this plan.

  [Backup and Restore Strategies](https://docs.baculasystems.com/BEDedicatedBackupSolutions/Virtualization/Hypervisors/openstack/BackupAndRestoreStrategies/index.html),
  [Backup Operations](https://docs.baculasystems.com/BEDedicatedBackupSolutions/Virtualization/Hypervisors/openstack/Operations/Backup/index.html),
  [OpenStack backup solution overview](https://www.baculasystems.com/openstack-backup-solution/)

**(b) The "openstack-vm" / Cinder-driver plugin** — an entirely different,
heavier architecture: a custom driver
(`/opt/stack/cinder/cinder/backup/drivers/bacula.py`) installed **on the
OpenStack controller itself**, running as the `cinder-backup` service
account, so that Bacula becomes a native target for `cinder backup-create`
the same way Swift/Ceph/NFS are today. This requires deploying into
OpenStack's own service host, not just talking to its API.
[Installation](https://docs.baculasystems.com/BEDedicatedBackupSolutions/Virtualization/Hypervisors/openstack-vm/Installation/index.html)

This plugin follows (a), the proxy-VM FD-plugin architecture — it needs
zero changes to the target OpenStack deployment, matches how every other
plugin in this tree is packaged/operated, and is the architecture Bacula
Enterprise itself leads with in its marketing. (b) is noted in §1 as
explicitly out of scope; revisit only if a user specifically wants Bareos
as a first-class Cinder backup backend.

### 3.3 TrilioVault

Implemented as a native OpenStack service (`workloadmgr`) rather than a
backup-tool plugin — instances are grouped into a **"workload"** (instances
+ their interconnects + volumes + metadata), snapshotted together, stored to
an **NFS or S3** backend. This "protect a group of related instances as one
unit" idea is worth keeping in mind for fleet-management tooling built
around this plugin (`TEAMING.md`) even though Trilio's
implementation (a whole new OpenStack service) is out of reach for a Bareos
plugin.
[TrilioVault data protection (OpenStack charm-guide)](https://docs.openstack.org/charm-guide/latest/admin/trilio.html)

### 3.4 Veeam

No dedicated OpenStack instance-backup capability exists today: Veeam Kasten
can snapshot Cinder-backed Kubernetes persistent volumes, but that's
Kubernetes-on-OpenStack, not general Nova instance backup — a real gap in
the ecosystem for a community/open-source tool to fill.
[Why Veeam Doesn't Work for OpenStack (And What Does)](https://openmetal.io/resources/blog/why-veeam-doesnt-work-for-openstack-and-what-does/)

### 3.5 Design consequences

The Bacula Enterprise research shapes two specific parts of this design:

1. **Proxy deployment model (§5).** VMware's VDDK lets a proxy be *any*
   host with API network access. That doesn't hold for Cinder: reading a
   volume's raw bytes efficiently requires **attaching it to something**,
   and the only broadly-supported way to do that is to attach it to a Nova
   instance — so, like Bacula Enterprise, **the proxy itself must be a
   Nova instance** inside (or with volume-attach rights into) the target
   project/cloud, not an arbitrary external host. This has security
   implications (§4) since cross-project attach typically needs more than
   a single-project-scoped application credential.
2. **Incremental strategy (§11).** Both Commvault (CRC) and Bacula
   Enterprise (block-map + per-block checksum) implement incrementals as
   *backend-agnostic content hashing of fixed-size blocks*, not
   storage-backend diff APIs. The Incus plugin in this same repo already
   implements exactly this primitive (chunking + per-chunk hash hijacked
   into stat fields, letting Bareos's own Accurate mode do the
   skip-unchanged-chunk work) — that's the v2 approach here, with a Ceph
   RBD-diff as a possible further optimization rather than the primary
   mechanism.

## 4. Authentication

Use `keystoneauth1`/`openstacksdk` (the standard OpenStack Python SDK), not
hand-rolled REST calls — it already handles token refresh, catalog lookup,
retries, and the two auth styles operators expect (also the two styles
Bacula Enterprise's plugin supports, §3.2):

- **Application credentials** (recommended): `application_credential_id` +
  `application_credential_secret`. Scoped to one project, independently
  revocable, no password rotation coupling.
- `clouds.yaml` reference via a `cloud=<name>` option (lets operators keep
  secrets out of Bareos config entirely, using `os-client-config` discovery).
- Fallback: explicit `auth_url`/`username`/`password`/`project_name`/`user_domain_name`/`project_domain_name`.

All secret-bearing options support the existing `#enc` base85 transform
from `BareosFdPluginBaseclass._add_options()` so `application_credential_secret#enc=...`
works exactly like other plugins' obfuscated options. Never pass secrets to
`bareosfd.JobMessage()` (which lands in the always-visible job log) — only to
`bareosfd.DebugMessage()`, matching what Proxmox/Incus already do for their
option dumps.

Required IAM shape: a dedicated backup role/project (or a role assignment
scoped per-project) with rights to list/create/delete Nova image snapshots
and Cinder volume snapshots, **plus volume attach/detach onto the proxy
instance**. If one proxy needs to back up instances across *multiple*
projects, volume attach typically requires a role that spans those
projects (or per-project proxy instances) — a tradeoff between "one proxy
+ one credential per project" and "one shared proxy with a broader
cross-project role." A multi-tenant/cloud-provider deployment sharing
proxies across many tenants can resolve this more precisely using Keystone
application-credential `access_rules` (fine-grained method+path+service
restriction); that design lives in `TEAMING.md`, since it's a
fleet-operations concern built on top of this plugin's per-job
credential options (§10), not a change to the plugin itself.

## 5. Backup flow (Full, instance-level)

One Bareos Job = one Nova instance, identified by **instance UUID**
(immutable; `name` is just an override convenience), matching the
`guestid`/`vmname` convention from Proxmox/VMware, and Bacula Enterprise's
`server=` option.

`start_backup_job()`:
1. Authenticate via `openstacksdk` (`openstack.connection.Connection`).
2. Resolve the instance; fail fast (`M_FATAL`) if not found or not owned by
   the configured project.
3. Collect instance metadata: flavor (or flavor snapshot, since flavors are
   mutable/deletable), image ref, attached volumes + device mapping
   (`os-volume_attachments`), network ports (fixed IPs, floating IPs,
   security groups, port IDs — not just "the network name", since restore
   needs enough to recreate ports), key name, availability zone, server
   metadata/tags, boot mode (boot-from-image vs boot-from-volume).
4. Best-effort quiesce: if `qemu-guest-agent` is reachable (out of band —
   Nova's API has no standard fsfreeze call), optionally shell out to a
   configured pre/post hook (`pre_snapshot_cmd`/`post_snapshot_cmd`, run
   *on the proxy host*, e.g. an Ansible/SSH wrapper) before/after the
   snapshot step. Without this, snapshots are **crash-consistent only** —
   every product surveyed in §3 (Commvault, Bacula Enterprise) makes the
   same disclosed tradeoff; it's an industry-standard limitation of
   staying agentless.
5. **Two ingestion paths, chosen per-disk**:
   - **Volume path (preferred, used for every Cinder-backed disk —
     boot-from-volume root disks and data volumes)**: create a Cinder
     snapshot of the volume (`force=True`, no in-guest quiesce available),
     create a new volume from that snapshot, **attach the temp volume to
     the proxy instance itself** (the FD host must therefore *be* a Nova
     instance — see §4), wait for the device to appear, then read the raw
     block device directly. This path also enables the v2 chunk-hash
     incremental scheme (§11) and avoids an unnecessary Glance round-trip,
     matching Bacula Enterprise's documented flow.
   - **Image path (fallback, used only for ephemeral/local disks that
     aren't Cinder volumes)**: Nova `create_image()` (instance snapshot →
     new Glance image), then stream via `image_client.download(image_id,
     stream=True)`. Stays **full-only**, matching Commvault's identical
     restriction for Glance-image backups.
6. Poll for `available`/`ACTIVE` status with a timeout (reuse the
   poll-with-backoff shape from `bareos-fd-proxmox.py`'s
   `_wait_for_io_process`).

`start_backup_file()` (called repeatedly — same enumeration pattern as
Incus/VMware, since one instance now has N disks):
1. First call: emit the instance-metadata JSON as a restore object
   (`FT_RESTORE_FIRST`, `savepkt.object = json...`), exactly like VMware's
   `create_restoreobject_savepacket` / `vm_config.json`.
2. Subsequent calls: one `FT_REG` virtual file per disk, named
   `@OPENSTACK/<instance-name>/<instance-uuid>/disk-<device>.raw` (per the
   `@PLUGIN-NAME/` convention from `docs/adr/0005-plugin-virtual-filenames.md`
   — deliberately similar in spirit to Bacula Enterprise's own
   `@Openstack/nova/server/<uuid>/...` layout, §3.2).
3. Return `bRC_Stop` once all disks are enumerated.

`plugin_io_open/read`: for the volume path, read directly from the attached
block device file descriptor (closest to what Proxmox does with
`io_process.stdout.fileno()`, just a device node instead of a pipe). For the
image path, open a streaming HTTP GET against Glance, fed through a
background thread into a bounded queue/pipe — reusing Incus's
producer/consumer chunking approach. **This path never buffers disk bytes
to the proxy's local disk** — it streams directly from the block device or
the Glance HTTP response to the FD↔SD pipe, the same way Proxmox streams
`Popen.stdout` directly rather than writing a local temp file. This isn't
just a performance choice: a shared-proxy multi-tenant deployment (see
`TEAMING.md`) depends on this plugin never leaving another
tenant's disk bytes sitting on local disk between jobs.

`end_backup_file()` / `end_backup_job()`: detach + delete the temp volume
and Cinder snapshot (volume path) or delete the temporary Glance image
(image path) unless `keep_snapshot=yes` is set. Wrap in `try/finally` so a
failed transfer doesn't orphan billable storage. Tag every object this
plugin creates (Glance image, Cinder snapshot, temp volume) with
`metadata={"created-by": "bareos", "bareos-job-id": ..., "bareos-instance-uuid": ...}`
— this plugin guarantees that tagging is always applied, even on partial
failure; anything that consumes it for garbage collection (`TEAMING.md`'s
GC tooling) is out of scope here but
depends on this plugin holding up its end.

## 6. Restore flow

`restore_object_data()`: capture the instance-metadata JSON (mirrors
VMware's `restore_object_data`).

Restore separates two independent questions: **does it create a real,
running instance**, and **does it touch/replace an existing instance**.
These are different operations with different risk profiles, so only the
second is gated behind an explicit opt-in:

- **Default: create a new instance, never touch an existing one.** Create
  Cinder volume(s) directly from the recovered raw data (attach to the
  proxy, write bytes, detach — the exact reverse of the backup volume path
  in §5, no Glance step needed for Cinder-backed disks), then
  `connection.compute.create_server()` using the saved instance JSON
  (flavor, networks/ports, security groups, key_name, AZ), attaching
  volumes in their original device order. This runs **unconditionally**
  — no `force` or other opt-in needed — because it cannot destroy
  anything: it only ever adds a new instance + new volumes, regardless of
  whether an instance from the original backup still exists. Nova doesn't
  require unique instance names within a project (only UUIDs, which it
  always assigns fresh on create), so there is no technical collision
  even if the original instance is still running under its original name
  — but for operator clarity, the new instance defaults to
  `<original-name>-restore-<jobid>` rather than reusing the original name,
  unless `instance_name` is explicitly given. Overriding `instance_name`,
  `project`/`target_project`, and network mapping at restore time is
  supported, matching how VMware/Proxmox/Bacula Enterprise all let you
  retarget name/folder/project on restore. For **cross-project restore**,
  follow Bacula Enterprise's documented pattern: attach the rebuilt volume
  to the proxy first, then transfer/re-own it into the target project
  before creating the server there.
- **Opt-in: `force=yes` — additionally replace an existing instance.** If
  an instance matching the original backed-up instance (by UUID, or by
  `instance_name` if that's been set to match an existing instance
  deliberately) is found in the target project, `force=yes` makes restore
  detach and delete that old instance's volumes and delete the old
  instance itself *before* creating the new one — see §6.1 for exactly
  what happens to the old volumes. Requires the existing instance to be
  `SHUTOFF` first (same safety precondition Proxmox enforces before
  overwrite). Without `force=yes`, an existing instance with a matching
  identity is simply left alone and restore proceeds with the
  non-destructive default above — restore never silently overwrites.
- **Opt-in fallback: `restoretoimage=yes` — image only, create nothing.**
  Upload the restored raw bytes back into Glance as a new image per disk
  and stop there — no Cinder volume, no Nova instance, same spirit as
  Proxmox's `restoretodisk=yes`. Useful for cautious/manual recovery,
  offline inspection of recovered data, or environments where automated
  Nova/Cinder object creation isn't wanted at all.

Regardless of mode, the safety preconditions of the destructive path
(`force=yes`'s `SHUTOFF` check, old-volume handling §6.1, clear job-log
messaging about what's about to be created/replaced/deleted) are core
work, not an afterthought — `force=yes` is the plugin's only path that can
destroy an existing resource.

### 6.1 What happens to the old instance's volumes on `force=yes`

The `force=yes` overwrite path defines what happens to the **replaced
instance's own volumes**, not just that it must be `SHUTOFF` — otherwise
this is exactly the kind of orphan/leak this plugin's cleanup discipline
(§5) is meant to avoid, just unapplied to this specific path:

- On `force=yes` overwrite, before deleting the old instance: enumerate its
  attached volumes (the same `os-volume_attachments` call used at backup
  time, §5) and **detach** them from the old instance as a distinct,
  logged step — never delete the old instance while it still holds
  attachments.
- Default behavior after detach: **delete** the old (now-detached) volumes,
  since `force=yes` is already an explicit "replace this" instruction and
  the restore is about to attach freshly-recreated volumes with the
  recovered data — keeping the old ones around by default would silently
  double-provision storage on every forced restore.
- Provide an explicit opt-out, `force_keep_old_volumes=yes`, for operators
  who want the pre-overwrite volumes retained (e.g. as a manual rollback
  point) instead of deleted — off by default, but the choice is logged
  clearly either way (`JobMessage` stating exactly which volume UUIDs were
  detached and deleted/kept).
- This detach-then-delete-or-keep sequence is tagged with the same
  `created-by=bareos`/job-id metadata convention as everything else this
  plugin creates (§5), so a restore that fails partway through overwrite
  doesn't produce untracked orphans either.
- This exact scenario — force-overwrite with old volumes present — is
  part of the automated test matrix (§12), since it's the plugin's only
  data-destroying code path.

## 7. Proxy sizing guidance

Bacula Enterprise publishes concrete proxy sizing numbers (§3.2) that are a
reasonable starting baseline, since the workload shape (attach a volume,
stream raw bytes, hash chunks) is the same:
- ~2 vCPU / 4 GB RAM per concurrently running backup job.
- +~500 MB RAM and +1 vCPU per additional 2-3 concurrent jobs.
- A small per-job working-directory footprint for the chunk/hash metadata
  used by the v2 incremental scheme (§11) — Bacula's own block-map files
  are on the order of a few hundred KB per job; ours should be similar
  since it's the same "one hash per fixed-size block" idea.

These numbers should be validated during implementation (§14, M1/M2), not
taken as guaranteed, but they're a sane starting point rather than guessing
from zero. (Sizing many proxies as a *fleet* — pool topology, per-proxy
concurrency ceilings at scale — is fleet-tooling scope; see `TEAMING.md`,
which builds directly on the single-proxy numbers here.)

### 7.1 Risk: snapshot-to-volume copy cost on non-COW backends

The volume-path ingestion mechanics (§5) have a **cost** worth stating
explicitly, since it qualifies the "backend-agnostic" framing:

- "Create volume from snapshot," the step that produces the temp volume
  the proxy attaches (§5), is **not uniformly cheap across Cinder
  backends**. On copy-on-write-capable backends (Ceph RBD clone, LVM thin,
  ZFS, several array vendors' native clone support) it's effectively a
  metadata operation — near-instant regardless of volume size. On backends
  without COW clone support — which includes a meaningful share of
  generic/older SAN drivers — Cinder falls back to a **full data copy at
  the storage layer**, proportional to volume size, which can take as long
  as the backup transfer itself and **temporarily doubles provisioned
  capacity** for that volume.
- Environments using both Ceph and SAN as Cinder backends will hit the
  non-COW, full-copy path for some fraction of instances from day one —
  this is a first-order cost/scheduling input, not an edge case.
- This cost applies on **every job, full or incremental** — the chunk-hash
  incremental scheme (§11) still takes a fresh crash-consistent Cinder
  snapshot every job and still goes through the same temp-volume-attach
  path before any hashing happens, so incrementals reduce
  *transfer/storage* cost, not this snapshot-to-volume-copy cost.
- **Before locking in sizing/concurrency assumptions**: measure actual
  snapshot-to-volume-copy latency, at realistic volume sizes, on at least
  one representative non-Ceph/SAN backend actually in use, during M1
  (§14) — not assumed from Ceph-only testing. This number directly feeds
  how long a proxy holds resources per job, and any per-proxy concurrency
  ceiling built on top of these numbers.
- Until that number exists, treat SAN-backed instances' effective backup
  window as unknown, and possibly much longer than Ceph-backed instances',
  in any capacity planning.

## 8. Design principle: one instance per Job

Like Proxmox and VMware, this plugin stays **one-instance-per-Job** — that
keeps the plugin simple and keeps Bareos's existing catalog/retention model
(per-Job retention, per-Job restore) working unmodified. The Director-side
Python plugin (`BareosDirPluginBaseclass`,
`core/src/plugins/dird/python/pyfiles/`) currently only exposes
job-lifecycle notification events (`bDirEventJobStart/End/Init/Run`), not
resource discovery or dynamic Job/FileSet creation, so dynamic per-fleet
Job generation isn't available as a shortcut through Director config alone.

"How do I back up many instances / how are FileSets and Jobs generated at
fleet scale / how are orphaned OpenStack resources garbage-collected" are
all answered by the companion tooling in `TEAMING.md`, not by this
plugin — this plugin's contract with that tooling is exactly the
options in §10 and the tagging convention in §5, and nothing more. This
keeps the plugin small and testable, and keeps "which VMs get backed up"
a policy question answered by ordinary operator tooling rather than baked
into the plugin or requiring Director core changes.

## 9. Cleanup / idempotency (this plugin's responsibility)

Because every backup creates billable Glance images, Cinder snapshots, and
*temporarily attached volumes*, failure modes matter more here than for
Proxmox (which only touches local disk). This plugin's responsibility, in
full:
- Tag every Glance image / Cinder snapshot / temp volume it creates with
  `metadata={"created-by": "bareos", "bareos-job-id": ..., "bareos-instance-uuid": ...}`
  (§5) — consistently, including on failure paths.
- Always clean up in `finally` blocks, not just the happy path — including
  detaching a temp volume from the proxy before deleting it (§5, §6.1).

Detecting and cleaning up whatever slips through anyway (a killed proxy
process, a crashed job) is an operational **garbage-collection** concern,
not something the plugin itself can do from inside a single job — that
tooling lives in `TEAMING.md` and depends entirely on the tagging
convention above being applied consistently.

## 10. Plugin option reference

Mirrors the `BareosFdProxmoxOptions`/Incus options-dict pattern
(typed, `check()` validates mandatory ones). The `include_volumes`/
`include_images` split is directly modeled on Bacula Enterprise's observed
`nova_server_backup=0`/`glance_image_backup=1` toggles (§3.2), renamed for
clarity:

| Option | Mandatory | Example | Notes |
|---|---|---|---|
| `cloud` | one-of with explicit auth | `cloud=mycloud` | `clouds.yaml` entry name |
| `auth_url` | one-of | `auth_url=https://keystone.example.org:5000/v3` | |
| `application_credential_id` | with `auth_url` | | |
| `application_credential_secret#enc` | with `auth_url` | base85, like other plugins | |
| `project_name` / `project_id` | one-of | | |
| `region_name` | no | | |
| `verify_ssl` | no, default yes | `verify_ssl=no` for lab/self-signed | |
| `proxy_instance_id` | yes | UUID of the Nova instance running this FD | needed for volume-path attach, §5 |
| `instance` | yes (backup) | UUID preferred, name allowed | |
| `include_volumes` | no, default yes | | Cinder volume-path backup, §5 |
| `include_images` | no, default no | | Nova/Glance image-path backup for ephemeral disks, §5 |
| `keep_snapshot` | no, default no | | |
| `poll_timeout` | no, default 900 | seconds | |
| `restoretoimage` | no, default no | see §6 — set `yes` to stop at a Glance image and create nothing | |
| `force` | no, default no | replace an existing matching instance instead of only creating a new one, §6 | |
| `force_keep_old_volumes` | no, default no | with `force=yes`: retain rather than delete the replaced instance's old volumes, §6.1 | |
| `instance_name` | no | target name for the new instance on restore; defaults to `<original-name>-restore-<jobid>`, §6 | |
| `target_project` | no | cross-project restore target, §6 | |
| `pre_snapshot_cmd` / `post_snapshot_cmd` | no | best-effort quiesce hook, §5 step 4 | |

### 10.1 Example FileSet/Job (backup)

```
FileSet {
  Name = "OpenStack-web01"
  Include {
    Options { signature = xxh128 }
    Plugin = "python"
             ":module_name=bareos-fd-openstack"
             ":cloud=prod"
             ":proxy_instance_id=8f6a...proxy-uuid"
             ":instance=3fa85f64-5717-4562-b3fc-2c963f66afa6"
  }
}

Job {
  Name = "openstack-web01"
  Client = "backup-proxy-fd"
  FileSet = "OpenStack-web01"
  JobDefs = "DefaultJob"
}
```

### 10.2 Example restore overrides

Default restore (no options needed) already creates a new, running instance
non-destructively, named `web01-restore-<jobid>` in the original project:

```
python:module_name=bareos-fd-openstack
```

Restore into a different project, under a chosen name, without touching any
existing instance:

```
python:instance_name=web01-restored:target_project=recovery
```

Restore and **replace** an existing (SHUTOFF) instance with the same
identity, deleting its old volumes:

```
python:force=yes
```

Restore to a Glance image only, creating nothing:

```
python:restoretoimage=yes
```

### 10.3 Deployment note

Unlike Proxmox/Incus, the FD does not need root on a hypervisor — but per
§3.5/§4, it **does** need to run as (or be attachable-to, via os-brick, as)
a Nova instance inside the target cloud so Cinder can attach volumes to it.
A minimal reference deployment: one small Nova instance per project (or per
region), running `bareos-fd` + this plugin, sized per §7.

For deploying more than one proxy — teaming pools, project-scoped teams,
region-scoped teams, and how those patterns compose for high availability
and throughput — see `PROXY_TOPOLOGY.md` in this directory. The automation
that makes a multi-proxy deployment self-managing (health-checked dispatch,
failover, per-tenant provisioning) ships as companion tooling documented
in `TEAMING.md`, not part of this file; `PROXY_TOPOLOGY.md` covers the
deployment shapes and Bareos-side config implications either way, whether
or not that automation is in use.

### 10.4 Related, smaller follow-up (not part of this plugin)

Extend `bareos_libcloud_api/get_libcloud_driver.py` to forward OpenStack
Keystone auth kwargs (`ex_force_auth_url`, `ex_tenant_name`,
`ex_force_auth_version`, etc.) so `bareos-fd-libcloud` can target
`OPENSTACK_SWIFT` directly. Worth doing alongside this plugin so "back up an
OpenStack cloud" has a Swift story too, but it's an independent, much
smaller change to an existing plugin rather than new functionality here.

## 11. Incremental backups (v2 roadmap)

Full-only for the initial release, mirroring Proxmox's own current
limitation (`ProxmoxPlugin.rst.inc` explicitly states "only supports Full
backups") — a normal, precedented phase-one scope cut in this codebase,
not a gap unique to this plugin.

**v2 plan**: implement backend-agnostic block-level incrementals using the
same primitive the **Incus plugin already implements in this tree**,
rather than a Ceph-RBD-specific diff:
- Read the attached volume's raw bytes (from the volume-path ingestion in
  §5) in fixed-size chunks (mirroring Incus's `chunk_size` option, e.g.
  64 MiB), compute a fast content hash per chunk (mirroring Incus's `hash`
  option), and skip/record all-zero chunks separately (mirroring
  `zero_digest`) so sparse regions never need transferring — the same
  sparse-savings behavior already visible in the Proxmox plugin's own log
  output ("backup is sparse: 7.46 GiB (74%) total zero data").
- Emit **one virtual file per chunk** and carry its content hash by
  hijacking spare stat-packet fields, exactly like Incus's
  `hijacked_stat_fields` mechanism — this lets Bareos's own **Accurate**
  mode do the "has this chunk changed since last job" comparison natively,
  instead of the plugin reinventing a bitmap/checksum file format from
  scratch the way Bacula Enterprise's `.bmp`/`.sha` files do. Functionally
  equivalent outcome (only changed blocks get re-transferred, on any
  storage backend — LVM, Ceph, NFS-backed Cinder), implemented by leaning
  on existing Bareos core behavior instead of a bespoke format.
- Every job (Full or Incremental) still takes a fresh crash-consistent
  Cinder snapshot of the current volume state — the "incremental" saving
  is in *bytes transferred/stored*, not in which OpenStack API calls
  happen (and does not reduce the §7.1 snapshot-to-volume-copy cost). This
  matches how Commvault describes its own CRC-based incrementals (§3.1): a
  full snapshot every time, cheaper transfer via content comparison.
- Per Commvault's precedent (§3.1), the **image path stays full-only**
  even in v2 — incrementals only apply to the volume path.

A pure **Ceph RBD diff export** (`rbd export-diff`/`import-diff`) remains a
possible *further* optimization for Ceph-backed Cinder specifically (skips
even reading unchanged blocks off storage, rather than reading-then-hashing
them), secondary to the chunk-hash approach above, which is universal and
has a working in-tree template.

## 12. Testing plan

### 12.1 Restore safety logic (automated, not just manual)

The `force`/`SHUTOFF`/cross-project restore logic (§6, §6.1) is this
plugin's only data-destroying code path — it deserves the same
automated-test rigor as any core code path:
- Fake-backed (`requests_mock`/WireMock, §12.2) test cases for: restore
  with no existing instance (clean create); restore with an existing
  instance and `force` *not* set (must leave it alone, create new
  non-destructively); restore with an existing instance that is *not*
  `SHUTOFF` and `force=yes` (must refuse); restore with an existing
  `SHUTOFF` instance and `force=yes` (must detach + delete old volumes per
  §6.1, then recreate); the same with `force_keep_old_volumes=yes` (old
  volumes detached but retained, not deleted); and a cross-project restore
  (volume attached to proxy, re-owned into target project, per §6).
- Release-blocking test coverage for M3 (§14), not follow-up polish.

### 12.2 Unit and systemtest layers

- Unit-test the option parsing / JSON restore-object (de)serialization with
  plain `unittest`, no live cloud needed (same idea as
  `filed/python/test/bareosfd_test.py`).
- Add `systemtests/tests/py3plug-fd-openstack/` following the
  `py3plug-fd-proxmox` layout (`etc/bareos/{bareos-dir,bareos-fd,bareos-sd}.d`,
  `CMakeLists.txt`, `testrunner-*`). Since there's no OpenStack control plane
  in CI, back the systemtest with a lightweight fake: either OpenStack's own
  unit-test fakes or a small `requests_mock`/WireMock double that implements
  just enough of the Keystone/Nova/Cinder/Glance surface the plugin calls
  (token issuance, snapshot lifecycle, volume attach/detach, image
  download) — this is the same fake backend §12.1's safety-logic tests run
  against. A real-DevStack integration test can exist too but should be
  opt-in/manual, not part of default CI, given the infrastructure cost.
- Manual/staging validation against a real OpenStack (DevStack or a small
  cloud) before calling this done: backup + restore of both a
  boot-from-image and a boot-from-volume instance, plus everything in
  §12.1 repeated against a real cloud at least once (the fake backend
  proves logic correctness; the manual pass proves the real APIs behave
  the way the fake assumes).

## 13. Packaging & docs checklist

- `core/src/plugins/filed/python/openstack/bareos-fd-openstack.py` (+ any
  `bareos_openstack_api/` submodule if it grows past one file, mirroring
  libcloud's layout).
- `debian/control.openstack` — new `bareos-filedaemon-openstack-python-plugin`
  package, `Requires: bareos-filedaemon, bareos-filedaemon-python3-plugin`,
  document `python3-openstacksdk` as the runtime Python dependency (check at
  packaging time whether it's available as a distro package on target
  distros or needs to be documented as a pip prerequisite, same open
  question Libcloud already has).
- `debian/bareos-filedaemon-openstack-python-plugin.install.in`:
  `@plugindir@/bareos-fd-openstack.py*`.
- `core/platforms/packaging/bareos.spec`: new
  `%package filedaemon-openstack-python-plugin` block, mirroring the
  libcloud/proxmox blocks.
- `docs/manuals/source/TasksAndConcepts/Plugins/FileDaemonPlugins/OpenStackPlugin.rst.inc`,
  linked from `FileDaemonPlugins.rst.inc`, written in the same
  Requirements/Installation/Configuration/Backup/Restore/Option-reference
  structure as `ProxmoxPlugin.rst.inc` and `VMwarePlugin.rst.inc`, including
  the proxy-must-be-a-Nova-instance requirement from §10.3 up front (this
  is the single most important thing for an operator to know before
  starting).
- `CHANGELOG.md` entry once merged.

## 14. Milestones

Target platform floor is **OpenStack 2024.1 (Caracal) and later**
(`openstacksdk` pinned accordingly, §13).

1. **M1 — Read-only proof of concept**: auth (against a Caracal+ cloud),
   resolve instance, snapshot → attach volume to a manually-created proxy
   instance → read raw bytes into a local file via a throwaway script (no
   Bareos integration yet). Validates the OpenStack-side mechanics,
   attach/detach timing, confirms the sizing assumptions in §7, and
   **measures the §7.1 snapshot-to-volume-copy cost on at least one
   non-Ceph/SAN backend** before any concurrency assumptions get locked in
   downstream.
2. **M2 — FD plugin, Full backup only**: `start_backup_job`/`start_backup_file`/
   `plugin_io_*`/`end_backup_file`, metadata restore object, volume-path +
   image-path ingestion, cleanup with consistent tagging (§9) from the
   start. Backup works end-to-end against DevStack.
3. **M3 — Restore, all three modes**: `restore_object_data` + `create_file`
   + `plugin_io_write`; the non-destructive create-new default, the
   `force=yes` replace-existing opt-in, and the `restoretoimage=yes`
   image-only opt-in all ship together (§6) — the safety preconditions
   (`force`/`SHUTOFF` check, old-volume handling §6.1, cross-project
   transfer) and their automated test coverage (§12.1) are core M3 work.
4. **M4 — Packaging + docs + systemtest**: everything in §12/§13.
5. **M5 (v2)** — Chunk+hash incremental backup on the volume path (§11,
   backend-agnostic first, given mixed Ceph+SAN environments), Ceph
   RBD-diff as a further optimization specifically for the Ceph-backed
   portion of the fleet.

Fleet-scale work (proxy-pool coordinator, multi-tenant onboarding, GC,
billing) is tracked as its own milestone sequence (T1-T5) in
`TEAMING.md`/`TEAMING_PLAN.md`, which depends on this plugin reaching at
least M3 before most of it can be meaningfully tested end-to-end. Native
Director-level teaming is tracked separately again in
`CLIENT_GROUP_PROPOSAL.md` (C1-C5), independent of both.

## 15. Open items

- SAN backend specifics (vendor/Cinder driver in use) — only matters if a
  SAN-specific optimization analogous to Ceph RBD-diff (§11) ever looks
  worthwhile; not needed for the backend-agnostic v1/v2 path, and directly
  informs the §7.1/M1 measurement task once known.
- Everything about proxy-fleet HA, multi-tenant isolation tiers, Masakari
  adoption, and fleet-scale concurrency is `TEAMING.md`'s concern, not
  this document's — see that document's own open items.
- Whether `CLIENT_GROUP_PROPOSAL.md`'s native Director feature is worth
  pursuing upstream is itself an open question — see that document's
  recommended path (C1: upstream design discussion, before any code).

## 16. Sources

§3 (competitive research):
- Commvault: [Backup Process for OpenStack Images and Instances](https://documentation.commvault.com/v11/commcell-console/backup_process_for_openstack_images_and_instances.html), [Cloud Feature Support for OpenStack](https://documentation.commvault.com/11.20/cloud_feature_support_for_openstack.html)
- Bacula Enterprise: [Backup and Restore Strategies](https://docs.baculasystems.com/BEDedicatedBackupSolutions/Virtualization/Hypervisors/openstack/BackupAndRestoreStrategies/index.html), [Backup Operations](https://docs.baculasystems.com/BEDedicatedBackupSolutions/Virtualization/Hypervisors/openstack/Operations/Backup/index.html), [openstack-vm Installation](https://docs.baculasystems.com/BEDedicatedBackupSolutions/Virtualization/Hypervisors/openstack-vm/Installation/index.html), [OpenStack backup solution overview](https://www.baculasystems.com/openstack-backup-solution/)
- TrilioVault: [TrilioVault data protection (OpenStack charm-guide)](https://docs.openstack.org/charm-guide/latest/admin/trilio.html)
- Veeam / OpenStack ecosystem gap: [Why Veeam Doesn't Work for OpenStack (And What Does)](https://openmetal.io/resources/blog/why-veeam-doesnt-work-for-openstack-and-what-does/)

§4 (application-credential scoping, expanded in `TEAMING.md`):
- Keystone application credential `access_rules`: [Add Fine Grained Restrictions to Application Credentials (keystone-specs)](https://specs.openstack.org/openstack/keystone-specs/specs/keystone/train/capabilities-app-creds.html), [OpenStack API-ref: Application Credentials](https://docs.openstack.org/api-ref/identity/v3/index.html?expanded=create-application-credential-detail)

Numeric/naming details cited above (Bacula's exact `.bmp`/`.bmpsha`
filenames, virtio/SCSI attach-count figures in §3.1) came from web search
and fetched vendor doc pages, not from primary spec documents read in
full. They're solid enough to justify the design decisions built on them
(backend-agnostic chunk-hashing, boot-from-volume proxies) — those hold up
on their own engineering merits even if a specific cited number drifts
slightly. Re-verify exact figures against current vendor docs before
quoting them verbatim in anything published externally.

Sources specific to fleet/HA/multi-tenancy/concurrency-at-scale (Masakari,
Trilio's multi-tenant positioning, Commvault VSA proxy teaming, and the
`PluginOptionsAcl` mechanics verified against `core/src/dird/ua_acl.cc`)
live in `TEAMING.md`'s own sources, since that's the content they
support.
