# ADR-0006: Disaster-Recovery Read Replica for Greengage 7

- **Status:** Accepted — implemented and validated end to end on branch `7.x-dr`
- **Date:** 2026-08-08
- **Supersedes:** ADR-0001 … ADR-0005, which are **retained as the proof-of-concept
  record**. They document how the design was arrived at, including two mechanisms that
  were adopted and then reversed. Nothing in them is needed to understand, build, or
  review the feature as it stands; read them only for the history of a particular
  decision. This ADR is the single statement of what the feature *is*.
- **Companion:** `doc/architecture/greengage-dr-read-replica.md` — how the mechanisms
  work, with the code map. This ADR records *what was decided and why*, and what a
  developer must not break.

---

## Context

Greengage 7 (PostgreSQL 12 base, MPP fork) has intra-cluster HA — per-segment mirrors and
FTS-driven failover recorded in `gp_segment_configuration` — but nothing that survives
losing the whole primary site, and nothing that provides a continuously updated,
queryable, promotable copy of a cluster. VMware/Broadcom Greenplum sells this as GPDR
"Read Replica Mode"; there is no open-source equivalent.

The goal is a **second, independent Greengage cluster** — its own coordinator and primary
segments, its own dbids, possibly different hosts — that

1. is kept continuously up to date from production,
2. serves **read-only distributed queries** with **transactionally consistent**,
   cluster-wide results while remaining in recovery, and
3. can be **promoted in place** to a normal online read-write cluster.

Plain PostgreSQL hot standby solves neither of the two hard parts:

- **Topology.** A physical replica byte-replays production's WAL, including writes to the
  shared topology catalog `gp_segment_configuration`. Left alone, the DR cluster can only
  describe *production's* hosts, ports and dbids — never itself — so its own coordinator
  cannot dispatch to its own segments.
- **Distributed consistency.** Each node replays its own WAL at its own pace, and a
  distributed commit is two-staged across nodes. An arbitrary per-node cut (for example
  `min(replay_lsn)`) can catch a transaction committed on the coordinator but only
  *prepared* — invisible — on the segments. On top of that, a Greengage coordinator
  normally allocates a distributed xid (gxid) and opens a *writer* gang even for a pure
  `SELECT`; both are illegal in recovery.

Constraints that shaped the answer:

- PostgreSQL 12 base: no `max_slot_wal_keep_size`, so an unbounded replication slot is a
  production-outage risk. Recovery internals (`KnownAssignedXids`, restartpoints) are
  fixed.
- Production must not need to know a replica is attached — transport is one-way.
- Minimise new core surface: prefer shipped primitives and existing dispatch machinery
  over new distributed protocols.

---

## Decision

Build the replica as **an independent cluster in permanent per-content archive recovery**,
made self-describing by an **apply-time topology redo filter over a frozen-tuple seed**,
serving reads through a **no-gxid standby-reader dispatch context** on top of the existing
hot-standby dispatch, with consistency provided by **recovering up to a distributed
restore point and serving the local MVCC snapshot each node freezes there**, driven from
**SQL functions plus one `ggdr` utility**.

### The replica

#### D1 — Replication unit: per-content physical WAL into an independent cluster

Each DR node continuously replays the WAL of its production counterpart *by content id*:
DR coordinator ← production coordinator, DR segment *cN* ← production primary *cN*. The
**segment count must match**; host layout, hostnames, ports and dbids are free. Each node
keeps its own independent WAL/LSN space.

**Rejected:** *logical replication or ETL* — no PG12-era logical decoding for an MPP
catalog plus distributed transactions, and it loses physical fidelity (indexes, AO segment
files). *Shared-storage / block replication* — a cold standby that cannot serve reads and
ties both sites to one storage product. *A cross-cluster streaming protocol* — new
machinery Greengage does not have, where per-content physical WAL reuses stock recovery on
every node.

#### D2 — Transport: WAL archiving / continuous restore, not streaming

Production pushes completed WAL segments per content to a shared archive
(`archive_command` with the Greengage `%c` content-id escape); each DR node pulls with
`restore_command`. No replication slots, no libpq connection between the clusters.

