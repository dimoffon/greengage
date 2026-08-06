# ADR-0004: Hot Standby *Is* DR Mode; Restore-Point-Only Recovery; SQL Recovery Control

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

4. **Recovery control was only reachable from a shell.** `gg_recovery` needs a connection to
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

`gg_recovery follow` is removed. A replica serves reads only while paused at a distributed
restore point. Reaching any other position is still possible (`pause`, or an advance in
flight), but is for inspection, not for serving, and `stats` reports no consistent serve
point there.

### D3 — The as-of-N distributed snapshot is **kept**

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

### D4 — Cluster-wide recovery control in SQL

`gp_dr_switch(restore_point, timeout)` and `gp_dr_promote(at_restore_point, timeout)`
(`xlogfuncs_gp.c`, OIDs 7017/7018) apply each step to every primary segment via
`CdbDispatchCommand()` and to the coordinator directly. Status keeps using the existing
`gp_stat_dr_replica` views; a third status surface would only be one more thing to keep in
step.

Two implementation notes worth preserving, because both were found the hard way:

- The local `ALTER SYSTEM` leg **cannot** go through SPI. These functions run inside a
  `SELECT`, and `standard_ProcessUtility` calls `PreventInTransactionBlock` for
  `AlterSystemStmt`; the local leg therefore calls `AlterSystemSetConfigFile()` directly.
- `gp_dr_promote()` waits for **this node** to leave recovery, not the segments. Segments
  cannot be polled from here: the QE protocol check refuses a standby QD talking to a
  promoted QE (and the reverse once this node is promoted), and they cannot be promoted
  synchronously either, because `recoveryPausesHere()` does not watch for the promote
  trigger — `pg_promote(true)` on a paused node would wait for a resume only another session
  could send. `gg_recovery promote`, which holds a connection per node, remains the path
  when the stronger guarantee is needed.

---

## Consequences

- One mode instead of two; promotion no longer restarts the cluster.
- `CATALOG_VERSION_NO` bumped (new `pg_proc` entries) — both clusters must be rebuilt.
- A latent bug surfaced and was fixed on the way: `gp_stat_dr_replica` called
  `pg_is_wal_replay_paused()` under a `CASE` guard, but the `UNION ALL` of an entry-DB row
  with a `gp_dist_random()` row defeats the short-circuit, so the whole view **errored on a
  promoted cluster** — exactly when an operator wants to read it. The guard is now a scalar
  subquery. This was never noticed because no test had read the view after promotion.
- Verified end to end on the container fixture: 36/36 (M1+M2 7, M2′ 5, M3 6, M5 6,
  `gg_recovery` 4, SQL recovery control 8), plus the V-20 dense fixture 5/5 and the
  `gg_walfilter` suite 40/40.
