# Bareos Core Feature Proposal: `Client Group` (native proxy teaming)

This proposes an actual Bareos Director core (C++) feature: a `Client
Group` resource that lets a Job reference a *pool* of Clients instead of
one, with the Director itself picking a healthy member and failing over —
the same thing Commvault's CommServe does natively for VSA proxy teaming.
`TEAMING.md` builds the pragmatic version of this *without* core changes
(a companion coordinator process); this document is the "actually native,
no extra process" version, now in scope to plan concretely.

## 1. Why this needs a core change, grounded in the actual source

Checked directly against `core/src/dird/`:

- A Job's `Client` directive is `CFG_TYPE_RES`, pointing to exactly **one**
  `ClientResource` (`R_CLIENT`) —
  `core/src/dird/dird_conf.cc:299`:
  `{ "Client", CFG_TYPE_RES, ITEM(res_job, client), {config::Code{R_CLIENT}}}`.
- That single Client pointer is copied straight onto the job control
  record at job start —
  `core/src/dird/job.cc:1704`: `jcr->dir_impl->res.client = job->client;`
  — no resolution step, no pool, nothing dynamic.
- `bconsole run ... client=<name>` **already supports overriding** the
  Client at submission time (`core/src/dird/ua_run.cc`, the `client=`
  command-line option feeding `rc.client`, used at lines ~2257/2281) —
  this is the existing insertion point a dynamic-selection feature would
  extend, not a new mechanism to invent.

So: no pool/group concept exists today, but the codebase already has
exactly one place where "which Client does this Job actually use" gets
decided late (at run/submit time) rather than baked into static config —
that's where this feature hooks in.

## 2. Proposed design

### 2.1 New resource: `Client Group`

A new Director config resource, e.g. `bareos-dir.d/clientgroup/*.conf`:

```
ClientGroup {
  Name = "openstack-proxies-region1"
  Members = "proxy1-fd", "proxy2-fd", "proxy3-fd"
  Dispatch Policy = LeastLoaded    # LeastLoaded | RoundRobin
  Health Check Interval = 60
  Failure Threshold = 3            # consecutive failures before exclusion
  Recovery Threshold = 3           # consecutive successes before re-inclusion
}
```

Mirrors the circuit-breaker parameters `TEAMING.md` §2 already specifies
for the external coordinator — same policy, moved into Director core.

### 2.2 New Job directive

```
Job {
  Name = "openstack-web01"
  ClientGroup = "openstack-proxies-region1"   # instead of Client = "..."
  FileSet = "OpenStack-web01"
  JobDefs = "DefaultJob"
}
```