A replication slot on PG12 pins production WAL with no cap, so a slow or offline replica
could fill production's `pg_wal`. An archive decouples the sites completely: production
cannot be harmed by DR lag, one faithful archive serves production PITR and N replicas and
cascades, and it is WAN-tolerant and restartable.

**Cost accepted: freshness.** The RPO floor is `archive_timeout` + transfer + restore
cadence + slowest-segment skew. This is a reporting/DR replica, not a synchronous standby.
The repository product (a plain directory with `cp` in the fixture, pgBackRest/WAL-G in
production) is deliberately orthogonal to the engine work.

#### D3 — DR mode is `hot_standby = on` while in recovery

```c
IsDRReplicaMode()  ==  EnableHotStandby && RecoveryInProgress()
```

There is no DR GUC and no marker file. `hot_standby = on` on a node in recovery *is* DR
mode: topology redo filter, read-only enforcement, standby distributed read, and the
frozen-snapshot serving path.

**Why not a second mode:** the predicate follows recovery rather than configuration, so
promotion disengages DR behaviour by itself — that is what makes promotion a single phase
with no restart. And `EnableHotStandby` is a postmaster GUC, so the startup process (redo
filter) and every backend (read-only enforcement) see the same value with no plumbing. A
marker-file gate previously needed a lazily-`stat()`ed per-backend cache precisely because
a backend never runs recovery, and getting that wrong had already made the read-only
guards silent no-ops once.

**Accepted cost — the mirror footgun.** `hot_standby` defaults to `off`, so mirrors and the
standby coordinator are unaffected in normal operation. But turning it on for one now
freezes that node's `gp_segment_configuration`, and a later FTS failover (or
`gpactivatestandby`) would promote a node describing a stale cluster. Documented rather
than defended against; the alternative reinstates the second key this removes. Test
scenario H-4 exists to keep the warning honest.

### Topology independence

#### D4 — Apply-time redo filter over a frozen-tuple seed

Two cooperating mechanisms let the replica describe itself while byte-replaying
production's WAL:

1. **Frozen-tuple seed (create time).** `ggseed_dr_topology`, in single-user mode,
   replaces `gp_segment_configuration`'s rows with DR-local rows and `VACUUM FREEZE`s them,
   so they are unconditionally visible with no CLOG dependency on production's XID
   timeline.
2. **Apply-time redo filter (forever after).** In the redo loop, a record whose block
   references *all* fall inside a protected set of shared topology catalogs is not applied
   — its `rm_redo` **and** its `checkXLogConsistency()` are both skipped
   (`dr_redo_filter.c`, wired in `StartupXLOG()`). The protected set is
   `gp_segment_configuration` plus its indexes and TOAST, `gp_configuration_history`,
   `gp_id` and `gp_version_at_initdb`, resolved to relfilenodes through the shared
   relmapper and matched only in the global tablespace. Replay LSN bookkeeping is
   untouched, so the node stays in lock-step with production's WAL position and DR
   `pg_wal` stays byte-identical to production's.

Two records name a relation in their payload rather than in a block tag, so the
block-reference rule cannot see them and each needs its own guard:

- **`relmap_redo`** — `DRRejectForbiddenRemap()` FATALs *before* any on-disk write if a
  protected catalog's relfilenode would change. That is `VACUUM FULL` / `CLUSTER` /
  `REINDEX` / `TRUNCATE` of a topology catalog on production: a loud, recoverable
  "re-create this node" stop instead of silent divergence.
- **`smgr_redo` truncate** — `XLOG_SMGR_TRUNCATE` for a protected relfilenode is skipped
  and logged. The DR's copy of these catalogs legitimately has a different page count from
  production's, because the seed's re-inserted rows can sit on a block production later
  frees; applying production's truncation would discard them. Whether it does depends on
  catalog size, which is why the dense fixture exists (scenario V-20).

