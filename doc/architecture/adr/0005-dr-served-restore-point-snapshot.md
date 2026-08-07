# ADR-0005: A DR Replica Serves the Local Snapshot Frozen at the Restore Point

- **Status:** Accepted — implemented on branch `7.x-dr`
- **Date:** 2026-08-07
- **Supersedes:** ADR-0004 D3 (keeping `CreateDRStandbyDistributedSnapshot()`), and with it
  the as-of-N distributed-snapshot builder of ADR-0001. ADR-0004's other decisions stand.

---

## Context

A DR replica recovers up to a distributed restore point *N*, pauses, and serves reads
there. That is the supported serving state (ADR-0004 D2). What was never specified is
what a read sees **during** the move from *N* to *N+1* — and the answer was: whatever
each node's replay happened to have reached.

Two measurements on the container fixture:

- **A read mid-advance is torn.** With WAL skewed so segment 0 replays ~500 MB between
  two restore points and segment 1 almost none, a single distributed transaction was
  observed half-committed across the segments for ~2.1 s (`0:1,1:1` → `1:1` → `0:1,1:1`).
  That state exists at neither restore point.
- **Holding the transaction does not help.** A `REPEATABLE READ` transaction spanning the
  advance read the old image, then the new one — same `now()`, clean `COMMIT`.

The advance is not rare: it is the normal operating rhythm of a DR replica. So the
guarantee the feature is sold on — "you are reading the cluster as it was at restore
point *N*" — held only while nothing was happening.

The first design attempt (approved, then discarded before implementation) was to freeze
the *distributed* snapshot: keep a redo-maintained `latestCompletedGxid` and pin an
as-of-*N* distributed snapshot. It is recorded under *Rejected alternatives* because the
two reasons it fails are the reasons the accepted design is shaped as it is.

---

## Decision

### D1 — Freeze the **local** snapshot on each node at the restore point; serve that

The distributed snapshot exists on a live cluster because there is no global order across
nodes. **A distributed restore point supplies exactly that order** — it is taken under
`TwophaseCommitLock` EXCLUSIVE, which is the whole premise of the feature. On a DR replica
the distributed layer is therefore redundant, and reconstructing it from redo is strictly
harder than not needing it.

Hot standby already maintains by redo the object a gxid reconstruction cannot produce:
`GetSnapshotData()`'s recovery branch builds `xmax = latestCompletedXid + 1` plus a
complete running set from `KnownAssignedXidsGetAndSetXmin()` — **including prepared
transactions**, which is precisely what a straddling 2PC transaction looks like on a
segment.

So: when the startup process applies the restore-point record and is about to pause, it
records that snapshot (`DRCaptureServedSnapshot()`, `xlog.c` →
`procarray.c`). Every read on the node is then answered from that image until the next one
is published, no matter how far replay has since run.

New shared memory (`dr_served_snapshot.{c,h}`), in its own `ShmemInitStruct` rather than
inside `TmControlBlock`, whose flexible array member must stay last.

### D2 — Publish only when **every** node has arrived

Two slots, `pending` and `served`:

- the startup process writes `pending` when *this* node pauses;
- `served ← pending` happens only once every node has arrived, which the coordinator
  establishes in `gg_dr_switch()` before dispatching the publish.

Publishing at each node's own pause would serve *N+1* on a node that got there early while
a lagging segment is still short of it — the same torn read, one restore point later. A
failed or cancelled switch simply leaves `served` at *N*: stale, but never wrong.

This is why `ggdr switch` is now a thin wrapper around `gg_dr_switch()` rather than a
per-node rearm-and-resume loop. Re-pointing each node by hand still parks them all in the
right place, but nothing would ever start serving it.

### D3 — Nothing to serve yet is a **query** error, not a snapshot error

A node that has never replayed to a restore point has only free-running state to offer.
`ExecutorStart()` refuses outright (`execMain.c`), on every node independently, so a
dispatched slice is refused on the segment that lacks a point even if the coordinator has
one.

