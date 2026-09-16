# Proxy Teaming, Multi-Tenancy, and Fleet Operations

Companion tooling shipped **with** `bareos-fd-openstack` (same directory,
same package) that turns a single proxy (`DESIGN.md`) into a
Commvault-VSA-style **team** of proxies: load-balanced, self-healing
dispatch across multiple proxy instances, plus the multi-tenant
onboarding, garbage collection, and fleet-scale scheduling a cloud
provider running many tenants needs on top of that. `PROXY_TOPOLOGY.md`
covers the deployment *shapes* (teaming / project-scoped / region-scoped);
this document covers the *tooling* that makes a team of proxies
self-managing rather than something an operator load-balances by hand.

## A note on how "native" this actually is

Commvault's proxy teaming is built into the CommServe itself — no extra
process to deploy. I checked whether Bareos's Director has an equivalent
extension point before designing this: it doesn't, today. There is no
"Client Group" or pool concept in `core/src/dird/dird_conf.cc` — a Job's
`Client =` is a single static reference — and the Director's Python plugin
API (`BareosDirPluginBaseclass`) is notification-only
(`bDirEventJobStart/End/Init/Run`), not a resource-discovery or
dynamic-scheduling hook.

Getting *truly* native, zero-extra-process teaming into Bareos means a
Director core (C++) feature — a real `Client Group` resource the Director
itself understands, matching what Commvault's CommServe does natively.
That's now planned as its own track: see `CLIENT_GROUP_PROPOSAL.md` in
this directory for the concrete design, grounded in the actual Director
source, and its milestone sequence (C1-C5). It's independent of, and not
a blocker on, everything below.

Until that lands (it's a substantial core contribution — realistically a
longer timeline than the plugin or this tooling), what's built here is the
pragmatic version available today: a small coordinator process that ships
**as part of this plugin's own package**, not as a separate product/
repository to track and release independently. It's still a process that
runs somewhere (on one of the proxies, or a small control host), but it's
this plugin's own tooling — installed by the same package, versioned with
the same plugin, not something with its own lifecycle. See §7 for exactly
how the two tracks relate.

## 1. Components

All of this lives under `core/src/plugins/filed/python/openstack/tools/`
in this same plugin directory, as a small internal package
(`bareos_openstack_fleet`) with CLI entry points, sharing the plugin's own
`openstacksdk`/`python-bareos` dependency versions:

- **Coordinator** (§2) — health-checks proxies, dispatches jobs
  least-loaded-first, requeues on proxy failure.
- **Generator** (§3.1) — turns "which instances should be backed up" into
  Bareos FileSet/Job config.
- **GC** (§3.3) — finds and removes orphaned OpenStack objects the plugin
  or a dead proxy left behind.
- **Onboarding** (§4) — for multi-tenant/cloud-provider deployments only:
  provisions per-tenant credentials, Bareos resources, and ACLs.

A single-proxy or small manually-managed deployment (§2 of
`PROXY_TOPOLOGY.md`) needs none of this — it's additive tooling for teams
large enough that manual management stops scaling, not a prerequisite for
running the plugin at all.

## 2. Coordinator

Bareos won't dynamically pick "the least-loaded healthy proxy" for a Job
on its own (§ above), so the coordinator is what actually makes a set of
proxy Clients behave like a Commvault-style team, before submitting each
per-instance backup:

- Checks proxy health (`status client=<proxy-fd>` via `python-bareos`) and
  drops unhealthy proxies from rotation after N consecutive failed checks
  (re-adding after M consecutive healthy ones — a standard health-check
  circuit-breaker pattern).
- Tracks each proxy's current concurrent-job count against its configured
  `Maximum Concurrent Jobs` (see `DESIGN.md` §7/§7.1 for the per-proxy
  sizing inputs this is built from — CPU/RAM and guest
  volume-attach-count ceilings), sized from proxy CPU/RAM using a formula
  in the spirit of Commvault's own (§2.4 below: "1 CPU ≈ 10 streams, 1
  stream ≈ 100 MB RAM") adapted to this plugin's coarser per-instance-job
  granularity — calibrate the actual constant empirically, since the
  workloads aren't identical.
- **Assigns work the same way Commvault's coordinator does** (§2.4): give
  every idle proxy one job first, only start doubling up once every proxy
  has at least one, and once all proxies are at their configured limit,
  round-robin any remainder — a well-understood
  least-loaded-then-round-robin pattern.
