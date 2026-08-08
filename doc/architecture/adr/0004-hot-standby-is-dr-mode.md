# ADR-0004: Hot Standby *Is* DR Mode; Restore-Point-Only Recovery; SQL Recovery Control

> **Historical — proof-of-concept record.** Superseded in full by
> [ADR-0006](0006-dr-read-replica.md), which states the design as it stands today in one
> self-contained document. This file is kept for the rationale behind a particular
> decision, including mechanisms that were adopted and later reversed; do not read it as a
> description of the current implementation.

- **Status:** Accepted — implemented on branch `7.x-dr`
- **Date:** 2026-08-06
- **Supersedes:** ADR-0001 D2 (the `gp_dr_replica` + `dr_replica.signal` two-key gate).
  ADR-0001's other decisions and ADR-0003 stand; only the *gate* they key off changes.

---

## Context

Four review findings prompted this change.

1. **The "as-of-N snapshot" framing oversold the guarantee.** A DR replica is worth
   reasoning about as *"recovered up to restore point N"* — a point-in-time image — not as
   a live cluster serving a masked snapshot. The second framing is hard to explain and
   invites the assumption that arbitrary cuts are consistent, which they are not.

2. **Free-running (`follow`) mode had nothing to offer.** Only a distributed restore point
   is taken under `TwophaseCommitLock`, so it is the only cut where the segments agree.
   A mode that serves reads at any other position ships a caveat, not a feature.

3. **Two near-identical modes.** DR mode existed *beside* hot standby, gated on
   `gp_dr_replica` + `dr_replica.signal`, while hot standby on its own has little practical
   use in Greengage: the only standbys in a cluster are mirrors and the standby
   coordinator; querying them loads the very hosts they protect, and an FTS failover onto a
   mirror strands the readers.

4. **Recovery control was only reachable from a shell.** `ggdr` needs a connection to
   every node; a client that can reach only the coordinator could observe DR status through
   the views but could not act on it.

---

## Decisions

### D1 — `IsDRReplicaMode()` is `EnableHotStandby && RecoveryInProgress()`

`hot_standby = on` on a node in recovery *is* DR mode: topology redo filter, read-only
enforcement, and the standby distributed-read path. `gp_dr_replica` and
`dr_replica.signal` are removed.

**Why this is better than a second mode:**

- **It follows recovery, not configuration.** The predicate goes false the instant recovery
  ends, so promotion disengages DR behaviour by itself. This deleted the entire second
  phase of promotion — stop every node, clear markers, `ALTER SYSTEM SET gp_dr_replica =
  off`, restart — which existed *only* because that GUC was `PGC_POSTMASTER`.
- **It needs no plumbing to be visible.** `EnableHotStandby` is a postmaster GUC, so the
  startup process (redo filter) and every backend (read-only enforcement) see the same
  value. The old marker needed a lazily-`stat()`ed per-backend cache precisely because a
  backend never runs recovery — a subtlety that had already caused one silent-no-op bug.

**Accepted cost — the mirror footgun.** `hot_standby` defaults to `off`, so mirrors and the
standby coordinator are unaffected in normal operation. But turning it on for one now
freezes its `gp_segment_configuration`, and a later FTS failover (or `gpactivatestandby`)
would promote a node describing a stale cluster. This is documented rather than defended
against; the alternative — keeping a marker file purely to distinguish the two — reinstates
the second key this ADR removes. Scenario H-4 exists to keep the warning honest.

### D2 — Recovery is supported only *up to a restore point*

`ggdr follow` is removed. A replica serves reads only while paused at a distributed
restore point. Reaching any other position is still possible (`pause`, or an advance in
flight), but is for inspection, not for serving, and `stats` reports no consistent serve
point there.

### D3 — The as-of-N distributed snapshot is **kept** — *superseded by ADR-0005*

> **Superseded.** ADR-0005 deletes `CreateDRStandbyDistributedSnapshot()` and serves the
> local snapshot frozen at the restore point instead. The reasoning below is left as
> written because it is where the straddle case is worked out — but its central claim is
> wrong: the in-doubt masking never made a straddling transaction invisible **on the
> coordinator**, because `XidInMVCCSnapshot()` gates the whole distributed block on
> `!IS_QUERY_DISPATCHER()`. See ADR-0005 D5.1.


The review proposed dropping `CreateDRStandbyDistributedSnapshot()` and its
`shmCommittedGxidArray` read, on the grounds that a restore point is already consistent.
Checking that against the code showed it is not sufficient:
`doNotifyingCommitPrepared()` acquires `TwophaseCommitLock` **after** the coordinator's
`XLOG_XACT_DISTRIBUTED_COMMIT` is written (`cdbtm.c`), so at a restore point a transaction
can be committed on the coordinator and prepared-only on every segment.

