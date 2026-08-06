# ADR-0001: Disaster-Recovery Read Replica for Greengage 7

- **Status:** Accepted — implemented and validated end-to-end on branch `7.x-dr` (proof of concept).
  **Amendment history:** ADR-0002 (2026-08) briefly superseded the *apply-time redo
  filter* mechanism of D3 with a restore-time WAL filter utility (`gg_walfilter` in
  `restore_command`); **ADR-0003 (2026-08) reversed that** — the in-backend redo filter
  (`dr_redo_filter.c` + the `relmap_redo` guard) is the default mechanism again, exactly
  as D3 describes (plus a consistency-check fix), and `gg_walfilter` remains as auxiliary
  tooling, ported to C under `src/bin/gg_walfilter`. The frozen-tuple seed half of D3 and
  all other decisions stand. `file:line` references below describe the code as of
  2026-07-28.
- **Date:** 2026-07-28 (decisions ratified 2026-06; implementation June–July 2026)
- **Deciders:** Greengage DR project
- **Companion document:** `doc/architecture/greengage-dr-read-replica.md` — the full
  architecture description of *how* the feature works. This ADR records *what was decided,
  what was rejected, and why*. All `file:line` references point at the implemented code on
  `7.x-dr`.
- **Superseded in part:** D2's `gp_dr_replica` + `dr_replica.signal` gate is replaced by
  ADR-0004 (hot standby *is* DR mode). D1/D3 and the rest stand.

---

## Context

Greengage 7 (PostgreSQL 12 base, MPP fork) has built-in intra-cluster HA — per-segment
mirrors, FTS-driven failover recorded in `gp_segment_configuration` — but nothing that
survives losing the whole primary site, and nothing that provides a continuously updated,
queryable, promotable copy of a cluster. VMware/Broadcom Greenplum sells this as **GPDR
"Read Replica Mode"**; there is no open-source equivalent.

The goal: a **second, independent Greengage cluster** (its own coordinator and primary
segments, its own dbids, possibly different hosts) that

1. is kept continuously up to date from production,
2. serves **read-only distributed queries** with **transactionally consistent**,
   cluster-wide results while remaining in recovery, and
3. can be **promoted in place** to a normal online read-write cluster on switchover.

Two things make this non-trivial in an MPP fork — plain PostgreSQL hot standby solves
neither:

- **Topology.** A physical replica byte-replays production's WAL, including writes to the
  shared topology catalog `gp_segment_configuration`. Left alone, the DR cluster can only
  ever describe *production's* hosts/ports/dbids — it can never describe itself, so its own
  coordinator cannot dispatch to its own segments.
- **Distributed consistency.** Each node replays its own WAL at its own pace, and a
  distributed commit is two-staged across nodes. An arbitrary per-node cut (e.g.
  `min(replay_lsn)`) can catch a transaction committed on the coordinator but only
  *prepared* (invisible) on segments — a torn read. Moreover, on a Greengage coordinator
  even a pure `SELECT` normally allocates a distributed xid (gxid) and opens a *writer*
  gang, both illegal in recovery.

Constraints that shaped the decisions:

- PostgreSQL 12 base: no `max_slot_wal_keep_size`, so an unbounded replication slot is a
  production outage risk; recovery internals (`KnownAssignedXids`, restartpoints) are
  fixed.
- Production must not need to know a replica is attached (transport is one-way).
- Minimize new core surface: prefer shipped primitives and existing dispatch machinery
  over new distributed protocols.

## Decision

Build the DR read replica as **an independent cluster in permanent per-content archive
recovery**, made self-describing by an **apply-time redo filter plus a frozen-tuple
topology seed**, serving reads through a **new no-gxid standby-reader dispatch context**
over the existing hot-standby dispatch, with consistency provided by **stop-and-go
restore-point-gated as-of-N snapshots**, and driven by a single control utility,
**`gg_recovery`**.

