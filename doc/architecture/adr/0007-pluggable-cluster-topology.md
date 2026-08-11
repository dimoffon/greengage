# ADR-0007: Cluster Topology Behind a Pluggable Store

- **Status:** Accepted — implemented on branch `7.x-dr` through phase P7. P8 (inverting the
  dense fixture) is planned and not yet done; the section that describes it says so.
- **Date:** 2026-08-11
- **Relates to:** [ADR-0006](0006-dr-read-replica.md) — this record is the reason several
  of ADR-0006's mechanisms are expected to be deleted rather than maintained.

---

## Context

Greengage keeps the cluster topology in exactly one place: `gp_segment_configuration`, a
**shared, WAL-logged catalog**. Every consumer — the dispatcher building a gang, FTS
marking a segment down, `gprecoverseg`, `gpexpand`, a DBA running `SELECT * FROM
gp_segment_configuration` — reads that catalog directly.

For a disaster-recovery read replica, that single fact is the source of nearly all the
complexity. A DR replica byte-replays production's WAL. The topology catalog is in that
WAL. So production's topology arrives at the replica and overwrites the replica's own —
different hosts, different ports, different data directories. The replica then dispatches
to production's segments, or to nothing.

Everything the DR branch built to stop that exists **only** because topology lives in a
replicated catalog:

| Mechanism | Where | Why it exists |
|---|---|---|
| Apply-time redo filter | ADR-0003, `xlog.c` / `heapam` redo path | Drop redo touching the topology relfilenode |
| Relmapper and smgr guards | same | Keep the filter honest when the relfilenode moves |
| A second implementation | ADR-0002, `gg_walfilter` | The same rules again, on the restore path |
| Single-user frozen seed | `ggseed_dr_topology` | Plant the replica's own topology so the filter has something to protect |
| Permanent-timeline-fork hazard | consequence of the seed | A frozen write in single-user mode is not replayable |

None of that is about disaster recovery. It is about a catalog being in the wrong place.
(The last two rows are gone as of P6 — see D9.)

The same fact costs non-DR work too: topology cannot be inspected or repaired without a
running coordinator, and any tool that wants to change it must be able to write a shared
catalog.

---

## Decisions

### D1 — A provider dispatch table, selected by a postmaster GUC

`GpTopologyRoutine` (`src/include/cdb/cdbtopology.h`) is a table of function pointers,
modelled on `smgrsw[]`. `gp_topology_source` (`PGC_POSTMASTER`, values `catalog` and
`file`) picks one for the life of the postmaster.

`PGC_POSTMASTER` rather than something reloadable: the provider is consulted before
shared memory is attached and by processes that never re-read configuration. A topology
that could change provider mid-life would have to be re-validated at every read.

### D2 — Reads are one call that returns the whole set

```c
GpSegConfigEntry *GpTopologyGetAll(MemoryContext cxt, int *nentries);
```

There is no "read one row" entry point. Every real caller wants the set (build a gang,
find the mirror of a content, count segments), and a store that is a file or a remote
blob has no cheaper way to answer a single row than to read all of them.

### D3 — Writes are a *write set*, not row operations

```c
GpTopoWriteSet *GpTopoBeginWrite(MemoryContext cxt, GpTopoWriteLevel level);
/* GpTopoInsert / GpTopoUpdate / GpTopoDelete against the set */
void GpTopoPersist(GpTopoWriteSet *ws);
void GpTopoEndWrite(GpTopoWriteSet *ws, bool commit);
void GpTopoCommitWrite(GpTopoWriteSet *ws);
```

This is the decision the whole interface turns on. Row operations are a *catalog* idiom.
An external store wants compare-and-swap over a blob; a file wants whole-file replace;
only a catalog can update one tuple in place. Exposing row operations would have forced
every non-catalog provider to reconstruct the set on each call.

So the caller edits an in-memory set and the provider persists it however it likes. The
catalog provider absorbs the difference by diffing `orig` against `work` on `dbid` and
issuing the DELETEs, then the UPDATEs, then the INSERTs — in that order, because the
unique indexes 7139 and 7140 will otherwise reject a legal rearrangement halfway through.