The refusal is in the executor and not in `GetSnapshotData()` on purpose. Connecting to a
database needs a snapshot, as does every utility path that could repair the situation; a
replica that refuses snapshots is a replica nobody can connect to, and there is no way back
in from there. Failing at query start keeps `psql`, `ALTER SYSTEM` and `pg_ctl reload`
working.

The frozen image is also self-restoring after a restart: shared memory does not outlive the
postmaster, so a restarted node arrives at its target with nothing served and publishes on
the spot (`DRServedSnapshotPublishIfNothingServed()`). A node in that state is not
advancing between two served points — it is converging on its armed target from cold, as
are its peers — so the early publish cannot tear. The exception is a node that restarts
*during* an advance: it comes back, replays to the new target, and serves it while its
peers still serve the old one, until the coordinator's `gg_dr_switch()` publishes the new
point everywhere. That window is left open deliberately; every way of closing it puts the
node back in the unrecoverable state above.

### D4 — Force the visibility map to report nothing all-visible

`VM_ALL_VISIBLE` lets index-only and bitmap scans return tuples **without consulting the
heap at all**, so no snapshot — however carefully frozen — is applied to them. Production's
`XLOG_HEAP2_VISIBLE` records replay during an advance and mark pages all-visible that hold
rows the frozen image must not show. `visibilitymap_get_status()` returns 0 in DR mode.

No snapshot design can close this one. The cost is losing the index-only fast path on a
replica. It is safe for the only other consumer, vacuum, which never runs here.

### D5 — Drop the DR distributed snapshot, and with it two silent ratchets

`CreateDRStandbyDistributedSnapshot()` is deleted; `haveDistribSnapshot` stays false for a
DR read. Two consequences that were independently worth having, both of which fall out for
free because `XidInMVCCSnapshot()` and `GetSnapshotData()` short-circuit on that flag:

- **The sticky ignore hint bits.** `distribXid < ds->xminAllDistributedSnapshots → IGNORE`
  sets `HEAP_X{MIN,MAX}_DISTRIBUTED_SNAPSHOT_IGNORE` on the tuple, permanently. With the
  old builder's watermark-derived value this fired for essentially every tuple.
- **`DistributedLogShared->oldestXmin`.** `DistributedLog_AdvanceOldestXmin()` breaks only
  when `distribXid >= xminAllDistributedSnapshots`; with a watermark it never broke and
  ratcheted to the replay frontier, after which `DistributedLog_CommittedCheck` stopped
  answering. The `!haveDistribSnapshot` path calls the read-only
  `DistributedLog_GetOldestXmin()` instead.

Because the coordinator no longer sends a distributed snapshot, `setupQEDtxContext()` can
no longer use "has a distributed snapshot" as its proxy for "is a dispatched query"; on a
standby QE it asks directly (`IS_STANDBY_QE()`). Without that, reader gangs would stay in
`DTX_CONTEXT_LOCAL_ONLY` and never sync with their writer's shared snapshot.

#### D5.1 — ADR-0004 D3 kept the builder for a benefit the code did not deliver

ADR-0004 D3 declined to remove the in-doubt masking, on this argument: a transaction can be
distributed-committed on the coordinator and prepared-only on the segments at the cut, so
`BEGIN; CREATE TABLE t; INSERT t; COMMIT;` would leave `t` in the coordinator catalog and
absent on the segments; the masking "is what makes such a transaction invisible on both
sides".

It never made it invisible on the coordinator. `XidInMVCCSnapshot()` gates the entire
distributed block on `!IS_QUERY_DISPATCHER()` (`snapmgr.c`) — the coordinator has no
distributed visibility test at all and decides by local xid, which for that transaction is
committed. The masking only ever applied on the segments, where the transaction is
prepared-only and therefore already invisible through `KnownAssignedXids`.

So removing the builder loses nothing that was working. The coordinator-side asymmetry D3
describes is real, pre-dates this change, survives it, and is bounded by the microseconds
between the coordinator's distributed-commit record and
`doNotifyingCommitPrepared()` taking `TwophaseCommitLock` SHARED. When it does bite, a
dispatched query fails loudly (the segments have no such relation) rather than returning a
wrong answer. Closing it properly still means widening the barrier on production's commit
path, which ADR-0004 rejected and this ADR does not revisit.