The individual decisions, with alternatives considered:

### D1 — Replication unit: per-content physical WAL into an independent cluster

Each DR node continuously replays the WAL of its production counterpart *by content id*:
DR coordinator ← production coordinator, DR segment *cN* ← production primary *cN*. The
**segment count must match**; host layout, hostnames, ports, and dbids are free. Each
node keeps its own independent WAL/LSN space.

**Alternatives rejected:**

- *Logical replication / ETL into a second cluster* — no PG12-era logical decoding for an
  MPP catalog + distributed transactions; loses physical fidelity (indexes, AO segment
  files); enormous surface.
- *Shared-storage / block-level replication* — gives a cold standby only; cannot serve
  reads; ties both sites to one storage product.
- *Cluster-level streaming with a distributed coordinator protocol* — requires new
  cross-cluster machinery Greengage does not have; per-content physical WAL reuses stock
  recovery on every node.

### D2 — Transport: WAL archiving / continuous restore (PITR model), not streaming

Production pushes completed WAL segments per content to a shared archive
(`archive_command`, using the GP `%c` content-id escape); each DR node pulls with
`restore_command`. No replication slots, no libpq connection between the clusters.

**Why:** a replication slot on PG12 pins production WAL with no cap
(`max_slot_wal_keep_size` arrived in PG13) — a slow or offline DR could fill production's
`pg_wal`. An archive decouples the sites completely: production cannot be harmed by DR
lag; one faithful archive serves production PITR, N DR replicas, and cascades; it is
WAN/latency tolerant and restartable.

**Cost accepted:** freshness. The RPO floor is `archive_timeout` + transfer + restore
cadence + slowest-segment skew — this is a reporting/DR replica, not a hot synchronous
standby. (A pgBackRest/WAL-G relay was evaluated for the repository role; the PoC fixture
uses a plain shared directory with `cp`, and the repository choice is deliberately
orthogonal to the engine work.)

### D3 — Topology independence: apply-time redo filter + frozen-tuple seed

> **Superseded in part by ADR-0002.** Mechanism 1 below (the in-backend apply-time
> filter) was replaced by `gg_walfilter`, which rewrites the same protected records into
> same-length `XLOG_NOOP`s on the `restore_command` fetch path — the "WAL rewriter"
> alternative rejected below, revisited: same-length rewriting after the archive fetch
> deletes no bytes and leaves the archive canonical. Mechanism 2 (the frozen-tuple
> seed) is unchanged. The relmap-guard halt (`DRRejectForbiddenRemap`) also moved into
> `gg_walfilter`.

Two cooperating mechanisms let the DR cluster describe itself while byte-replaying
production's WAL:

1. **Apply-time redo filter.** In the redo loop, a record whose block references *all*
   fall inside a protected set of shared topology catalogs is **not applied** (its
   `rm_redo` is skipped); everything else replays normally
   (`DRRedoShouldFilter`, `src/backend/access/transam/dr_redo_filter.c:195-225`, wired at
   `xlog.c:7727-7728`). The protected set is `gp_segment_configuration` (OID 5036) + its
   indexes (7139/7140) and TOAST (6092/6093), `gp_configuration_history` (5106), `gp_id`
   (5101), `gp_version_at_initdb` (5103), resolved to relfilenodes via the shared
   relmapper and matched only in the global tablespace
   (`dr_redo_filter.c:43-52, 71-104, 164-183`). Replay LSN bookkeeping is untouched — the
   node stays in lock-step with production's WAL position; `pg_wal` stays byte-identical
   to production's.
2. **Frozen-tuple seed.** At create time, `gpseed_dr_topology` (single-user mode) replaces
   `gp_segment_configuration`'s rows with DR-local rows and `VACUUM FREEZE`s them, so they
   are unconditionally visible with no CLOG dependency on production's XID timeline. The
   filter then protects those pages from production's replayed writes forever after.

**Alternatives rejected:**