**Accepted cost.** The file provider renames at `persist()` time, not at commit. A
transaction that rolls back after `gp_add_segment_primary()` still leaves the store
advanced. That is a genuine divergence from the catalog provider; it is asserted
explicitly in `src/test/topology_file/run.sh` rather than left to be discovered.

### D4 — Lock level is the caller's choice

`GpTopoWriteLevel` is `GP_TOPO_WRITE_ROW` (a row or two, concurrent readers fine) or
`GP_TOPO_WRITE_SERIALIZED` (whole-set exclusivity held to commit). FTS marking one
segment down does not need what `gpexpand` needs, and the provider cannot infer the
difference from the edits alone.

### D5 — The write set knows it can be aborted underneath it

`GpTopoWriteOutcome` has three values, not two: `COMMIT`, `ROLLBACK`, and `ABORTED`. The
third exists because a transaction abort can reach the write set through the xact and
subxact callbacks, and at that point touching the relcache to close a relation is not
safe. The catalog provider needs to know which of the two failure paths it is on.

The callbacks are keyed on transaction nest level, following the `postgres_fdw` pattern.

### D6 — The `file` provider, and what its `generation` field is for

`$PGDATA/gp_topology` is a text file — one entry per line, CRC-32C over everything but
the last line, read and written from both backend and frontend through
`src/common/gp_topology_file.c` (the `#ifdef FRONTEND` split follows
`controldata_utils.c`).

`generation` is deliberately two things at once:

- the **CAS token** — a writer reads generation *N*, and its rename is only valid if the
  store is still at *N*;
- the **bootstrap marker** — generation 0 means "initdb wrote this and nobody has
  registered a cluster". A dispatcher refuses to start on generation 0; utility mode
  starts, because that is how an operator bootstraps.

The shared-memory watermark (`GpTopoGenerationSeen`) is a hint, not the authority: 0
means "unknown", and the authority is always the file under `GpTopologyLock`.

### D7 — `gp_segment_configuration` becomes a view over the active store

The shared catalog is renamed `gp_segment_configuration_internal`; the familiar name
becomes an ordinary view in `pg_catalog`:

```sql
CREATE VIEW gp_segment_configuration AS
    SELECT * FROM pg_catalog.gp_get_segment_configuration()
        AS gp_segment_configuration;
```

This is what makes the provider real for everything outside the backend. Before it, a
cluster running the `file` provider had a topology the backend used and a catalog every
SQL caller read — and the catalog was empty. gpMgmt had to *refuse* to operate on such a
cluster (a refusal added in P4 and deleted here).

Four sub-decisions inside it are worth recording, because each was tested and each could
plausibly have gone the other way.

**D7a — `proexeclocation => 'c'`, because the alternative is silently wrong answers.**

This was decided twice. The first answer was to omit it: with the default execution
location plus `provolatile => 'v'` the function scan gets the same Entry locus a
shared-catalog scan gets, and the plans in the tree came out byte-identical. That
evidence was real but incomplete — every shape tested was uncorrelated.

Inside a **correlated** subplan the planner does not give it Entry locus. It runs the
scan on the segments, where no topology is readable, so the subquery yields NULL. The
shared catalog raises `Passing parameters across motion is not supported` for exactly
that shape; `src/test/regress/sql/rpt.sql:650` is in the tree to assert it. Measured on a
three-segment cluster, with the topology holding dbids 2, 3, 4 at contents 0, 1, 2:

| | result |
|---|---|
| shared catalog | `ERROR: Passing parameters across motion is not supported (cdbmutate.c:2052)` |
| function scan, no `proexeclocation` | three rows, all NULL — **wrong**, the answer is 2, 3, 4 |
| function scan, `proexeclocation => 'c'` | `ERROR: cannot execute EXECUTE ON COORDINATOR function in a subquery with arguments from outer query (pathnode.c:2968)` |

So the marking is required. An error the catalog also raised beats an answer that is
quietly wrong.

