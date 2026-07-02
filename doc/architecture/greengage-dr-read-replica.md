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
7. [`gg_recovery` utility reference](#7-gg_recovery-utility-reference)
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
work, plus the tooling (`gg_recovery`) to drive the lifecycle.

### 1.4 Milestone map

The implementation is organized as milestones; this document is structured around them:

| Milestone | Concern | Where in this doc |
|-----------|---------|-------------------|
| M1 | Topology independence (redo filter + frozen seed) | §4.2 |
| M2 | Read-only enforcement | §4.3 |
| M2′ | Standby distributed read (dispatch while in recovery) | §4.4 |
| M3 | Consistent reads (restore-point-gated, as-of-N snapshot) | §4.5, §5 |
| M4 | Build a DR replica (`gg_recovery create-replica`) | §6.1 |
| M5 | Observability (`gp_stat_dr_replica`, `pg_last_paused_restore_point`) | §4.6 |
| M6 | Promotion (`gg_recovery promote`) | §6.3 |

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
                                                                     gg_recovery drives the whole cluster
                                                                     as one: switch / follow / pause /
                                                                     stats / promote / create-replica
```

### 2.1 Properties

- **Per-content continuous restore.** DR coordinator replays production's coordinator
  WAL; DR segment *cN* replays production primary *cN*'s WAL. The **segment count must
  match**; host layout is free. Each node has its **own independent WAL/LSN space**.
- **DR-local topology**, protected from replayed WAL by the apply-time redo filter (§4.2).
- **Read-only** end-to-end (§4.3), enforced with a DR-specific error, but still able to
  **dispatch distributed reads** to segments in recovery (§4.4).
- **Restore-point-gated consistency** (§4.5): the cluster pauses replay at a distributed
  restore point *N* and serves an **as-of-N** distributed snapshot — a transactionally
  consistent cut, not a `min(replay_lsn)` approximation.
- **Promote in place** to an online read-write cluster (§6.3).

### 2.2 Component inventory — DR-new vs. reused

Being explicit about provenance matters for maintenance:

**DR-new code** (2026 commits on `7.x-dr`):

- `IsDRReplicaMode()` + the M1 topology redo filter (`dr_redo_filter.c`).
- M2 read-only enforcement + `hot_standby` auto-enable.
- The `DTX_CONTEXT_QD_STANDBY_READER` distributed-transaction context.
- The M3 as-of-N distributed snapshot builder `CreateDRStandbyDistributedSnapshot()`.
- The M5 served-restore-point accessor + `gp_stat_dr_replica` views.
- The `gg_recovery` / `gpseed_dr_topology` utilities.

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
| 2 | **Topology independence via an apply-time redo filter + frozen-tuple seed**, not a WAL rewriter | You cannot delete bytes from physical WAL (LSN = byte offset; `xl_prev` chain; page `pd_lsn` interlock). Rewriting records in place would corrupt the canonical archive or force a second copy. Filtering at *apply* time keeps `pg_wal` byte-identical to production. |
| 3 | **Stop-and-go consistency** (pause at restore point *N*, serve, advance to *N+1*), reusing the shipped `gp_pause_on_restore_point_replay` GUC | Greengage already ships the pause primitive. Stop-and-go gives a clean static as-of-N image, minimal MVCC masking, and **zero recovery-conflict exposure during serve windows** (replay is stopped). Conflicts are confined to advance bursts. |
| 4 | **Reuse upstream hot-standby dispatch**; add only the no-gxid standby-reader context | Minimizes new core surface; the risky gang/snapshot machinery already exists and is tested. |
| 5 | **Whole-cluster failover only** for the PoC (mirrorless DR) | Keeps the novel cross-cluster work in focus; DR-local mirrors + DR-local FTS are deferred. |

---

## 4. How it is implemented

### 4.1 The DR-mode gate

Everything keys off one predicate:

```c
bool IsDRReplicaMode(void);   /* src/backend/access/transam/xlog.c:5597-5623 */
```

It returns true **iff both**:

1. the **`gp_dr_replica`** GUC is on — a `PGC_POSTMASTER` boolean, default `false`
   (`guc_gp.c:551-559`; variable `cdbvars.c:337`; name registered in
   `unsync_guc_name.h:147`); and
2. the **`dr_replica.signal`** marker file exists in the data directory
   (`DR_REPLICA_SIGNAL_FILE`, `xlog.h:404`).

Requiring **both** is deliberate: promotion removes the marker, so DR mode disengages even
if the GUC value lingers in a config file until the next restart. The marker is detected at
startup in `readRecoverySignalFile()` (`xlog.c:5490`, sets the flag at `xlog.c:5554-5561`)
alongside `standby.signal`; because a backend forks from the postmaster and never runs
recovery itself, `IsDRReplicaMode()` lazily `stat()`s and caches the marker on first use in
a backend.

**`hot_standby` is auto-enabled.** So the operator never has to set it, the postmaster
forces `EnableHotStandby = true` when `gp_dr_replica` is set and the marker is present
(`postmaster.c:1091-1101`). It `stat()`s the marker directly there because the startup
child's flag is not yet visible in the postmaster.

### 4.2 M1 — Topology independence

Two cooperating mechanisms let the DR cluster describe *itself* while replaying
production's WAL.

#### 4.2.1 Apply-time redo filter

In the redo loop, before dispatching a record to its resource-manager `rm_redo`:

```c
/* src/backend/access/transam/xlog.c:7727-7728 */
if (!DRRedoShouldFilter(xlogreader))
    RmgrTable[record->xl_rmid].rm_redo(xlogreader);
```

`DRRedoShouldFilter()` (`src/backend/access/transam/dr_redo_filter.c:195-225`):

- returns `false` (apply normally) unless `IsDRReplicaMode()`;
- iterates the record's block references; if **every** referenced block falls in a
  **protected set**, it returns `true` and the record's `rm_redo` is **skipped**. If *any*
  referenced block is outside the set, the record is applied normally. A record with no
  block references is always applied.

The **protected set** is the cluster-topology catalogs, resolved from raw BKI OIDs
(`dr_redo_filter.c:43-52`) to relfilenodes via the **shared relmapper**
(`DRResolveProtectedRelfilenodes()`, `dr_redo_filter.c:71-104`, using
`RelationMapOidToFilenode(reloid, /*shared=*/true)`):

| OID | Catalog |
|-----|---------|
| 5036 | `gp_segment_configuration` |
| 7139 / 7140 | its two indexes |
| 6092 / 6093 | its TOAST table + index |
| 5106 | `gp_configuration_history` |
| 5101 | `gp_id` |
| 5103 | `gp_version_at_initdb` |

A block is protected only if it is in the **global tablespace and shared**
(`spcNode == GLOBALTABLESPACE_OID && dbNode == InvalidOid`,
`DRRelfilenodeIsProtected()`, `dr_redo_filter.c:164-183`).

Two important consequences:

- **The replay LSN still advances.** Skipping `rm_redo` does not touch the LSN
  bookkeeping — `replayEndRecPtr` (`xlog.c:7708-7711`) and `lastReplayedEndRecPtr`
  (`xlog.c:7746-7749`) are set unconditionally around the redo call. The DR node stays
  in lock-step with production's WAL position; it just doesn't *apply* the protected
  records.
- **`pg_wal` stays byte-identical to production's.** The filter changes what is *applied*,
  not the bytes. This preserves faithful `pg_waldump`/PITR and deterministic crash
  recovery (the filter re-applies identically on restart).

#### 4.2.2 Frozen-tuple seed

After a base backup is restored, `gp_segment_configuration` still holds production's rows.
`gpseed_dr_topology` (`gpMgmt/bin/gpseed_dr_topology`) replaces them with DR-local rows
**before** `standby.signal` is placed:

1. start the coordinator **standalone** (`postgres --single`, `allow_system_table_mods=on`);
2. `DELETE FROM gp_segment_configuration`, insert the DR-local rows from a topology TSV;
3. `VACUUM FREEZE gp_segment_configuration` — the rows get `xmin = FrozenTransactionId`, so
   they are **unconditionally visible with no CLOG dependency** (production's replayed XID
   timeline can never make them invisible);
4. `CHECKPOINT`, shut down.

Because those pages are frozen **and** the redo filter blocks production's records from
overwriting them, the DR-local topology persists through replay. The frozen tuples are
what make the DR cluster genuinely describe itself.

#### 4.2.3 Protecting against topology-catalog rewrites

If production ever `VACUUM FULL` / `CLUSTER` / `REINDEX` / `TRUNCATE`s a protected catalog,
its relfilenode changes and a shared relmap update is WAL-logged. There is **no
preemptive block on the production side**. Instead the DR replica **halts reactively**:
`relmap_redo()` (`relmapper.c:1032-1048`) calls `DRRejectForbiddenRemap()`
(`dr_redo_filter.c:138-158`), which `ereport(FATAL)`s if an incoming mapping would change a
protected catalog's relfilenode — halting the postmaster *before* any on-disk write, so the
replay LSN does not advance past the offending record. The fix is to re-create that DR node
from a fresh base backup. (This is a documented contract limitation, §9.)

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
coordinator-entry row with segment rows, as `gp_stat_dr_replica` does) are planned onto a
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
replay (`SetRecoveryPause(true)` + `recoveryPausesHere()`, `xlog.c:6205`). The node keeps
serving reads while paused. To **advance** to *N+1*: set the GUC to the next restore point
and `SIGHUP` (the paused startup process re-reads config via `HandleStartupProcInterrupts()`
→ `ProcessConfigFile`, `startup.c:159-168`), then `pg_wal_replay_resume()`. Order matters —
re-point *before* resume, or the node free-runs to end-of-WAL.

A clean cross-node cut is reached when **every** node is paused at the **same** restore
point. That is exactly the "consistent serve point" `gg_recovery` computes (§7).

#### 4.5.3 The as-of-N distributed snapshot

When a standby-reader coordinator builds a snapshot, `GetSnapshotData()` routes to the
DR-specific builder (`procarray.c:2570-2577`):

```c
if (distributedTransactionContext == DTX_CONTEXT_QD_STANDBY_READER)
    CreateDRStandbyDistributedSnapshot(ds);
else
    CreateDistributedSnapshot(ds);
```

`CreateDRStandbyDistributedSnapshot()` (`procarray.c:2166`) reconstructs the distributed
snapshot **from replayed state** — it cannot mint a live one during recovery. See §5 for
the full flow.

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

- **`gp_stat_dr_replica`** — one row per node
  (`gp_segment_id, role, dr_replica, in_recovery, replay_lsn, replay_time, is_paused,
  restore_point`). The coordinator row `UNION ALL`s the per-segment rows via
  `gp_dist_random('gp_id')`, so each node reports its own local recovery state.
- **`gp_stat_dr_replica_summary`** — one-row rollup:
  `node_count, all_dr_replica, all_in_recovery, all_paused, consistent_restore_point,
  rpo_seconds`. `consistent_restore_point` is non-`NULL` **only** when every node is paused
  at the same restore point
  (`CASE WHEN count(*) = count(restore_point) AND count(DISTINCT restore_point) = 1 THEN
  max(restore_point) ELSE NULL END`); `rpo_seconds` is
  `extract(epoch FROM now() - min(replay_time))`.

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
  (`KnownAssignedXids`). Nothing DR-specific here.
- **Distributed (cluster-wide):** a `DistributedSnapshot` that maps each tuple's creating
  `gxid` to visible / invisible so all segments agree. On production this is minted live
  from `nextGxid`; on the DR it must be **reconstructed from replayed state**.

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
segments; the coordinator side is handled by the snapshot builder below.

### 5.3 `CreateDRStandbyDistributedSnapshot()` — step by step

`src/backend/storage/ipc/procarray.c:2166`, called under `ProcArrayLock`:

1. **Upper bound.** `xmax = ShmemVariableCache->nextGxid` (`procarray.c:2177`). On the DR,
   `nextGxid` is advanced during redo by `XLOG_NEXTGXID` records, so it is a safe upper
   bound of "all gxids that could exist as of the replayed position." `xmin` is seeded to
   `xmax` and lowered below.
2. **In-doubt / invisible set.** Loop `i` over `[0, *shmNumCommittedGxacts)`; for each
   `gxid = shmCommittedGxidArray[i]` that is valid and `< xmax`, lower `xmin` to it and
   **add it to `ds->inProgressXidArray`** (`procarray.c:2180-2189`).
3. **Publish.** `ds->xmin = ds->xminAllDistributedSnapshots = xmin`, `ds->xmax = xmax`,
   `ds->count`, and a fresh `distribSnapshotId` (`procarray.c:2193-2197`).

**The semantic inversion.** On a live primary, `shmCommittedGxidArray` holds
distributed-committed transactions — *visible*. On the DR standby they are placed in the
**in-progress (invisible) set**. That is exactly the straddle compensation: a transaction
that is committed on the coordinator but possibly prepared-only on the segments is treated
as **in-doubt ⇒ invisible** everywhere, so the cross-node read is consistent. A transaction
that has been fully *forgotten* (past its 2PC completion) is absent from the array ⇒
visible. The array's shared-memory backing is wired at `cdbtm.c:1149-1150`.

### 5.4 End-to-end: a distributed read on the paused DR

```
client ──SELECT──▶ DR coordinator (in recovery, hot_standby, paused at restore point N)
  │
  ├─ ExecCheckXactReadOnly: read-only? ── if writing/locking → ERROR (DR replica, §4.3)
  │
  ├─ setupRegularDtxContext → DTX_CONTEXT_QD_STANDBY_READER   (no gxid, no WAL, §4.4)
  │
  ├─ GetSnapshotData → CreateDRStandbyDistributedSnapshot(ds)  (as-of-N, §5.3)
  │        local snapshot = this node's hot-standby recovery snapshot
  │
  ├─ plan (read-only) → build gang on the DR segments (mirror selection, is_hs_dispatch)
  │
  └─ dispatch ▶ each DR segment QE (also in recovery):
         • DTX context info carries the as-of-N distributed snapshot, no gxid
         • writer-gang gxid check exempted via IS_STANDBY_QE()  (§4.4)
         • local visibility = segment's own hot-standby snapshot; distributed = ds
         • rows ▶ coordinator ▶ client
```

### 5.5 Why stop-and-go makes this coherent without a snapshot leader

The hardest part of feeding a writer-less reader gang a coherent snapshot is *coherence
under concurrent replay* — two slices on one segment could otherwise capture different local
snapshots as replay advances between them. **Under stop-and-go this problem collapses:**
while the cluster is paused at *N*, **replay is stopped**, so the local recovery snapshot is
**static** for the whole serve window. Every slice and every cursor reader on a segment sees
the same unchanging image, and the distributed dimension is the fixed as-of-N `ds`. This is
why the feature reuses the upstream hot-standby dispatch path directly and did **not** need
to build the bespoke "snapshot-leader" machinery from the SR-2 design note. (The trade-off:
a query that *spans* an advance can hit standard hot-standby recovery conflicts — see §9.)

---

## 6. Operational workflows

All three workflows are driven by the `gg_recovery` utility (§7). Examples assume the
utility is on `PATH` or invoked as `python3 .../gpMgmt/bin/gg_recovery`, run **on the DR
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
gg_recovery create-replica \
    --topology     /archive/dr_topology.tsv \
    --basebackup-dir /archive/basebackup \
    --wal-archive  /archive/wal \
    --pause-at     rp_initial          # optional: pause at this restore point
```

What it does, in order (`cmd_create_replica`):

1. **Safety / idempotence.** Refuse if any target datadir has a `postmaster.pid`; refuse a
   non-empty datadir unless `--force`.
2. **Restore** each instance's base backup into its DR datadir (`cp -a`, `chmod 700`);
   verify each has a `backup_label`.
3. **Archive-completeness gate (fail-closed).** For each content, verify the WAL archive
   directory is non-empty and contains the backup's **START WAL segment**. If not, abort —
   because resuming without the archived `[REDO..backup-end]` WAL could replay the seed's
   forked WAL and permanently fork the timeline.
4. **Frozen-topology seed** on the coordinator: save `backup_label` + `global/pg_control`
   aside, run `gpseed_dr_topology` (§4.2.2), then **restore** the saved recovery-start state
   so recovery resumes from the backup checkpoint, and **re-fetch** any backup-tail WAL
   segment from the archive over `pg_wal` (so the seed's WAL can never win over the archive).
5. **Arm DR mode** on every node: empty `postgresql.auto.conf`; append `gp_dr_replica = on`,
   `restore_command`, `recovery_target_timeline = 'current'`, and (if `--pause-at`) the
   pause GUC; create `dr_replica.signal` + `standby.signal`.
6. **Start** each node in archive recovery (`pg_ctl`, coordinator in `gp_role=dispatch`,
   segments in `gp_role=execute`) and poll until all accept read connections.

At the end the DR cluster is live in recovery, refusing writes, and (with `--pause-at`)
paused at a consistent restore point ready to serve as-of-N reads.

### 6.2 Start / drive replication

Replication is continuous archive recovery — it runs by itself once armed. `gg_recovery`
controls *how far* the cluster replays and whether it exposes a consistent cut:

```bash
# Advance the whole cluster to a new consistent restore point and pause there:
gg_recovery switch rp_hourly_42        # → every node paused at rp_hourly_42 (as-of-N cut)

# Continuous mode (freshest reads, NO cross-node consistent cut):
gg_recovery follow                     # clear the pause target, free-run to end-of-WAL

# Immediately halt replay on every node at wherever it is (NOT a consistent cut):
gg_recovery pause

# Inspect:
gg_recovery stats
```

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
gg_recovery switch rp_failover_point       # reach a consistent cut first (if not already)
gg_recovery promote --at rp_failover_point  # promote there; --yes to skip the prompt
```

`cmd_promote` (two phases):

- **Precondition guard.** Refuse unless every node is in recovery, paused, and at **one**
  consistent restore point (equal to `--at` if given).
- **Phase 1 — exit recovery at N.** On each node `pg_promote(false)` + `pg_wal_replay_resume()`,
  **segments first, then the coordinator**, polling each out of recovery. This rides the
  `reachedContinuousRecoveryTarget` → `RECOVERY_TARGET_ACTION_PROMOTE` hook, so recovery ends
  exactly after N.
- **Phase 2 — disengage DR mode.** `gp_dr_replica` is `PGC_POSTMASTER`, so it (and the
  cached `dr_replica.signal`) only lift on restart: `ALTER SYSTEM SET gp_dr_replica = off`,
  stop each node, remove `dr_replica.signal` + `standby.signal`, and restart **segments
  first, then the coordinator** (so the coordinator's freshly-started FTS probes live
  segments). `--no-restart` stops after phase 1 for demo/inspection (nodes out of recovery
  but still DR-read-only until restart).

After promotion `IsDRReplicaMode()` is false, writes succeed, and FTS/DTX run normally.
Promotion is **irreversible** (recovery only moves forward).

---

## 7. `gg_recovery` utility reference

`gpMgmt/bin/gg_recovery` — a single-file Python 3 CLI that drives a DR read-replica's
recovery, treating the whole cluster (coordinator + every primary segment) as one.

### 7.1 Connection model

Run it **on the DR coordinator host**. The coordinator is reached through the standard
libpq environment (`PGPORT`, default 7000; `PGDATABASE`, default `postgres`); the node list
and each segment's port come from the DR's own `gp_segment_configuration` (`role='p'`). All
connections are **utility mode** (`PGOPTIONS='-c gp_role=utility'`), so `switch`/`follow`/
`stats` keep working even when there is no consistent serve point.

> **PoC scope:** every node is reached by its **local socket** on this host (single-host
> demo). A multi-host deployment needs per-segment `pg_hba`/ssh or one invocation per host;
> the segment `address` is kept in the node list for exactly that generalization.

### 7.2 Commands

| Command | Purpose |
|---------|---------|
| `switch <restore_point>` | Stop-and-go: re-point every node to `<restore_point>`, resume, and wait until **all** are paused there (a consistent as-of-N cut). |
| `pause` | Immediately pause replay on every node at its current point. **Not** a consistent cut. |
| `follow` | Continuous recovery: clear the pause target everywhere and free-run to end-of-WAL. Freshest reads, no consistent cut. |
| `stats` | Per-node recovery statistics + a cluster summary (mode, consistent serve point, RPO). |
| `promote [--at <rp>] [--no-restart] [--yes] [--timeout <s>]` | Promote the DR cluster to online read-write at a single consistent restore point. Irreversible. |
| `create-replica --topology <tsv> --basebackup-dir <dir> --wal-archive <dir> [--restore-command <cmd>] [--pause-at <rp>] [--no-start] [--force]` | Build the whole DR replica from per-instance base backups (§6.1). |

### 7.3 Examples

```bash
# Status — the summary line tells you if it is safe to serve consistent reads:
$ gg_recovery stats
  content  role         in_recovery paused restore_point          replay_lsn        replay_age
  -1       coordinator  yes         yes    rp_hourly_42            0/9A00028         12s
  0        segment      yes         yes    rp_hourly_42            0/7C00190         12s
  1        segment      yes         yes    rp_hourly_42            0/7C00210         13s

  cluster: 3 node(s), all in recovery, all paused
  consistent serve point: rp_hourly_42   (safe to serve as-of-N reads)
  RPO: ~13s behind production's last replayed commit

# Advance the whole cluster to the next hourly restore point (consistent cut):
$ gg_recovery switch rp_hourly_43

# Switch heavy reporting off a stable older point, run continuous otherwise:
$ gg_recovery follow

# Planned switchover:
$ gg_recovery switch  rp_failover
$ gg_recovery promote --at rp_failover --yes

# Build a fresh DR replica (overwrite existing datadirs), armed but not started:
$ gg_recovery create-replica \
      --topology /archive/dr_topology.tsv \
      --basebackup-dir /archive/basebackup \
      --wal-archive /archive/wal \
      --pause-at rp_initial --force --no-start
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
  the redo filter, the as-of-N snapshot builder, and enforcement/observability.
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
- **PoC operational gaps** (see §9): single-host `gg_recovery` connectivity, no automated
  restore-point scheduler, mirrorless DR (no in-DR HA), no automatic
  latest-complete-common-restore-point computation on unclean failure.

---

## 9. Corner cases and limitations

**Consistency / correctness**

- **The straddle is handled; `min(replay_lsn)` is not a cut.** Only a distributed restore
  point is straddle-free at the segments; the coordinator side treats
  `shmCommittedGxidArray` as invisible (§5). Never approximate a consistent read with raw
  replay LSNs.
- **`pause` is not a consistent cut.** An immediate pause halts each node wherever it is;
  only `switch <rp>` (all nodes at the same restore point) is safe to serve consistent
  reads or to promote from.
- **`switch`/`promote` only move forward.** You cannot switch back to an earlier restore
  point; once `follow` has drained past a point, re-`switch`ing to it will time out. Reach a
  fresh consistent cut with a restore point that is still ahead.

**Topology filter**

- **Production `VACUUM FULL`/`CLUSTER`/`REINDEX`/`TRUNCATE` on a protected topology catalog
  halts the DR.** It is caught reactively as a `FATAL` in `relmap_redo`
  (`DRRejectForbiddenRemap`), *not* blocked on production. Recovery is to re-base-backup +
  re-seed that DR node. Treat these catalogs as immutable while a DR replica is attached.
- **The filter reduces WAL *apply*, not WAL *volume*.** Topology records are tiny; the
  filter does not save bandwidth. Bandwidth is a `wal_compression` / relay concern.
- **Frozen-seed footprint (bounded, self-healing).** `VACUUM FREEZE` of
  `gp_segment_configuration` does in-place relstats updates on a few `pg_class` pages
  (`pg_class` is *not* protected); production changes co-located on those pages within the
  `(backup-LSN, seed-LSN]` window are LSN-shadowed until the next production FPI heals them
  — harmless on a read-only replica. A production-grade create utility should seed against a
  scratch copy and transplant back only the protected forks.

**Seeding / create**

- **Timeline-fork hazard around the seed.** The single-user seed consumes `backup_label`
  and advances `pg_control`; `create-replica` preserves and restores the recovery-start
  state and **re-fetches** backup-tail WAL from the archive, and it **gates fail-closed** on
  the backup's START WAL segment being present in the archive. Without those, an archive
  miss could replay the seed's forked WAL and permanently fork the timeline.
- **`--scratch-seed` is reserved / not implemented** in the PoC.

**Operational (PoC scope)**

- **`gg_recovery` is single-host.** It reaches every node by local socket; multi-host needs
  `pg_hba`/ssh or per-host invocation.
- **No restore-point scheduler.** Production must create distributed restore points itself
  (cadence = the RPO knob); the PoC does not ship the scheduler.
- **Mirrorless DR / whole-cluster failover only.** No DR-local mirrors and no DR-local FTS;
  a DR node loss means re-creating that node. FTS is inert on the DR by construction (it
  never leaves `PM_HOT_STANDBY`).
- **Unclean production loss.** Computing the *latest complete common restore point* when
  some segments archived past N and others did not is not automated; promote from a cut you
  have verified is consistent (`stats` → `consistent serve point`).
- **Production HA events fork timelines.** A production primary→mirror failover switches that
  segment's timeline; the DR must follow via the archived `.history` file. Reliable
  history-file archiving is required; "mirror failover while DR is attached" is a scenario to
  test explicitly. DR nodes are configured with `recovery_target_timeline = 'current'`.
- **Re-sync is a full re-base-backup.** There is no cross-WAN `pg_rewind`; a fallen-behind or
  gap-hit DR node is rebuilt with `create-replica` for that node + re-seed.

**Version / contract**

- Identical segment (content) count, major version and `CATALOG_VERSION_NO`, block size,
  checksum/`wal_log_hints` setting, and encoding on both clusters — physical replication is
  byte-level and mismatches are fatal. Host layout may differ.

---

## 10. Appendix

### 10.1 Code map

| Area | Location |
|------|----------|
| DR mode gate | `IsDRReplicaMode()` `xlog.c:5597-5623`; decl `xlog.h:319`; marker `DR_REPLICA_SIGNAL_FILE` `xlog.h:404`; startup detect `xlog.c:5490,5554-5561` |
| `gp_dr_replica` GUC | `guc_gp.c:551-559` (`PGC_POSTMASTER`); var `cdbvars.c:337`; name `unsync_guc_name.h:147` |
| hot_standby auto-enable | `postmaster.c:1091-1101` |
| Redo filter | `dr_redo_filter.c` (`DRRedoShouldFilter` :195-225, protected OIDs :43-52, resolve :71-104, match :164-183, remap-reject :138-158); dispatch `xlog.c:7727-7728`; header `dr_redo_filter.h:32-49`; remap redo `relmapper.c:1032-1048` |
| Frozen seed | `gpMgmt/bin/gpseed_dr_topology` |
| Read-only enforcement | `execMain.c:1651-1660`; DTM backstop `cdbtm.c:281-284` |
| FTS/autovac (structural) | `postmaster.c:6545-6578` (bgworker), `postmaster.c:2132-2134` (autovac) |
| Standby-reader context | `DTX_CONTEXT_QD_STANDBY_READER` `cdbtm.h:142-149`; select `cdbtm.c:1729-1743`; skips `cdbtm.c:417,452`, `execMain.c:666`; serialize `cdbdisp_dtx.c:158-177` |
| Standby QE exemption | `IS_STANDBY_QE()` `cdbvars.h:760`; use `xact.c:2510-2517` (and assert `xact.c:2438-2444`) |
| Hot-standby dispatch (reused) | `cdbutil.c:1027-1051`; `is_hs_dispatch` `cdbdisp_query.c:894,1006-1008`; protocol check `postgres.c:5559-5568`; gang `cdbgang.c:699` |
| As-of-N snapshot | `CreateDRStandbyDistributedSnapshot()` `procarray.c:2166`; wiring `procarray.c:2570-2577`; array backing `cdbtm.c:1149-1150` |
| Pause mechanism | GUC `guc_gp.c:4727-4736`; `pauseRecoveryOnRestorePoint()` `xlog.c:5976` (call `xlog.c:7811-7812`); `recoveryPausesHere()` `xlog.c:6205`; SIGHUP `startup.c:159-168` |
| Served-N accessor | `pausedRestorePointName` `xlog.c:773` (capture :6005-6008, clear :6246); `GetPausedRestorePointName()` `xlog.c:6390-6396`; `pg_last_paused_restore_point()` `xlogfuncs.c:606-615` (OID 7016) |
| Views | `system_views.sql:1542-1573` (`gp_stat_dr_replica`, `gp_stat_dr_replica_summary`, `pg_catalog`) |
| Distributed barrier | `gp_create_restore_point()` `xlogfuncs_gp.c:35` (barrier :89-100) |
| Utility | `gpMgmt/bin/gg_recovery` |

### 10.2 GUCs

| GUC | Context | Default | Role |
|-----|---------|---------|------|
| `gp_dr_replica` | `PGC_POSTMASTER` | `false` | Marks a DR replica; drives read-only enforcement + redo filter. Set off on promotion. |
| `gp_pause_on_restore_point_replay` | `PGC_SUSET` | `''` | Pause replay when the named restore point is replayed; re-point + resume to advance. |

### 10.3 Catalog / function objects

- `pg_last_paused_restore_point()` → `text` (OID 7016) — served restore point, or `NULL`.
- `gp_stat_dr_replica` (view, `pg_catalog`) — per-node recovery state.
- `gp_stat_dr_replica_summary` (view, `pg_catalog`) — cluster rollup incl.
  `consistent_restore_point`, `rpo_seconds`.

### 10.4 Test fixture

`src/test/dr/` — a two-container fixture (production + DR sharing an `/archive` volume) that
builds the DR via `gg_recovery create-replica` and exercises M1/M2/M2′/M3/M5 plus
`gg_recovery` `switch`/`follow`/`stats`/`promote` end-to-end
(`docker-compose -f src/test/dr/docker-compose.yml up --build`).

### 10.5 Related design notes (repo root)

- `greengage7-dr-readonly-replica-poc-plan.md` — master plan (transport, topology,
  milestones, risk register).
- `greengage7-dr-standby-distributed-read-mode.md` — the standby distributed-read design.
- `greengage7-dr-SR2-reader-gang-snapshot-sync.md` — reader-gang snapshot analysis (note:
  its snapshot-leader scheme was superseded by stop-and-go; see §5.5).