- *WAL proxy / rewriter* (rewrite or drop topology records in transit): you cannot delete
  bytes from physical WAL — LSN is a byte offset, `xl_prev` chains records, and page
  `pd_lsn` interlocks with buffer state. In-place rewriting corrupts the canonical archive
  or forces maintaining a second, divergent WAL stream. Filtering at *apply* time keeps
  one faithful archive for PITR, `pg_waldump`, and N replicas.
- *Same-topology replica* (DR mirrors production's catalog): the DR coordinator would
  dispatch to production's addresses; renders the replica non-self-describing and
  unroutable on a different site.
- *Production-side DDL bans on topology catalogs*: production cannot know a replica is
  attached (archive transport is one-way), and routine `VACUUM` must stay allowed. Instead
  the DR side **halts reactively**: `relmap_redo` FATALs before any on-disk write if a
  protected catalog's relfilenode would change (`DRRejectForbiddenRemap`,
  `dr_redo_filter.c:138-158`, called from `relmapper.c:1032-1048`) — a loud, recoverable
  "re-seed this node" stop rather than silent corruption. Only `VACUUM FULL` / `CLUSTER` /
  `REINDEX` / `TRUNCATE` of a topology catalog trigger it.

**Cost accepted:** a bounded, self-healing seed footprint — the seed's `VACUUM FREEZE`
updates a few `pg_class` rows in place (`pg_class` is deliberately *not* protected), so
production catalog changes co-located on those pages within the sub-second
`(backup-LSN, seed-LSN]` window are LSN-shadowed until the next full-page image heals
them. Documented; a production-grade create utility should seed against a scratch copy.

### D4 — DR-mode gate: GUC **and** signal file, with `hot_standby` auto-enabled

`IsDRReplicaMode()` (`xlog.c:5597-5623`) is true iff **both** the `PGC_POSTMASTER` GUC
`gp_dr_replica` is on **and** the `dr_replica.signal` marker file exists. Requiring both
means promotion (which removes the marker) disengages DR behavior even while the GUC
value lingers in a config file until restart. Because backends fork from the postmaster
and never run recovery, the predicate lazily `stat()`s and caches the marker per process —
a lesson from a real bug where the flag was set only in the startup process and the M2
guards were silent no-ops in backends. `hot_standby` is force-enabled by the postmaster in
DR mode (`postmaster.c:1091-1101`) so the operator cannot mis-configure a non-queryable
replica.

### D5 — Read-only enforcement: executor gate + DTM backstop; FTS/autovacuum quiescent structurally

- **Executor gate** (`ExecCheckXactReadOnly`, `execMain.c:1651-1660`): in DR mode anything
  that is not a pure `SELECT` — including `SELECT … FOR UPDATE/SHARE` (row locks), which
  stock hot standby *exempts* — fails with an explicit
  `cannot execute … on a read-only disaster-recovery replica` error.
- **DTM backstop** (`currentDtxActivate`, `cdbtm.c:281-284`): allocating a gxid mutates
  cluster-wide state (`nextGxid`) without WAL, so recovery alone would not catch it;
  in DR mode it errors. By construction the standby-reader path (D6) never reaches it —
  the backstop is a guardrail.
- **No DR-specific disabling of FTS/autovacuum/DTX-recovery is needed:** a DR node stays
  permanently in `PM_HOT_STANDBY` and those launchers start only at `PM_RUN`
  (`postmaster.c:6545-6578`, `postmaster.c:2132-2134`). Chosen deliberately over adding
  `IsDRReplicaMode()` checks in each daemon — structural quiescence cannot regress.

### D6 — Standby distributed read: a new no-gxid QD context over the existing hot-standby dispatch