**What the marking costs, stated plainly.** `pathnode.c`'s check is cruder than
`cdbmutate.c`'s: it refuses *any* EXECUTE ON COORDINATOR function in a subquery carrying
outer parameters, whether or not a motion is actually involved. So one shape that works
against the catalog now errors — a correlated reference whose outer relation is *itself*
coordinator-only, and therefore needs no motion:

```sql
SELECT (SELECT (SELECT dbid FROM gp_segment_configuration
                 WHERE dbid = numsegments LIMIT 1)) FROM gp_distribution_policy;
```

`qp_correlated_query.sql` holds the only instance in the tree; its expected output records
the error. This is a real, if narrow, regression against the catalog, and it is the price
of not answering the `rpt.sql` shape wrongly.

The earlier claim that `proexeclocation => 'c'` breaks `qp_correlated_query.sql:978` was
simply wrong: that query errors identically either way, because the skip-level-correlation
check fires first.

**D7b — `prorows => 16` is load-bearing.** At the planner's default of 1000, the join
order flips in every in-tree query that joins topology against segment data. 16 keeps the
estimate in the range a real cluster occupies and the plans in the shape they had when
this was a nearly-empty catalog.

**D7c — The function returns no rows on a segment, but the entry db is not a segment.**
The guard is `Gp_role == GP_ROLE_EXECUTE && !IS_QUERY_DISPATCHER()`. Under the catalog
provider the shared copy on a segment is empty anyway, so suppressing there changes
nothing; under a per-node store it is populated, and answering would turn a query that
silently returns nothing today into one that silently returns something. Utility mode is
deliberately unaffected — an operator can still ask a single node what its own store says.

The `IS_QUERY_DISPATCHER()` half is load-bearing and was found by test, not by reading.
The entry db runs as a QE — `Gp_role` really is `GP_ROLE_EXECUTE` there — but it *is* the
coordinator, and it is where an Entry-locus slice of a dispatched plan executes. Without
the exclusion, a bare `SELECT` off `gp_segment_configuration` works (the QD backend runs
it itself) while the same scan inside a slice returns nothing: `INSERT ... SELECT ... FROM
gp_segment_configuration` silently inserts zero rows. That is the shape
`src/test/regress/sql/query_finish_pending.sql:43` uses to size a table by segment count,
and it went from 150000 rows to none.

**D7d — The function is aliased back to the view's name.** The planner pulls a view this
simple up, and the surviving range-table entry is what `EXPLAIN` prints and what
qualifies column names. Without the alias, every plan and every
`gp_segment_configuration.port = ...` qualification in the tree would start reading
`gp_get_segment_configuration`.

### D8 — Writers do not go through the view

The view is read-only, and that is the intent: a topology write has to go through the
provider so the active store is the one that changes. The two places gpMgmt still issued
a raw `UPDATE gp_segment_configuration` now call `gp_update_segment_mode_status(dbid,
mode, status)` (oid 7202), where a NULL mode or status means "leave that column alone".
The backout scripts gpMgmt emits no longer need `SET allow_system_table_mods = true`.

Everything that genuinely means *the catalog* — `gpcheckcat`'s coordinator-only table
list, the DR seeding scripts, the tests that assert on physical rows or `ctid` — is
repointed at `gp_segment_configuration_internal` by name.

---

## Consequences

**What this buys.** Once DR runs on the `file` provider (P6), production's topology
catalog can arrive in the WAL and land harmlessly in `gp_segment_configuration_internal`,
because nothing reads it. The redo filter, the relmapper and smgr guards, the
`gg_walfilter` rules, the frozen single-user seed and its timeline-fork hazard all become
dead code — that is P7. This ADR is the reason ADR-0006 D4 is expected to be *deleted*
rather than maintained.

**`gp_segment_configuration` is no longer updatable.** A raw `UPDATE` or `INSERT` against
it now fails with `cannot update view "gp_segment_configuration"`. Any external tool that
wrote the catalog directly breaks and must move to the SQL functions. This is deliberate,
not incidental.