**Rejected:** *rewriting or dropping topology records in transit* — you cannot delete bytes
from physical WAL (LSN is a byte offset, `xl_prev` chains records, page `pd_lsn` interlocks
with buffer state), and in-place rewriting either corrupts the canonical archive or forces
a second divergent stream. A same-length `XLOG_NOOP` rewrite on the restore path *is*
viable and was shipped once (`gg_walfilter`), then withdrawn as the default: it makes a
correctness invariant depend on configuration — a hand-edited `restore_command` silently
loses topology protection — it costs real CPU on the fetch path, and it stops DR `pg_wal`
being byte-identical to production's. **`gg_walfilter` survives as auxiliary C tooling**
(`src/bin/gg_walfilter`, installed to `$GPHOME/bin`) for offline inspection, forensics,
partial replicas and opt-in restore-path use.

*Production-side DDL bans on topology catalogs* were also rejected: production cannot know
a replica is attached, and routine `VACUUM` must stay allowed. The replica halts reactively
instead.

**Cost accepted:** a bounded, self-healing seed footprint. The seed's `VACUUM FREEZE`
updates a few `pg_class` rows in place (`pg_class` is deliberately not protected), so
production catalog changes co-located on those pages inside the sub-second
`(backup-LSN, seed-LSN]` window are LSN-shadowed until the next full-page image heals
them. A production-grade create utility should seed against a scratch copy.

### Read-only, and reads that dispatch

#### D5 — Executor gate plus DTM backstop; daemons quiescent structurally

- **Executor gate** (`ExecCheckXactReadOnly`): in DR mode anything that is not a pure
  `SELECT` — including `SELECT … FOR UPDATE/SHARE`, which stock hot standby *exempts* —
  fails with an explicit `cannot execute … on a read-only disaster-recovery replica`.
- **DTM backstop** (`currentDtxActivate`): allocating a gxid mutates cluster-wide
  `nextGxid` without writing WAL, so recovery alone would not catch it. In DR mode it
  errors. The standby-reader path never reaches it by construction; it is a guardrail.
- **Nothing disables FTS, autovacuum or DTX recovery.** A DR node stays permanently in
  `PM_HOT_STANDBY` and those launchers start only at `PM_RUN`. Chosen over sprinkling
  `IsDRReplicaMode()` checks through each daemon: structural quiescence cannot regress.

#### D6 — A no-gxid standby-reader context over the existing hot-standby dispatch

`DTX_CONTEXT_QD_STANDBY_READER` is selected whenever the dispatcher runs in DR mode. In it
the coordinator never activates a distributed transaction — no gxid, no WAL — and every
DTX-activation side path (InitPlan/subplan dispatch, internal subtransactions) is
short-circuited **keyed on `IsDRReplicaMode()`, not on the context value**, because those
paths run before the context has been resolved. That distinction was found the hard way and
is easy to reintroduce.

Gang creation and routing **reuse the pre-existing upstream hot-standby dispatch**
(`IS_HOT_STANDBY_QD()` / `IS_STANDBY_QE()`, mirror selection in `cdbutil.c`, the
`is_hs_dispatch` wire flag). Two targeted exemptions were needed:

- Plans that put read-only work on a *writer* gang (a coordinator-row `UNION ALL`
  segment-rows view, for instance) hit a QE check requiring a valid gxid, which a standby
  never has — exempted for `IS_STANDBY_QE()`.
- `setupQEDtxContext()` used "the dispatch carried a distributed snapshot" as its proxy for
  "this is a dispatched query". Since D8 sends no distributed snapshot, a standby QE now
  asks `IS_STANDBY_QE()` directly. Without this, reader gangs stay in
  `DTX_CONTEXT_LOCAL_ONLY` and never sync with their writer's shared snapshot.

**Rejected:** the bespoke snapshot-leader / gang-member-0 scheme, machinery to keep a
writer-less reader gang's local snapshots coherent under concurrent replay. It is
unnecessary once every node serves one frozen image (D8): reader gangs copy from their
writer's `SharedLocalSnapshotSlot` and the writer's snapshot is that image. This was the
single largest complexity avoidance in the project.

### Consistency

#### D7 — Recovery is supported only *up to a restore point*

