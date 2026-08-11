# ADR-0007: Cluster Topology Behind a Pluggable Store

- **Status:** Accepted — implemented on branch `7.x-dr` through phase P5. P6–P8 (switching
  DR to the file provider, deleting the redo filter, inverting the dense fixture) are
  planned and not yet done; the sections that describe them say so.
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

**D7a — No `proexeclocation`.** The plan for this phase specified
`proexeclocation => 'c'` (EXECUTE ON COORDINATOR). It cannot be used: it raises a hard
error when a subquery correlates into the function, and one in-tree query already does
exactly that (`src/test/regress/sql/qp_correlated_query.sql:978`). Omitting it and
marking the function `provolatile => 'v'` reaches the *same* Entry locus through
`create_functionscan_path`. Verified on a real two-segment cluster: byte-identical plans,
the correlated shape plans without error, identical rows and column types.

**D7b — `prorows => 16` is load-bearing.** At the planner's default of 1000, the join
order flips in every in-tree query that joins topology against segment data. 16 keeps the
estimate in the range a real cluster occupies and the plans in the shape they had when
this was a nearly-empty catalog.

**D7c — The function returns no rows on a QE.** `Gp_role == GP_ROLE_EXECUTE` returns an
empty tuplestore. Under the catalog provider the shared copy on a segment is empty
anyway, so nothing changes; under a per-node store it is populated, and answering there
would turn a query that silently returns nothing today into one that silently returns
something. Utility mode is deliberately unaffected — an operator can still ask a single
node what its own store says.

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
gp_segment_configuration`. Six in-tree expected files carry the change.

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

**`proexeclocation => 'c'` on the SRF.** Rejected under D7a on evidence: it breaks a
correlated-subquery shape that is already in the tree, and it buys nothing the default
execution location plus `VOLATILE` does not already give.

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
5. **`generation` only ever increases,** and a writer that did not observe the current
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
| P6 | DR switches to `file` | Not started |
| P7 | Delete the redo filter and the seed | Not started |
| P8 | Invert the dense fixture | Not started |