### D6 — Conflicts are the safety net; set `max_standby_archive_delay = 0`

Serving a frozen image means publishing `MyPgXact->xmin` from it, which makes
`ResolveRecoveryConflictWithSnapshot()` cancel any reader whose image needs a tuple replay
has removed. That is the agreed policy — the right answer or none — with no retention and
no replay throttling.

It also makes conflicts common rather than exceptional, so the 30 s default would add 30 s
to every advance that hits one. `ggdr create-replica` writes `max_standby_archive_delay = 0`
(and the streaming twin). `-1` is not the opposite extreme but a trap: it blocks the startup
process inside redo, so replay stops altogether.

---

## Rejected alternatives

**Freeze the distributed snapshot** — keep a redo-maintained `latestCompletedGxid` (max over
replayed distributed commits) and pin an as-of-*N* distributed snapshot. Rejected on two
independent grounds:

1. **`max()` over replayed commits is not a valid `xmax`.** Transactions commit out of gxid
   order. Take T_A (gxid 100), prepared on the segments before *N*, whose coordinator
   distributed-commit record is written *after* *N*; and T_B (gxid 101), fully committed and
   forgotten before *N*. At *N* the horizon is 101, so `xmax = 102`, and T_A is in no
   in-doubt set because nothing has been logged for it yet. After the advance T_A is locally
   committed on the segments and `100 < 102` → visible in the "as-of-*N*" image. Ordinary
   overlapping transactions break it. A correct `xmax` needs the complete set of gxids below
   it that had not completed at the cut, and redo cannot enumerate that on the coordinator.
2. **The coordinator has no distributed test at all** (D5.1). Pinning only the distributed
   snapshot would leave the coordinator planning against catalogs at replay-now while the
   segments executed as of *N* — catalog skew that does not exist today. The distributed
   route *creates* a bug.

Both disappear under the local-snapshot design: `KnownAssignedXids` contains prepared
transactions natively, and local snapshots are what the coordinator actually uses.

**Retain WAL / throttle replay so readers are never cancelled.** Out of scope by the agreed
policy: correctness only, advance latency unchanged.

---

## Consequences

- A read served while the cluster advances from *N* to *N+1* returns the *N* image, or is
  cancelled. `ggdr pause` no longer changes what reads see at all — it stops the WAL, not
  the served point.
- **No catalog change: no `CATALOG_VERSION_NO` bump, no re-initdb.** Binary-only upgrade
  from the ADR-0004 build.
- Index-only and bitmap-heap scans lose their all-visible fast path on a replica (D4).
- A replica catching up to its first restore point refuses queries instead of answering
  from free-running replay state (D3). `ggdr create-replica` already waits for each node to
  come up before its post-build checks, so this shows up as a longer wait, not a failure —
  but a replica built with no `--pause-at` never reaches a servable state.
- One restart window stays open by design (D3, last paragraph).
- Verified end to end on the container fixture: **42/42** (M1+M2 7, M2' 5, M3 11, M5 6,
  `ggdr` 4, SQL recovery control 9), plus the V-20 dense fixture 5/5 and the `gg_walfilter`
  suite. The five new M3 assertions are the C-12 regression: rather than racing a real
  advance, the fixture *holds* the dangerous state — segments driven to `dr_rp2` while the
  coordinator stays at `dr_rp1` — and asserts a read still returns the `dr_rp1` answer,
  including through a forced index-only scan over a table production VACUUMed on both sides
  of `dr_rp1` (D4).
- Two bugs surfaced on the way, both now fixed here:
  - `DRServedSnapshotMaxXids()` originally called `GetMaxSnapshotXidCount()`, which reads
    `procArray->maxProcs` — but the shared-memory *sizing* pass runs before
    `CreateSharedProcArray()`, so every postmaster start (`initdb` included) segfaulted. The
    capacity is now spelled out from `MaxBackends`, with an assertion that the two agree
    once the proc array exists.
  - `gg_dr_switch()` resumed replay on *every* node, including one already stopped at the
    target, which sent it past a restore point with nothing ahead to stop at — so the wait
    loop never saw it paused and the switch hung. It now skips nodes that are already there.
