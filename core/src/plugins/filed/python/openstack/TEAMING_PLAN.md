# Implementation Plan: Proxy Teaming Tooling

The actionable coding plan for the companion tooling in `TEAMING.md` — that
document covers architecture and rationale; this one turns its §1-§6 into
concrete tasks, file layout, and per-milestone deliverables. Section
references below (§N) point at `TEAMING.md` unless stated otherwise. This
tooling ships inside the plugin's own directory/package — there is no
separate repository or product to stand up.

## Prerequisites

Nothing here can be meaningfully tested end-to-end until the plugin itself
(`DESIGN.md`) has reached its **M3** (backup + all three restore modes
working — `DESIGN.md` §14). Two things can start earlier, against the
plugin's fake-backed test harness (`DESIGN.md` §12.2) rather than a real
proxy:
- The generator/onboarding CLIs (§3.1/§4.5 of `TEAMING.md`), since they
  only need to know the plugin's *option names* (`DESIGN.md` §10), not its
  runtime behavior.
- The coordinator's fake-backed tests (§6 of `TEAMING.md`), since they
  exercise scheduling/health-check logic against a mocked
  `python-bareos` and mocked OpenStack API, not a real proxy.

Before T1 starts, confirm against the plugin's own M1 output:
- The measured snapshot-to-volume-copy cost on at least one non-Ceph/SAN
  backend (`DESIGN.md` §7.1) — this number sets realistic per-proxy
  concurrency ceilings (§2.3 of `TEAMING.md`) rather than guessed ones.
- The plugin's actual `openstacksdk` minimum version (tied to the Caracal+
  floor, `DESIGN.md` §14) — this tooling tracks the same pin.

## File layout

Everything lives under this plugin's own directory, as an internal
package the debian/rpm package can optionally ship alongside
`bareos-fd-openstack.py` (see "Packaging" below) — not a separate
installable product:

```
core/src/plugins/filed/python/openstack/
  bareos-fd-openstack.py         (the plugin itself, DESIGN.md)
  bareos_openstack_api/          (if the plugin grows past one file, DESIGN.md §13)
  DESIGN.md
  PROXY_TOPOLOGY.md
  TEAMING.md
  TEAMING_PLAN.md                 (this file)
  CLIENT_GROUP_PROPOSAL.md        (separate track, no code here)
  tools/
    bareos_openstack_fleet/
      __init__.py
      config/
        __init__.py
        schema.py                 tenant/tier/pool config schema, TEAMING.md §2.2
      coordinator/                T1 — TEAMING.md §2
        __init__.py
        pool.py                   pool topology (shared / dedicated), PROXY_TOPOLOGY.md §2-§3
        health.py                 proxy health-check + circuit breaker
        dispatch.py               least-loaded/round-robin assignment
        state.py                  derive in-flight state from Bareos catalog + OpenStack tags
        cli.py                    `bareos-openstack-coordinator` entry point
      generator/                  T2 — TEAMING.md §3.1
        __init__.py
        instances.py               openstacksdk instance discovery/filtering
        render.py                  FileSet/Job .conf template rendering
        cli.py                     `bareos-openstack-genconfig` entry point
      gc/                          T2 — TEAMING.md §3.3
        __init__.py
        scan.py                    tagged-orphan discovery (images/snapshots/volumes/attachments)
        catalog.py                 python-bareos job-history cross-check
        cli.py                     `bareos-openstack-gc` entry point
      onboarding/                  T3 — TEAMING.md §4
        __init__.py
        credentials.py             access_rules application-credential provisioning
        bareos_resources.py        Catalog/Pool/Storage/Profile/Console creation
        acl.py                     PluginOptionsAcl regex generation
        cli.py                     `bareos-openstack-onboard` entry point
      billing/                     T3/T4 — TEAMING.md §4.6
        __init__.py
        report.py                  per-tenant byte rollup from the catalog
    tests/
      fakes/                       shared fake Keystone/Nova/Cinder/Glance + fake python-bareos
        openstack_fake.py
        bareos_fake.py
      unit/
        test_pool.py
        test_dispatch.py
        test_health.py
        test_generator.py
        test_gc.py
        test_onboarding.py
        test_acl_regex.py          property-based, TEAMING.md §6
      staging/                     opt-in, needs a real multi-proxy OpenStack — not run in default CI
        test_kill_proxy_midjob.py
        test_onboard_offboard_tenant.py
        test_gc_against_real_orphans.py
  ops/
    systemd/
      bareos-openstack-coordinator.service
      bareos-openstack-gc.timer / .service
    ansible/                       proxy provisioning (Nova anti-affinity group, boot-from-volume, bareos-fd install)
      provision_proxy_pool.yml
```

## Packaging

Ship `tools/` as part of the same `bareos-filedaemon-openstack-python-plugin`
package (`DESIGN.md` §13), or as a small additional package
(`bareos-filedaemon-openstack-python-plugin-tools`) depending on how heavy
its own dependencies turn out to be (`openstacksdk` is shared with the
plugin already; `python-bareos` and any templating library are the only
likely additions) — decide this during T1 once the dependency list is
concrete, not before. Either way, it's versioned and released with the
plugin, not on a separate schedule.