**The view is public-SELECT and droppable, with a caveat.** It is an ordinary view: no
ACL entry, so `public` has `SELECT`, exactly as the catalog had. Nothing in the backend
resolves it by OID — the backend reads the provider directly — so dropping it disables
only SQL callers. But it cannot be dropped bare: `gp_stat_replication` and
`pg_max_external_files` select from it, so `DROP VIEW` reports them and requires
`CASCADE`.

**`EXPLAIN` output changes for every plan that touches the topology.** `Seq Scan on
gp_segment_configuration` becomes `Function Scan on gp_get_segment_configuration
gp_segment_configuration`. Six in-tree expected files carry that change, and two more
(`rpt`, `qp_correlated_query`, with their `_optimizer` variants) record the error text
that replaced a plan under D7a.

**Anything reading the topology's *storage* rather than its *content* must say
`_internal`.** The name now resolves to a relation with no relfilenode, no size and no
rows of its own, and most of those operations fail quietly:
`pg_relation_size('gp_segment_configuration')` answers 0, `VACUUM` warns and skips, and
`gp_toolkit.gp_db_files_current` — which reports `pg_class.relname` for each file found on
a segment — simply never lists it. Each of those was a live bug in this change until a
test or a grep found it.

**Position in `system_views.sql` is load-bearing.** `gp_stat_replication` and
`pg_max_external_files` select from the view and `initdb` runs the file top to bottom, so
the view must be created before both.

**`GpSegmentConfigRelationName` still reads `"gp_segment_configuration"`.** It is
error-message text, and those messages are about the topology, not about which relation
happens to hold it.

**Not done: `pg_upgrade`.** Renaming a shared catalog and adding a view is a catalog
version bump, which this branch takes, but no upgrade path from a pre-provider cluster
has been written or tested.

**`NUM_INDIVIDUAL_LWLOCKS` shifted** when `GpTopologyLock` (id 65) was added, so a tree
built across that commit needs `make clean`.

---

## Rejected alternatives

**Keep the catalog and keep filtering redo.** This is the state ADR-0003 describes and it
works, but it costs two implementations of the same rules (in-backend and
`gg_walfilter`), a seed that must be written in single-user mode, and a hazard — a frozen
single-user write is not replayable, so the seeded replica's timeline cannot be rejoined.
Every one of those is a permanent maintenance cost paid for a catalog's location.

**Make the topology catalog `UNLOGGED` or non-shared.** Neither is available: the
dispatcher needs it before a database is chosen (so it must be shared), and an unlogged
shared catalog is not a thing PostgreSQL 12 has.

**Expose row operations in the provider interface.** Rejected under D3 — it is the
catalog's idiom, and it would have made every other provider read-modify-write the whole
set behind the caller's back on each call, with no way to batch.

**Omitting `proexeclocation` on the SRF.** Adopted first, then reversed under D7a: the
plans really are identical in the uncorrelated shapes, but a correlated subplan runs the
scan on the segments and answers NULL instead of erroring. The reversal cost a catalog
version and one narrower in-tree behaviour, and it is not negotiable — a topology read
that returns nothing looks exactly like a topology with nothing in it.

---

## Invariants a developer must not break

1. **No backend code opens or scans `gp_segment_configuration_internal` directly.** Only
   `cdbtopology_catalog.c` may. Everything else goes through `GpTopologyGetAll()` or a
   write set. Naming `GpSegmentConfigRelationId` to *classify* the relation is fine and
   two places do it — `IsSharedRelation()` in `catalog.c` and the coordinator-only set in
   `gpexpand.c`; opening it anywhere else is a bug.
2. **No SQL in the tree writes `gp_segment_configuration`.** It is a view; writes go
   through the `gp_add_segment*` / `gp_remove_segment*` /
   `gp_update_segment_mode_status()` functions.
3. **The view is created before `gp_stat_replication` in `system_views.sql`.**
4. **`prorows` on `gp_get_segment_configuration` stays small.** Changing it changes plan
   shapes across the regression suite.