`Client` and `ClientGroup` are mutually exclusive on a Job/JobDefs,
validated at config-parse time (extend the existing config validation
path near `dird_conf.cc`'s Job resource item table).

### 2.3 Resolution logic

At the same point `job->client` is currently read directly (`job.cc:1704`
and the `run`-command override paths in `ua_run.cc`), insert a resolution
step:
- If the Job references a `Client`, behave exactly as today — zero change
  to that path.
- If the Job references a `Client Group`, resolve it to one concrete,
  healthy member Client using the group's dispatch policy, *then* proceed
  through the same code path a normal single-Client job already uses.
  Least-loaded needs a definition of "load" the Director can see cheaply
  — active-job count per Client is already knowable Director-side (it's
  tracking running JCRs); reuse that rather than inventing a new metric.
- Health state per member: the Director already knows when a Client is
  unreachable (connection failures surface today via `status client` and
  job failures). Extend that existing tracking with the
  failure/recovery-threshold counters from §2.1, scoped per Client Group
  membership rather than adding a wholly new subsystem.

### 2.4 Failover semantics

This is the core payoff: when a resolved Client turns out to be
unreachable (job fails to even start, or dies mid-job), a
`RescheduleOnError`-eligible Job re-resolves the `Client Group` on retry —
picking a *different* healthy member — rather than blindly retrying the
same dead Client, which is the actual limitation `TEAMING.md` §2 works
around externally today ("Bareos's `RescheduleOnError` ... by default
reschedule against the *same* `Client =`"). Fixing that at the
reschedule/requeue path in Director core is what turns this from "a
config convenience" into "actual native failover."

## 3. Relationship to `TEAMING.md`'s coordinator

Not a prerequisite for anything already planned — `TEAMING.md`'s
coordinator works today, independent of this. If/when `Client Group`
ships in Bareos core:

- Dispatch and failover (`TEAMING.md` §2) become native — a deployment can
  drop the external coordinator process for basic teaming and just use
  `ClientGroup = "..."` Jobs.
- Garbage collection, multi-tenant onboarding, and fleet-scale scheduling
  (`TEAMING.md` §3-§5) stay external regardless — those aren't job-dispatch
  concerns, and Director core has no reason to grow OpenStack-specific or
  multi-tenant-billing logic.
- This benefits **every** Bareos plugin with a multi-host backup-proxy
  pattern (VMware with multiple proxies, Proxmox across multiple cluster
  members, this OpenStack plugin), not just this one — a materially
  stronger case for proposing it to Bareos upstream than a
  single-plugin-specific feature would be.

## 4. Scope and effort, stated honestly

This is a real core feature, not a small patch:
- New config resource type + parser wiring (`dird_conf.cc`/`.h`) —
  moderate, follows an existing, well-understood pattern for adding a
  resource type.
- Resolution logic at job dispatch (`job.cc`, `ua_run.cc`) — moderate to
  substantial; needs care around every place that currently assumes
  `job->client` is already a concrete, resolved pointer.
- Health tracking — reusing existing Director-side Client connectivity
  awareness is the plan, but the exact current mechanism needs a closer
  read of the Director's connection-handling code before implementation
  starts (not yet verified beyond "the Director already knows when a
  Client is unreachable" at a high level).
- Testing: unit coverage plus `systemtests/tests/` coverage at the same
  rigor as any core Director feature — significantly more than a plugin's
  own test suite, since this touches shared scheduling code every Job
  type goes through.
- Docs: new directive description files under
  `docs/manuals/source/manually_added_config_directive_descriptions/`
  (`dir-job-ClientGroup.rst.inc`, etc.), following the existing pattern.

## 5. Recommended path

Because this touches core Director scheduling semantics shared by every
Job in every Bareos deployment, not just this plugin, treat it as its own
proposal track, not something folded silently into the OpenStack plugin
work:

1. **C1 — Upstream design discussion**: open a GitHub discussion/issue
   with Bareos maintainers describing this proposal (this document is the
   starting draft) before writing implementation code — gauge maintainer
   interest and get feedback on the resource/directive shape while it's
   still cheap to change. Consider filing a `docs/adr/` entry alongside
   this, matching how `0004-use-grpc-for-rpc-needs.md` and
   `0005-plugin-virtual-filenames.md` record this kind of Director-facing
   architectural decision in this tree.
2. **C2 — Config resource prototype**: implement and land the
   `Client Group` resource + parser + `ClientGroup =` Job directive, with
   validation, but *no* dynamic resolution yet (a group with one member
   behaves like today's `Client =`). Gets the config surface reviewed and
   stable before the harder scheduling logic.
3. **C3 — Resolution logic**: implement least-loaded/round-robin
   resolution at job dispatch, using existing active-job-count tracking.
4. **C4 — Health tracking + failover**: extend Client connectivity
   tracking with the failure/recovery thresholds, and make
   `RescheduleOnError` re-resolve the group rather than pin to the
   original Client.
5. **C5 — Core systemtests + docs**: full test coverage and directive
   documentation before calling this mergeable.

Not blocking on `TEAMING.md`'s own milestones (T1-T5) — the two tracks are
independent, and `TEAMING.md`'s coordinator remains the working answer for
proxy teaming while this track is in progress, however long that takes.
