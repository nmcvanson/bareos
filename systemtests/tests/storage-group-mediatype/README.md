# Systemtest for storage groups whose members differ

Two Storage resources, each with **its own archive directory** — which is the whole point. The older
`storage-group-run` test points both of its devices at the same directory, so it cannot observe where
data physically lands, and six green cases there proved less than they appeared to.

```
FileA -> device FileStorageA, Media Type FileA, storage/a, device limit 1
FileB -> device FileStorageB, Media Type FileB, storage/b
```

Each case checks that these agree: the device actually written, the volume's catalog `MediaType`,
its catalog `Storage` and the directory the volume file is really in — and that a restore reads the
same data back from the same device.

## The failure this was built to reproduce

The Director chose one member and recorded its Media Type and `StorageId` against the volume, but
`ReserveWriteDevice` announced **every** member of the list and let the Storage Daemon reserve
whichever device it could. Under contention the daemon picked a different member, and nothing
detected it.

Case 3 — `FileStorageA` busy, so the daemon took `FileStorageB` instead:

```
JobId 6: Storage group: 2 candidates, using "FileA" (Job resource, policy ListedOrder).
JobId 6: Using Device "FileStorageB" to write.
bareos-sd JobId 6: Labeled new Volume "Full-0003" on device "FileStorageB" (storage/b).
```

The catalog said media type `FileA` and storage `FileA`; the label physically on the volume said
`FileB`, confirmed with `bls`; the file was in `storage/b`. The backup reported
`Termination: Backup OK` and **the restore hung** on a mount request that could never be satisfied.

## What fixed it

The Director now offers the Storage Daemon **only the member the policy chose**, instead of the whole
list — see `OpenMessageChannelToWriteStorage` in `core/src/dird/backup.cc`. The daemon has no alternative to substitute, so the Director's choice is
binding and everything derived from it describes what actually happened.

With the fix, case 3 reads:

```
   Director chose    : FileA
   SD wrote to device: FileStorageA
   volume Full-0001: catalog MediaType=FileA Storage=FileA, file in storage/a
   restore (JobId 7): T
```

Note that `FileStorageA` is busy in that case: the daemon now **waits** for the chosen device rather
than quietly writing somewhere else. That is the intended trade — correctness over an unrequested
substitution.

## The cases

Cases 1–4 use `FileA`/`FileB`, which have **different** media types. Cases 5, 6 and 10 use
`FileC`/`FileD`, which **share** one media type — the configuration the first failure pushes an
administrator into.

| # | Setup | Asserts |
|---|---|---|
| 1 | Idle, `ListedOrder`, declared `FileA, FileB` | chooses `FileA` |
| 2 | Idle, `ListedOrder`, declared `FileB, FileA` | chooses `FileB` |
| 3 | `FileStorageA` busy (device limit 1), `ListedOrder` | chooses `FileA` **and writes to it** — the case that exposed the first failure |
| 4 | Same load, `LeastUsed` | chooses `FileB`, because `FileA` has a running job |
| 5 | Six alternating backups across `FileC`/`FileD`, shared media type | **no volume condemned** — the case that exposed the second failure |
| 6 | Same, but with `FileD` **disabled at runtime** | no volume condemned — the case that exposed the gate defect below |
| 7 | `FileUnreachable, FileA`, where the first member's daemon is not there | the job **fails over** to `FileA` and terminates OK |
| 8 | a **copy** job whose `Next Pool` declares `FileA, FileC`, with `FileStorageA` busy | the copy's volume is physically where the catalog says it is |
| 9 | `backup-mixed` writing a bootstrap file | the file names the Storage and the device that were written |
| 10 | restore the last job each shared-media-type member wrote | each restore reads from the device that wrote it, and the data matches the source |
| 11 | `list jobs storage=FileC` / `storage=FileD` / an unknown name | each member lists exactly its own jobs; an unknown storage is refused |

## Running it

```sh
cd <build>
ctest -R "system:storage-group-mediatype" --output-on-failure
```

Read the report rather than trusting the exit status — a case that silently never ran still looks
like a pass:

```sh
cd <build>/systemtests/tests/storage-group-mediatype
./test-setup
./testrunner-storage-group-mediatype
cat tmp/mediatype-report.txt
```

`test-cleanup` deletes `tmp/`, so copy the report out before cleaning up.

## Case 5 — the second failure, shared Media Type

`FileC` and `FileD` share one Media Type (`FileShared`) across `storage/c` and `storage/d`. This is
the configuration an administrator is pushed into to avoid the first failure, and it failed
differently: volume selection filtered on pool and media type only, with **no storage predicate for
disk devices**, so whichever member ran next was handed the previous member's appendable volume,
could not find the file in its own directory, and marked a healthy volume `Error`. Six backups
produced five condemned volumes and every job still reported `Backup OK`.

The data was never lost. What was destroyed is volume rotation: an `Error` volume is excluded from
the recycle query but still counts against `Maximum Volumes`.

This one reproduces with **none** of the storage group policy code — two disk storages sharing a
Pool and a Media Type are enough.

The case runs six alternating backups and asserts that nothing was condemned. With the fix:

```text
| mediaid | volumename | volstatus | mediatype  | storage |
|       3 | Full-0003  | Append    | FileShared | FileC   |
|       4 | Full-0004  | Append    | FileShared | FileD   |
```

Two volumes, one per member, both healthy and reused. The fix: volume search is scoped to the
chosen member for a grouped job (`FindNextVolumeForAppend` in `core/src/dird/next_vol.cc`), and that
scope survives the retry which otherwise gives up the restriction.

## Case 6 — a group that has lost a member