5. **Anything that suppresses a topology read on a QE excludes the entry db.** See D7c:
   `Gp_role == GP_ROLE_EXECUTE` alone is not "is a segment", and the difference is silent.
6. **`generation` only ever increases,** and a writer that did not observe the current
   generation must not rename over the store.

---

## Status of the phases

| Phase | Subject | State |
|---|---|---|
| P0 | Promote path excludes the DR replica | Done |
| P1 | Provider read side | Done |
| P2 | Write seam; segadmin and FTS on write sets | Done |
| P3 | Remaining readers re-pointed | Done |
| P4 | The `file` provider, `gg_topology`, `initdb` | Done |
| P5 | The view, the catalog rename, `gp_update_segment_mode_status()` | Done |
| P6 | DR switches to `file`; the frozen seed deleted | Done |
| P7 | Delete the redo filter | Done |
| P8 | Invert the dense fixture | Not started |

### D10 — the redo filter is deleted, and T6 is what says it was safe (P7)

`dr_redo_filter.{c,h}` and its test are gone, the redo hook in `xlog.c` collapses back to an
unconditional `rm_redo()` + `checkXLogConsistency()`, and the `relmapper.c` remap guard and
`storage.c` truncate guard go with them. The replica now replays production's WAL in full —
including `gp_segment_configuration_internal` and `gp_configuration_history`.

**The two halves are true at once, and that is the design.** The fixture asserts both: the
replica serves `dr` (T1, T2) *and* its catalog carries production's replayed change (T3).
Under P6 the second was false, because the filter was still blocking that record.

**T6 is the acceptance test.** Production runs `wal_consistency_checking = 'all'`, so every
record carries a full-page image and the replica compares its own page against production's
after redo; a mismatch is a `FATAL` naming the relation and block (`xlog.c:1545`). Measured:
1043 full-page images in the first WAL segment alone, zero inconsistencies across all three
nodes. This could not be run before — the filter skipped the consistency check alongside
redo, so agreement proved nothing about the records being skipped. That is why the deletion
needed its own acceptance test rather than a green suite.

**What was deliberately kept.** `IsDRReplicaMode()` and every non-filter consumer:
read-only enforcement, the served-snapshot capture, the DTM backstop, the visibility-map
bypass, promote-in-place. `gg_walfilter` keeps its generic rules, `wf_apply_relmap_update()`
and `wf_truncate_*()`; only the Greengage-specific `--gp-dr-topology` preset goes. That
preset was the sole setter of `rules.remap_guard`, so deleting it would have made the guard
unreachable — it is now `--halt-on-remap`, and the two test classes that exercised it are
repointed rather than deleted.

**`gp_id` no longer needs protection, and nobody should re-add it.** It was on the protected
list because a replica whose `gp_id` was overwritten would report production's dbid. Nothing
reads `gp_id` for topology any more — `gg_stat_dr_replica` uses it only through
`gp_dist_random('gp_id')`, which relies on the "exactly one row per node" contract, and that
contract is a property of the relation, not of who wrote it last.

**What the new design does not protect.** After promotion the replica's
`gp_segment_configuration_internal` holds production's replayed rows, so anyone flipping a
promoted DR back to `gp_topology_source = catalog` gets a cluster describing production.
`create-replica` arms `file` in `postgresql.conf` so it survives promotion, which is the
mitigation; the failure needs a deliberate configuration change to reach.

### D9 — the DR replica keeps its topology in a file, and the seed is gone (P6)

`ggdr create-replica` now writes the DR-local topology into **every** node's
`$PGDATA/gp_topology` and arms `gp_topology_source = file`, instead of running
`ggseed_dr_topology` — a single-user `postgres --single` that DELETEd, re-INSERTed and
`VACUUM FREEZE`d the shared catalog on the coordinator.

Deleting the seed deleted its scaffolding with it: the `backup_label`/`pg_control`
save-restore that existed because `postgres --single` consumes the first and advances the
second, the backup-tail WAL re-fetch that existed because the seed appended its own WAL to
the backup-end segment, and `--scratch-seed`, a reserved workaround for the seed's
`VACUUM FREEZE` footprint on `pg_class`. **With them goes the permanent-timeline-fork
hazard** the seed's own header documented at length: on an archive miss, recovery replayed
the seed's forked WAL and the fork was unrecoverable.

