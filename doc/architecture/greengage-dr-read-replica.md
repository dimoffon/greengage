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
| M1 | Topology independence (redo filter + frozen seed) | §4.2 |
| M2 | Read-only enforcement | §4.3 |
| M2′ | Standby distributed read (dispatch while in recovery) | §4.4 |
| M3 | Consistent reads (restore-point-gated, as-of-N snapshot) | §4.5, §5 |
| M4 | Build a DR replica (`ggdr create-replica`) | §6.1 |
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
                                                                     stats / promote / create-replica
```

### 2.1 Properties

- **Per-content continuous restore.** DR coordinator replays production's coordinator
  WAL; DR segment *cN* replays production primary *cN*'s WAL. The **segment count must
  match**; host layout is free. Each node has its **own independent WAL/LSN space**.
- **DR-local topology**, protected from replayed WAL by the apply-time redo filter (§4.2).
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

- `IsDRReplicaMode()` + the M1 topology redo filter (`dr_redo_filter.c`).
- M2 read-only enforcement.
- The `DTX_CONTEXT_QD_STANDBY_READER` distributed-transaction context.
- The M3 served restore-point snapshot (`dr_served_snapshot.c`, ADR-0005).
- The M5 served-restore-point accessor + `gg_stat_dr_replica` views.
- The `gg_dr_switch()` / `gg_dr_promote()` recovery-control functions (§6.4).
- The `ggdr` / `ggseed_dr_topology` utilities.

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
| 3 | **Stop-and-go consistency** (pause at restore point *N*, serve, advance to *N+1*), reusing the shipped `gp_pause_on_restore_point_replay` GUC | Greengage already ships the pause primitive. It gives a clean as-of-N image with **zero recovery-conflict exposure during serve windows** (replay is stopped); conflicts are confined to advance bursts. Each node also *freezes* that image (ADR-0005), so the window holds while the cluster advances rather than only while it sits still. |
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
whole DR behaviour: the apply-time topology redo filter (§4.2), read-only enforcement
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
  GUC, so the startup process (which runs the redo filter) and every backend (which runs the
  read-only enforcement) see the same value. The old design needed a lazily-`stat()`ed,
  per-backend cache of the marker file precisely because a backend never runs recovery.
- **`hot_standby` defaults to `off`**, so ordinary mirrors and the standby coordinator are
  unaffected. **Do not turn it on for them.** The topology filter would freeze their
  `gp_segment_configuration`, and a later FTS failover (or `gpactivatestandby`) would promote
  a node describing a stale cluster. `hot_standby = on` means *"this node is a separate DR
  cluster"*.

### 4.2 M1 — Topology independence

Two cooperating mechanisms let the DR cluster describe *itself* while replaying
production's WAL.

#### 4.2.1 Apply-time redo filter

In the redo loop, before dispatching a record to its resource-manager `rm_redo`:

```c
/* src/backend/access/transam/xlog.c:7730-7742 */
if (!DRRedoShouldFilter(xlogreader))
{
    RmgrTable[record->xl_rmid].rm_redo(xlogreader);

    if ((record->xl_info & XLR_CHECK_CONSISTENCY) != 0)
        checkXLogConsistency(xlogreader);
}
```

The consistency check (`wal_consistency_checking` on production sets
`XLR_CHECK_CONSISTENCY` on records) is skipped **together with** redo: a filtered
record's pages were deliberately left untouched, so comparing them against
production's page images would spuriously `FATAL`.

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
`ggseed_dr_topology` (`gpMgmt/bin/ggseed_dr_topology`) replaces them with DR-local rows
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

#### 4.2.3 Records that name a relation in the payload, not in a block tag

`DRRedoShouldFilter()` decides from a record's *block references*, so a record that names its
relation only in its payload is invisible to it — and is always applied, since that is also
the shape of a commit record. Two such record types touch the protected catalogs, and each
gets its own guard.

**Relfilenode rewrites → halt.** If production ever `VACUUM FULL` / `CLUSTER` / `REINDEX` /
`TRUNCATE`s a protected catalog, its relfilenode changes and a shared relmap update is
WAL-logged. There is **no preemptive block on the production side**. Instead the DR replica
**halts reactively**: `relmap_redo()` (`relmapper.c:1032-1048`) calls
`DRRejectForbiddenRemap()` (`dr_redo_filter.c:138-158`), which `ereport(FATAL)`s if an
incoming mapping would change a protected catalog's relfilenode — halting the postmaster
*before* any on-disk write, so the replay LSN does not advance past the offending record.
The fix is to re-create that DR node from a fresh base backup. (This is a documented
contract limitation, §9.)

**Heap truncation → skip.** A plain `VACUUM` that frees trailing pages of a protected
catalog emits `XLOG_SMGR_TRUNCATE`, which carries its `RelFileNode` in `xl_smgr_truncate`
and registers no buffers. Applied blindly it would truncate the DR's *own* file — the one
holding the frozen-seeded DR-local rows, whose page count legitimately differs from
production's, because the seed's re-inserted rows can land on a block production has since
freed. `smgr_redo()` therefore consults `DRRedoShouldFilterRelFileNode()`
(`dr_redo_filter.c:186-217`, call site `storage.c:698-715`) and **skips the truncation**,
logging it at `LOG`.

The asymmetry is deliberate. An ignored truncation has no lasting consequence: the node
keeps its own pages, the protected set still resolves to the same filenode, and replay
continues correctly. A remap is different — it repoints the catalog at a filenode the
protected set no longer covers, so the filter would silently stop protecting it from then
on. Unrecoverable divergence earns a `FATAL`; a harmless one does not, and halting every DR
node on a routine production autovacuum would be a far worse failure than the one being
prevented.

#### 4.2.4 Auxiliary tooling: `gg_walfilter` (src/bin)

`gg_walfilter` (`src/bin/gg_walfilter/`, a C frontend program installed into
`$GPHOME/bin`) is a **record-level WAL filter for archived segments**: it rewrites
selected records into same-length `XLOG_NOOP` records (the walbouncer technique) with
the CRC recomputed, so framing, LSNs and xid tracking stay intact while the record's
redo becomes a no-op. It is **not part of the default DR data path** — the in-backend
redo filter above is the engine invariant that protects the topology — but it ships
with the server for:

- **offline inspection** — `gg_walfilter inspect <segment>` reports, per record, what
  a rule set would filter and why (`--gp-dr-topology` selects the DR preset);
- **offline filtering** — `gg_walfilter filter` neutralizes matching records in local
  segment files (generic rules: `--exclude-relfilenode`, `--exclude-database`,
  `--exclude-tablespace`, `--protect-mapped-oid`), e.g. for partial replicas or
  redacted WAL hand-offs;
- **opt-in restore-path filtering** — `gg_walfilter restore` is a full
  `restore_command` entry point (fetch → filter → atomically install, with
  boundary-continuation state and deterministic archive lookback), for deployments
  that want filtering *outside* the engine.

Its filter decision is intentionally identical to the engine's, and tracks **both** of the
engine's rules (§4.2.3): rewrite iff the record has ≥1 block reference and every one matches
a rule (`DRRedoShouldFilter()`), **or** it is an `XLOG_SMGR_TRUNCATE` whose payload
`RelFileNode` matches (`DRRedoShouldFilterRelFileNode()`, `wf_truncate_target()` /
`wf_truncate_matches()` in `filter.c`). Keeping the two in step is the point of the tool:
`inspect` is only useful if it predicts what a DR node will actually do with the WAL. Where
the tool cannot resolve a truncate's target from the segment in hand — the record's leading
bytes land past the segment end — it fails closed under the DR preset rather than let an
unidentified truncation through, the same posture it takes for a boundary-spanning relmap
update.

History: an earlier revision of this branch used `gg_walfilter` (then a Python script) *as*
the default topology-protection mechanism via `restore_command` (ADR-0002); ADR-0003 restored
the in-backend filter as the default and kept the tool, ported to C, as tooling.

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

See **ADR-0005** for the decision and its alternatives; the mechanism is:

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
`HEAP_X*_DISTRIBUTED_SNAPSHOT_IGNORE` hint bits (ADR-0005 D5).

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
  restore_point`). The coordinator row `UNION ALL`s the per-segment rows via
  `gp_dist_random('gp_id')`, so each node reports its own local recovery state.