The cluster recovers up to a **distributed restore point** *N*, pauses there, serves reads,
then advances to *N+1*, in a loop. The barrier is production-side:
`gp_create_restore_point()` holds `TwophaseCommitLock` EXCLUSIVE across dispatching the
restore-point record to every node, so no commit-prepared broadcast can straddle the cut
*at the segments*. A cross-node serve point exists exactly when **every node is stopped at
the same restore-point name**.

There is **no free-running mode**. Reaching any other position is still possible (`ggdr
pause`, or an advance in flight) but is for inspection, not for serving: no other cut has a
cross-node guarantee to offer, and a mode that serves reads there ships a caveat rather
than a feature.

**Rejected:** *`min(replay_lsn)` cuts* — unsound, because LSNs are not comparable across
nodes and the coordinator's `XLOG_XACT_DISTRIBUTED_COMMIT` is written outside the two-phase
barrier. *Free-running replay with a published gxid horizon* — a continuous-horizon
protocol, where Greengage already ships the pause primitive
(`gp_pause_on_restore_point_replay`, `pauseRecoveryOnRestorePoint()`).

#### D8 — Serve the local snapshot frozen at the restore point; no distributed snapshot

Pausing alone is not enough. While the cluster advances from *N* to *N+1* its nodes sit at
different WAL positions, so anything read from live state is torn — measured: a single
distributed transaction half-committed across segments for ~2.1 s, a state that exists at
neither restore point. Holding a `REPEATABLE READ` transaction open does not help.

So each node **freezes its ordinary local MVCC snapshot at the moment it stops** at the
restore point — `xmax = latestCompletedXid + 1` plus the running set from
`KnownAssignedXidsGetAndSetXmin()` — and answers every read from that image until the next
one is published. Hot standby already maintains exactly the right object by redo, prepared
transactions included, which is what a straddling 2PC transaction looks like on a segment.

**Publication is cluster-wide, and that is the load-bearing part.** Two slots in
`dr_served_snapshot.c`: the startup process writes `pending` on arrival; `gg_dr_switch()`
flips `served ← pending` on every node only once all of them report being stopped there.
Publishing at each node's own pause would serve *N+1* on whichever arrived first while a
lagging segment was still short of it — the same torn read, one restore point later. A
cancelled or crashed switch leaves `served` at *N*: stale, never wrong. This is why `ggdr
switch` is a wrapper around `gg_dr_switch()` and not a per-node rearm-and-resume loop.

**A DR read carries no distributed snapshot at all** (`haveDistribSnapshot` stays false). A
distributed restore point already supplies the cross-node order a distributed snapshot
exists to provide, and the per-node frozen images agree by construction. Keeping one
actively hurt: its `xmax` could only be `nextGxid`, a pre-allocation watermark logged in
batches of 8192, so later-arriving transactions fell below it and became visible; and it
drove two silent ratchets — the sticky `HEAP_X*_DISTRIBUTED_SNAPSHOT_IGNORE` hint bits and
`DistributedLog_AdvanceOldestXmin()`, after which `DistributedLog_CommittedCheck` stopped
answering. Both short-circuit on `haveDistribSnapshot`, so both close by deletion.

**Rejected:** *freezing a distributed snapshot* whose `xmax` comes from a redo-maintained
`latestCompletedGxid`. `max()` over replayed commits is not a valid horizon — transactions
commit out of gxid order, so a transaction prepared before *N* whose coordinator commit
record lands after *N* is below the horizon and in no in-doubt set, hence visible in the
supposed as-of-*N* image. And `XidInMVCCSnapshot()` gates the whole distributed block on
`!IS_QUERY_DISPATCHER()`, so pinning only the distributed snapshot would leave the
coordinator planning against replay-now catalogs while the segments executed as of *N*.

