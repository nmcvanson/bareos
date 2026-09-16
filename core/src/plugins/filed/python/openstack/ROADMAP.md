# OpenStack Plugin Initiative: Full Plan & PR Roadmap

Single entry point tying together every document in this directory, the
full cross-track milestone timeline, and the concrete pull-request plan
for landing it in `bareos/core`. Read this first; it links to the detail
docs rather than repeating them.

Current state: all five documents below exist as **untracked, uncommitted
files** in this working tree (on top of `bareos/core` @ `cf4a1c928`) — no
PR has been opened yet. §3 is the plan for that.

## 1. What's being built (one line each)

| Document | What it covers |
|---|---|
| [`DESIGN.md`](DESIGN.md) | The plugin itself, `bareos-fd-openstack` — agentless Nova/Cinder backup and restore. A self-contained, mergeable `bareos/core` FD plugin, same shape as Proxmox/Incus/VMware/Libcloud. |
| [`PROXY_TOPOLOGY.md`](PROXY_TOPOLOGY.md) | Reference architecture for deploying more than one proxy: teaming, project-scoped, and region-scoped patterns, and their HA/performance tradeoffs. Documentation only, no code. |
| [`TEAMING.md`](TEAMING.md) | Companion tooling shipped **with** the plugin (same package): a proxy-pool coordinator, fleet generator, garbage collector, and multi-tenant onboarding — the practical, available-now way to get Commvault-VSA-style teaming. |
| [`TEAMING_PLAN.md`](TEAMING_PLAN.md) | The coding plan for `TEAMING.md` — file layout, task breakdown, definition-of-done per milestone (T1–T5). |
| [`CLIENT_GROUP_PROPOSAL.md`](CLIENT_GROUP_PROPOSAL.md) | The *actually* native answer: a proposed Bareos Director core feature (`Client Group` resource) that would let the Director itself pick and fail over between proxies, no companion process at all — grounded in the real Director source (`dird_conf.cc`, `job.cc`, `ua_run.cc`). Independent, longer-horizon track (C1–C5), gated on upstream maintainer buy-in. |

## 2. Full milestone timeline (all tracks, one picture)

Three independent tracks, each with its own milestone doc — shown here
merged into one timeline so dependencies between tracks are visible at a
glance. Track letter shows origin: **M** = plugin (`DESIGN.md` §14),
**T** = teaming tooling (`TEAMING_PLAN.md`), **C** = Director core feature
(`CLIENT_GROUP_PROPOSAL.md` §5).

```
Plugin (M)         M1 → M2 → M3 → M4 → M5
                    │    │    │         │
                    │    │    │         └─ v2 incremental (chunk-hash)
                    │    │    └─ restore (all 3 modes) — required before
                    │    │       T1–T3 can be tested end-to-end
                    │    └─ backup (Full only) — required before T1/T3's
                    │       fake-backed tests are meaningful
                    └─ read-only PoC; measures §7.1's snapshot-copy cost,
                       which feeds T1's proxy-capacity sizing

Teaming (T)                  T1 → T2 → T3 → T4 → T5
                              │    │    │
                              │    │    └─ needs plugin M3 (restore modes,
                              │    │       force=yes semantics) for onboarding's
                              │    │       PluginOptionsAcl generation to be
                              │    │       testable against real behavior
                              │    └─ needs plugin M2 (tagging convention, §9)
                              │       for GC to have anything real to find
                              └─ can start once plugin M2 exists (fake-backed
                                 tests only); real end-to-end needs M3

Director core (C)   C1 → C2 → C3 → C4 → C5
                     └─ upstream discussion — can start anytime, does not
                        block or get blocked by M or T. Realistically the
                        slowest track (external review, core-affecting).
```

Practical reading: **M1→M2→M3 is the critical path.** Nothing else can be
validated end-to-end until M3 lands. T1/T2 can be coded and unit-tested
in parallel with M2/M3 (against fakes), but T3 needs real M3 behavior.
C-track is fully decoupled and should be kicked off early (§3) precisely
*because* it's slow and gated on external parties, not because it's more
important.

## 3. Pull request roadmap

Bareos core PRs are reviewed as focused, self-contained changes (see the
existing Proxmox/Incus/VMware plugin history in this tree for scale
reference — each landed as a small number of tightly-scoped PRs, not one
giant one). This plan follows that norm rather than one PR per milestone
or one PR for everything.

