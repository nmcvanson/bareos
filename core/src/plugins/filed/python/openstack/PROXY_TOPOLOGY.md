# Proxy Node Topology: Teaming, Project-Scoped, and Region-Scoped Deployments

Reference architecture for deploying `bareos-fd-openstack` proxy instances
for high availability and high performance. `DESIGN.md` establishes that
the proxy must be a Nova instance able to attach Cinder volumes to itself
(§3.5, §4, §10.3) — this document covers how to deploy more than one of
them, and why you'd choose each pattern.

**Scope note**: this document describes deployment *shapes* and their
Bareos-side configuration implications — how many proxies, scoped to what,
and what that means for Client/Job/FileSet resources and concurrency
limits. The *automation* that makes multi-proxy deployments self-managing
(health-checking, load-balanced dispatch, automatic failover, per-tenant
credential/ACL provisioning) is implemented in `TEAMING.md`, companion
tooling shipped alongside this plugin in this same directory. A single
proxy, or a small manually-managed team, works with nothing but the
Bareos config described here — `TEAMING.md`'s tooling is what you add
when a team gets large
enough that manual management stops scaling, not a prerequisite for using
more than one proxy.

## 1. Why more than one proxy

A single proxy (§10.3) is both a throughput ceiling and a single point of
failure:
- **Throughput**: one proxy's concurrency is bounded by its vCPU/RAM (§7)
  and its guest disk-controller's volume-attach limit (§7.1 references the
  same category of constraint Commvault documents for its own backup
  proxies — virtio supports far more attached volumes at once than SCSI).
  Backing up more instances than that ceiling allows means either serializing
  jobs behind a queue (slow) or adding proxies (fast).
- **Availability**: if the one proxy is down (maintenance, crash, its
  hosting compute node failing), no backups run until it's back, and any
  job it was mid-way through fails.

Three deployment patterns address this, at increasing levels of isolation
and operational cost. They compose rather than compete — see §5.

## 2. Pattern 1: Teaming proxy nodes (the base pattern)

The foundational pattern: N proxy instances instead of one, each capable of
handling any backup/restore job routed to it.

- **Placement**: deploy via a Nova **anti-affinity server group** so no two
  proxies in the team share a hypervisor — a single compute-host failure
  should never be able to take out more than one proxy in the team.
- **Bareos-side shape**: each proxy is registered as its own Bareos
  **Client** resource. Bareos has no native concept of "a pool of
  interchangeable Clients for one Job" — a Job's `Client =` directive is a
  static config reference — so which proxy handles which instance's backup
  is decided at Job/config level, not dynamically by Bareos itself.
  - **Manually managed team** (no extra tooling): assign specific
    instances to specific proxy Clients by hand when writing FileSet/Job
    config, spreading load roughly evenly. Works fine for a handful of
    proxies and a stable, small set of instances; failover means an
    operator manually re-pointing a Job's `Client =` at a different proxy
    and re-running the job.
  - **Automated team**: `TEAMING.md`'s coordinator picks a proxy per job
    at submission time (least-loaded-first, then
    round-robin) and requeues onto a different proxy automatically if one
    fails mid-job. This is what "teaming" means in practice once you're
    past manual scale — see `TEAMING.md` §2 for the mechanics.
- **Sizing**: `Maximum Concurrent Jobs` on each proxy's Client resource,
  set from §7's vCPU/RAM guidance and §7.1's guest volume-attach ceiling —
  this bounds how many simultaneous instance-backups one proxy in the team
  can run.
- **Performance**: aggregate throughput scales with team size — N proxies
  give roughly N times the concurrent-job capacity of one, since each Job
  is a single sequential FD↔SD stream (§5) with no internal parallelism to
  exploit instead.
- **Availability**: a proxy failure removes only that proxy's capacity and
  fails only jobs currently running on it, rather than stopping backups
  for the whole fleet. Minimum team size for any real HA benefit is **2**
  — a "team" of one is just a single proxy with extra config.

## 3. Pattern 2: Project-scoped proxy nodes

A teaming pool (pattern 1) dedicated to **one OpenStack project (tenant)**
rather than shared across many.

- **Why**: isolation. A project-scoped team only ever attaches volumes
  belonging to that one project, so its credential can be scoped tightly
  to that single project (§4) without needing the more precise
  `access_rules` restriction that a shared team requires to stay safe
  across tenants. It also gives **physical host-level isolation**: another
  project's disk bytes are never processed on this team's proxies, which a
  shared team can only approximate through in-plugin hygiene (never
  buffering to local disk, clearing credentials between jobs — §5, §9),
  not through physical separation.
- **When to use it**: large or regulated tenants who need proxy-level
  isolation, not just credential-level isolation; or a single-tenant/
  enterprise deployment, where "project-scoped" is simply the natural
  shape since there's only one project to begin with — patterns 2 and 1
  collapse into the same thing in that case.