**Known residual asymmetry.** That same gate means the coordinator decides visibility by
local xid alone. At a restore point that catches the window between the coordinator's
distributed-commit record and `doNotifyingCommitPrepared()` taking `TwophaseCommitLock`
SHARED — microseconds — a transaction writing on *both* sides is committed on the
coordinator and prepared-only on the segments. Its catalog change is visible on the
coordinator and absent on the segments, so a query against it fails loudly (`relation …
does not exist`) rather than returning a wrong answer. Closing it means widening the
barrier on production's commit path to simplify the replica, which is not worth it.

#### D9 — Nothing to serve is a *query* error, not a snapshot error

A node that has never replayed to a restore point has only free-running state to offer, so
`ExecutorStart()` refuses outright — on every node independently, so a dispatched slice is
refused on the segment that lacks a point even when the coordinator has one.

The refusal is in the executor and **not** in `GetSnapshotData()` deliberately: connecting
to a database needs a snapshot, as does every utility path that could repair the situation.
A replica that refuses snapshots is a replica nobody can connect to, with no way back in.
Failing at query start keeps `psql`, `ALTER SYSTEM` and `pg_ctl reload` working.

The frozen image lives in shared memory and does not outlive the postmaster, so a restarted
node self-publishes the first time it reaches a restore point again. **One window is left
open by design:** a node restarted *during* an advance comes back, replays to the new
target and serves it while its peers still serve the old one, until the coordinator's
`gg_dr_switch()` publishes the new point everywhere. Every way of closing it puts the node
back in the unreachable state above. Scenario H-2b tracks it.

#### D10 — Force the visibility map to report nothing all-visible

`VM_ALL_VISIBLE` lets index-only and bitmap scans return tuples **without consulting the
heap at all**, so no snapshot — however carefully frozen — is applied to them, and
production's `XLOG_HEAP2_VISIBLE` records replay during an advance. No snapshot design can
close this; `visibilitymap_get_status()` returns 0 in DR mode. The cost is losing the
index-only fast path on a replica. Safe for the only other consumer, vacuum, which never
runs here (D5).

#### D11 — Recovery conflicts are the safety net; `max_standby_archive_delay = 0`

Replay can remove a row version the frozen image still needs. Serving that image publishes
`MyPgXact->xmin` from it, so `ResolveRecoveryConflictWithSnapshot()` cancels the reader.
The policy is deliberately **the right answer or none**: no retention holder, no replay
throttling, and a cancelled QE fails the whole distributed query.

That also makes conflicts routine rather than exceptional, so the stock 30 s delay would be
added to every advance that hits one. `ggdr create-replica` writes
`max_standby_archive_delay = 0` (and the streaming twin). `-1` is not the opposite extreme
but a trap: it blocks the startup process inside redo, so replay stops altogether.

### Operations

#### D12 — Whole-cluster failover; mirrorless DR

The DR cluster runs without mirrors and without FTS; the failover unit is the whole
cluster. `gg_dr_promote()` promotes segments first, then the coordinator, so the
coordinator's FTS finds live segments when it starts probing. There is no second phase and
no restart — DR behaviour is keyed on recovery (D3), so it lifts with recovery itself.
Losing a DR node means re-creating it. DR-local mirrors, DR-local FTS and per-node repair
are additive and orthogonal to the cross-cluster mechanics this had to prove.

#### D13 — Control plane in SQL, plus one utility, plus first-class observability

- **SQL** — `gg_dr_switch(restore_point)` and `gg_dr_promote()` apply each step to every
  primary segment via `CdbDispatchCommand()` and to the coordinator directly, so a client
  that can reach only the coordinator can drive recovery. Neither takes a timeout: how long
  a switch takes is a property of how much WAL lies between here and the target, and a
  timeout that fires leaves the target armed anyway, reporting a failure that is not one.
  Both block and are interruptible, so `pg_cancel_backend()` is the way out — the mechanism
  operators already have for every long query. `gg_dr_promote()` takes no arguments either:
  a replica is promoted from where it *is*, and it validates and reports that point rather
  than asking to be told.
- **Utility** — `ggdr` (`create-replica` / `switch` / `pause` / `stat` / `promote`) treats
  the whole cluster as one object; `create-replica` restores per-instance base backups,
  frozen-seeds the topology, arms every node and starts it, with a fail-closed
  archive-completeness gate and a post-build topology check.
