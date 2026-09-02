# Greengage 7 — Disaster-Recovery Read Replica

**Architecture Description Document**

Status: proof-of-concept, branch `7.x-dr`. This document describes the DR read-replica
feature **as implemented** (not the aspirational design). Every mechanism below is
grounded in the source; `file:line` references point at the shipped code.

---

## Table of contents

1. [Overview](#1-overview)
2. [High-level architecture](#2-high-level-architecture)
3. [Key design decisions](#3-key-design-decisions)
4. [How it is implemented](#4-how-it-is-implemented)
5. [Snapshot synchronization — detailed flow](#5-snapshot-synchronization--detailed-flow)
6. [Operational workflows](#6-operational-workflows)
7. [`ggdr` utility reference](#7-ggdr-utility-reference)
8. [Pros and cons](#8-pros-and-cons)
9. [Corner cases and limitations](#9-corner-cases-and-limitations)
10. [Appendix — code map, GUCs, catalog objects](#10-appendix)

---

## 1. Overview

### 1.1 What this is

A **DR read replica** is a *second, independent Greengage cluster* — its own coordinator
and primary segments, its own `dbid`s — kept continuously up to date from a production
cluster and serving **read-only distributed queries** while permanently in archive
recovery. On a switchover it is promoted in place to a normal online read-write cluster.

It is the open-source equivalent of VMware/Broadcom **GPDR "Read Replica Mode"**: same
archive/PITR-based transport, same "read-only cluster in recovery that you can promote"
posture, built entirely on Greengage 7 (PostgreSQL 12 base MPP fork) with no proprietary
tooling.

### 1.2 The problem it solves

Greengage's built-in HA (`gp_segment_configuration` mirrors + FTS) protects against
*node* loss inside one cluster. It does **not** give you:

- a **geographically separate** copy that survives loss of the whole primary site;
- a copy you can **query read-only** for reporting/offload while it stays current;
- a **promotable** standby cluster for planned or unplanned failover.

Physical streaming replication is single-node PostgreSQL; it has no notion of an MPP
cluster's distributed transactions, its cross-segment consistency, or its topology
catalog. This feature supplies those missing pieces.

### 1.3 Why it is not "just turn on hot_standby"

Two things break if you naively point a base backup of production at `standby.signal`:

1. **Topology.** A restored DR node's `gp_segment_configuration` describes *production*
   (production's hosts/ports/dbids). Left alone, production's replayed WAL keeps
   overwriting it, so the DR cluster can never describe *itself*.
2. **Distributed dispatch.** On a Greengage coordinator, dispatching even a pure `SELECT`
   goes through the *write* path of the distributed transaction manager: it allocates a
   distributed xid (`gxid`) by writing shared memory and opens a **writer** gang. Both are
   illegal for a process in recovery.

The feature is, in essence, the set of targeted core changes that make those two things
work, plus the tooling (`ggdr`) to drive the lifecycle.

### 1.4 Milestone map

The implementation is organized as milestones; this document is structured around them:

| Milestone | Concern | Where in this doc |
|-----------|---------|-------------------|
| M1 | Topology independence (file-backed topology store) | §4.2 |
| M2 | Read-only enforcement | §4.3 |
| M2′ | Standby distributed read (dispatch while in recovery) | §4.4 |
| M3 | Consistent reads (restore-point-gated, as-of-N snapshot) | §4.5, §5 |
| M4 | Build a DR replica (`ggdr create`) | §6.1 |
| M5 | Observability (`gg_stat_dr_replica`, `pg_last_paused_restore_point`) | §4.6 |
| M6 | Promotion (`ggdr promote`, `gg_dr_promote()`) | §6.3, §6.4 |

---

## 2. High-level architecture

```
     PRODUCTION (read/write)              ARCHIVE REPOSITORY          DR CLUSTER (read-only, in recovery)
   ┌──────────────────────────┐      (shared FS / S3 / NFS,         ┌─────────────────────────────────┐
   │ Coordinator  archive_cmd │─push─▶  per-content layout):        │ DR Coordinator   restore_command │
   │ Segment c0   archive_cmd │─push─▶    wal/seg-1/…  (coordinator) │ DR Segment c0    restore_command │
   │ Segment c1   archive_cmd │─push─▶    wal/seg0/…   (content 0)   │ DR Segment c1    restore_command │
   │   …                      │           wal/seg1/…   (content 1)   │   …                              │
   └───────────┬──────────────┘           basebackup/seg<c>/        │ • hot_standby ON (auto)          │
               │                                    ▲               │ • apply-time REDO FILTER          │
   gp_create_restore_point(N)                       │               │   (protects topology catalogs)    │
   → distributed barrier under      ─── restore_command pulls ──────┤ • DR-local topology (own dbids)   │
     TwophaseCommitLock; every                                      │ • pause replay at restore point N │
     node writes XLOG_RESTORE_POINT                                 └───────────────┬───────────────────┘
                                                                     ggdr drives the whole cluster
                                                                     as one: switch / pause /       
                                                                     stats / promote / create
```

### 2.1 Properties

- **Per-content continuous restore.** DR coordinator replays production's coordinator
  WAL; DR segment *cN* replays production primary *cN*'s WAL. The **segment count must
  match**; host layout is free. Each node has its **own independent WAL/LSN space**.
- **DR-local topology**, out of reach of replayed WAL because it is not in the catalog at
  all (§4.2).
- **Read-only** end-to-end (§4.3), enforced with a DR-specific error, but still able to
  **dispatch distributed reads** to segments in recovery (§4.4).
- **Restore-point-gated consistency** (§4.5): the cluster recovers **up to** a distributed
  restore point *N*, pauses there, and every node **freezes its local MVCC snapshot** at
  that point and answers every read from it — a transactionally consistent cut, not a
  `min(replay_lsn)` approximation, and one that survives the cluster advancing towards
  *N+1* underneath the reader. This is the *only* serving state; there is no free-running
  mode, because no other cut has a cross-node guarantee to offer.
- **Promote in place** to an online read-write cluster (§6.3).

### 2.2 Component inventory — DR-new vs. reused

Being explicit about provenance matters for maintenance:

**DR-new code** (2026 commits on `7.x-dr`):

- `IsDRReplicaMode()`, and the file-backed topology provider the replica runs on
  (`cdbtopology_file.c`, ADR-0007).
- M2 read-only enforcement.
- The `DTX_CONTEXT_QD_STANDBY_READER` distributed-transaction context.
- The M3 served restore-point snapshot (`dr_served_snapshot.c`, ADR-0006 D8).
- The M5 served-restore-point accessor + `gg_stat_dr_replica` views.
- The `gg_dr_switch()` / `gg_dr_promote()` recovery-control functions (§6.4).
- The `ggdr` utility and the `gg_topology` store tool.

**Reused upstream Greengage hot-standby dispatch** (commit `35e95b9c`, 2023, *"Enable hot
standby dispatch"* — predates and is independent of DR):

- `IS_HOT_STANDBY_QD()` / `IS_STANDBY_QE()` macros.
- Mirror-segment selection on a hot-standby QD (`cdbutil.c`).
- The `is_hs_dispatch` wire flag and the QE-side protocol check.

The DR feature **layers no-gxid, as-of-N standby-reader semantics on top of the existing
hot-standby dispatch** rather than reimplementing gang routing. (Note: the elaborate
"snapshot-leader / gang-member-0" scheme in the early SR-2 design note was **not** built;
under stop-and-go, replay is paused while serving, so the reader path runs against a
static snapshot and the upstream dispatch suffices — see §5.5.)

---

## 3. Key design decisions

| # | Decision | Rationale |
|---|----------|-----------|
| 1 | **Archive / continuous-restore transport** (PITR), not streaming replication | No replication slot ⇒ no production WAL pinning (PG12 lacks `max_slot_wal_keep_size`); one faithful archive is reusable for production PITR *and* N DR replicas; WAN-latency tolerant. |
| 2 | **Topology independence by moving topology out of the replicated catalog**, into `$PGDATA/gg_topology` behind a provider | Production's rows travel in the WAL because the topology is a *shared catalog*; a store production cannot address needs no protection at all. The filter-and-seed design this replaced (ADR-0006 D4) worked, but cost a redo hook, two guards, a seed step and a timeline-fork hazard. Rewriting the WAL itself was never an option: LSN = byte offset, plus the `xl_prev` chain and the page `pd_lsn` interlock. |
| 3 | **Stop-and-go consistency** (pause at restore point *N*, serve, advance to *N+1*), reusing the shipped `gp_pause_on_restore_point_replay` GUC | Greengage already ships the pause primitive. It gives a clean as-of-N image with **zero recovery-conflict exposure during serve windows** (replay is stopped); conflicts are confined to advance bursts. Each node also *freezes* that image (ADR-0006 D8), so the window holds while the cluster advances rather than only while it sits still. |
| 4 | **Reuse upstream hot-standby dispatch**; add only the no-gxid standby-reader context | Minimizes new core surface; the risky gang/snapshot machinery already exists and is tested. |
| 5 | **Whole-cluster failover only** for the PoC (mirrorless DR) | Keeps the novel cross-cluster work in focus; DR-local mirrors + DR-local FTS are deferred. |

---

## 4. How it is implemented

### 4.1 The DR-mode gate — hot standby *is* DR mode

Everything keys off one predicate:

```c
bool
IsDRReplicaMode(void)          /* src/backend/access/transam/xlog.c */
{
    return EnableHotStandby && RecoveryInProgress();
}
```

There is **no separate DR mode**. Enabling `hot_standby` on a node in recovery gives it the
whole DR behaviour: reads served from its own topology store (§4.2), read-only enforcement
(§4.3), and the standby distributed-read path (§4.4). Earlier revisions carried a
`gp_dr_replica` GUC plus a `dr_replica.signal` marker alongside `hot_standby`; both are
gone.

**Why fold rather than add a mode.** Stock hot standby has little practical use in
Greengage on its own — the only standbys in a cluster are mirrors and the standby
coordinator, and querying those loads the very hosts they exist to protect, while an FTS
failover onto a mirror strands the sessions reading it. Rather than carry a second,
near-identical mode beside it, `hot_standby` is repurposed to mean the thing that *is*
useful: a queryable, read-only, topology-independent replica.

Three consequences are worth stating explicitly:

- **It follows recovery, not configuration.** The predicate goes false the moment recovery
  ends, so promotion disengages DR behaviour by itself — no restart, no GUC to unset, no
  marker to delete. This is what removed the old two-phase promote (§6.3).
- **It is visible everywhere without plumbing.** `EnableHotStandby` is a `PGC_POSTMASTER`
  GUC, so the startup process and every backend (which runs the
  read-only enforcement) see the same value. The old design needed a lazily-`stat()`ed,
  per-backend cache of the marker file precisely because a backend never runs recovery.
- **`hot_standby` defaults to `off`**, so ordinary mirrors and the standby coordinator are
  unaffected. **Do not turn it on for them.** A mirror is a node *inside* a cluster and must
  stay ready to be promoted into it; DR mode gives it a replica's semantics instead — reads
  served only from a published restore point, writes refused, and (with
  `gg_topology_source = file`) a topology of its own rather than the cluster's.
  `hot_standby = on` means *"this node is a separate DR cluster"*.

### 4.2 Topology independence — the replica describes itself

A DR replica is a *different cluster* from the one whose WAL it replays: its own dbids,
hosts, ports and data directories. Production's `gp_segment_configuration` is a **shared
catalog**, so its rows travel in the WAL like any other heap tuple, and something has to
stop the replica from adopting them.

The answer is that the replica does not read that catalog. Cluster topology sits behind a
**provider** (ADR-0007), and a replica selects the file-backed one:

- **`gg_topology_source = file`** (`PGC_POSTMASTER`) on every node, written by
  `ggdr create` — coordinator *and* segments, because the GUC is exempt from the QD/QE sync
  check and a half-armed cluster would otherwise come up silently;
- the store is **`$PGDATA/gg_topology`**: text, CRC-32C over everything but the last line,
  and **not WAL-logged**, so nothing production writes can reach it;
- **`gp_segment_configuration` is a view** over `gp_get_segment_configuration()`, which
  reads the active provider — so gpMgmt, in-tree SQL and user queries all see the store the
  node actually runs on, under either provider.

Production's rows still arrive. They replay into `gp_segment_configuration_internal` — the
renamed catalog — which on a replica **nothing reads**. The fixtures assert both halves at
once: the replica's catalog carries production's change, and the topology it serves does
not move (T1/T3, `src/test/dr/scripts/dr-entrypoint.sh`).

**What this replaced.** Until P6/P7 the replica kept its topology by *filtering production's
topology records out of redo* and seeding its own catalog with frozen tuples. It worked, and
it cost: a hook in the redo loop, a `relmapper.c` remap guard, a `storage.c` truncate guard,
a single-user seed step inside `create`, and a permanent timeline-fork hazard around that
seed. Putting topology somewhere production cannot address deleted all of it. The replica
now replays production's WAL **in full**, which `wal_consistency_checking = 'all'` on
production turns into a byte-level proof that it does (T6: 1043 full-page images in the
first segment, zero inconsistencies). ADR-0006 D4 and the §4.2 of earlier revisions of this
document describe the filter; it is history, not code.

One consequence is worth stating plainly, because it used to be a prohibition: production
may now run `VACUUM`, `VACUUM FULL`, `REINDEX` and `TRUNCATE` on its topology catalog with
a replica attached. Each rewrites a mapped shared catalog's relfilenode and emits a shared
relmap update — the record the old design halted on with `FATAL` — and the replica replays
them and notices nothing (§10.4, V-13′).

#### 4.2.1 Production HA events fork a timeline

Production's topology also changes for reasons of its own, and two of those move a content's
WAL onto a **new timeline**:

| Event | What production does to its topology | What the replica must do |
|---|---|---|
| Segment primary → mirror (FTS) | flips `role`/`status`/`mode` **in place**; the promoted mirror starts timeline 2 | follow the fork, for that content only |
| Coordinator → standby coordinator (`gpactivatestandby`) | **deletes** the old coordinator's row and promotes the standby's, which keeps its own dbid (`segment_config_activate_standby()`, `segadmin.c`) | follow the fork, for content `-1` |

Both are survivable, and each has a fixture (§10.4). Two settings are what make them work,
both written by `ggdr create` on every node:

- **`recovery_target_timeline = 'latest'`.** A promoted node archives a `.history` file and
  writes everything after the promotion on the new timeline. The archive is keyed on
  **content** (`%c`), which a promotion does not change, so the new timeline lands in the
  directory that replica's node already reads. Pinned to `'current'`, a replica cannot
  follow — and it does not fail loudly: it waits for WAL nobody will ever write again, which
  from the outside is indistinguishable from an idle production.
- **`archive_mode = off`.** The base backup carries production's `archive_mode` and its
  `archive_command`, and that command names production's archive. A node in recovery
  archives nothing, so this matters only once the replica is **promoted** — at which point
  it would start writing its own timeline into the archive production is still filling.
  Under `'latest'` that is not merely untidy: the next replica built from that archive would
  find the promoted replica's history file as the newest timeline and recover onto *it*
  rather than production's. One promotion would quietly poison every rebuild after it.

For the failover to be followable the **mirror must be archiving too** — `archive_mode = on`
(not `always`) on production's mirrors and standby coordinator, which stays silent while
they are in recovery and starts the moment one is promoted. There is nobody left to
configure it afterwards.

The catalog churn itself — role/status updates, `gp_configuration_history` inserts, the
deleted coordinator row — replays like any other record and changes nothing the replica
serves.

#### 4.2.2 Auxiliary tooling: `gg_walfilter` (`src/bin/gg_walfilter`)

The WAL rewriter survives as **tooling**, not as a mechanism the replica depends on. It is a
hand-written walker (deliberately not `xlogreader`: a record spilling across a segment
boundary must be decided from its in-segment prefix, and `DecodeXLogRecord()` on a
zero-padded prefix fails open) with three uses:

- **`inspect`** — say what a WAL segment contains and which records a given rule set would
  rewrite;
- **offline filtering** with explicit rules (`--exclude-db`, `--exclude-tablespace`,
  `--protect-mapped-oid`), e.g. for partial replicas or redacted WAL hand-offs;
- **opt-in restore-path filtering** — `gg_walfilter restore` is a complete `restore_command`
  entry point, for deployments that want filtering *outside* the engine.

`--halt-on-remap` halts a restore (exit 3) if production rebuilds a mapped catalog. It was
`--gp-dr-topology` while the engine had a matching guard; the preset was that guard's only
setter, so deleting the guard would have made the option unreachable, and it was renamed to
say what it does instead.

History: an earlier revision used `gg_walfilter` (then Python) *as* the default topology
protection via `restore_command` (ADR-0002); ADR-0003 restored the in-backend filter as the
default and kept the tool, ported to C; P7 deleted the filter and left the tool.


### 4.3 M2 — Read-only enforcement

Two layers refuse writes with a DR-specific, clearer message than stock hot standby.

**Primary gate — `ExecCheckXactReadOnly()` (`src/backend/executor/execMain.c:1651-1660`):**

```c
if (IsDRReplicaMode() &&
    (plannedstmt->commandType != CMD_SELECT ||   /* any DML / utility */
     plannedstmt->rowMarks   != NIL ||           /* SELECT ... FOR UPDATE/SHARE */
     plannedstmt->hasModifyingCTE ||             /* data-modifying CTE */
     plannedstmt->intoClause  != NULL ||         /* SELECT INTO / CTAS */
     plannedstmt->refreshClause != NULL))        /* REFRESH MATVIEW */
    ereport(ERROR,
        (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
         errmsg("cannot execute %s on a read-only disaster-recovery replica", ...)));
```

Stock hot standby forces `XactReadOnly` but still *exempts* `SELECT ... FOR UPDATE/SHARE`
(row locks). A DR replica must reject those too, so the DR check runs first and shadows the
write/lock cases with the DR message. Ordinary read-only-transaction cases still fall
through to the stock `"cannot execute X in a read-only transaction"` path.

**DTM backstop — `currentDtxActivate()` (`src/backend/cdb/cdbtm.c:281-284`):**

```c
if (IsDRReplicaMode())
    ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
        errmsg("cannot start a distributed transaction on a read-only disaster-recovery replica")));
```

Allocating a `gxid` mutates cluster-wide `nextGxid` — the deepest write. Anything that
reaches here is an attempted write/2PC; coordinator-local reads never do.

**FTS and autovacuum are neutralized *structurally*, not by a DR guard.** A DR node stays
permanently in postmaster state `PM_HOT_STANDBY` and never reaches `PM_RUN`. The FTS probe
bgworker (`BgWorkerStart_DtxRecovering`) is only started at `PM_RUN`
(`bgworker_should_start_now()`, `postmaster.c:6545-6578`), and the autovacuum launcher only
at `PM_RUN` (`postmaster.c:2132-2134`). So neither ever runs on a DR node — no probe writes
to `gp_segment_configuration`, no vacuum. There is no `IsDRReplicaMode()` check needed for
this.

### 4.4 M2′ — Standby distributed read

This is the piece that makes a coordinator *in recovery* dispatch a distributed `SELECT`.

**A new distributed-transaction context** `DTX_CONTEXT_QD_STANDBY_READER`
(`src/include/cdb/cdbtm.h:142-149`). The coordinator selects it in
`setupRegularDtxContext()` when it is a dispatcher in DR mode
(`cdbtm.c:1729-1743`):

```c
if (isDtxQueryDispatcher())
    setDistributedTransactionContext(IsDRReplicaMode()
        ? DTX_CONTEXT_QD_STANDBY_READER          /* no gxid, no WAL */
        : DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE);
```

In this context the coordinator **never** calls `currentDtxActivate()`, so
`MyTmGxact->gxid` stays invalid — no `nextGxid++`, no WAL. Every DTX-activation side path
is additionally short-circuited under DR mode: `setupDtxTransaction()` early-returns
(`cdbtm.c:417`), the InitPlan/subplan DTX setup is skipped (`execMain.c:666`), and internal
BEGIN/RELEASE/ROLLBACK subtransaction dispatch is a no-op (`cdbtm.c:452`).

**Dispatch rides on the pre-existing hot-standby infrastructure.** A hot-standby QD selects
the **mirror** entry for each segment (`cdbutil.c:1027-1051`) — on the DR cluster those are
the segments in continuous recovery — and marks the dispatch with the `is_hs_dispatch` wire
flag (`cdbdisp_query.c:894, 1006-1008`); the QE-side protocol check
(`postgres.c:5559-5568`) refuses any primary/standby mismatch. The DTX context info that
travels to segments carries the distributed snapshot but **not** a gxid, and (unlike the
distributed-capable path) does not write a shared local snapshot for the standby-reader case
(`qdSerializeDtxContextInfo()`, `cdbdisp_dtx.c:158-177`).

**The writer-gang gxid exemption.** Some read-only plans (e.g. a `UNION ALL` of a
coordinator-entry row with segment rows, as `gg_stat_dr_replica` does) are planned onto a
**writer** gang, which reaches the writer-context path that normally requires a valid gxid.
On a standby that gxid never exists, so the check is exempted for standby QEs
(`xact.c:2510-2517`):

```c
if (DistributedTransactionContext != DTX_CONTEXT_QE_AUTO_COMMIT_IMPLICIT &&
    QEDtxContextInfo.distributedXid == InvalidDistributedTransactionId &&
    !IS_STANDBY_QE())               /* exempt: standby QE never has a gxid */
    elog(ERROR, "distributed transaction id is invalid in context %s", ...);
```

where `IS_STANDBY_QE()` is `EnableHotStandby && IS_QUERY_EXECUTOR_BACKEND() &&
RecoveryInProgress()` (`cdbvars.h:760`). This is safe because writes are already refused at
the coordinator by M2 — a "writer" gang on a standby only ever executes read-only work.

### 4.5 M3 — Consistent reads (stop-and-go)

A distributed read is safe only if its visibility cut is one where **no distributed
transaction straddles segments**. Two ingredients:

#### 4.5.1 The barrier

Production creates periodic **distributed restore points** with
`gp_create_restore_point('N')` (`src/backend/access/transam/xlogfuncs_gp.c:35`). It holds
`TwophaseCommitLock` in `LW_EXCLUSIVE` while dispatching to every segment
(`CdbDispatchCommand(..., DF_NEED_TWO_PHASE | DF_CANCEL_ON_ERROR, ...)`,
`xlogfuncs_gp.c:89-100`), so the QD and every segment write their own `XLOG_RESTORE_POINT`
record at a **consistent distributed cut** — no commit-prepared broadcast can slip through
the barrier.

#### 4.5.2 Pausing every node at N (stop-and-go)

Each DR node is configured with `gp_pause_on_restore_point_replay = 'N'`
(`guc_gp.c:4727-4736`, `PGC_SUSET`). When the redo loop replays the matching restore-point
record, `pauseRecoveryOnRestorePoint()` (`xlog.c:5976`, called at `xlog.c:7811-7812`) pauses
replay (`SetRecoveryPause(true)` + `recoveryPausesHere()`). Just before it pauses, the node
also **freezes the snapshot** it will serve (§4.5.3). The node keeps serving reads while
paused. To **advance** to *N+1*, use `gg_dr_switch('N+1')`: it re-points the pause GUC on
every node, reloads, resumes, waits for all of them to arrive, and only then publishes the
new frozen image cluster-wide. Order matters — re-point *before* resume, or the node
free-runs to end-of-WAL.

A clean cross-node cut is reached when **every** node is paused at the **same** restore
point. That is exactly the "consistent serve point" `ggdr` computes (§7).

#### 4.5.3 The snapshot frozen at N

See **[ADR-0006](adr/0006-dr-read-replica.md) D8** for the decision and its alternatives; the mechanism is:

- **Capture.** `pauseRecoveryOnRestorePoint()` calls `DRCaptureServedSnapshot(rp)`
  (`procarray.c`), which takes `ProcArrayLock` SHARED and records
  `xmax = latestCompletedXid + 1` plus the complete running set from
  `KnownAssignedXidsGetAndSetXmin()` — an ordinary hot-standby recovery snapshot, taken at
  the instant replay stops. It lands in the `pending` slot of `dr_served_snapshot.c`,
  tagged with the restore-point name.
- **Publish.** `gg_dr_switch()` flips `served ← pending` on every node once *all* of them
  report being paused at that name. A node that arrived early keeps serving the previous
  point until then; that is what makes an advance atomic from a reader's point of view.
- **Serve.** `GetSnapshotData()`'s recovery branch fills from `served` instead of scanning
  `KnownAssignedXids` live, and publishes `MyPgXact->xmin` from it — which is what turns
  recovery conflicts into the safety net (§4.5.4).

There is deliberately **no distributed snapshot** on a DR read. A restore point already
supplies the cross-node order a distributed snapshot exists to provide, and the per-node
frozen images agree by construction. `haveDistribSnapshot` stays false, which also keeps
segments off the gxid-based visibility rules and off the sticky
`HEAP_X*_DISTRIBUTED_SNAPSHOT_IGNORE` hint bits (ADR-0006 D8).

Because the coordinator no longer dispatches a distributed snapshot, `setupQEDtxContext()`
(`cdbtm.c`) can no longer use "has a distributed snapshot" as its proxy for "is a
dispatched query"; on a standby QE it asks `IS_STANDBY_QE()` directly. Without that, reader
gangs stay in `DTX_CONTEXT_LOCAL_ONLY` and never sync with their writer's shared snapshot.

#### 4.5.4 What a frozen image cannot cover by itself

- **The visibility map.** `VM_ALL_VISIBLE` lets index-only and bitmap scans skip the heap
  entirely, so no snapshot is applied to those tuples. Production's `XLOG_HEAP2_VISIBLE`
  records replay during an advance. `visibilitymap_get_status()` therefore returns 0 in DR
  mode — the index-only fast path is given up on a replica.
- **Removed rows.** Replay can vacuum away a row the frozen image still needs. The frozen
  `xmin` is published, so `ResolveRecoveryConflictWithSnapshot()` cancels that reader. The
  policy is the right answer or none; `max_standby_archive_delay = 0` is set at build time
  so a conflict does not add 30 s to the advance.
- **Nothing frozen yet.** A node that has not replayed to any restore point refuses queries
  at `ExecutorStart()` rather than answering from free-running state. The refusal is in the
  executor, not in `GetSnapshotData()`, so connecting and `ALTER SYSTEM` keep working.

### 4.6 M5 — Observability

**A faithful "served restore point" accessor.** When a node pauses, the actually-reached
restore-point name (from the replayed WAL record, *not* the configured GUC) is captured
under a spinlock into `XLogCtlData.pausedRestorePointName` (`xlog.c:773`, captured at
`xlog.c:6005-6008`, cleared on resume in `SetRecoveryPause(false)`, `xlog.c:6246`). It is
read by `GetPausedRestorePointName()` (`xlog.c:6390-6396`) and surfaced as the SQL function
`pg_last_paused_restore_point()` (`xlogfuncs.c:606-615`, OID 7016) — which returns `NULL`
when not paused at a restore point and, unlike `pg_is_wal_replay_paused()`, never errors on
a promoted node.

Reading the record's own name (not the GUC) is what makes the value **faithful**: if you
re-point the pause GUC to a name that was never reached, the view still reports the point
actually served.

**Cluster-wide views** (`src/backend/catalog/system_views.sql:1542-1573`, in **`pg_catalog`**,
`GRANT SELECT ... TO PUBLIC`):

- **`gg_stat_dr_replica`** — one row per node
  (`gp_segment_id, role, dr_replica, in_recovery, replay_lsn, replay_time, is_paused,
  restore_point, served_restore_point, served_restore_point_time`). The coordinator row `UNION ALL`s the per-segment
  rows via `gp_dist_random('gp_id')`, so each node reports its own local recovery state.
  The last two columns answer different questions and a replica can answer them
  differently: `restore_point` (`pg_last_paused_restore_point()`) is where replay stopped,
  `served_restore_point` (`pg_last_served_restore_point()`) is what reads are answered as
  of. Only the cluster-wide publish moves the second, so re-pointing a **subset** of nodes
  (`ggdr switch --content`) leaves them stopped at N and still serving N-1.
- **`gg_stat_dr_replica_summary`** — one-row rollup:
  `node_count, all_dr_replica, all_in_recovery, all_paused, consistent_restore_point,
  consistent_paused_point, rpo_seconds`. `consistent_restore_point` is the **served**
  rollup — non-`NULL` only when every node is serving the same point, which is what
  "safe to serve" means — and `consistent_paused_point` is the same rollup over
  `restore_point`, which is what a promotion would cut at. They differ exactly while a
  subset switch is outstanding. `rpo_seconds` is the **age of the served cut** —
  `extract(epoch FROM now() - min(served_restore_point_time))`, i.e. how long ago
  production created the restore point these reads answer as of, which is exactly
  what promoting here would lose. It is not the age of the last replayed commit:
  that measures replay liveness, so an idle production drives it up without bound
  on a replica holding every row production has.

There is deliberately **no cross-node LSN column** — each node has its own WAL/LSN space, so
LSNs are not comparable across nodes. The consistency signal is "all at the same restore
point", not "LSNs are close".

---

## 5. Snapshot synchronization — detailed flow

This is the correctness core: how a distributed read on the DR sees a transactionally
consistent, cluster-wide image.

### 5.1 The two dimensions of visibility

A Greengage read has two visibility components:

- **Local (per node):** ordinary PostgreSQL MVCC. On the DR, each node is a stock
  hot-standby backend — visibility comes from the node's recovery snapshot
  (`KnownAssignedXids`).
- **Distributed (cluster-wide):** a `DistributedSnapshot` that maps each tuple's creating
  `gxid` to visible / invisible so all segments agree. On production this is minted live
  from `nextGxid`.

**On a DR replica only the first one is used.** The distributed dimension exists because a
live cluster has no global order across nodes; a distributed restore point *is* that order,
taken under `TwophaseCommitLock` EXCLUSIVE. What the replica must do instead is stop each
node's local snapshot from drifting once replay moves on — which is what §5.3 does. See
ADR-0006 D8 for why the alternative (reconstructing a distributed snapshot from replayed
state) was implemented, reviewed, and rejected.

### 5.2 The straddle — why a barrier is required

A distributed commit is two-staged. Critically, the coordinator's
`XLOG_XACT_DISTRIBUTED_COMMIT` record is written **outside** the `gp_create_restore_point`
`TwophaseCommitLock` barrier, whereas the segments' commit-prepared happens under 2PC. So at
an arbitrary LSN cut a distributed transaction *T* can be:

- **committed on the coordinator** (its gxid is in `shmCommittedGxidArray`), but
- **only prepared (invisible) on the segments.**

That is the **straddle**. A naive cross-segment `min(replay_lsn)` cut would expose *T* on
the coordinator but not the segments — a torn, non-atomic read. The `TwophaseCommitLock`
barrier of a **distributed restore point** is the only cut guaranteed straddle-free at the
segments.

On the segments *T* needs no special handling: it is prepared-only there, so its local xid
is still in `KnownAssignedXids` and the ordinary recovery snapshot already reads it as
in-progress. The coordinator side is genuinely asymmetric — its own local xact is committed
— and stays so; see [ADR-0006 D8](adr/0006-dr-read-replica.md) ("Known residual
asymmetry") for what the exposure actually is and why it is not closed.

### 5.3 The served restore-point snapshot — step by step

`src/backend/access/transam/dr_served_snapshot.c`, plus `DRCaptureServedSnapshot()` in
`procarray.c` (the capture has to live there, next to `KnownAssignedXids`).

1. **Capture, on arrival.** `pauseRecoveryOnRestorePoint()` (`xlog.c`) has just applied the
   restore-point record and is about to stop replay. It calls
   `DRCaptureServedSnapshot(rp)`, which takes `ProcArrayLock` SHARED and records
   `xmax = ShmemVariableCache->latestCompletedXid + 1`, the running set from
   `KnownAssignedXidsGetAndSetXmin()` (which includes prepared transactions), and
   `suboverflowed`. Because replay is stopping at that instant, this *is* the restore
   point's image.
2. **Hold as `pending`.** The image is tagged with the restore-point name and parked. It is
   not served yet: the nodes do not arrive together, and serving *N+1* on the first arrival
   while a segment is still short of it is the same torn read, one restore point later.
3. **Publish when all have arrived.** `gg_dr_switch(N+1)` polls `dr_all_paused_at()` — every
   node stopped, and stopped *at that name* — then dispatches the per-node publish, which
   flips `served ← pending` iff `pending.rpName` matches. A cancelled or crashed switch
   leaves `served` at *N*: stale, never wrong.
4. **Serve.** `GetSnapshotData()`'s recovery branch fills `xmin`/`xmax`/`subxip`/
   `suboverflowed` from `served` instead of scanning live, and sets `MyPgXact->xmin` from it
   — which enlists `ResolveRecoveryConflictWithSnapshot()` as the safety net for free.

**Reader gangs need no extra work.** They copy from the writer's `SharedLocalSnapshotSlot`,
and the writer's snapshot is the frozen one, so a whole gang is coherent by construction.

**Restart.** The image lives in shared memory, which the postmaster does not outlive. A
restarted node publishes on its own the first time it reaches a restore point
(`DRServedSnapshotPublishIfNothingServed()`); until then `ExecutorStart()` refuses queries.
See ADR-0006 D9 for the one window this leaves open.

### 5.4 End-to-end: a distributed read on the paused DR

```
client ──SELECT──▶ DR coordinator (in recovery, hot_standby, paused at restore point N)
  │
  ├─ ExecCheckXactReadOnly: read-only? ── if writing/locking → ERROR (DR replica, §4.3)
  │
  ├─ setupRegularDtxContext → DTX_CONTEXT_QD_STANDBY_READER   (no gxid, no WAL, §4.4)
  │
  ├─ GetSnapshotData → filled from the image frozen at N (§5.3); no distributed snapshot
  │
  ├─ ExecutorStart: is anything published?  if not → ERROR (nothing consistent to serve)
  │
  ├─ plan (read-only) → build gang on the DR segments (mirror selection, is_hs_dispatch)
  │
  └─ dispatch ▶ each DR segment QE (also in recovery):
         • DTX context info carries no distributed snapshot and no gxid; the QE context is
           chosen from IS_STANDBY_QE() instead (§4.5.3)
         • writer-gang gxid check exempted via IS_STANDBY_QE()  (§4.4)
         • visibility = that segment's OWN image frozen at N
         • rows ▶ coordinator ▶ client
```

### 5.5 Why this is coherent without a snapshot leader

The hardest part of feeding a writer-less reader gang a coherent snapshot is *coherence
under concurrent replay* — two slices on one segment could otherwise capture different local
snapshots as replay advances between them. **Freezing the image at the restore point
collapses this problem**, and collapses it more completely than pausing alone did: while the
cluster is paused the live snapshot happens to be static, but during an advance it is not,
and before this change a read taken mid-advance drifted with replay (measured: a distributed
transaction visible on one segment and absent on another for ~2.1 s). Serving the frozen
image makes every slice, every cursor, and every fresh statement see the same unchanging
picture from the moment *N* is published until *N+1* is, no matter what replay is doing
underneath. This is why the feature reuses the upstream hot-standby dispatch path directly
and did **not** need the bespoke "snapshot-leader" machinery from the SR-2 design note.
(The trade-off: a query that *spans* an advance can hit standard hot-standby recovery
conflicts and be cancelled — see §9.)

---

## 6. Operational workflows

All three workflows are driven by the `ggdr` utility (§7). Examples assume the
utility is on `PATH` or invoked as `python3 .../gpMgmt/bin/ggdr`, run **on the DR
coordinator host**, with `PGPORT`/`PGDATABASE` pointing at the DR coordinator.

### 6.1 Configure (build) a DR cluster

**Prerequisites on production:**

- `wal_level = replica`, `archive_mode = on`, an `archive_command` pushing each completed
  segment **per content** to the shared archive (the GP `%c` escape resolves to the
  content id, e.g. `test ! -f /archive/wal/seg%c/%f && cp %p /archive/wal/seg%c/%f`);
- a **per-instance base backup** taken with
  `pg_basebackup -X stream --target-gp-dbid <dbid>` into `basebackup/seg<content>/`
  (the `--target-gp-dbid` is hard-required by GP `pg_basebackup`);
- a **DR-local topology TSV** describing the *DR* cluster's dbids/ports/datadirs with
  DR-local hostnames/addresses (10 tab columns: `dbid content role preferred_role mode
  status port hostname address datadir`).

**Build the replica** — one command does restore + seed + arm + start for the whole
cluster:

```bash
ggdr create \
    --topology /archive/dr_topology.tsv \
    --backup   /archive/basebackup \
    --wal      /archive/wal \
    --pause-at rp_initial              # optional: pause at this restore point
```

What it does, in order (`cmd_create`):

1. **Safety / idempotence.** Refuse if any target datadir has a `postmaster.pid`; refuse a
   non-empty datadir unless `--force`.
2. **Restore** each instance's base backup into its DR datadir (`cp -a`, `chmod 700`);
   verify each has a `backup_label`.
3. **Archive-completeness gate (fail-closed).** For each content, verify the WAL archive
   directory is non-empty and contains the backup's **START WAL segment**. This was once a
   corruption gate — an archive miss could replay the deleted seed's forked WAL — and is now
   a usability one: without those segments the node starts and then waits forever for a file
   that is not coming, which is a far worse symptom to debug than a refusal here.
4. **Write the DR-local topology store** into **every** node's `$PGDATA/gg_topology`, via
   `gg_topology write` (§4.2). Every node, not just the coordinator: only the coordinator's
   copy is read while the replica serves, but a segment still holding production's entries
   would describe the wrong cluster the moment this replica is promoted and its FTS begins
   writing the store. The base backup carried production's copy of this file; this is the
   overwrite it exists for, and it must happen before any node starts.
5. **Arm the replica** on every node: empty `postgresql.auto.conf` (dropping production's
   sync-rep settings); append `gg_topology_source = file`, `hot_standby = on`,
   `archive_mode = off`, `restore_command`, `recovery_target_timeline = 'latest'`,
   `max_standby_archive_delay = 0` / `max_standby_streaming_delay = 0`, and (if `--pause-at`)
   the pause GUC; create `standby.signal`. `hot_standby = on` is the whole switch — there is
   no separate DR GUC or marker file (§4.1). For why the last two of those settings are
   `'latest'` and `off` rather than the obvious defaults, see §4.2.1.
6. **Start** each node in archive recovery (`pg_ctl`, coordinator in `gp_role=dispatch`,
   segments in `gp_role=execute`) and poll until all accept read connections.
7. **Post-build topology check** (`_check_topology()`): require **every** node to report
   `gg_topology_source = file`, and the coordinator's `gp_segment_configuration` to describe
   *this* cluster — one row per topology entry, matching `dbid`/`content`/`port`/`hostname`.
   Both halves fail the build rather than surfacing later as confusing symptoms: a node left
   on the catalog provider would describe production, and because the GUC is exempt from the
   QD/QE sync check a coordinator-on-file/segments-on-catalog split comes up silently.

At the end the DR cluster is live in recovery, refusing writes, verified to describe itself,
and (with `--pause-at`) paused at a consistent restore point ready to serve as-of-N reads.

### 6.2 Start / drive replication

Replication is continuous archive recovery — it runs by itself once armed. `ggdr`
controls *how far* the cluster replays and whether it exposes a consistent cut:

```bash
# Advance the whole cluster to a new consistent restore point and pause there:
ggdr switch rp_hourly_42        # → every node paused at rp_hourly_42 (as-of-N cut)

# Immediately halt replay on every node at wherever it is (NOT a consistent cut):
ggdr pause

# Inspect:
ggdr stat
```

There is deliberately **no free-running mode**. A replica only ever serves while paused at a
restore point: that is the one cut taken under `TwophaseCommitLock`, and therefore the only
one where the segments agree. Reads taken while replay advances have no cross-node guarantee
to offer, so the mode was removed rather than shipped with a caveat.

`switch <rp>` is the workhorse: it re-points the pause target on every node
(`ALTER SYSTEM` + reload), resumes replay, and waits until **all** nodes are paused at
`<rp>` — i.e. a consistent serve point. `<rp>` must be a restore point production already
created and that is **ahead** of the current position (recovery only moves forward). Note
`switch` can be issued *ahead of time*: if you arm it for a restore point that has not been
created yet, the following nodes catch it cleanly when its WAL arrives.

### 6.3 Promote the DR

Promotion turns the read-only DR cluster into a normal online read-write cluster, cutting
every node at the **same** restore point.

```bash
ggdr switch rp_failover_point       # reach a consistent cut first (if not already)
ggdr promote --at rp_failover_point  # promote there; --yes to skip the prompt
```

`cmd_promote` (two phases):

- **Precondition guard.** Refuse unless every node is in recovery, paused, and at **one**
  consistent restore point (equal to `--at` if given).
- **Phase 1 — exit recovery at N.** On each node `pg_promote(false)` + `pg_wal_replay_resume()`,
  **segments first, then the coordinator**, polling each out of recovery. This rides the
  `reachedContinuousRecoveryTarget` → `RECOVERY_TARGET_ACTION_PROMOTE` hook, so recovery ends
  exactly after N.
- **There is no phase 2.** DR behaviour is keyed on "in recovery with hot standby"
  (§4.1), so it disengages the instant recovery ends. The old design needed a second phase —
  stop every node, clear the markers, `ALTER SYSTEM SET gp_dr_replica = off`, restart — purely
  because that GUC was `PGC_POSTMASTER`. Removing the mode removed the restart with it.

After promotion `IsDRReplicaMode()` is false, writes succeed, and FTS/DTX run normally.
Promotion is **irreversible** (recovery only moves forward).

The same cut can be taken from SQL with `gg_dr_promote()` (§6.4), which is what a client
that can reach only the coordinator would use.

### 6.4 Driving recovery from SQL

Everything above is also available as SQL, dispatched cluster-wide from the coordinator, so
a client that can reach only the coordinator can drive a DR replica without shell access to
each host:

| Function | Purpose |
|---|---|
| `CALL gg_dr_switch(restore_point text)` | Re-point every node's pause target at `restore_point`, resume replay, wait until all of them are paused there, then start serving that cut everywhere. A **procedure**, not a function, so that it can wait without holding a snapshot: on a replica every snapshot is the image frozen at the served point, and a backend holding one is cancelled by the first prune replay applies behind it — which is what the earlier `SELECT gg_dr_switch()` form suffered on every switch that overlapped production activity ([ADR-0009](adr/0009-dr-control-plane-holds-no-snapshot.md)). Returns when the cut is consistent; cancel it like any statement (the arming stays applied — `ggdr switch` says so). Refused inside `REPEATABLE READ`/`SERIALIZABLE` and from within a function, where the snapshot cannot be dropped. |
| `CALL gg_dr_switch_node(restore_point text, publish bool)` | The per-node half the coordinator dispatches to each segment: arm the pause target (`false`) or start serving the image frozen there (`true`). Refused on a coordinator in dispatch mode; usable by hand on one node in utility mode. |
| `gg_dr_promote()` → `bool` | Promote the whole cluster at its current consistent restore point. Refuses unless every node is paused at one point, equal to `at_restore_point` when given. Irreversible. |

Status has no function of its own: `gg_stat_dr_replica` and `gg_stat_dr_replica_summary`
(§4.6) already report it cluster-wide, and duplicating them as a function would only create
a second thing to keep in step.

```sql
CALL gg_dr_switch('rp_hourly_43');     -- advance the cluster, wait for the cut
SELECT * FROM gg_stat_dr_replica_summary;   -- confirm consistent_restore_point
SELECT gg_dr_promote();            -- promote here
```

Both are superuser-only, must run on the coordinator in dispatch mode, and refuse outright
on a cluster that is not in recovery. Each applies its step to every primary segment with
`CdbDispatchCommand()` and to the coordinator by calling the same builtin directly. The
steps themselves are the same ones `ggdr` performs — `ALTER SYSTEM` on the pause GUC,
`pg_reload_conf()`, `pg_wal_replay_resume()`, and for promotion `pg_promote(false)` — and
they are legal in recovery because none of them writes WAL: `ALTER SYSTEM` writes
`postgresql.auto.conf` and the rest touch shared memory only.

What the switch dispatches is chosen so that no statement on a segment ever holds the
frozen image: the per-node step is `CALL gg_dr_switch_node(...)`, a procedure that drops
the snapshot its portal pushed before it does anything, and the poll is
`SHOW gg_dr_paused_restore_point`, a read-only GUC whose show hook reads the startup
process's shared state — `SHOW` is one of the utility statements that take no snapshot at
all. Neither goes through the executor, so neither is refused while a node has nothing to
serve, and neither can be cancelled by a recovery conflict.

Ordering matters in both, and is the reason these are functions rather than a documented
recipe: `switch` must re-point *before* resuming, or a node free-runs past the target to the
end of the WAL; `promote` must reach the segments before the coordinator, so the
coordinator's FTS finds them live when it starts probing.

---

## 7. `ggdr` utility reference

`gpMgmt/bin/ggdr` — a single-file Python 3 CLI that drives a DR read-replica's
recovery, treating the whole cluster (coordinator + every primary segment) as one.

### 7.1 Connection model

Run it **on the DR coordinator host**. The coordinator is reached through the standard
libpq environment (`PGPORT`, default 7000; `PGDATABASE`, default `postgres`); the node list
and each segment's port come from the DR's own `gp_segment_configuration` (`role='p'`). All
connections are **utility mode** (`PGOPTIONS='-c gp_role=utility'`), so `switch`/`stats` keep working even when there is no consistent serve point.

> **PoC scope:** every node is reached by its **local socket** on this host (single-host
> demo). A multi-host deployment needs per-segment `pg_hba`/ssh or one invocation per host;
> the segment `address` is kept in the node list for exactly that generalization.

### 7.2 Commands

| Command | Purpose |
|---------|---------|
| `switch <restore_point>` | Stop-and-go: re-point every node to `<restore_point>`, resume, wait until **all** are paused there, and publish that cut for reading. A wrapper around `gg_dr_switch()`; the publish step is why it cannot be done node by node. |
| `pause` | Immediately pause replay on every node at its current point. **Not** a consistent cut. |
| `stat` | Per-node recovery statistics + a cluster summary (mode, the point reads are served as of, the point replay stopped at, RPO). |
| `promote [--at <rp>] [--no-restart] [--yes] [--timeout <s>]` | Promote the DR cluster to online read-write at a single consistent restore point. Irreversible. |
| `create -t/--topology <tsv> -b/--backup <dir> -w/--wal <dir> [-r/--restore-command <cmd>] [-p/--pause-at <rp>] [--no-start] [-f/--force]` | Build the whole DR replica from per-instance base backups (§6.1). |

### 7.3 Examples

```bash
# Status — the summary line tells you if it is safe to serve consistent reads:
$ ggdr stat
  content  role         in_recovery paused serve_at               stopped_at             replay_lsn        replay_age
  -1       coordinator  yes         yes    rp_hourly_42           rp_hourly_42            0/9A00028         12s
  0        segment      yes         yes    rp_hourly_42           rp_hourly_42            0/7C00190         12s
  1        segment      yes         yes    rp_hourly_42           rp_hourly_42            0/7C00210         13s

  cluster: 3 node(s), all in recovery, all paused
  serving reads as of: rp_hourly_42   (safe to serve as-of-N reads)
  replay stopped at:   rp_hourly_42
  RPO: the served cut was taken on production ~13s ago; anything committed
       after it is not here, and is what promoting now would lose.

# The two point columns agree for a cluster driven only by whole-cluster switches.
# After 'switch <rp> --content …' they do not, and that is the state to read
# carefully: every node stopped at the new point, all of them still serving the
# old one, because nothing published.
  serving reads as of: rp_hourly_42   (safe to serve as-of-N reads)
  replay stopped at:   rp_hourly_43   <-- REACHED but NOT SERVED; reads still answer as of the point above
        publish it with 'ggdr switch rp_hourly_43' (no --content); a promotion would cut here, not at what is served.

# Advance the whole cluster to the next hourly restore point (consistent cut):
$ ggdr switch rp_hourly_43


# Planned switchover:
$ ggdr switch  rp_failover
$ ggdr promote --at rp_failover --yes

# Build a fresh DR replica (overwrite existing datadirs), armed but not started:
$ ggdr create -t /archive/dr_topology.tsv \
      -b /archive/basebackup \
      -w /archive/wal \
      -p rp_initial -f --no-start
```

---

## 8. Pros and cons

### 8.1 Pros

- **Faithful, reusable archive.** The apply-time filter keeps DR `pg_wal` **byte-identical**
  to production, so one archive serves production PITR, cascading DR replicas, and faithful
  `pg_waldump` — no second, rewritten WAL copy.
- **No production WAL pinning.** Archive transport uses **no replication slot**, so a slow
  or offline DR never bloats production's `pg_wal` (PG12 has no `max_slot_wal_keep_size`).
  Retention is the archive's concern.
- **Transactionally consistent distributed reads.** Reads are pinned to a
  `TwophaseCommitLock`-guarded distributed restore point and an as-of-N snapshot — a real
  cross-segment cut, not a `min(replay_lsn)` approximation. The straddle is handled
  explicitly.
- **Conflict-free serve windows.** While paused at N, replay is stopped ⇒ **zero
  recovery-conflict cancellation** for queries that start and finish within a serve window.
- **Minimal new core surface.** It reuses the shipped `gp_pause_on_restore_point_replay`
  primitive and the existing hot-standby dispatch, adding only the standby-reader context,
  the topology provider, the served restore-point snapshot, and enforcement/observability.
- **Promote in place**, cutting every node at the same restore point, with a clean
  two-phase disengage of DR mode.
- **WAN-tolerant / geo-separable.** Nothing requires a low-latency link; hosts and layout
  may differ (only the segment count must match).

### 8.2 Cons

- **Freshness is coarser than streaming.** RPO floor ≈ `archive_timeout` + archive transfer
  + restore cadence + slowest-segment skew. Reads are as-of the last restore point, not
  real-time.
- **Advances still cause hot-standby recovery conflicts.** A query that *spans* an
  `N → N+1` advance can be delayed then cancelled (`max_standby_archive_delay`); with a
  backend per segment, cancelling any one fails the whole distributed query.
- **Per-segment archive skew gates the whole cluster.** The cluster can only advance to N
  once **every** segment has archived and replayed through its N-LSN. The slowest archiver
  limits everyone.
- **New production-critical dependency: the archive.** Its availability, throughput,
  retention, and (for object stores) eventual consistency all matter.
- **Config is inherited and frozen** (roles, resource groups/queues, GUCs from replicated
  catalogs); changing them on the DR is unsupported.
- **PoC operational gaps** (see §9): single-host `ggdr` connectivity, no automated
  restore-point scheduler, mirrorless DR (no in-DR HA), no automatic
  latest-complete-common-restore-point computation on unclean failure.

---

## 9. Corner cases and limitations

**Consistency / correctness**

- **The straddle is handled; `min(replay_lsn)` is not a cut.** Only a distributed restore
  point is straddle-free at the segments, where a straddling transaction is prepared-only
  and therefore invisible through `KnownAssignedXids` (§5.2). Never approximate a
  consistent read with raw replay LSNs. The coordinator's own local xact *is* committed at
  such a cut — a narrow, loud, pre-existing asymmetry: see ADR-0006 D8.
- **`pause` is not a consistent cut — but it does not expose one either.** An immediate
  pause halts each node wherever it is, so the *replay* positions no longer agree and
  `consistent_restore_point` goes NULL. Reads go on answering as of the last point
  `switch` published. Only `switch <rp>` moves the served point, and only it is safe to
  promote from.
- **`switch`/`promote` only move forward.** You cannot switch back to an earlier restore
  point. Reach a fresh consistent cut with a restore point that is still ahead. Calling
  `switch` with the point the cluster is *already* stopped at is well defined and cheap: it
  re-publishes without resuming, which is what a restarted node needs.
- **The served image does not survive a restart, and one restart window is left open.** It
  lives in shared memory; a restarted node re-freezes and self-publishes the first time it
  reaches a restore point, and refuses queries until then. A node restarted *during* an
  advance publishes the new point before its peers do — see ADR-0006 D9 for why that is
  preferred to the alternative.
- **Index-only and bitmap scans lose the all-visible fast path** on a replica (ADR-0006
  D10). Correct answers, more heap fetches.

**Topology**

- **Production may run maintenance on its topology catalog.** `VACUUM`, `VACUUM FULL`,
  `REINDEX` and `TRUNCATE` were all prohibited while the replica read that catalog; they are
  now ordinary WAL to a replica that reads its own store (§4.2, fixture V-13′). The
  `relmap_redo` `FATAL` and the `smgr_redo` truncate guard that enforced the prohibition are
  deleted.
- **The replica's topology store is not WAL-logged, which is exactly the point and also the
  cost.** It is a plain file per node: it is not backed up by `pg_basebackup` of the replica,
  not repaired by replay, and a node that loses it will not start in dispatch mode. Rebuild
  it with `gg_topology write`, or re-run `ggdr create` for that node.
- **`gg_topology_source` is exempt from the QD/QE GUC-sync check**, so a
  coordinator-on-file/segments-on-catalog split does not announce itself. `ggdr create`
  arms every node in one loop and its post-build check requires all of them to report
  `file` (§6.1).

**Create**

- **The archive gate is fail-closed and stays that way.** `create` refuses unless each
  content's archive holds the backup's START WAL segment. It guarded a corruption case while
  the seed existed (an archive miss could replay the seed's forked WAL and fork the timeline
  permanently); with the seed gone it guards a usability one — a node that starts without
  those segments waits forever for a file that is not coming.

**Operational (PoC scope)**

- **`ggdr` is single-host.** It reaches every node by local socket; multi-host needs
  `pg_hba`/ssh or per-host invocation.
- **No restore-point scheduler.** Production must create distributed restore points itself
  (cadence = the RPO knob); the PoC does not ship the scheduler.
- **Mirrorless DR / whole-cluster failover only.** No DR-local mirrors and no DR-local FTS;
  a DR node loss means re-creating that node. FTS is inert on the DR by construction (it
  never leaves `PM_HOT_STANDBY`).
- **Unclean production loss.** Computing the *latest complete common restore point* when
  some segments archived past N and others did not is not automated; promote from a cut you
  have verified is consistent (`stats` → `consistent serve point`).
- **Production HA events fork timelines — and the replica follows.** A primary→mirror
  failover, or a coordinator activated from its standby, moves that content onto a new
  timeline; DR nodes run `recovery_target_timeline = 'latest'` and cross the fork using the
  archived `.history` file (§4.2.1, fixtures in §10.4). What this *requires of production*
  is the standing condition: **mirrors and the standby coordinator must be configured to
  archive**, or the WAL after a promotion never reaches the archive at all.
- **`gpexpand` invalidates the replica.** Changing production's segment count leaves the
  replica describing a cluster that no longer exists; rebuild it after the expansion.
- **Re-sync is a full re-base-backup.** There is no cross-WAN `pg_rewind`; a fallen-behind or
  gap-hit DR node is rebuilt with `create` for that node.

**Version / contract**

- Identical segment (content) count, major version and `CATALOG_VERSION_NO`, block size,
  checksum/`wal_log_hints` setting, and encoding on both clusters — physical replication is
  byte-level and mismatches are fatal. Host layout may differ.

---

## 10. Appendix

### 10.1 Code map

| Area | Location |
|------|----------|
| DR mode gate | `IsDRReplicaMode()` = `EnableHotStandby && RecoveryInProgress()`, `xlog.c`; decl `xlog.h` |
| Topology provider | `cdbtopology.c` (provider layer, write sets), `cdbtopology_file.c` (the file store), `cdbtopology_catalog.c` (the catalog store), format in `src/common/gg_topology_file.c` so initdb and frontends link it |
| WAL-filter tool (auxiliary) | `src/bin/gg_walfilter/` (`gg_walfilter.c` CLI, `segment.c` walker/decoder, `filter.c` rules + NOOP rewrite + lookback, `state.c` boundary state, `fetch.c` fetch/install); tests `src/test/dr/test_gg_walfilter.py` (black-box) |
| Topology store tool | `src/bin/gg_topology` (`dump` / `bootstrap` / `write`) |
| Read-only enforcement | `execMain.c:1651-1660`; DTM backstop `cdbtm.c:281-284` |
| FTS/autovac (structural) | `postmaster.c:6545-6578` (bgworker), `postmaster.c:2132-2134` (autovac) |
| Standby-reader context | `DTX_CONTEXT_QD_STANDBY_READER` `cdbtm.h:142-149`; select `cdbtm.c:1729-1743`; skips `cdbtm.c:417,452`, `execMain.c:666`; serialize `cdbdisp_dtx.c:158-177` |
| Standby QE exemption | `IS_STANDBY_QE()` `cdbvars.h:760`; use `xact.c:2510-2517` (and assert `xact.c:2438-2444`) |
| Hot-standby dispatch (reused) | `cdbutil.c:1027-1051`; `is_hs_dispatch` `cdbdisp_query.c:894,1006-1008`; protocol check `postgres.c:5559-5568`; gang `cdbgang.c:699` |
| Served restore-point snapshot | `dr_served_snapshot.{c,h}` (pending/served slots, publish, fill); capture `DRCaptureServedSnapshot()` `procarray.c`, called from `pauseRecoveryOnRestorePoint()` `xlog.c`; serve in `GetSnapshotData()`'s recovery branch; shmem registration `ipci.c` |
| Nothing-to-serve refusal | `standard_ExecutorStart()` `execMain.c` (`IsDRReplicaMode() && !DRServedSnapshotIsPublished()`) |
| Visibility-map bypass closed | `visibilitymap_get_status()` `visibilitymap.c` returns 0 in DR mode |
| QE context without a distributed snapshot | `setupQEDtxContext()` `cdbtm.c` — `isDispatchedQuery = haveDistributedSnapshot \|\| IS_STANDBY_QE()` |
| Pause mechanism | GUC `guc_gp.c:4727-4736`; `pauseRecoveryOnRestorePoint()` `xlog.c:5976` (call `xlog.c:7811-7812`); `recoveryPausesHere()` `xlog.c:6205`; SIGHUP `startup.c:159-168` |
| Served-N accessor | `pausedRestorePointName` `xlog.c:773` (capture :6005-6008, clear :6246); `GetPausedRestorePointName()` `xlog.c:6390-6396`; `pg_last_paused_restore_point()` `xlogfuncs.c:606-615` (OID 7016) |
| Views | `system_views.sql:1542-1573` (`gg_stat_dr_replica`, `gg_stat_dr_replica_summary`, `pg_catalog`) |
| Distributed barrier | `gp_create_restore_point()` `xlogfuncs_gp.c:35` (barrier :89-100) |
| SQL recovery control | `gg_dr_switch()` / `gg_dr_promote()` + helpers `xlogfuncs_gp.c`; catalog `pg_proc.dat` OIDs 7017/7018 |
| Utility | `gpMgmt/bin/ggdr` |

### 10.2 GUCs

| GUC | Context | Default | Role |
|-----|---------|---------|------|
| `hot_standby` | `PGC_POSTMASTER` | `false` | On a node in recovery, makes it a DR replica: read-only enforcement, the served restore-point snapshot, standby distributed read. Disengages with recovery. |
| `gp_pause_on_restore_point_replay` | `PGC_SUSET` | `''` | Pause replay when the named restore point is replayed; re-point + resume to advance. `gg_dr_switch()` maintains it. |
| `max_standby_archive_delay` | `PGC_SIGHUP` | `0` on a DR node (`ggdr create` writes it) | A frozen `xmin` makes recovery conflicts routine; the stock 30 s would be added to every advance that hits one. `-1` is a trap, not the other extreme — it blocks the startup process inside redo. |

### 10.3 Catalog / function objects

- `pg_last_paused_restore_point()` → `text` (OID 7016) — served restore point, or `NULL`.
- `gg_stat_dr_replica` (view, `pg_catalog`) — per-node recovery state.
- `gg_stat_dr_replica_summary` (view, `pg_catalog`) — cluster rollup incl.
  `consistent_restore_point`, `rpo_seconds`.
- `gg_dr_switch(text)` procedure (OID 7017) — advance the cluster to a restore point;
  `CALL` it. `gg_dr_switch_node(text, bool)` procedure (OID 7021) is its per-node half.
- `gg_dr_paused_restore_point` (read-only GUC) — the point this node is paused at, or
  empty; what the coordinator `SHOW`s on each segment while a switch waits.
- `gg_dr_promote()` → `bool` (OID 7018) — promote at the current cut.

### 10.4 Test fixture

`src/test/dr/` — two-container fixtures (production + DR sharing an `/archive` volume), all
building the replica with `ggdr create` against the same image. Four scenarios, each with its
own compose file and its own verdict line, so a failure attributes to one of them:

| Fixture | Production does | Verdict line |
|---|---|---|
| `docker-compose.yml` | the default run: topology change, restore points, straddle, promotion | six `… TEST: PASS` banners |
| `docker-compose.maint.yml` | `VACUUM` / `VACUUM FULL` / `REINDEX` / `TRUNCATE` on its topology catalog | `V-13' VERDICT:` |
| `docker-compose.failover.yml` | **runs with mirrors** and fails a segment over between two restore points | `FAILOVER VERDICT:` |
| `docker-compose.costandby.yml` | **runs with a standby coordinator** and activates it between two restore points | `STANDBY-ACTIVATION VERDICT:` |

The last two are the timeline-fork cases of §4.2.1; both assert that the forked content
followed production onto the new timeline, that the *other* contents did not, and that the
replica's catalog took production's topology change while the topology it serves did not
move. See `src/test/dr/README.md`.

Scenario catalogue: [`greengage-dr-test-scenarios.md`](greengage-dr-test-scenarios.md) —
what to test, what is already automated, and the expected behaviour of each corner case.

### 10.5 Related design notes (repo root)

- `greengage7-dr-readonly-replica-poc-plan.md` — master plan (transport, topology,
  milestones, risk register).
- `greengage7-dr-standby-distributed-read-mode.md` — the standby distributed-read design.
- `greengage7-dr-SR2-reader-gang-snapshot-sync.md` — reader-gang snapshot analysis (note:
  its snapshot-leader scheme was superseded by stop-and-go; see §5.5).