- **`gg_stat_dr_replica_summary`** — one-row rollup:
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
  (`KnownAssignedXids`).
- **Distributed (cluster-wide):** a `DistributedSnapshot` that maps each tuple's creating
  `gxid` to visible / invisible so all segments agree. On production this is minted live
  from `nextGxid`.

**On a DR replica only the first one is used.** The distributed dimension exists because a
live cluster has no global order across nodes; a distributed restore point *is* that order,
taken under `TwophaseCommitLock` EXCLUSIVE. What the replica must do instead is stop each
node's local snapshot from drifting once replay moves on — which is what §5.3 does. See
ADR-0005 for why the alternative (reconstructing a distributed snapshot from replayed
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
— and stays so; see [ADR-0005 D5.1](adr/0005-dr-served-restore-point-snapshot.md) for why
the in-doubt masking that was supposed to cover it never did, and what the residual
exposure actually is.

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
See ADR-0005 D3 for the one window this leaves open.

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
ggdr create-replica \
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
   aside, run `ggseed_dr_topology` (§4.2.2), then **restore** the saved recovery-start state
   so recovery resumes from the backup checkpoint, and **re-fetch** any backup-tail WAL
   segment from the archive over `pg_wal` (so the seed's WAL can never win over the archive).
5. **Arm the replica** on every node: empty `postgresql.auto.conf`; append `hot_standby = on`,
   `restore_command`, `recovery_target_timeline = 'current'`, and (if `--pause-at`) the
   pause GUC; create `standby.signal`. `hot_standby = on` is the whole switch — there is no
   separate DR GUC or marker file (§4.1).
6. **Start** each node in archive recovery (`pg_ctl`, coordinator in `gp_role=dispatch`,
   segments in `gp_role=execute`) and poll until all accept read connections.
7. **Post-build topology check** (`_check_seeded_topology()`): read the coordinator's
   `gp_segment_configuration` back and require it to describe *this* cluster — one row per
   topology entry, matching `dbid`/`content`/`port`/`hostname`. A silent seed failure would
   leave the replica describing production, so a mismatch **fails the build** rather than
   surfacing later as confusing symptoms. The check also reports the catalog's heap size and
   the block its live rows landed on: above block 0 means this node's topology depends on the
   §4.2.3 truncation guard, which is worth knowing at a glance. Only the coordinator is
   checked, because only the coordinator is seeded.

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
| `gg_dr_switch(restore_point text)` → `bool` | Re-point every node's pause target at `restore_point`, resume replay, and wait until all of them are paused there. Returns `true` at the consistent cut, `false` on timeout (the arming has still been applied — keep watching `gg_stat_dr_replica_summary`). |
| `gg_dr_promote()` → `bool` | Promote the whole cluster at its current consistent restore point. Refuses unless every node is paused at one point, equal to `at_restore_point` when given. Irreversible. |

Status has no function of its own: `gg_stat_dr_replica` and `gg_stat_dr_replica_summary`
(§4.6) already report it cluster-wide, and duplicating them as a function would only create
a second thing to keep in step.

```sql
SELECT gg_dr_switch('rp_hourly_43');   -- advance the cluster, wait for the cut
SELECT * FROM gg_stat_dr_replica_summary;   -- confirm consistent_restore_point
SELECT gg_dr_promote();            -- promote here
```

Both are superuser-only, must run on the coordinator in dispatch mode, and refuse outright
on a cluster that is not in recovery. Each applies its step to every primary segment with
`CdbDispatchCommand()` and to the coordinator through SPI, so the two legs take the same
path a client would. The steps themselves are the same ones `ggdr` performs —
`ALTER SYSTEM` on the pause GUC, `pg_reload_conf()`, `pg_wal_replay_resume()`, and for
promotion `pg_promote(false)` — and they are legal in recovery because none of them writes
WAL: `ALTER SYSTEM` writes `postgresql.auto.conf` and the rest touch shared memory only.

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
| `stat` | Per-node recovery statistics + a cluster summary (mode, consistent serve point, RPO). |
| `promote [--at <rp>] [--no-restart] [--yes] [--timeout <s>]` | Promote the DR cluster to online read-write at a single consistent restore point. Irreversible. |
| `create-replica --topology <tsv> --basebackup-dir <dir> --wal-archive <dir> [--restore-command <cmd>] [--pause-at <rp>] [--no-start] [--force]` | Build the whole DR replica from per-instance base backups (§6.1). |

### 7.3 Examples

```bash
# Status — the summary line tells you if it is safe to serve consistent reads:
$ ggdr stat
  content  role         in_recovery paused restore_point          replay_lsn        replay_age
  -1       coordinator  yes         yes    rp_hourly_42            0/9A00028         12s
  0        segment      yes         yes    rp_hourly_42            0/7C00190         12s
  1        segment      yes         yes    rp_hourly_42            0/7C00210         13s

  cluster: 3 node(s), all in recovery, all paused
  consistent serve point: rp_hourly_42   (safe to serve as-of-N reads)
  RPO: ~13s behind production's last replayed commit

# Advance the whole cluster to the next hourly restore point (consistent cut):
$ ggdr switch rp_hourly_43


# Planned switchover:
$ ggdr switch  rp_failover
$ ggdr promote --at rp_failover --yes

# Build a fresh DR replica (overwrite existing datadirs), armed but not started:
$ ggdr create-replica \
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
  the redo filter, the served restore-point snapshot, and enforcement/observability.
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
  such a cut — a narrow, loud, pre-existing asymmetry: see ADR-0005 D5.1.
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
  advance publishes the new point before its peers do — see ADR-0005 D3 for why that is
  preferred to the alternative.
- **Index-only and bitmap scans lose the all-visible fast path** on a replica (ADR-0005
  D4). Correct answers, more heap fetches.

**Topology filter**

- **Production `VACUUM FULL`/`CLUSTER`/`REINDEX`/`TRUNCATE` on a protected topology catalog
  halts the DR.** It is caught reactively as a `FATAL` in `relmap_redo`
  (`DRRejectForbiddenRemap`), *not* blocked on production. Recovery is to re-base-backup +
  re-seed that DR node. Treat these catalogs as immutable while a DR replica is attached.
- **Plain `VACUUM` truncation of a protected catalog is skipped, not fatal** (§4.2.3). The
  DR's copy of these catalogs legitimately has a different page count from production's,
  because the seed's re-inserted rows may sit on a block production later frees; applying
  production's truncation would discard them. This was a real defect — the guard in
  `smgr_redo` was added after the payload-vs-block-tag gap was found; see scenario V-20.
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
| DR mode gate | `IsDRReplicaMode()` = `EnableHotStandby && RecoveryInProgress()`, `xlog.c`; decl `xlog.h` |
| Redo filter | `dr_redo_filter.c` (`DRRedoShouldFilter` :229-259, protected OIDs :43-52, resolve :71-104, match :164-183, remap-reject :138-158, `DRRedoShouldFilterRelFileNode` :207-217); dispatch + consistency-check skip `xlog.c:7730-7742`; header `dr_redo_filter.h:32-56`; remap redo `relmapper.c:1032-1048`; smgr truncate guard `storage.c:698-715` |
| WAL-filter tool (auxiliary) | `src/bin/gg_walfilter/` (`gg_walfilter.c` CLI, `segment.c` walker/decoder, `filter.c` rules + NOOP rewrite + lookback, `state.c` boundary state, `fetch.c` fetch/install); tests `src/test/dr/test_gg_walfilter.py` (black-box) |
| Frozen seed | `gpMgmt/bin/ggseed_dr_topology` |
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
| `hot_standby` | `PGC_POSTMASTER` | `false` | On a node in recovery, makes it a DR replica: read-only enforcement, topology redo filter, standby distributed read. Disengages with recovery. |
| `gp_pause_on_restore_point_replay` | `PGC_SUSET` | `''` | Pause replay when the named restore point is replayed; re-point + resume to advance. `gg_dr_switch()` maintains it. |
| `max_standby_archive_delay` | `PGC_SIGHUP` | `0` on a DR node (`ggdr create-replica` writes it) | A frozen `xmin` makes recovery conflicts routine; the stock 30 s would be added to every advance that hits one. `-1` is a trap, not the other extreme — it blocks the startup process inside redo. |

### 10.3 Catalog / function objects

- `pg_last_paused_restore_point()` → `text` (OID 7016) — served restore point, or `NULL`.
- `gg_stat_dr_replica` (view, `pg_catalog`) — per-node recovery state.
- `gg_stat_dr_replica_summary` (view, `pg_catalog`) — cluster rollup incl.
  `consistent_restore_point`, `rpo_seconds`.
- `gg_dr_switch(text)` → `bool` (OID 7017) — advance the cluster to a restore point.
- `gg_dr_promote()` → `bool` (OID 7018) — promote at the current cut.

### 10.4 Test fixture

`src/test/dr/` — a two-container fixture (production + DR sharing an `/archive` volume) that
builds the DR via `ggdr create-replica` and exercises M1/M2/M2′/M3/M5 plus
`ggdr` `switch`/`pause`/`stats` plus the SQL control functions end-to-end
(`docker-compose -f src/test/dr/docker-compose.yml up --build`).

`src/test/dr/docker-compose.dense.yml` — a second, opt-in fixture for the §4.2.3 truncation
guard: it densifies production's `gp_segment_configuration` before the base backup so the
DR's seed lands its rows on a trailing block, then vacuums production to emit the truncation.
Run against a guarded and an unguarded build it reports `TOPOLOGY SURVIVED` vs.
`TOPOLOGY DESTROYED`. See `src/test/dr/README.md`.

Scenario catalogue: [`greengage-dr-test-scenarios.md`](greengage-dr-test-scenarios.md) —
what to test, what is already automated, and the expected behaviour of each corner case.

### 10.5 Related design notes (repo root)

- `greengage7-dr-readonly-replica-poc-plan.md` — master plan (transport, topology,
  milestones, risk register).
- `greengage7-dr-standby-distributed-read-mode.md` — the standby distributed-read design.
- `greengage7-dr-SR2-reader-gang-snapshot-sync.md` — reader-gang snapshot analysis (note:
  its snapshot-leader scheme was superseded by stop-and-go; see §5.5).