## Config model (needed before T1 can be coded meaningfully)

A single YAML config drives all the CLIs, since they all need to agree on
tenant/tier/pool definitions — see `TEAMING.md` §2.2 for the schema.
Task: finalize it in `config/schema.py` (with validation) as the very
first T1 task — every other component reads from it.

## T1 — Coordinator core

Goal: a proxy pool that survives one proxy dying mid-job, with no manual
intervention needed to keep backing up the rest of the fleet.

Tasks:
1. `config/schema.py` — load/validate the config model (`TEAMING.md`
   §2.2).
2. `pool.py` — resolve which proxies (Bareos Client names) belong to a
   given pool, for the shared tier only to start
   (`PROXY_TOPOLOGY.md` §2). Dedicated-tier pool resolution is a T5 task,
   but the data model should not need to change for it later — validate
   that now.
3. `health.py` — `status client=<name>` via `python-bareos`, N-failures-out
   / M-successes-back-in circuit breaker (`TEAMING.md` §2). Unit test
   against `tests/fakes/bareos_fake.py`.
4. `state.py` — derive "what's currently running where" from
   `python-bareos` active-job queries + OpenStack resource tags
   (`DESIGN.md` §5/§9) on every coordinator tick, rather than an in-memory
   dict — this is what makes restart-safety fall out for free instead of
   needing a separate persistence layer.