- **Cost**: N proxies × M projects, rather than N proxies shared across
  all of them — meaningfully more infrastructure to run for a
  multi-tenant deployment that gives every tenant a dedicated team.
- **Composition**: a project-scoped team is still a team (pattern 1) — it
  still wants ≥2 proxies, anti-affinity placement, and the same
  sizing/concurrency guidance. Scoping is about *who* a team serves, not a
  different mechanism.

## 4. Pattern 3: Region-scoped proxy nodes

A teaming pool dedicated to **one OpenStack region** (or availability
zone).

- **Why it's not optional past one region**: Cinder volume attach and Nova
  instance operations don't cross OpenStack region boundaries — a proxy in
  region A cannot attach a volume belonging to an instance in region B.
  So the moment a deployment spans more than one region, **region-scoped
  proxy teams are a hard requirement**, not a performance optimization
  like the isolation choice in pattern 2. Every region needs its own
  independent proxy team (or teams).
- **Performance benefit within a region**: even for AZs within a single
  region (where cross-AZ attach *is* usually possible), keeping a proxy
  team local to the AZ it's protecting avoids unnecessary cross-AZ
  network traffic for volume attach and data transfer, and keeps backup
  latency proportional to local, not inter-AZ, network characteristics.
- **Composition**: region-scoping is the **outermost, structural** split.
  Within one region's proxy team(s), the shared-vs-project-scoped choice
  from pattern 2 still applies independently — a deployment can run a
  shared regional team for most tenants and carve out project-scoped
  teams within that same region for specific large tenants.

## 5. How the patterns compose

Region-scoping is mandatory once more than one region is in play; teaming
is the mechanism every pool uses regardless of scope; project-scoping is
an optional, per-tenant isolation upgrade layered on top. In practice, a
deployment's proxy topology decomposes as:

```
per region (mandatory split — Cinder/Nova can't attach across regions)
  └── shared team (default; teaming pattern applied across all
      │            projects assigned to this region's shared tier)
      └── ≥2 proxies, anti-affinity, one Bareos Client each

  └── project-scoped team(s) (opt-in, per large/regulated tenant)
      └── ≥2 proxies, anti-affinity, one Bareos Client each,
          dedicated to that one project
```

A single-region, single-tenant deployment collapses this to its simplest
form: one region (trivially satisfied), one shared-or-project-scoped team
(the distinction is moot with only one tenant) of ≥2 proxies. A
multi-region, multi-tenant cloud-provider deployment uses the full
structure — this is exactly what `TEAMING.md`'s onboarding tooling
(§4, `TEAMING_PLAN.md` T3/T5) automates: routing each tenant into the
right region's shared or dedicated team based on its configured tier.

## 6. Comparison

| | Shared, region-scoped | Project-scoped (per region) | Notes |
|---|---|---|---|
| Throughput | High — capacity pooled across all tenants in the region | Lower per-tenant ceiling unless separately sized generously | Shared pools smooth out bursty per-tenant load better |
| Blast radius (credential) | Bounded by `access_rules` per-tenant credentials | Naturally bounded — team only ever holds one project's credential | Both are workable; project-scoped needs less careful credential design |
| Blast radius (host) | Sequential, hygiene-dependent (§5/§9 of `DESIGN.md`) | Physical — another tenant's bytes never touch this team | Project-scoped is the stronger guarantee |
| Cost | Lowest — one pool serves many tenants | Highest — dedicated infrastructure per tenant | Cost scales with isolation |
| Operational complexity | Lower — fewer pools to manage | Higher — one more pool per opted-in tenant | |
| Cross-region | N/A within one pool | N/A within one pool | Region-scoping is orthogonal to this axis — always applies |

## 7. Sizing reference

Per-proxy sizing (vCPU/RAM, guest volume-attach ceiling, and the
snapshot-to-volume-copy cost risk on non-COW Cinder backends that affects
how long a proxy holds resources per job) is covered in `DESIGN.md` §7 and
§7.1 — this document is about *how many* proxies and *how they're scoped*,
not the per-proxy numbers themselves. Validate those per-proxy figures
first (during the plugin's own M1, `DESIGN.md` §14), then use them to size
team counts for whichever topology a deployment chooses here.

## 8. Relationship to `TEAMING.md`

This document defines the deployment shapes; `TEAMING.md` — companion
tooling shipped alongside this plugin, in this same directory —
implements the automation that makes teaming, project-scoping, and
region-scoping self-managing at real scale: health-checked dispatch and
failover (§2), per-tenant credential/ACL provisioning for project-scoped
and shared tiers alike (§4), and fleet-wide scheduling/concurrency
concerns across regions (§5). Keep the two documents consistent with each
other — a topology change here (e.g. a new pattern, a changed minimum
team size) should be reflected in `TEAMING.md`'s pool-topology
references, and vice versa. `CLIENT_GROUP_PROPOSAL.md` covers a further
option — a native Bareos Director feature that would eventually let the
Director itself resolve a proxy team, instead of `TEAMING.md`'s external
coordinator.