- Prefers the proxy in the **same AZ/region as the target instance's
  compute host**, to keep volume-attach and data-transfer traffic local
  rather than crossing AZ boundaries.
- Issues the job (`run job=... client=<chosen-proxy> yes`) against the
  chosen proxy.
- **On proxy failure mid-job**, requeues that instance's backup for the
  next available healthy proxy rather than just failing — the same
  "reset to Not Started and return to the queue" behavior Commvault
  documents for its own coordinator (§2.4), which is the actual mechanism
  (not Bareos's own `RescheduleOnError`, which only retries against the
  *same* `Client =`) that gives this design its failover.
- The coordinator itself must not become a new single point of failure.
  Commvault handles *coordinator* failure by promoting the next proxy in
  the list and resuming for whatever wasn't finished yet (§2.4) — full
  leader election is more machinery than this needs, but the coordinator
  is at least **stateless/restart-safe**: it derives "what's running
  where" from the Bareos catalog (active Jobs per Client) and OpenStack
  resource tags (`DESIGN.md` §9) on every run rather than keeping
  authoritative state only in its own memory/disk, so restarting it after
  a crash (on the same host or a standby) doesn't lose track of in-flight
  work or double-schedule anything.

### 2.1 Proxy failure and recovery

Where Masakari (OpenStack's VM-HA service) isn't deployed, a watchdog is
the primary recovery mechanism, not a fallback — Masakari becomes a
drop-in upgrade if/when it's deployed, without changing anything else
here.

**Automatic proxy *self-healing* (detect a dead proxy and automatically
rebuild/replace the instance) is a deliberate later-phase item, not part
of the initial rollout.** What matters for a correct initial rollout: the
coordinator detects a dead proxy via health check, takes it out of
rotation, and requeues its in-flight job onto a different healthy proxy —
that's the part that keeps backups working through a proxy failure.
Getting the *failed proxy itself* back into the pool (rebuild, replace)
starts as a manual runbook step (an alert fires, an operator runs
`nova server rebuild` or recreates the instance from the same
Heat/Ansible template used to provision it). Automating that replacement
is a natural follow-up once the coordinator/pool mechanics — the part
that actually matters for correctness — are proven; pair it with Masakari
adoption, since both are the same category of "make proxy recovery
faster," not "make it correct."

- Proxy root disks are **boot-from-volume on Cinder** from the start,
  manual-recovery or not — it makes even a manual rebuild trivial, and is
  also what lets Masakari or an automated watchdog be adopted later with
  zero redesign (Masakari's one hard requirement is shared storage for the
  instance disk, which boot-from-volume already satisfies).
- Proxies are **stateless/disposable by design** — no locally-held state
  matters beyond the lifetime of one job's transient volume attachment.
  This is what makes both horizontal scaling and replacement-on-failure
  tractable; proxies are cattle, not pets.
- Whatever Cinder snapshot/temp-volume/attachment was left on a dead proxy
  or its target instance is exactly what GC (§3.3) cleans up.

### 2.2 Configuration

A single YAML config drives the coordinator, generator, GC, and
onboarding tools together, since they all need to agree on
tenant/tier/pool definitions:

```yaml
regions:
  - name: region1
    proxy_pools:
      shared:
        az: nova
        min_size: 2
        max_size: 8
        flavor: backup-proxy.small
        image: backup-proxy-base
      # dedicated pools are generated per-tenant at onboarding time,
      # not listed statically here
tenants:
  acme-prod:
    project_id: "...uuid..."
    tier: shared            # shared | dedicated, see §4.1
    isolation:
      catalog: shared        # shared | dedicated, see §4.2
      storage: shared        # shared | dedicated, see §4.4
    backup_selector:
      metadata_tag: "bareos_backup=yes"
    schedule:
      window: "01:00-05:00"   # jitter offset derived from tenant id within this window, §5.4
```

### 2.3 Sizing reference

Per-proxy sizing (vCPU/RAM, guest volume-attach ceiling, and the
snapshot-to-volume-copy cost risk on non-COW Cinder backends) is covered
in `DESIGN.md` §7/§7.1 — the coordinator's capacity formula (§2 above)
consumes those numbers rather than duplicating them.

### 2.4 Prior art: Commvault VSA proxy teaming

Commvault's Virtual Server Agent (VSA) solves precisely this "pool of
backup proxies, load-balanced and failure-tolerant" problem for
VMware/Hyper-V/Azure/etc., and its documented mechanics directly informed
the coordinator design above:

- **Coordinator role**: "the first proxy in the list acting as a
  coordinator" — one designated proxy directs traffic across the whole
  pool for a given virtualization client, rather than every proxy
  independently deciding what to pick up. Commvault implements this
  CommServe-side, natively; this tooling implements the equivalent as a
  companion process because (per the note at the top of this document)
  Bareos's Director has no equivalent native extension point today.
- **Resource-to-capacity formula**: each proxy reports its CPU core count
  and total RAM to the coordinator; Commvault's documented default is
  "each CPU in a proxy can support 10 streams, and each stream requires
  100 MB of memory" — proxy job capacity is a function of measured
  hardware, not a fixed number picked once. Streams here are Commvault's
  finer-grained per-disk parallel transfer unit, one level below this
  plugin's per-instance Job granularity, so the constant isn't copied
  verbatim — but "derive concurrency limits from live CPU/RAM rather than
  a static guess" is directly reusable.
- **Distribution algorithm**: fill every idle proxy with one job first,
  only assign a second job to a proxy once all proxies have at least one,
  and once every proxy is at its resource-derived limit, distribute any
  remainder round-robin — the coordinator (§2) uses the same algorithm.
- **Mid-job proxy failure**: a VM whose backup was in progress on a proxy
  that died is "reset to Not Started and returned to the queue" for the
  next available proxy — i.e. the coordinator itself performs the
  failover-to-a-different-proxy retry, exactly what §2/§2.1 implement,
  since core Bareos's `RescheduleOnError` alone only retries against the
  same `Client =`.
- **Coordinator failure**: handled by promoting the next proxy in the list
  to coordinator, which resumes the job for whatever VMs weren't finished
  yet — the basis for the "coordinator must be stateless/restart-safe"
  requirement in §2.
- Deployment guidance ("at least one access node per hypervisor, more as
  needed for the planned workload, with load balancing across them") is
  the same per-region/per-AZ pool sizing philosophy as `PROXY_TOPOLOGY.md`,
  and Commvault's docs note proxies are chosen with affinity to the same
  datacenter as the workload being protected, mirroring the AZ/region
  locality preference in §2.
  [Backup Workload Distribution](https://documentation.commvault.com/v11/commcell-console/backup_workload_distribution.html),
  [Identifying VSA Proxies](https://documentation.commvault.com/2024e/commcell-console/identifying_vsa_proxies.html),
  [Virtual Server Instance Properties (Proxies)](https://documentation.commvault.com/v11/commcell-console/virtual_server_instance_properties_proxies.html),
  [VM Dispatch and Proxy Selection for VMware Backups](https://docs.commvault.com/11.40/commcell-console/vm_dispatch_and_proxy_selection_for_vmware_backups.html)

## 3. Fleet operations (generator + GC)

### 3.1 Per-instance FileSet/Job generation

A plain Python CLI, using `openstacksdk` + `python-bareos`:
- Lists instances matching a filter (server metadata tag
  `bareos_backup=yes`, or `--project`). An optional `bareos_workload=<name>`
  tag (inspired by TrilioVault's "workload" grouping — protect a set of
  related instances + their networking as one labeled unit) lets it
  order/group Jobs without implying atomic multi-instance consistency
  (still out of scope).
- Emits/updates `bareos-dir.d/fileset/openstack-<uuid>.conf` and
  `bareos-dir.d/job/openstack-<uuid>.conf`, one per instance, each pointing
  at the resolved instance UUID and the chosen proxy pool's Client name.
- Runs from cron/Ansible/CI, followed by `bconsole reload` — the same
  operational pattern real deployments already use for large fleets of
  per-VM Proxmox/VMware Jobs.

### 3.2 Design principle this builds on

The plugin itself stays **one-instance-per-Job** (`DESIGN.md` §8) — the
generator is what turns "back up this whole fleet" into that many
per-instance Jobs, rather than the plugin or Bareos core needing to
understand fleets at all.

### 3.3 Garbage collection

Because every backup creates billable Glance images, Cinder snapshots, and
*temporarily attached volumes*, and a dead proxy (§2.1) or a crashed job
can leave any of these behind:
- List `created-by=bareos` objects (images, snapshots, volumes, and —
  importantly — **stuck volume attachments on a proxy**) older than N
  hours with no corresponding recent successful Job in the catalog
  (queryable via `python-bareos`/`bconsole llist`), and clean them up.
- Same "leaked snapshot/attachment" problem every snapshot-based backup
  tool has (Bacula Enterprise's own cleanup step exists for the same
  reason, `DESIGN.md` §3.2) — solved once here as an operational script
  rather than assumed away.
- Runs on a schedule (e.g. hourly), independent of the coordinator, as a
  safety net for whatever the plugin's own `finally`-block cleanup
  (`DESIGN.md` §9) didn't catch (proxy crash mid-job, etc.) — this
  depends entirely on the plugin's tagging convention (`DESIGN.md` §5/§9)
  being applied consistently.

## 4. Multi-tenancy (for cloud-provider deployments)

Trilio and Commvault both lead their OpenStack/MSP positioning with
"multi-tenant, self-service, policy-based" rather than treating it as an
afterthought. Isolation is offered as **tiers**, since how much isolation
a deployment needs is a cost/complexity tradeoff, not a single correct
answer. A single-tenant/enterprise deployment can skip this whole section.

Default launch tier: **credential + console/self-service** (§4.1 + §4.3),
with dedicated Catalog/Storage (§4.2/§4.4) and a dedicated proxy team
(`PROXY_TOPOLOGY.md` §3) offered as explicit, opt-in upgrades per tenant
rather than built for every tenant from day one — this tier ships fastest
while still giving every tenant real self-service restore (§4.3) and real
credential-level blast-radius containment (§4.1), the two properties that
matter most at scale, since standing up a dedicated Postgres catalog and
Pool per tenant does not obviously buy safety proportional to its
operational cost at that scale. Large/regulated tenants can be promoted to
dedicated Catalog/Storage/proxy-team later without a redesign — every tier
here layers, rather than replaces.

### 4.1 Credential isolation (mandatory baseline)

One Keystone **application credential per tenant project**, scoped with
Keystone's `access_rules` — a real Keystone capability that restricts an
application credential to an explicit list of `{method, path, service}`
tuples, independent of how broad the underlying role is (e.g. allow only
volume snapshot create/delete, volume attach/detach, server list/show —
nothing else, even if the role nominally grants more). This is what lets
a **shared, horizontally-scaled proxy pool** (`PROXY_TOPOLOGY.md` §2)
hold one narrowly-scoped credential per tenant and select the right one
per job, instead of needing either "one proxy per tenant" (expensive at
fleet scale) or "one proxy with a broad cross-tenant role" (a security
liability for a provider).
[Add Fine Grained Restrictions to Application Credentials (keystone-specs)](https://specs.openstack.org/openstack/keystone-specs/specs/keystone/train/capabilities-app-creds.html),
[OpenStack API-ref: Application Credentials](https://docs.openstack.org/api-ref/identity/v3/index.html?expanded=create-application-credential-detail)

Handling at rest: don't bake tenant credentials into shared `Plugin =`
strings in FileSet config (unmanageable at fleet scale, and risks one
tenant's Plugin Options — which appear in job logs and restore dialogs —
being visible to operators handling another tenant's job). Two options,
increasing in rigor:
- Simpler: store per-tenant `application_credential_id`/`secret` using the
  plugin's existing `#enc` obfuscation and have the generator (§3.1) inject
  them per-Job at config-generation time.
- More rigorous: have the plugin fetch the credential itself from a
  secrets API (Barbican, Vault) at `start_backup_job()` time given only a
  tenant identifier, so no tenant secret is ever at rest in Bareos
  Director config at all. This one *does* touch plugin code
  (`DESIGN.md`'s `start_backup_job()`, §5) — a genuine future enhancement
  to the plugin itself, not just this tooling.

**Residual risk, stated directly rather than glossed over**: `access_rules`
credentials (above) bound the *API-level* blast radius of a compromised
proxy, but on the **shared-pool tier** (`PROXY_TOPOLOGY.md` §2 default),
the same proxy host still sequentially handles raw disk bytes and
in-memory credentials for *different tenants' jobs* one after another.
§4.7's blast-radius principle isn't fully backed at the host level by
credential scoping alone — it needs explicit host-level hygiene:
- The plugin's ingestion path never buffers disk bytes to the proxy's
  local disk — this is a plugin requirement (`DESIGN.md` §5); verify it
  holds rather than assuming.
- The coordinator (§2) verifies — not just attempts — that a job's temp
  volume is fully **detached and deleted** and its credential is cleared
  from the proxy's memory/environment before marking that proxy eligible
  for the next tenant's job; a leftover attachment or a live credential
  held past job end is exactly the kind of leak GC (§3.3) exists to catch
  after the fact, but the goal is to not rely on GC for this.
- This is the shared tier's **accepted residual risk model**: sequential,
  host-level tenant separation backed by hygiene discipline in the
  plugin/coordinator, not physical isolation. The dedicated-team tier
  (`PROXY_TOPOLOGY.md` §3) is the answer for tenants who need physical
  host isolation instead — call that tradeoff out by name in
  operator-facing docs, rather than letting "multi-tenant" imply a
  stronger guarantee than the shared tier actually gives.

### 4.2 Catalog isolation

A separate Bareos **Catalog** resource — its own database, via the
`DbName`/`DbAddress`/`DbDriver` directives — per tenant, or per tenant tier
(e.g. one shared catalog for small tenants, dedicated catalogs for
large/regulated ones). Strongest isolation; more databases to
operate/upgrade/back up. An explicit tier choice per tenant, not a
default.

### 4.3 Console/self-service isolation

This is the tier that lets a tenant admin log into `bconsole` or the
WebUI and manage **only their own** backups: one Bareos **Profile** +
**Console** per tenant, using the ACL directives `ClientAcl`, `JobAcl`,
`FileSetAcl`, `PoolAcl`, `StorageAcl`, `CatalogAcl`, `CommandAcl` — scoped
to exactly that tenant's resources.

Because a restore's `force=yes` can replace a real Nova instance
(`DESIGN.md` §6), the ACL layer needs to actually constrain *which*
plugin options a tenant's console can pass, not just which Jobs/Clients it
can see. Bareos's `PluginOptionsAcl` directive
(`dir-console-PluginOptionsAcl` and `dir-profile-PluginOptionsAcl`) is the
mechanism, **verified against Director source**
(`core/src/dird/ua_acl.cc`,
`AclAccessOk`/`CompareAclListValueWithItem`/`FindInAclList`, call site
`core/src/dird/ua_run.cc:2013`):

- `PluginOptionsAcl` matches against the **entire Plugin Options string as
  one value**, not a structured per-key allowlist. There is no concept of
  "allow any value for `instance`, but only this value for
  `target_project`" as separate constraints.
- Each ACL list entry is either `*all*` (allow everything), an exact
  case-insensitive string match, or — if it contains regex metacharacters
  — a POSIX extended regex matched against the **whole string**, requiring
  a full match, not a substring match. A `!` prefix makes an entry an
  explicit deny.

So the mechanism works, but only as "does this exact Plugin Options string
as a whole match one of these per-tenant full-string patterns," not "is
this one option's value within bounds." Concretely, restricting a tenant to
their own `target_project` means the generator (§3.1) must produce a
**tenant-specific regex covering the entire expected option string** (e.g.
anchoring `instance=` to that tenant's known instance UUIDs and
`target_project=` to their own project, while still matching whatever
combination of other options — `force`, `instance_name`, etc. — should be
legal), and regenerate/redeploy it whenever that tenant's allowed options
change (new instance provisioned, etc.). This is workable — the same
generated-config-per-tenant pattern the generator already uses for
FileSets/Jobs — but more brittle than a simple per-key allowlist, and
built and tested as such (§5) rather than assumed to be a lightweight
one-line ACL. With that in place, a tenant self-serves restores of their
own instances without provider staff in the loop, with no plausible path
to restoring into (or even seeing) another tenant's project.

### 4.4 Storage isolation

Per-tenant (or per-tier) **Pool** + **Storage/Device**, optionally
per-tenant Volume encryption keys (Bareos supports per-Volume encryption),
so one tenant's backup data is separated not just logically (ACLs) but
physically/cryptographically.

### 4.5 Onboarding workflow

Given a tenant/project id, one command:
1. Create the tenant's scoped `access_rules` application credential (§4.1).
2. Verify/create a dedicated Catalog if that tenant is on the dedicated
   isolation tier (§4.2), otherwise use the shared default Catalog.
3. Create Pool/Storage (dedicated tier) or confirm shared Pool assignment.
4. Create the tenant's Profile + Console (§4.3).
5. Assign the tenant to a proxy pool: shared per-AZ pool (default) or a
   new dedicated per-project teaming pool (`PROXY_TOPOLOGY.md` §3), per
   the tenant's tier.
6. Generate per-instance FileSet/Job entries (§3.1).
7. Tag every created OpenStack-side object and Bareos resource with the
   tenant id, for both GC (§3.3) and billing (§4.6).

### 4.6 Billing / usage metering

Per-Job "FD Bytes Written"/"SD Bytes Written" are already recorded in the
Bareos catalog and shown in every job summary — a periodic report using
`python-bareos` (`llist jobs client=...`) rolls these up per tenant for
chargeback without needing any new instrumentation in the plugin itself.

### 4.7 Blast-radius principle

State this explicitly in operator-facing docs, and treat every tier above
as a control toward it: **a compromised or buggy job for tenant A must not
be able to read, restore into, or delete anything belonging to tenant B.**
Different deployments will land on different tiers (§4.1 is mandatory,
§4.2/§4.4 are cost/isolation tradeoffs) — document which tier a given
deployment actually achieves rather than asserting "multi-tenant" as an
unqualified boolean.

## 5. Concurrency & performance at fleet scale

The per-proxy/per-job sizing inputs — concurrency directive locations,
guest volume-attach limits, single-stream-per-job architecture — are
documented in `DESIGN.md` §7/§7.1 as properties of the plugin/one proxy;
this section covers the fleet-scale concerns built on top of them.

### 5.1 OpenStack control-plane limits, not just Bareos-side ones

A large simultaneous fleet backup can hit Nova/Cinder **API rate limits**
and **per-project quota classes** (max volumes, max snapshots, max
volume-gigabytes) well before it hits any Bareos-side concurrency limit.
The coordinator (§2) applies **jittered start times** and **bounded
concurrent API calls per project**, not just per proxy — avoiding a
"thundering herd" of simultaneous snapshot-create calls at the top of
every hour, which stresses both the OpenStack API and the underlying
storage backend (e.g. Ceph OSD load from many concurrent snapshots).

### 5.2 Storage Daemon-side elasticity

Local-disk File Devices don't scale write concurrency well as tenant count
grows. Bareos core already ships an S3-compatible object-storage SD
backend (`dplcompat`) and a `dedupable_device` backend. For a multi-tenant
deployment, back Storage with an elastic object-storage target — which
could even be the *same* OpenStack cloud's Ceph RadosGW/Swift S3 endpoint
— so write concurrency scales with the number of concurrent Storage/Device
instances rather than the I/O ceiling of one local disk. `dedupable_device`
is worth evaluating too: many tenants' instances likely share large common
blocks (shared base OS images), so SD-side global dedup is complementary
to, not a substitute for, the plugin's source-side chunk-hash incremental
scheme (`DESIGN.md` §11) — one reduces bytes *transferred*, the other
reduces bytes *stored*.

### 5.3 The biggest lever is still the plugin's incremental scheme — but it doesn't cover snapshot-copy cost

Full-every-night at fleet scale is both a network problem and an OpenStack
storage-backend problem (concurrent full-volume reads across hundreds of
tenants simultaneously). This means the plugin's chunk-hash incremental
scheme (`DESIGN.md` §11) is weighted **higher priority** for a
cloud-provider deployment than for a single-enterprise one.

`DESIGN.md` §7.1's snapshot-to-volume copy cost happens on *every* job
regardless of Full vs. Incremental, since both still take a fresh Cinder
snapshot and attach a volume built from it before any hashing/transfer
starts. Incrementals cut *bytes transferred and stored*; they do nothing
for the storage-side copy-on-snapshot cost on non-COW backends (relevant
for a mixed Ceph+SAN environment). Both risks need their own mitigation —
the plugin shipping incrementals doesn't solve the snapshot-copy-cost
risk; that risk is this tooling's problem to size around
(`PROXY_TOPOLOGY.md` pool sizing, §5.1 scheduling), since it directly
affects how long a proxy holds resources per job at fleet scale.

### 5.4 Scheduling spread

The generator (§3.1) staggers per-tenant backup start times (e.g. derives
a time-of-night offset within the maintenance window from a hash of the
tenant id) instead of one shared "everyone at 02:00" schedule — same
concurrency-flattening idea as §5.1, applied to the Bareos Scheduler side
of the system rather than the OpenStack API side.

## 6. Testing plan

- Fake-backed (`requests_mock`/WireMock) coordinator tests: proxy health
  detection and rotation, least-loaded/round-robin dispatch, requeue on
  simulated proxy failure, coordinator restart-safety (kill and restart
  the coordinator process mid-fleet-run, verify no double-scheduling and
  no lost in-flight job).
- Fake-backed generator/GC tests: generator produces exactly the expected
  FileSet/Job config for a given fake instance list; GC correctly
  identifies and removes tagged orphans without touching untagged/
  non-Bareos resources.
- Fake-backed onboarding tests: onboarding produces exactly the expected
  Bareos config + OpenStack credential + ACL objects.
- `PluginOptionsAcl` regex-generation tests (§4.3): given a tenant's
  allowed instance set and project, the generated ACL regex accepts every
  intended Plugin Options string and rejects every attempt to set a
  different `target_project` or an unauthorized `force`/`instance` value —
  worth property-testing (generate many random plugin-option strings,
  assert accept/reject matches intent) given how brittle §4.3 notes this
  mechanism to be.
- A staging pass against a real multi-proxy, multi-tenant OpenStack
  deployment before calling any of this production-ready: kill a proxy
  mid-job and confirm requeue + eventual manual-runbook recovery; onboard
  and then fully offboard a test tenant; run the GC pass against
  deliberately-orphaned resources.

## 7. Native Director support (separate proposal track)

For "no companion process at all" — matching Commvault's fully-native
CommServe teaming — the real fix is a Bareos Director core feature, not
more Python tooling here. This is now planned concretely, grounded in the
actual Director source (`Job.Client` is a single `CFG_TYPE_RES` pointer,
`core/src/dird/dird_conf.cc:299`, resolved once at job start,
`core/src/dird/job.cc:1704` — no pool concept exists today): see
`CLIENT_GROUP_PROPOSAL.md` in this directory for the full design (a new
`Client Group` resource, a `ClientGroup =` Job directive, Director-side
least-loaded/round-robin resolution and failover) and its own milestone
track (C1-C5), independent of this document's T1-T5.

If/when that ships, it subsumes the coordinator (§2) for dispatch and
failover — a deployment could drop the external coordinator process
entirely and use native `ClientGroup = "..."` Jobs. It doesn't touch GC,
onboarding, or fleet-scale scheduling (§3-§5), which stay external
regardless. Not a blocker on anything in this document — the coordinator
here is the working answer today; `CLIENT_GROUP_PROPOSAL.md` is the path
to making it unnecessary later.

## 8. Milestones

1. **T1 — Coordinator core**: pool topology (shared tier only to start,
   `PROXY_TOPOLOGY.md` §2), health-check + least-loaded/round-robin
   dispatch + requeue-on-failure (§2), stateless/restart-safe design.
   Manual runbook for failed-proxy replacement (§2.1) — automated
   self-healing is a later phase.
2. **T2 — Generator + GC**: §3.1 (FileSet/Job generation) and §3.3 (GC),
   which don't depend on multi-tenancy machinery and can ship early to
   make even single-tenant teams manageable.
3. **T3 — Multi-tenant onboarding**: §4 end to end — credential isolation
   (§4.1) and console/self-service (§4.3) tier, including the
   `PluginOptionsAcl` regex generation and its test suite (§6).
4. **T4 — Fleet-scale concurrency hardening**: §5 — jittered scheduling,
   quota-aware API call bounding, elastic SD backend adoption.
5. **T5 — Dedicated tiers**: per-tenant Catalog/Storage (§4.2/§4.4) and
   dedicated proxy-team tier (`PROXY_TOPOLOGY.md` §3) as opt-in upgrades;
   Masakari adoption if/when deployed (§2.1); automated proxy self-healing
   design.

Depends on the plugin itself (`DESIGN.md` §14) reaching at least M3
(backup + all three restore modes working) before most of this can be
meaningfully tested end-to-end. T2's generator/GC and T1's coordinator
fake-backed tests can start earlier, against the plugin's own fake-backed
test harness (`DESIGN.md` §12.2).

See `TEAMING_PLAN.md` for the concrete repository layout, task breakdown,
and definition-of-done checklists per milestone.