- **Observability** — `gg_stat_dr_replica` / `gg_stat_dr_replica_summary`, and
  `pg_last_paused_restore_point()`, which reports the restore point **actually reached**
  (captured from the replayed record, not the configured GUC, which already names *N+1*
  mid-advance). `consistent_restore_point` — non-NULL iff all nodes are stopped at one name
  — is the safe-to-serve/safe-to-promote signal. There is deliberately **no cross-node LSN
  column**: per-node LSN spaces make such comparisons meaningless.
- **Naming** — everything this feature adds takes the `gg_` prefix. Objects it merely
  *uses* keep theirs (`gp_create_restore_point`, `gp_pause_on_restore_point_replay`,
  `gp_segment_configuration`), and `pg_last_paused_restore_point()` is named alongside the
  `pg_*` recovery accessors it sits with (`pg_is_wal_replay_paused`,
  `pg_last_wal_replay_lsn`) rather than after this feature.

Two implementation notes worth preserving, both found the hard way:

- The local `ALTER SYSTEM` leg **cannot** go through SPI. These functions run inside a
  `SELECT`, and `standard_ProcessUtility` calls `PreventInTransactionBlock` for
  `AlterSystemStmt`; the local leg calls `AlterSystemSetConfigFile()` directly, and the
  per-node leg is a dispatched *function call*, not a dispatched utility statement.
- `gg_dr_promote()` waits for **this node** to leave recovery, not the segments. Segments
  cannot be polled from here — the QE protocol check refuses a standby QD talking to a
  promoted QE — and they cannot be promoted synchronously either, because
  `recoveryPausesHere()` does not watch the promote trigger, so `pg_promote(true)` on a
  paused node would wait for a resume only another session could send. `ggdr promote`,
  which holds a connection per node, remains the path when the stronger guarantee is
  wanted.

---

## Consequences

### Positive

- **Production is untouchable by DR failures.** No slots, no connections from DR; a dead
  replica costs production nothing.
- **One faithful archive.** DR `pg_wal` stays byte-identical to production's, so the same
  archive drives production PITR, multiple replicas, and honest `pg_waldump` forensics.
- **Topology protection is an engine invariant**, not configuration: it holds for every WAL
  source with nothing to wire, and a bare `cp` `restore_command` is safe.
- **Reads are transactionally consistent cluster-wide, and stay so under advance.** A read
  served while the cluster moves from *N* to *N+1* returns the *N* image or is cancelled.
- **Small, auditable core delta.** One redo-loop hook plus the filter file, two
  `relmapper.c` guards, one `smgr_redo` guard, the DR predicate, two read-only checks, one
  DTX context enum with its short-circuits, two QE exemptions, the frozen-snapshot module
  with its capture and serve points, one visibility-map early return, one executor gate, two
  SQL functions, one accessor and two views. The risky machinery — recovery, pause,
  dispatch, gangs — is all pre-existing and shipped.
- **Promotable in place** at a named, verified restore point, in one phase with no restart.

### Accepted costs

- **Coarse freshness.** RPO = restore-point cadence + archive latency + slowest-segment
  skew, and the slowest archiver gates the whole cluster's advance.
- **Advances can cancel long queries** (D11), and a cancelled QE fails the whole distributed
  query. Mitigation is cadence tuning, not elimination.
- **The archive becomes production-critical infrastructure.** Availability, retention and
  completeness now matter operationally; `create-replica` fails closed on archive gaps
  rather than risking a timeline fork.
- **Topology catalogs are contractually immutable on production** while a replica is
  attached: `VACUUM FULL` / `CLUSTER` / `REINDEX` / `TRUNCATE` of one halts the replica with
  a `FATAL` and requires re-creating that node (D4). Chosen over silent divergence.
- **Index-only and bitmap scans lose their all-visible fast path** on a replica (D10).
- **Inherited, frozen configuration.** Roles, resource groups and replicated-catalog GUCs
  come from production; changing them on the replica is unsupported.