Status legend: **Not started** (no branch yet) / **Draft** (branch
exists, not yet opened) / **Open** (PR # filled in, under review) /
**Merged**. Update this table in place as PRs progress — it's meant to be
the live tracker, not a one-time plan.

| # | PR title | Covers | Depends on | Status | PR link |
|---|---|---|---|---|---|
| PR-1 | `docs: add OpenStack plugin design documents` | This directory's 5 `.md` files, docs-only, no code | — | Not started | _(fill in once opened)_ |
| PR-2 | `filed: add bareos-fd-openstack plugin (backup)` | `DESIGN.md` M1+M2 — auth, instance resolution, volume/image ingestion paths, cleanup/tagging (§9) | PR-1 (docs land first so reviewers have context) | Not started | |
| PR-3 | `filed: bareos-fd-openstack restore support` | `DESIGN.md` M3 — all three restore modes, §6.1 old-volume handling, §12.1 automated safety tests | PR-2 | Not started | |
| PR-4 | `filed: bareos-fd-openstack packaging, docs, systemtest` | `DESIGN.md` M4 — debian/rpm packaging, `OpenStackPlugin.rst.inc`, `systemtests/tests/py3plug-fd-openstack/` | PR-3 | Not started | |
| PR-5 | `filed: bareos-fd-openstack incremental backups (v2)` | `DESIGN.md` M5 — chunk-hash incremental on the volume path | PR-4 | Not started | |
| PR-6 | `filed: openstack proxy-team coordinator` | `TEAMING_PLAN.md` T1 — coordinator core, fake-backed tests | PR-2 (needs the plugin's tagging/option contract to exist) | Not started | |
| PR-7 | `filed: openstack fleet generator + GC` | `TEAMING_PLAN.md` T2 | PR-3 (GC needs real restore/force semantics to reason about) | Not started | |
| PR-8 | `filed: openstack multi-tenant onboarding` | `TEAMING_PLAN.md` T3 — credential/ACL provisioning, `PluginOptionsAcl` regex generation + property tests | PR-3, PR-7 | Not started | |
| PR-9 | `filed: openstack fleet concurrency hardening` | `TEAMING_PLAN.md` T4 | PR-6, PR-7 | Not started | |
| PR-10 | `filed: openstack dedicated tiers, Masakari, proxy self-healing` | `TEAMING_PLAN.md` T5 | PR-8, PR-9 | Not started | |
| PR-11 | `dird: RFC discussion — Client Group resource` (GitHub Discussion/Issue, not a code PR) | `CLIENT_GROUP_PROPOSAL.md` C1 | — (independent of everything above) | Not started | |
| PR-12 | `dird: add Client Group config resource` | `CLIENT_GROUP_PROPOSAL.md` C2 — resource + parser + `ClientGroup =` directive, no dispatch logic yet | PR-11 (maintainer buy-in first) | Not started | |
| PR-13 | `dird: Client Group dispatch resolution` | `CLIENT_GROUP_PROPOSAL.md` C3 — least-loaded/round-robin resolution at job dispatch | PR-12 | Not started | |
| PR-14 | `dird: Client Group health tracking + failover` | `CLIENT_GROUP_PROPOSAL.md` C4 — failure/recovery thresholds, reschedule-time re-resolution | PR-13 | Not started | |
| PR-15 | `dird: Client Group systemtests + docs` | `CLIENT_GROUP_PROPOSAL.md` C5 | PR-14 | Not started | |

### 3.1 Sequencing notes

- **PR-1 first, docs-only**: gives reviewers the full design context
  before any code lands, and is trivially reviewable/mergeable on its own
  — matches how a controversial-scope plugin (this one touches Nova/
  Cinder resource lifecycle, not just a passive backup read) benefits
  from establishing agreement on the design before the diff shows up.
- **PR-2 through PR-5 are the plugin's own critical path** (§2) — strictly
  sequential, each unlocks the next. This is also the minimum needed
  before `TEAMING.md`'s tooling (PR-6+) can be tested end-to-end.
- **PR-6/PR-7 can start in parallel with PR-3/PR-4** if resourcing allows
  more than one contributor — both depend only on PR-2's contract
  (plugin options + tagging convention), not on restore being finished.
  PR-8 is the one that genuinely needs PR-3's restore/force semantics.
- **PR-11 (C-track) should be opened early**, not last — it's an RFC/
  discussion, not code, costs little to start, and is realistically the
  slowest-moving item here (external maintainer review of a
  scheduling-semantics core change). Opening it in parallel with PR-1/PR-2
  means the discussion has time to happen while the plugin itself is
  being built and reviewed, instead of only starting once everything else
  is already merged.
- **PR-9/PR-10 and PR-12 onward are all explicitly lower priority** than
  getting the plugin (PR-2 through PR-5) merged and working in the field —
  don't let fleet-scale polish or the core-feature track block or delay
  shipping the plugin itself.

### 3.2 What "done" looks like at each stage

- **After PR-5**: the plugin is a complete, standalone, mergeable
  contribution — usable today by anyone running a single proxy per
  project/region (`DESIGN.md` §10.3), with no dependency on anything else
  in this table.
- **After PR-8**: a cloud provider can run a multi-tenant, multi-proxy
  deployment with automated teaming and self-service restore, entirely
  with tooling that ships alongside the plugin.
  - **After PR-10**: the full menu from `PROXY_TOPOLOGY.md`/`TEAMING.md` is
    available — dedicated tiers, Masakari, automated proxy self-healing.
- **After PR-15** (if pursued): proxy teaming becomes a native Bareos
  Director capability, usable by any plugin with a multi-proxy pattern,
  not just this one — at which point `TEAMING.md`'s coordinator (PR-6)
  becomes optional rather than necessary.

## 4. How to keep this document current

- When a PR in §3 is actually branched, opened, or merged, update its
  **Status** and **PR link** columns in place — this table is the source
  of truth for where the initiative stands, not a point-in-time snapshot.
- If a milestone's scope changes in its own document (`DESIGN.md` §14,
  `TEAMING_PLAN.md`, `CLIENT_GROUP_PROPOSAL.md` §5), reflect that here too
  — the PR breakdown should always match what the milestone docs actually
  say, not drift from them.