Three details are worth keeping:

- **The base backup carries production's `gp_topology`.** P4 deliberately did not exclude
  it from `basebackup.c`, on the grounds that a node needing a different topology has it
  overwritten after the copy. A DR node is that node, and `create-replica` is that
  overwrite. It happens before the first start, which matters: production never writes the
  file, so it arrives at initdb's generation 0, and a coordinator armed with `file` refuses
  to start in dispatch mode on a generation-0 store. Fail-closed in the right direction.
- **Every node gets the file, not just the coordinator.** Only the coordinator's copy is
  read while the replica serves. But `gp_topology_source` is exempt from the QD/QE GUC-sync
  check (`unsync_guc_name.h`), so a half-armed cluster comes up silently, and a segment
  left holding production's entries would start describing the wrong cluster the moment the
  replica is promoted and FTS begins writing. Writing and arming happen in one loop.
- **Nothing writes the store while the replica is in recovery — but that had to be made
  true, it was not.** FTS is fine on its own: it is a `BgWorkerStart_DtxRecovering` worker,
  which `bgworker_should_start_now()` grants only in `PM_RUN`, never `PM_HOT_STANDBY`. The
  first version of this record stopped there and was wrong. `file_persist()` is also
  reachable from segadmin's SQL functions, and those are plain `CMD_SELECT`s that
  `ExecCheckXactReadOnly()` lets through — so `SELECT gp_update_segment_mode_status(...)`
  on a read-only replica durably rewrote `$PGDATA/gp_topology`.

  The catalog provider had refused it, but only as a side effect: writing a catalog needs
  an XID and `GetNewTransactionId()` refuses one during recovery. A file needs no XID, so
  moving the topology out of the catalog silently dropped a safety property nobody had
  written down. `GpTopoBeginWrite()` now refuses outright when `RecoveryInProgress()`, which
  puts the rule where it covers every provider and every caller instead of falling out of
  one implementation. It does not block promotion: `gp_activate_standby()` runs from
  `UpdateCatalogForStandbyPromotion()` (`xlog.c:8568`), after `SharedRecoveryState` becomes
  `RECOVERY_STATE_DONE` (`xlog.c:8505`).

  Found by adversarial review, not by writing this paragraph — which is the point of
  running one. The fixture now asserts both halves: the write is refused, *and* the store
  is still at generation 1 afterwards.

**What P6 proves, and what it does not.** The plan expected one green fixture pass with the
filter still compiled in to show the DR no longer depends on it. It does not: a pass with
the filter present and firing shows compatibility, not independence, and there is no switch
to turn the filter off — `IsDRReplicaMode()` is `EnableHotStandby && RecoveryInProgress()`,
and `hot_standby` also gates the read path, the frozen-snapshot serve, and promote-in-place.
So the proof is made positively instead. Nothing seeds the catalog any more, so
`gp_segment_configuration_internal` on the replica holds the rows that came in the base
backup — production's — while the replica serves its own. Measured on the fixture:

```
what the promoted cluster serves    the catalog it no longer reads
1|-1|p|n|u|dr                       1|-1|primary
2| 0|p|n|u|dr                       2| 0|primary
3| 1|p|n|u|dr                       3| 1|primary
```

That assertion does not change in P7; only the catalog's contents do, once production's
updates stop being filtered out.

**A cost that is now unpaid-for.** `DRRejectForbiddenRemap()` is `FATAL`: a replayed shared
relmap update for a protected OID halts the DR postmaster. Under `file` that means a
production `VACUUM FULL`, `CLUSTER` or `REINDEX` of the topology catalog still takes the
replica down permanently — over a relation the replica no longer reads. The risk is
unchanged from before P6, but from here it buys nothing. P7 removes it, and P8's V-13′ is
precisely that scenario.