For segment-only data that is harmless — prepared rows are locally invisible anyway — but a
transaction touching both sides is not: `BEGIN; CREATE TABLE t; INSERT t; COMMIT;` would
leave `t` in the coordinator catalog and absent on the segments. The in-doubt masking is
what makes such a transaction invisible on both sides, so it stays.

The alternative considered and rejected for now was widening the barrier — taking
`TwophaseCommitLock` before the distributed commit record — which would make a restore point
straddle-free by construction and let the masking go. It was rejected because it lengthens a
lock hold on production's commit path to simplify the replica.

#### D3.1 — Worked example: why shared memory is still needed

Take the transaction that makes the problem concrete, because it writes on **both** sides:

```sql
BEGIN;
  CREATE TABLE t (id int) DISTRIBUTED BY (id);   -- catalog: coordinator AND segments
  INSERT INTO t SELECT generate_series(1, 100);  -- data: segments only
COMMIT;
```

**On production**, the commit is two-staged, and the restore-point barrier does *not* cover
the whole of it. `gp_create_restore_point('N')` holds `TwophaseCommitLock` EXCLUSIVE
(`xlogfuncs_gp.c`), while `doNotifyingCommitPrepared()` takes it SHARED — but only *after*
the coordinator has already written its distributed commit record (`cdbtm.c`):

```
  QD (coordinator)                     QE0            QE1        WAL each node writes
  ───────────────────────────────────────────────────────────────────────────────────
   │                                    │              │
   ├── PREPARE ────────────────────────▶│              │         QE0: XACT_PREPARE
   ├── PREPARE ───────────────────────────────────────▶│         QE1: XACT_PREPARE
   │                                    │              │
   ├─ insertedDistributedCommitted()    │              │         QD : XACT_DISTRIBUTED_COMMIT
   │    · QD's LOCAL xact commits ....................................  (pg_class row for t)
   │    · gxid → shmCommittedGxidArray  │              │
   │                                    │              │
  ═══ restore point N can land HERE ═══════════════════════════   QD + every QE:
       gp_create_restore_point('N')                                    XLOG_RESTORE_POINT
       TwophaseCommitLock EXCLUSIVE                              ← the only straddle window
   │                                    │              │
   ├─ LWLockAcquire(TwophaseCommitLock, SHARED)        │         ← barrier starts only now
   ├── COMMIT PREPARED ────────────────▶│              │         QE0: XACT_COMMIT_PREPARED
   ├── COMMIT PREPARED ───────────────────────────────▶│         QE1: XACT_COMMIT_PREPARED
   ├─ doInsertForgetCommitted() ..................................  QD : XACT_DISTRIBUTED_FORGET
   └─ LWLockRelease(TwophaseCommitLock)
```

Because the SHARED acquire brackets only the broadcast and the forget, a restore point is
all-or-nothing **for the segments** — every QE has committed *T*, or none has. It says
nothing about the coordinator, which committed earlier and outside the barrier.

**On the DR**, recovered up to exactly that restore point:

| node | replayed | state of *T* |
|---|---|---|
| coordinator | `XACT_DISTRIBUTED_COMMIT` | local xact **committed** → `t` is in `pg_class` |
| segment c0 | `XACT_PREPARE` only | **prepared**, not committed → `t` absent, rows invisible |
| segment c1 | `XACT_PREPARE` only | **prepared**, not committed → `t` absent, rows invisible |

This asymmetry is not a bug in redo; it is faithful replay of what production actually
wrote. `xact_redo_distributed_commit()` (`xact.c`) does two things, and the second is the
one that matters here:

```c
if (TransactionIdIsValid(xid))
    xact_redo_commit(parsed, xid, lsn, origin_id);   /* the QD's own catalog change */

redoDistributedCommitRecord(parsed->distribXid);      /* gxid → shmCommittedGxidArray */
```

and `xact_redo_distributed_forget()` removes the gxid again. So on a replica the array holds
precisely *"distributed transactions whose coordinator commit has been replayed but whose
forget has not"* — the in-doubt set, maintained for free by redo. That is the shared-memory
state the review proposed to stop keeping.

Note the `TransactionIdIsValid(xid)` guard: a QD-read-only distributed transaction (a plain
`INSERT`, whose data lives only on segments) has no local xid on the coordinator, so nothing
becomes visible there and the straddle is invisible. It is exactly the mixed transaction —
one that writes coordinator catalog *and* segment data — that exposes it. That is why the
existing straddle test in `src/test/dr` passes either way: `dr_straddle` is a plain
distributed `INSERT`.

**Without the masking**, a query at *N* sees the coordinator's half of a transaction and none
of the segments':

```
  SELECT * FROM t;
    coordinator: t exists in pg_class, plan built, dispatch to segments
    segments   : ERROR: relation "t" does not exist        ← torn read
```

**With the masking**, `CreateDRStandbyDistributedSnapshot()` reads that same array and places
every gxid in it into the snapshot's **in-progress (invisible)** set. *T* is therefore
invisible on the coordinator too, and the cut is one where *T* simply has not happened yet —
consistent, and matching what the segments show:

```
  SELECT * FROM t;
    ERROR: relation "t" does not exist    ← consistent: T is in-doubt everywhere
```

Advance one restore point past *T*'s forget and it appears everywhere at once.

So the shared-memory array is not an optimisation or a leftover: it is the only record on the
replica of *which distributed transactions the coordinator has committed ahead of its
segments*, and dropping it would make restore-point reads torn for any transaction that
writes on both sides — DDL above all.

### D4 — Cluster-wide recovery control in SQL

`gg_dr_switch(restore_point, timeout)` and `gg_dr_promote(at_restore_point, timeout)`
(`xlogfuncs_gp.c`, OIDs 7017/7018) apply each step to every primary segment via
`CdbDispatchCommand()` and to the coordinator directly. Status keeps using the existing
`gg_stat_dr_replica` views; a third status surface would only be one more thing to keep in
step.

Two implementation notes worth preserving, because both were found the hard way:

- The local `ALTER SYSTEM` leg **cannot** go through SPI. These functions run inside a
  `SELECT`, and `standard_ProcessUtility` calls `PreventInTransactionBlock` for
  `AlterSystemStmt`; the local leg therefore calls `AlterSystemSetConfigFile()` directly.
- `gg_dr_promote()` waits for **this node** to leave recovery, not the segments. Segments
  cannot be polled from here: the QE protocol check refuses a standby QD talking to a
  promoted QE (and the reverse once this node is promoted), and they cannot be promoted
  synchronously either, because `recoveryPausesHere()` does not watch for the promote
  trigger — `pg_promote(true)` on a paused node would wait for a resume only another session
  could send. `ggdr promote`, which holds a connection per node, remains the path
  when the stronger guarantee is needed.

### D5 — Naming and signatures (review follow-up)

Applied after the first round of use:

- **`gg_` prefix for everything this feature adds.** `gg_dr_switch`, `gg_dr_promote`,
  `gg_stat_dr_replica`, `gg_stat_dr_replica_summary`. `gp_` is the historical Greenplum
  prefix; new Greengage objects take `gg_`. Pre-existing objects the feature *uses* keep
  their names (`gp_create_restore_point`, `gp_pause_on_restore_point_replay`,
  `gp_segment_configuration`), as does `pg_last_paused_restore_point()`, which is
  deliberately named alongside the `pg_*` recovery accessors it sits with
  (`pg_is_wal_replay_paused`, `pg_last_wal_replay_lsn`) rather than after this feature.

- **No timeout arguments.** How long a switch takes is a property of how much WAL lies
  between here and the target, which the caller cannot usefully guess. Worse, a timeout
  that fires leaves the pause target armed anyway, so it reported a failure that was not
  one. Both functions now block until done; the wait is interruptible, so
  `pg_cancel_backend()` — or Ctrl-C — is the way out, which is the mechanism operators
  already have for every other long query.

- **`gg_dr_promote()` takes no arguments.** A replica is promoted from where it *is*, and
  where it is, is a restore point — that is the only state it serves from. The old
  `at_restore_point` argument only let the caller assert something the function already
  validates. The validation stays (every node paused, all at the same point) and the
  function reports the point it found; it no longer asks to be told.

- **Recovery-position columns clear on promotion.** `replay_lsn` and `replay_time` do not
  error once recovery ends — they keep reporting the last replayed position forever. On a
  promoted cluster that is stale trivia presented as status, and it made `rpo_seconds` grow
  without bound for a cluster that has no RPO at all. Both are now NULL out of recovery, and
  `rpo_seconds` with them.

- **Utilities take the `gg` prefix too:** `gg_recovery` is renamed `ggdr` (its `stats`
  subcommand to `stat`, matching the view it prints), and `gpseed_dr_topology` becomes
  `ggseed_dr_topology`. `pg_last_paused_restore_point()` deliberately stays `pg_*`, per
  the reasoning above.


---

## Consequences

- One mode instead of two; promotion no longer restarts the cluster.
- `CATALOG_VERSION_NO` bumped (new `pg_proc` entries) — both clusters must be rebuilt.
- A latent bug surfaced and was fixed on the way: `gg_stat_dr_replica` called
  `pg_is_wal_replay_paused()` under a `CASE` guard, but the `UNION ALL` of an entry-DB row
  with a `gp_dist_random()` row defeats the short-circuit, so the whole view **errored on a
  promoted cluster** — exactly when an operator wants to read it. The guard is now a scalar
  subquery. This was never noticed because no test had read the view after promotion.
- Verified end to end on the container fixture: 36/36 (M1+M2 7, M2′ 5, M3 6, M5 6,
  `ggdr` 4, SQL recovery control 8), plus the V-20 dense fixture 5/5 and the
  `gg_walfilter` suite 40/40.