A new distributed-transaction context, **`DTX_CONTEXT_QD_STANDBY_READER`**
(`cdbtm.h:142-149`), selected whenever the dispatcher runs in DR mode
(`cdbtm.c:1729-1743`). In this context the coordinator never activates a distributed
transaction — no gxid allocation, no WAL — and every DTX-activation side path
(InitPlan/subplan dispatch, internal subtransactions) is short-circuited **keyed on
`IsDRReplicaMode()`, not on the context value** (`cdbtm.c:417,452`, `execMain.c:666`),
because those paths run before the context is resolved.

Gang creation and routing **reuse the pre-existing upstream hot-standby dispatch**
(commit `35e95b9c`: `IS_HOT_STANDBY_QD()`/`IS_STANDBY_QE()`, mirror-selection in
`cdbutil.c:1027-1051`, the `is_hs_dispatch` wire flag). One targeted exemption was added:
plans that put read-only work on a *writer* gang (e.g. coordinator-row `UNION ALL`
segment-rows views) hit a QE check requiring a valid gxid, which a standby never has —
exempted for `IS_STANDBY_QE()` (`xact.c:2510-2517`), safe because writes are already
refused at the coordinator.

**Alternative rejected:** the bespoke "snapshot-leader / gang-member-0" scheme from the
early SR-2 design note (`greengage7-dr-SR2-reader-gang-snapshot-sync.md`) — machinery to
keep a writer-less reader gang's local snapshots coherent **under concurrent replay**.
Not built: under stop-and-go (D7) replay is paused during serve windows, so per-node local
snapshots are static and the existing dispatch suffices. This was the single largest
complexity avoidance in the project.

### D7 — Consistency model: stop-and-go restore-point-gated reads

The cluster pauses replay at a **distributed restore point N**, serves as-of-N reads
while paused, then advances to N+1 (re-point the shipped
`gp_pause_on_restore_point_replay` GUC + SIGHUP + `pg_wal_replay_resume()`), in a loop.
The barrier: `gp_create_restore_point()` on production holds `TwophaseCommitLock`
exclusively across dispatching the restore-point record to every node
(`xlogfuncs_gp.c:89-100`), so no commit-prepared broadcast can straddle the cut *at the
segments*. A cross-node consistent serve point exists exactly when **every node is paused
at the same restore-point name**.

**Alternatives rejected:**

- *`min(replay_lsn)` cuts* — unsound. LSNs are not comparable across nodes (independent
  WAL spaces), and the coordinator's `XLOG_XACT_DISTRIBUTED_COMMIT` is written *outside*
  the two-phase barrier, so a transaction can be committed-on-coordinator but
  prepared-only (invisible) on segments at any non-barrier cut — the **straddle**.
- *Free-running replay + published gxid horizon* — the original master-plan
  recommendation, adopted because stop-and-go was believed to need core re-targeting
  work. **Reversed** when review found Greengage already ships the pause primitive
  (`gp_pause_on_restore_point_replay`, `guc_gp.c:4727-4736`;
  `pauseRecoveryOnRestorePoint`, `xlog.c:5976`). Stop-and-go on a shipped primitive beats
  a new continuous-horizon protocol: a static image while serving (zero recovery-conflict
  cancellations inside a serve window, coherent multi-slice reads without D6's rejected
  snapshot-leader machinery) at the cost of coarser freshness.
- *Retention holder / conflict-free advances* — not built for the PoC; queries that span
  an N→N+1 advance accept standard hot-standby cancellation
  (`max_standby_archive_delay`).

### D8 — As-of-N snapshot: reconstruct from replayed state, with the inverted in-doubt set

In recovery the coordinator cannot mint a live distributed snapshot (the stock
`CreateDistributedSnapshot` hard-errors mid-2PC-recovery, freezes `xmax`, and walks
process arrays that are empty in recovery). A DR-specific builder,
**`CreateDRStandbyDistributedSnapshot()`** (`procarray.c:2166`, wired at
`procarray.c:2570-2577`), reconstructs the snapshot from replayed state:

- **`xmax = ShmemVariableCache->nextGxid`** — redo-advanced by `XLOG_NEXTGXID`, a safe
  upper bound while paused (no committed-replayed tuple can carry a higher gxid, and a
  paused replica applies no further WAL). A separately redo-tracked horizon field was
  implemented first and **reverted** — `nextGxid` is simpler and equally correct under
  stop-and-go.
- **In-progress (invisible) set = `shmCommittedGxidArray`** — the
  distributed-committed-but-not-yet-forgotten transactions. This is a deliberate
  **semantic inversion** versus a live primary (where that array means *visible*): a
  transaction in that array at the cut is exactly a potential straddler — committed on the
  coordinator, possibly prepared-only on segments — so it is treated as in-doubt ⇒
  invisible everywhere, making the cross-node read atomic. Validated empirically with a
  fault-injected mid-broadcast 2PC straddle on a 2-segment fixture (the straddler is
  excluded at the cut, visible after advancing past its forget record).

### D9 — Scope: whole-cluster failover only; mirrorless DR (PoC)

The DR cluster runs without mirrors and without FTS; the failover unit is the whole
cluster (`gg_recovery promote`, segments-first then coordinator, then a restart cycle to
drop `PGC_POSTMASTER` DR mode). Losing a DR node means re-creating it
(`gg_recovery create-replica`). DR-local mirrors/FTS and per-node repair are deferred —
they are additive and orthogonal to the novel cross-cluster mechanics this PoC had to
prove.

### D10 — Control plane: one `gg_recovery` utility + first-class observability

A single-file Python CLI (`gpMgmt/bin/gg_recovery`) treats the whole DR cluster as one
object: `create-replica` (restore + WAL-fork-safe seed + arm + start, with a fail-closed
archive-completeness gate), `switch <rp>` (advance to a consistent cut), `follow`
(continuous, freshest, no cut), `pause`, `stats`, `promote`. Observability ships in the
engine: `gp_stat_dr_replica` / `gp_stat_dr_replica_summary` views and
`pg_last_paused_restore_point()` (OID 7016), which reports the restore point **actually
reached** (captured from the replayed record, not the configured GUC — the GUC already
points at N+1 mid-advance). The summary's `consistent_restore_point` (non-NULL iff all
nodes are paused at one name) is the operator's safe-to-serve/safe-to-promote signal;
there is deliberately **no cross-node LSN column**, because per-node LSN spaces make such
comparisons meaningless.

## Consequences

### Positive

- **Production is untouchable by DR failures.** No slots, no connections from DR; a dead
  DR replica costs production nothing (D2).
- **One faithful archive.** DR `pg_wal` stays byte-identical to production's (apply-time
  filtering, D3), so the same archive drives production PITR, multiple DR replicas, and
  honest `pg_waldump` forensics. *(ADR-0002 weakens this: the archive remains canonical,
  but DR `pg_wal` now holds per-replica neutralized copies.)*
- **Reads are transactionally consistent cluster-wide** — a real distributed-restore-point
  cut with explicit straddle handling (D7+D8), not an LSN approximation. Inside a serve
  window replay is stopped, so queries are immune to recovery-conflict cancellation.
- **Small, auditable core delta.** The engine changes are: one redo-loop hook + filter
  file, one relmap guard, the DR gate, two read-only checks, one DTX context enum + its
  short-circuits, one QE exemption, one snapshot builder, one accessor + two views. The
  risky machinery (recovery, pause, dispatch, gangs) is all pre-existing and shipped.
- **Promotable in place** at a named, verified restore point; DR mode disengages cleanly
  (marker + GUC + restart ordering, D4/D9).

### Negative / accepted costs

- **Coarse freshness.** RPO = restore-point cadence + archive latency + slowest-segment
  skew; the slowest archiver gates the whole cluster's advance. Streaming-level RPO was
  explicitly traded away (D2, D7).