Added after the container stack found a defect that cases 1–5 could not see.

The scoping in case 5 was gated on a job "having a group", and that was decided by measuring the
storage list. But the policy **rebuilds** that list with only the surviving candidates, so the moment a
member is dropped for `Enabled = no` the list is down to one, the job stops looking grouped, the
scoping switches off, and case 5's attrition comes straight back. Every test kept both members
enabled, so none of them could see it. `disable storage=` in a running stack produced it immediately.

The fix records the fact at job setup instead of re-deriving it later, and case 6 covers it:
`disable storage=FileD`, two backups, `enable storage=FileD`, asserting nothing was condemned.

> **The order matters, and it is load-bearing.** Unscoped volume selection takes the **most recently
> written** appendable volume. Case 5 ends on `backup-shared-reversed`, which writes via `FileD`, so
> `FileD` owns that volume. Disabling `FileC` instead would send the job to `FileD`, which would then
> pick its own volume and prove nothing — the first version of this case did exactly that and passed
> with the bug reinstated. It must disable the member that **owns the newest volume**, leaving the
> other one running and offered a volume it cannot find. Do not "simplify" this.

## Case 7 — failover to the next member

`FileUnreachable` carries the real address and password and `sd2_port`, which is allocated to this
test and which nothing in it ever listens on. It is therefore unreachable by construction and cannot
collide with another test running in parallel. `ListedOrder` puts it first, so the Director has to
try it, fail, and move on.

```text
Storage group: 2 candidates, using "FileUnreachable" (Job resource, policy ListedOrder).
Warning: Could not connect to Storage daemon on ...:30605. ERR=Connection refused
Warning: Storage group: "FileUnreachable" could not take the job, trying "FileA".
Using Device "FileStorageA" to write.
Storage group: failed over to "FileA" after 2 attempts.
Termination:            Backup OK
```

The case covers three changes at once, and fails differently if any is reverted:

- **The failure must not be fatal.** The socket layer reports a refused connection with
  `Jmsg(M_FATAL)`, and `M_FATAL` sets `JS_FatalError` inside the message handler where no loop can
  intercept it, so the job used to die on its first candidate with a working member next in line.
- **The member must be tried at all.** `FileUnreachable` differs from `FileA` by port, so it was a
  different Storage Daemon, and a group used to be confined to one: the member was dropped before
  the policy saw it and the job ran on `FileA` with nothing to fail over from.
- **The loop must advance.** Stopping after the first candidate fires four of this case's
  assertions.

`StorageGroupConnectTimeout = 5` in this test's Director config, so the case measures the failover
rather than the timeout.

> **Why an unreachable daemon rather than a missing device.** The first design for this case gave
> `FileUnreachable` a `Device =` naming a device the Storage Daemon does not have. That member
> reserves *successfully*: with `JustInTimeReservation = Yes`, the default since 23.1.0,
> `reserve.cc` skips reservation entirely for an append job and replies `OK_device` with the literal
> name `"JustInTime Device"`, binding a real device only at the first data record. Under stock
> settings a device reservation cannot fail, so an unreachable daemon is the only failure the loop
> can see.

## Case 8 — the Next Pool list is a storage group too

Migrate, copy and VirtualFull take their write storage list from `Next Pool`, and no storage group
directive touches that path: no policy, no `Enabled` filter, no log line. The whole list used to go
to the Storage Daemon, which reserved whichever device it could, while `CopyWstorage` had already
pointed `write_storage` at the list's **first** member and every volume the job wrote was recorded
against it. The same divergence as case 3, reached a different way.

`CopyDst` declares `Storage = FileA, FileC`. With `FileStorageA` busy, the two behaviours differ:
offered the whole list the daemon takes `FileStorageC` and the catalog is wrong; offered one
storage it waits for `FileStorageA` and the catalog is right. The case asserts on the volume's
**physical location**, not on a log line.

> **Four things this case has to get right, each of which it got wrong first.**
>
> - It must not read `Using Device` out of the shared job log. The first version did, matched the
>   line belonging to `backup-slow-a`, and passed while the copy terminated *"Copying -- no files
>   to copy"*. There is now an explicit guard that fails the case if no `CopyDst` volume exists.
> - `backup-copysrc` must override **`Full Backup Pool`** as well as `Pool`. `DefaultJob` sets
>   `Full Backup Pool = Full`, and the level-specific pool wins for a Full backup, so setting
>   `Pool` alone sends the backup to the wrong pool and leaves the copy nothing to do.
> - `CopySrc` must read from `FileB`, not `FileA`. The Director refuses a copy whose read and write
>   storage are the same resource.
> - The substitute candidate must be a **third** device. With the read side on `FileB`, the daemon
>   will not hand `FileStorageB` to the write side of the same job either, so it waits for
>   `FileStorageA` whether or not the narrowing is in place and the case proves nothing. `FileC`,
>   in `storage/c`, is visible.
>
> Also: a Copy job is a **control** job. It queues a child and terminates `Copying OK` at once, so
> waiting on the id `run` returned comes back while the child is still reading. Use `wait`.

## Case 10 — a restore reads from the member that wrote

A restore does not apply the policy: it reads each volume from the Storage the catalog records against
it. With `FileC` and `FileD` sharing a Media Type, either could mount the other's volume as far as
the media type goes, but only the member that wrote it has the file — so a wrong catalog record
would send the restore to a directory where the volume is not, and it would never complete.

The case restores the last job cases 5 and 6 ran through each member, compares the data with the
source, and checks that the Storage Daemon read from the device that wrote it. Cases 1–4 make the
same three checks for their own restores.