- **The mirror footgun** (D3), the coordinator-side straddle asymmetry (D8) and the
  restart-during-advance window (D9) are documented, not defended against.
- **PoC gaps** — deferred, not design limits: `ggdr` reaches nodes by local socket
  (single-host); no restore-point scheduler on production; no automated
  latest-common-restore-point computation after an unclean production loss; mirrorless DR.

### Notes for a developer picking this up

- **Both clusters must match** on major version, `CATALOG_VERSION_NO`, block size,
  checksum/`wal_log_hints` setting, encoding and segment (content) count. Physical
  replication is byte-level and mismatches are fatal. This feature bumped
  `CATALOG_VERSION_NO` twice while its catalog objects were added and then renamed
  (commits `2306ff75eae`, `d3f52326bcd`); nothing since has needed a re-`initdb`, and
  keeping it that way is worth real effort — a bump means re-`initdb` on production, a
  fresh base backup, and rebuilding every replica.
- **Invariants that must survive refactoring:** DTX activation is gated on
  `IsDRReplicaMode()`, never on the DtxContext value (D6). The frozen image is published
  only after *every* node has arrived (D8). The nothing-to-serve refusal stays out of
  `GetSnapshotData()` (D9). Shared-memory *sizing* runs before `CreateSharedProcArray()`,
  so nothing in a `ShmemSize()` may call `GetMaxSnapshotXidCount()`.
- **The regression story is the container fixture, not unit tests.** Three silent-no-op
  bugs — the relmapper not being loaded in the startup process, the DR flag being invisible
  in backends, and an unguarded `XLOG_SMGR_TRUNCATE` destroying the seeded topology — all
  passed unit tests and were caught only end to end. Two more (a NULL deref in the
  shared-memory sizing pass, and a switch that resumed nodes already at the target) were
  caught only by running the fixture.

---

## Validation

`src/test/dr/` — two containers, production and DR, one coordinator plus two segments each,
sharing an `/archive` volume, with the replica built by `ggdr create-replica`:

- **M1 + M2 (7/7)** — topology filtered *selectively*: production's catalog change is
  invisible while its post-backup user table is visible; DR-local hostnames survive replay;
  live hot-standby reads work; `INSERT` / `CREATE TABLE` / `FOR UPDATE` refused.
- **M2′ (5/5)** — real dispatched distributed reads in recovery: aggregation across both
  segments, InitPlan, multi-slice JOIN; writes refused under dispatch.
- **M3 (11/11)** — the serve/advance loop; a fault-injected 2PC **straddle** invisible at
  the cut and visible after the advance; and the **mid-advance skew** case, which is held
  still rather than raced — the segments are driven to `dr_rp2` while the coordinator stays
  at `dr_rp1`, and a read must keep returning the `dr_rp1` answer, including through a
  forced index-only scan over a table production VACUUMed on both sides of `dr_rp1` (D10).
- **M5 (6/6)** — faithful served-restore-point reporting, including after the pause GUC is
  re-pointed at a name that was never reached.
- **`ggdr` (4/4)** and **SQL recovery control (9/9)** — stat → switch → promote, ending
  with a promoted cluster that accepts distributed writes with no restart.
- **Dense V-20 fixture (5/5)** — production vacuum-truncates a dense topology catalog; the
  replica's seeded rows, which sit on a trailing block, survive (D4).
- **`gg_walfilter` suite** — black-box against the installed binary, gating the build.

---

## References

- Architecture description, code map and operations:
  `doc/architecture/greengage-dr-read-replica.md`
- Scenario catalogue, expected behaviour and measured results:
  `doc/architecture/greengage-dr-test-scenarios.md`
- Proof-of-concept record (historical rationale only): ADR-0001 … ADR-0005 in this
  directory.
- Test fixture: `src/test/dr/` (see its `README.md`).
- Same content as a browsable page, with a diagram of the publish step (D8):
  <https://claude.ai/code/artifact/2919af1b-adc3-45ce-9dad-2278fbf7d866> — a private link,
  readable only by people it has been shared with. This file is the source of truth; the
  page is a convenience and can lag it.