- **Advance windows can cancel long queries** (standard hot-standby conflict rules); a
  cancelled QE fails the whole distributed query. Mitigation is cadence/`max_standby_*`
  tuning, not elimination (D7).
- **The archive becomes production-critical infrastructure** — availability, retention,
  and completeness now matter operationally; `create-replica` fails closed on archive
  gaps rather than risking a timeline fork.
- **Topology catalogs are contractually immutable on production** while a replica is
  attached: `VACUUM FULL`/`CLUSTER`/`REINDEX`/`TRUNCATE` of one halts the DR with a FATAL
  and requires re-creating the node (D3). Chosen over silent divergence.
- **Inherited, frozen configuration.** Roles, resource groups, and replicated-catalog GUCs
  come from production; changing them on the DR is unsupported.
- **PoC gaps** (deferred, not design limits): `gg_recovery` reaches nodes by local socket
  (single-host); no restore-point scheduler on production; no automated
  latest-common-restore-point computation after an unclean production loss; mirrorless DR.

### Neutral / notes

- The seed's bounded `pg_class` LSN-shadow window (D3) is documented and self-healing;
  eliminate it later with a scratch-copy seed.
- Re-sync after archive gap loss is a full re-`create-replica` of the node; there is no
  cross-WAN `pg_rewind`.
- Production primary→mirror failovers fork that content's timeline; the DR follows via
  archived `.history` files (`recovery_target_timeline='current'`) — an explicitly
  test-worthy scenario.

## Validation

The decision set is validated by the end-to-end fixture in `src/test/dr/` — two
containers (production + DR, 1 coordinator + 2 segments each) sharing an `/archive`
volume, with the DR built by `gg_recovery create-replica`:

- **M1+M2 7/7** — topology filtered *selectively* (production's catalog change invisible,
  its post-backup user table visible), DR-local hostnames survive replay, live
  hot-standby reads work, `INSERT`/`CREATE TABLE`/`FOR UPDATE` refused.
- **M2′ 5/5** — real dispatched distributed reads in recovery (aggregation across both
  segments, InitPlan, multi-slice JOIN), writes refused under dispatch.
- **M3 6/6** — stop-and-go serve/advance loop, and the manufactured 2PC **straddle**
  correctly excluded at the cut and visible after the advance.
- **M5 6/6, `gg_recovery` 4/4** — faithful served-restore-point reporting; stats → switch
  → follow → promote lifecycle (promoted cluster accepts writes).

Process findings worth recording: two silent-no-op bugs (relmapper not loaded in the
startup process; DR flag invisible in backends) were caught **only** by the end-to-end
fixture after unit tests passed, and an adversarial review found a timeline-fork hazard
in the seed that a passing demo masked (fixed by recovery-start preservation + archive
re-fetch + the fail-closed gate in `create-replica`). Both are the reason this feature's
regression story is the docker fixture, not unit tests alone.

## References

- Architecture description (mechanism detail, code map, operations):
  `doc/architecture/greengage-dr-read-replica.md`
- Design notes (repo root): `greengage7-dr-readonly-replica-poc-plan.md` (master plan),
  `greengage7-dr-standby-distributed-read-mode.md` (standby distributed read),
  `greengage7-dr-SR2-reader-gang-snapshot-sync.md` (superseded snapshot-leader analysis —
  see D6/D7)
- Test fixture: `src/test/dr/` (`docker compose -f src/test/dr/docker-compose.yml up --build`)
- Key commits (`git log origin/7.x..7.x-dr`): `9bd62843dc6` (M1 filter),
  `65e3cdb2df1` (M2), `042cca76123`/`f9a46c92395` (M2′ context), `d60154a4081` +
  `cdacc44119a` (M3 snapshot + straddle), `05ab3941756` (M4 seed),
  `ea90295089a` (M5 views), `d3f45c03b47` … `1ee2ea727d3` (`gg_recovery`),
  `d9690739ecc` (architecture doc)