5. `dispatch.py` — resource-derived capacity (start with a fixed
   provisional constant per proxy, replace with the CPU/RAM-derived
   formula once the plugin's M1 sizing numbers, `DESIGN.md` §7, are in),
   least-loaded-then-round-robin assignment (`TEAMING.md` §2/§2.4),
   AZ/region locality preference.
6. `cli.py` — the coordinator daemon loop: tick → health-check → assign
   queued jobs → requeue failed jobs. Runs under the systemd unit in
   `ops/systemd/`.
7. Fake-backed tests (`tests/unit/test_pool.py`, `test_dispatch.py`,
   `test_health.py`): proxy health detection/rotation, least-loaded/
   round-robin dispatch, requeue-on-simulated-failure, restart-safety
   (kill and restart the coordinator process mid-run, assert no
   double-scheduling, no lost in-flight job) — per `TEAMING.md` §6.
8. `ops/ansible/provision_proxy_pool.yml` — Nova anti-affinity server
   group, boot-from-volume proxy instances, `bareos-fd` + plugin install.
   Manual failed-proxy replacement stays a documented runbook step here
   (`TEAMING.md` §2.1) — no automation task for that in T1.

Definition of done: kill a proxy mid-job in a real (or DevStack) multi-proxy
pool; the in-flight backup requeues onto a different proxy and completes;
the dead proxy is cleanly excluded from further scheduling without manual
config changes.

## T2 — Generator + GC CLI

Goal: fleets can be managed without hand-editing Bareos config, and leaked
OpenStack resources get found and removed automatically.

Tasks:
1. `generator/instances.py` — `openstacksdk` instance listing by
   `bareos_backup=yes` tag / `--project`, optional `bareos_workload=`
   grouping (`TEAMING.md` §3.1).
2. `generator/render.py` — Jinja2 (or plain string templates) for
   `bareos-dir.d/fileset/openstack-<uuid>.conf` and
   `bareos-dir.d/job/openstack-<uuid>.conf`; idempotent (re-running
   produces no diff if nothing changed) so it's safe to run from cron.
3. `generator/cli.py` — `bareos-openstack-genconfig --region ...
   [--project ...]`, writes config, optionally runs `bconsole reload` at
   the end.
4. `gc/scan.py` — list `created-by=bareos`-tagged Glance images / Cinder
   snapshots / volumes / attachments older than a threshold
   (`TEAMING.md` §3.3).
5. `gc/catalog.py` — cross-check against `python-bareos` job history;
   only delete objects with no corresponding recent successful Job.
6. `gc/cli.py` — `bareos-openstack-gc [--dry-run]`; the systemd timer in
   `ops/systemd/` runs this hourly.
7. Fake-backed tests: generator produces the exact expected `.conf` output
   for a given fake instance list; GC correctly identifies and removes
   tagged orphans while leaving untagged/non-Bareos resources alone
   (including a case with an object that *looks* orphaned but has a recent
   successful Job — must not be deleted).

Definition of done: point the generator at a test project, get correct
FileSet/Job config for every tagged instance; deliberately orphan a
snapshot and a stuck attachment, confirm `bareos-openstack-gc` removes
both and nothing else.

## T3 — Multi-tenant onboarding

Goal: onboarding a new tenant is one command, and that tenant can
self-serve restores without seeing or reaching any other tenant's data.

Tasks:
1. `onboarding/credentials.py` — create a Keystone application credential
   scoped with `access_rules` limited to exactly the API calls the plugin
   needs (list/create/delete Nova image snapshots, Cinder volume
   snapshots, volume attach/detach, server list/show) — `TEAMING.md` §4.1.
2. `onboarding/bareos_resources.py` — create/confirm Catalog (shared vs.
   dedicated per config), Pool/Storage, Profile, Console for the tenant
   (`TEAMING.md` §4.2-§4.4).
3. `onboarding/acl.py` — generate the tenant's `PluginOptionsAcl`
   full-string regex from their allowed instance UUIDs + their own
   project id (`TEAMING.md` §4.3). This is the most failure-prone piece in
   the whole tooling (brittle by design, per `TEAMING.md` §4.3) — budget
   real time for the property-based test suite below, not just the
   happy-path generator.
4. Wire `onboarding/cli.py` (`bareos-openstack-onboard <tenant-id>`) to
   call all of the above plus `generator` (T2) in sequence, per the
   tenant's config entry.
5. `tests/unit/test_acl_regex.py` — property-based: generate many random
   Plugin Options strings, assert the generated regex accepts every one
   that should be legal for the tenant and rejects every one that isn't
   (wrong `target_project`, wrong `instance`, unauthorized `force`) — per
   `TEAMING.md` §6.
6. `billing/report.py` — per-tenant `llist jobs client=...` byte rollup
   (`TEAMING.md` §4.6); simple enough to ship alongside onboarding rather
   than as its own milestone.

Definition of done: `bareos-openstack-onboard acme-prod` produces a
working, logged-in-testable Console for that tenant whose restore options
are provably (via the property tests) unable to target another tenant's
project or instances.

## T4 — Fleet-scale concurrency hardening

Goal: a full-fleet nightly run doesn't trip OpenStack API rate limits or
create a scheduling thundering-herd.

Tasks:
1. Add per-project bounded-concurrency + jitter to the coordinator's API
   calls (`TEAMING.md` §5.1) — wrap OpenStack SDK calls used during
   dispatch in a per-project semaphore/rate limiter.
2. Add schedule-time jitter to the generator's emitted Job schedules
   (`TEAMING.md` §5.4) — hash tenant id into an offset within the
   tenant's configured window.
3. Document (not necessarily automate in T4) migrating Storage/Device to
   the `dplcompat` S3-compatible backend for elastic write concurrency
   (`TEAMING.md` §5.2) — an ops runbook change, not new Python code,
   unless a deployment specifically needs it automated.

Definition of done: a simulated full-fleet run (many tenants, many
instances, against the fake backend) shows bounded/staggered API call
timing rather than a synchronized burst at the top of the run.

## T5 — Dedicated tiers, Masakari, proxy self-healing

Goal: the opt-in upgrades from the isolation-tier menu (`TEAMING.md` §4)
and proxy-fleet resilience menu (`TEAMING.md` §2) actually exist, not just
as documented options.

Tasks:
1. Dedicated per-tenant proxy team provisioning (`pool.py` extended,
   `provision_proxy_pool.yml` parameterized per-tenant) —
   `PROXY_TOPOLOGY.md` §3.
2. Dedicated Catalog/Storage provisioning path in `onboarding/`
   (`TEAMING.md` §4.2/§4.4) — mostly config-driven given T3's groundwork.
3. Masakari adoption path (`ops/ansible/` — how to enable it for the proxy
   server group) — `TEAMING.md` §2.1.
4. Automated proxy self-healing: watchdog detects a dead proxy and
   triggers `nova server rebuild`/recreate automatically, replacing the
   manual runbook step from T1 — `TEAMING.md` §2.1. Deliberately last,
   since it's the least load-bearing-for-correctness item in the whole
   plan (T1's coordinator requeue logic already keeps backups working
   without this).

Definition of done: a tenant can be promoted from shared to dedicated tier
without service interruption; a killed proxy is automatically replaced
without operator action.

## Cross-cutting checklist (applies to every milestone)

- **Secrets**: never log a tenant's `application_credential_secret`, even
  at debug level — mirror the plugin's own `#enc`/`DebugMessage`-only
  discipline (`DESIGN.md` §4).
- **Idempotency**: every CLI here (`genconfig`, `onboard`, `gc`) should be
  safe to re-run — re-running `bareos-openstack-onboard` on an
  already-onboarded tenant should reconcile, not duplicate or error.
- **Dry-run**: `gc` and `onboard` both support `--dry-run` from their first
  version, not bolted on later — these are the two commands most likely to
  be run against production by mistake.
- **CI**: unit tests (fake-backed) run on every change; `tests/staging/`
  tests are explicitly excluded from default CI and documented as
  "run manually against a real multi-proxy OpenStack before a release."

## Relationship to `CLIENT_GROUP_PROPOSAL.md`

Entirely independent track. If that core-Director feature ships, T1's
coordinator (`pool.py`/`health.py`/`dispatch.py`) becomes optional for
basic dispatch/failover — a deployment could switch to native
`ClientGroup = "..."` Jobs instead. T2-T5 (generator, GC, onboarding,
billing, dedicated tiers) are unaffected either way, since none of them
are job-dispatch concerns.
