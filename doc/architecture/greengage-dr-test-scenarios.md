# Greengage 7 DR Read Replica — Test & Demonstration Scenarios

Companion to [`greengage-dr-read-replica.md`](greengage-dr-read-replica.md). That document
describes *what the feature is*; this one is the catalogue of **scenarios that demonstrate
it, bound its supported surface, and probe its corner cases** — each with the expected
behaviour and the mechanism that produces it.

Every "expected behaviour" below is tied to a mechanism in the shipped code. Where a
scenario has **not** been run yet and the outcome is a prediction from reading the code,
it is marked **`PREDICTED`** — those are the ones most worth running first, because two of
them (T-3, HA-1) predict *failures*.

---

## How to read this document

| Column | Meaning |
|---|---|
| **ID** | Stable scenario id, referenced from test code / bug reports |
| **Status** | `AUTO` = already asserted by the container fixture (`src/test/dr/scripts/dr-entrypoint.sh`); `NEW` = to be added; `MANUAL` = operator demo, not automatable in the current fixture |
| **Verdict** | `PASS` = works as designed; `REFUSE` = must be rejected with a specific error; `DEGRADE` = works but with a documented cost; `HALT` = the DR intentionally stops; **`GAP`** = known/suspected defect |

Base fixture: `docker-compose -f src/test/dr/docker-compose.yml up --build` — a production
container (coordinator + 2 segments, archiving per content) and a DR container sharing an
`/archive` volume, built with `ggdr create-replica`.

---

## Group A — Build, arm, and baseline lifecycle

| ID | Scenario | Expected behaviour & why | Status | Verdict |
|---|---|---|---|---|
| **A-1** | `ggdr create-replica` from per-content base backups + WAL archive | Every node restored, coordinator frozen-seeded with DR-local topology, replica armed (`hot_standby`, `restore_command`, `standby.signal`), all nodes started in archive recovery and accepting read connections, and the post-build topology check confirming the seed describes this cluster (V-21). | AUTO | PASS |
| **A-2** | DR describes **itself**, not production | `gp_segment_configuration.hostname` on the DR reads `dr`, never production's host. The frozen-tuple seed (`xmin = FrozenTransactionId`) is unconditionally visible with no CLOG dependency, and the redo filter blocks production's records from overwriting it. | AUTO | PASS |
| **A-3** | Production mutates `gp_segment_configuration` (hostname change) | DR value is **unchanged**. `DRRedoShouldFilter()` sees every referenced block in the protected set and skips `rm_redo`; `lastReplayedEndRecPtr` still advances, so the DR stays in LSN lock-step. | AUTO | PASS |
| **A-4** | Redo filter is **selective** — a user table created on production *after* the base backup | Table appears on the DR. Proof the filter only skips the topology catalogs, not ordinary WAL. | AUTO | PASS |
| **A-5** | `create-replica` refuses to clobber a live cluster | Aborts if any target datadir has `postmaster.pid`; refuses a non-empty datadir without `--force`. | NEW | REFUSE |
| **A-6** | `create-replica` with an **incomplete WAL archive** (backup's START WAL segment absent) | Aborts fail-closed *before* starting any node. Without this gate, recovery could replay the seed's forked WAL and permanently fork the timeline. | NEW | REFUSE |
| **A-7** | `pg_wal` on the DR is **byte-identical** to production's for the same content | `pg_waldump` diff / checksum of a replayed segment matches production's archived copy. The filter changes what is *applied*, never the bytes — this is what keeps the archive reusable for production PITR and for a second DR. | NEW | PASS |
| **A-8** | Two DR replicas built from **one** archive | Both build and serve independently. No replication slot exists, so neither pins production WAL nor interferes with the other. | NEW (MANUAL) | PASS |
| **A-9** | Segment-count mismatch (DR topology has N+1 contents) | Build must fail or the DR must refuse to serve. Per-content replay requires an exact content-count match; host layout is free. | NEW | REFUSE |
| **A-10** | Version / block-size / checksum mismatch between clusters | Startup fails on the standard physical-replication contract checks (`CATALOG_VERSION_NO`, block size, checksums, encoding). Nothing DR-specific — but must be demonstrated to fail *loudly*, not subtly. | NEW (MANUAL) | REFUSE |

---

## Group B — Query support surface

This is the "what can I actually run on it" matrix. Two enforcement layers produce these
answers: `ExecCheckXactReadOnly()` (`execMain.c:1651`) for planned statements with a
DR-specific message, and `check_xact_readonly()` (stock hot standby) for utility
statements. The DTM backstop in `currentDtxActivate()` (`cdbtm.c:281`) catches anything
that tries to allocate a `gxid`.

### B.1 — Supported (must succeed)

| ID | Query shape | Why it works | Status |
|---|---|---|---|
| **Q-1** | Coordinator-only catalog read (`select … from gp_segment_configuration`) | Plain hot-standby backend read; no dispatch. | AUTO |
| **Q-2** | Single-slice distributed `SELECT` (`select count(*) from t`) | Coordinator enters `DTX_CONTEXT_QD_STANDBY_READER` (no gxid, no WAL), dispatches with `is_hs_dispatch` to segments in recovery. | AUTO |
| **Q-3** | `select gp_segment_id from t` — proves rows are really served by a DR segment | Real gang dispatch, not a coordinator-local shortcut. | AUTO |
| **Q-4** | InitPlan-bearing read (`select (select count(*) from t)`) | The InitPlan/subplan DTX setup is skipped under DR mode (`execMain.c:666`), so no gxid is demanded. | AUTO |
| **Q-5** | Multi-slice **JOIN** (redistribute/broadcast motion, QE_READER consume path) | Reader gangs work; under stop-and-go every slice sees the same static snapshot. | AUTO |
| **Q-6** | Aggregates, window functions, `GROUP BY`/`ORDER BY` with **spill to disk** | Work files are ordinary temp files — no WAL, no catalog write. | NEW |
| **Q-7** | Read-only CTEs, subqueries, `UNION ALL` mixing a coordinator-entry row with segment rows | The last one is planned onto a **writer** gang; the gxid check is exempted for standby QEs via `IS_STANDBY_QE()` (`xact.c:2510`). `gg_stat_dr_replica` itself is this shape. | AUTO (via M5) |
| **Q-8** | Multi-statement read transaction (`BEGIN; SELECT; SELECT; COMMIT`) | Repeatable within a serve window — replay is paused, so the local recovery snapshot is static. | NEW |
| **Q-9** | `DECLARE CURSOR` / `FETCH` within one serve window | Same static-snapshot argument. **Caveat:** a cursor held across an `N → N+1` advance is exposed to recovery conflicts (see C-5). | NEW |
| **Q-10** | `EXPLAIN` and `EXPLAIN ANALYZE` of a **SELECT** | `commandType == CMD_SELECT`, no rowMarks → passes the DR gate. | NEW |
| **Q-11** | `PREPARE` / `EXECUTE` of a SELECT; plan reuse across an advance | Plan-cache invalidations arrive as replayed catalog invalidations. | NEW |
| **Q-12** | Reads of **AO / AOCO** and partitioned tables | Append-optimized reads consume `pg_aoseg` + visimap, all replayed. Worth explicit coverage since AO is the common GP storage. | NEW |
| **Q-13** | `SET`, `SHOW`, `RESET`, `DISCARD`, session GUC changes | No WAL, no catalog write. | NEW |
| **Q-14** | `ALTER SYSTEM SET` + `pg_reload_conf()` | Writes `postgresql.auto.conf`, not WAL — allowed on a standby. `ggdr switch` depends on this. | AUTO (via `ggdr`) |
| **Q-15** | `pg_last_paused_restore_point()`, `gg_stat_dr_replica`, `gg_stat_dr_replica_summary` | Read-only accessors over `XLogCtl` + `gp_dist_random('gp_id')`. | AUTO |
| **Q-16** | Read from an external table (gpfdist / PXF readable) | Read-only dispatch. **Confirm empirically** — external scans have their own gang requirements. | NEW `PREDICTED` |

### B.2 — Not supported (must be refused, with the right error)

| ID | Statement | Expected error & layer | Status |
|---|---|---|---|
| **Q-20** | `INSERT` / `UPDATE` / `DELETE` | `cannot execute <tag> on a read-only disaster-recovery replica` — `ExecCheckXactReadOnly` (DR-specific message, ahead of the stock one). | AUTO |
| **Q-21** | `SELECT … FOR UPDATE / FOR SHARE` | Same DR message via `plannedstmt->rowMarks != NIL`. **This is the case stock hot standby would allow** — the DR must not. | AUTO |
| **Q-22** | Data-modifying CTE (`WITH x AS (INSERT …) SELECT`) | Same DR message via `hasModifyingCTE`. | NEW |
| **Q-23** | `SELECT INTO` / `CREATE TABLE AS` | Same DR message via `intoClause`. | NEW |
| **Q-24** | `REFRESH MATERIALIZED VIEW` | Same DR message via `refreshClause`. (Reading an existing matview is fine — it serves whatever was replayed.) | NEW |
| **Q-25** | DDL: `CREATE TABLE`, `ALTER TABLE`, `DROP`, `TRUNCATE`, `CREATE INDEX` | Stock `cannot execute … in a read-only transaction` from `check_xact_readonly`. | AUTO (CREATE TABLE) |
| **Q-26** | **`CREATE TEMP TABLE`** | Also refused — `T_CreateStmt` is in `check_xact_readonly`'s list with **no temp exemption**. Consequence: **no temp-table staging on the DR.** Report tooling that materialises intermediates into temp tables will not run unmodified. | NEW |
| **Q-27** | `VACUUM`, `ANALYZE`, `REINDEX`, `CLUSTER` **on the DR** | Refused (read-only utility). Also structurally impossible: the autovacuum launcher only starts at `PM_RUN`, and a DR node never leaves `PM_HOT_STANDBY`. **DR statistics are whatever production's ANALYZE produced.** | NEW |
| **Q-28** | `CREATE/ALTER/DROP ROLE`, `GRANT`, `ALTER … OWNER` | Refused. **Roles, passwords and privileges on the DR are frozen at whatever production replicates** — you cannot grant a reporting user access on the DR alone. | NEW |
| **Q-29** | `nextval()` / `setval()` / `currval()` on a sequence | Refused during recovery. Reading `last_value` from the sequence relation works. | NEW |
| **Q-30** | `BEGIN; … PREPARE TRANSACTION 'x';` and any 2PC | DTM backstop: `cannot start a distributed transaction on a read-only disaster-recovery replica`. Allocating a `gxid` mutates cluster-wide `nextGxid` — the deepest write. | NEW |
| **Q-31** | `SET TRANSACTION ISOLATION LEVEL SERIALIZABLE` | `cannot use serializable mode in a hot standby` (stock PG). READ COMMITTED and REPEATABLE READ work. | NEW |
| **Q-32** | `COPY … FROM` (import) | Refused (write path). | NEW |
| **Q-33** | `COPY t TO …` (export, direct table form) | **Determine empirically.** `COPY (SELECT …) TO` is a planned SELECT and should pass; the direct-table GP dispatch path may demand a writer gang and hit the DTM backstop. A working bulk-export path off the DR is a headline use case, so this needs a definite answer. | NEW `PREDICTED` |
| **Q-34** | `pg_switch_wal()`, `pg_create_restore_point()`, `gp_create_restore_point()` on the DR | Refused — recovery-mode functions. Restore points are created on **production** only. | NEW |
| **Q-35** | A PL/pgSQL function that reads, then writes | Reads execute; the statement that writes raises at execution time. Confirms enforcement is not merely a parse-time gate. | NEW |

---

## Group C — Consistency: stop-and-go and the straddle

| ID | Scenario | Expected behaviour & why | Status | Verdict |
|---|---|---|---|---|
| **C-1** | All nodes paused at restore point `N`; read twice | Identical results. Replay is stopped, so the local recovery snapshot is static and the distributed dimension is a fixed as-of-N `DistributedSnapshot`. **Zero recovery-conflict exposure inside a serve window.** | AUTO | PASS |
| **C-2** | Advance `N → N+1` (`ggdr switch`), re-read | Results move forward atomically to the new cut — data written on production between N and N+1 becomes visible together. | AUTO | PASS |
| **C-3** | **The straddle.** A distributed 2PC txn *T* whose coordinator `XLOG_XACT_DISTRIBUTED_COMMIT` lands *before* the restore point but whose commit-prepared broadcast lands *after* (injected with the `dtm_broadcast_commit_prepared` fault) | At the cut, *T*'s rows are **invisible everywhere** and no error is raised. `CreateDRStandbyDistributedSnapshot()` places `shmCommittedGxidArray` entries into the **in-progress (invisible)** set — the semantic inversion that compensates for the straddle. After advancing past *T*'s forget, its rows appear. | AUTO | PASS |
| **C-4** | `ggdr pause` (immediate pause), then read | Reads *work* but the cut is **not** cluster-consistent — each node halted wherever it was. `gg_stat_dr_replica_summary.consistent_restore_point` is `NULL`. Operators must treat this as "inspect a node", never "serve a report". | NEW | DEGRADE |
| **C-5** | A long analytical query spanning an **advance** (`gg_dr_switch` to the next restore point), while production has run `VACUUM` | The query can be delayed then cancelled: `canceling statement due to conflict with recovery … User query might have needed to see row versions that must be removed`. With one backend per segment, **cancelling any one segment fails the whole distributed query**. `hot_standby_feedback` cannot help — there is no streaming connection back to production. Since free-running mode was removed, this is now confined to advance bursts rather than being the steady state. | NEW | DEGRADE |
| **C-6** | Same as C-5 with `max_standby_archive_delay = -1` | Queries survive; **replay stalls instead**, so RPO grows for as long as the query runs. Demonstrates the knob is a trade, not a fix. | NEW | DEGRADE |
| **C-7** | Transaction open on the DR **spanning an advance**, holding a lock on a table production rewrote | **Measured — see [C-7/V-10 results](#appendix--c-7--v-10-measured-vacuum-full-under-a-live-reader).** The reader is cancelled, not fed a torn view: `canceling statement due to conflict with recovery / User was holding a relation lock for too long`. Its whole transaction aborts; after rollback the next statement sees the new cut. The reader also **delays the advance for the whole cluster** by `max_standby_archive_delay`. | **RUN** | DEGRADE (fails safe) |
| **C-8** | `switch` to a restore point **already drained past** | Times out — recovery only moves forward. Recovery is to pick a restore point still ahead. | NEW | REFUSE |
| **C-9** | `switch` armed for a restore point production has **not created yet** | Succeeds when the WAL arrives — nodes catch it cleanly instead of overshooting. This is the pattern the promote test uses. | AUTO | PASS |
| **C-10** | Per-segment archive **skew**: one segment lags | The cluster cannot reach a consistent cut until the slowest segment has archived *and* replayed through its N-LSN. `stats` shows the laggard. **The slowest archiver gates everyone.** | NEW | DEGRADE |
| **C-11** | Compare DR as-of-N against a production snapshot taken at N | Row-for-row equality on every table across every segment. The end-to-end correctness assertion the milestone tests only approximate with counts. | NEW | PASS |

---

## Group D — Production maintenance while the DR is attached

The question behind this group: *what does routine production housekeeping do to a DR
replica?*

### D.1 — `VACUUM` / `ANALYZE` (routine)

| ID | Scenario | Expected behaviour & why | Status | Verdict |
|---|---|---|---|---|
| **V-1** | `VACUUM` (lazy) a user table on production while the DR is **paused** | Nothing observable. Prune/freeze/VM records queue in the archive; they apply on the next advance. Data content unchanged (dead tuples are invisible either way). | NEW | PASS |
| **V-2** | Same, but the vacuum's WAL is replayed during an **advance** with a long query running | The prune records carry `latestRemovedXid`; a DR query whose snapshot predates it is cancelled after `max_standby_archive_delay` (snapshot conflict). **This is the single most likely real-world DR query failure.** | NEW | DEGRADE |
| **V-3** | `VACUUM` that **truncates** trailing empty pages of a user table | `XLOG_SMGR_TRUNCATE` replays; any DR query holding a buffer pin / lock on that relation is subject to a lock conflict. Data correct afterwards. | NEW | PASS |
| **V-4** | `ANALYZE` a table on production | `pg_statistic` rows (regular WAL) and `pg_class.reltuples/relpages` (`XLOG_HEAP_INPLACE`) replay onto the DR. **DR plans then match production's** — this is why you do *not* need (and cannot run) ANALYZE on the DR. | NEW | PASS |
| **V-5** | `ANALYZE` on production for a relation whose `pg_class` row shares a page touched by the **frozen seed** | Documented bounded artefact: the seed's `VACUUM FREEZE` of `gp_segment_configuration` does in-place relstats updates on a few `pg_class` pages (`pg_class` is *not* protected). Production changes co-located on those pages inside the `(backup-LSN, seed-LSN]` window are **LSN-shadowed** until the next production full-page image heals them. Impact is planner statistics only — never data correctness. **Verify convergence**: after a checkpoint + FPI on production, the DR's `reltuples` catches up. | NEW `PREDICTED` | DEGRADE |
| **V-6** | Autovacuum running normally on production for hours | DR keeps up; no DR-side vacuum ever runs (no autovacuum launcher below `PM_RUN`), so **the DR never diverges by vacuuming independently** — its heap is a byte-level image of production's. | NEW | PASS |

### D.2 — `VACUUM FULL` / `CLUSTER` / `REINDEX` (rewriting)

| ID | Scenario | Expected behaviour & why | Status | Verdict |
|---|---|---|---|---|
| **V-10** | `VACUUM FULL` a large **user** table on production | Correct on the DR, but with three costs: (1) the whole table is re-WAL'd (`wal_level = replica` forces it) → **archive burst and an RPO spike proportional to table size**; (2) the replayed **AccessExclusiveLock** cancels DR queries touching that table after `max_standby_archive_delay` — [measured](#appendix--c-7--v-10-measured-vacuum-full-under-a-live-reader); (3) transient **2× disk** on the DR while both relfilenodes exist. Schedule inside a paused window and advance afterwards. | NEW | DEGRADE |
| **V-11** | `CLUSTER` a user table | Identical to V-10 — same rewrite mechanics. | NEW | DEGRADE |
| **V-12** | `REINDEX` a user index | Index rebuilt via WAL on the DR; lock conflict for queries using that index during the advance. | NEW | DEGRADE |
| **V-13** | **`VACUUM FULL` / `CLUSTER` / `REINDEX` / `TRUNCATE` on a protected topology catalog** (`gp_segment_configuration`, its indexes/TOAST, `gp_configuration_history`, `gp_id`, `gp_version_at_initdb`) | The DR **halts, deliberately and loudly**: `relmap_redo()` → `DRRejectForbiddenRemap()` raises `FATAL` *before* any on-disk write, so the replay LSN never advances past the offending record. The node's postmaster goes down. **Recovery is to rebuild that DR node from a fresh base backup + re-seed.** There is no preemptive block on production — treat these catalogs as immutable while a DR is attached. This is the sharpest operational contract in the feature and must be demonstrated, not just documented. | NEW | HALT (by design) |
| **V-14** | `VACUUM FULL pg_class` (or any *non*-protected mapped catalog) on production | Replays normally — `DRRejectForbiddenRemap` only rejects remaps of the protected set. | NEW | PASS |

### D.3 — Known gap to confirm

| ID | Scenario | Expected behaviour & why | Status | Verdict |
|---|---|---|---|---|
| **V-20** | Production `VACUUM` **truncates trailing pages of a protected topology catalog**. Reproduced by `src/test/dr/docker-compose.dense.yml` | **Was a real defect; now fixed and verified end to end** ([results](#appendix--v-20-reproduced-and-fixed-end-to-end)): unguarded build → DR left with 0 topology rows; guarded build → 3 rows intact, guard log line on the real record. `XLOG_SMGR_TRUNCATE` is emitted with `XLogRegisterData` only, **no registered buffers** (`storage.c:315-326`), so it has zero block references, and `DRRedoShouldFilter()` deliberately applies any such record (unit-tested as `test_dr_mode_no_blocks_applies`) — correctly, since that is also a commit record's shape. The truncate branch of `smgr_redo` had no protected-relfilenode guard, and the seed never rewrites the relation (`DELETE` → `INSERT`s → `VACUUM FREEZE`, `ggseed_dr_topology:107-110`), so production's record named **the same `RelFileNode`** holding the DR's frozen topology. Confirmed reproducible: against a dense page the seed's rows land on a trailing block a later production truncation would discard (see [V-21 results](#appendix--v-21-measured-on-the-fixture)). **Fixed** by `DRRedoShouldFilterRelFileNode()` (`dr_redo_filter.c:207-217`), called from `smgr_redo` (`storage.c:698-715`), which **skips** the truncation and logs at `LOG`. Skip rather than `FATAL`: an ignored truncation has no lasting consequence, whereas halting would take every DR node down on a routine production autovacuum. Expected behaviour now: the DR keeps its own pages, replay continues, one `LOG` line per skipped truncation. | NEW | PASS (regression) |
| **V-21** | Assert the DR's topology seed took, and where its rows physically landed | Now **wired into `ggdr create-replica` as step 7** (`_check_seeded_topology()`): it reads `gp_segment_configuration` back from the coordinator and requires it to match the topology file on `dbid`/`content`/`port`/`hostname` — a silent seed failure **fails the build**. It also reports the heap size and the block holding the live rows; above block 0 means this node's topology depends on the truncation guard. Verified in both fixtures: the default one reports `1 page(s), live rows on block 0` and stays quiet; the dense one reports `12 page(s), live rows on block 11` and emits the note. First measured manually — see [results](#appendix--v-21-measured-on-the-fixture). | **RUN + automated** | PASS |

---

## Group E — Production DDL and schema change

| ID | Scenario | Expected behaviour & why | Status | Verdict |
|---|---|---|---|---|
| **E-1** | `CREATE TABLE` + load on production | Appears on the DR at the next cut. | AUTO (A-4) | PASS |
| **E-2** | `DROP TABLE` on production while a DR query reads it | AccessExclusiveLock replay cancels the DR query after `max_standby_archive_delay`; afterwards the table is gone from the DR. | NEW | DEGRADE |
| **E-3** | `ALTER TABLE … ADD COLUMN` / rewriting `ALTER TABLE` | Same lock-conflict class as E-2; rewriting variants also carry the V-10 WAL-burst cost. | NEW | DEGRADE |
| **E-4** | `DROP DATABASE` on production while DR sessions are connected to that database | Those DR sessions are **terminated** on replay (database recovery conflict). Expected and correct. | NEW | DEGRADE |
| **E-5** | `CREATE TABLESPACE` on production with a path that does not exist on the DR host | Replay fails. Physical replication needs the tablespace path to exist on every DR host. Documented contract; demonstrate the failure mode so operators recognise it. | NEW `PREDICTED` | HALT |
| **E-6** | `CREATE EXTENSION` on production | Catalog entries replay; the extension's `.so` must also be installed on the DR hosts or queries using it fail at load. | NEW | PASS w/ prerequisite |

---

## Group F — Production HA events (the switchover-to-mirror question)

This is the group the current fixture does not cover at all, and where the sharpest
open question lives.

**Prerequisite for the whole group:** production **mirrors must archive too.** With
`archive_mode = on` a standby does not archive; a promoted mirror starts archiving only
once it becomes primary. Its `%c` content id is unchanged, so it writes to the same
`wal/seg<content>/` directory the DR already reads. Reliable archiving of the
`.history` file is equally required.

| ID | Scenario | Expected behaviour & why | Status | Verdict |
|---|---|---|---|---|
| **HA-1** | **Production segment fails over to its mirror** (FTS promotes; that content's timeline goes 1 → 2) while the DR is attached | **Failure, confirmed by code reading** (chain traced end to end — see [HA-1 analysis](#appendix--ha-1-verified-against-the-code) below). The DR node for that content replays every TLI-1 segment the *old* primary managed to archive, then requests the next TLI-1 segment — which either was never archived (the old primary died mid-segment) or exists only as `….partial`. `restore_command` misses, and because the timeline goal is `CONTROLFILE` the node **never rescans for `00000002.history`** and never switches to TLI 2 — even though the TLI-2 segments are sitting in the same archive directory it is already reading. It retries the same missing filename every `wal_retrieve_retry_interval` **forever**. The failed restore logs at **DEBUG2** — invisible at default `log_min_messages` — so the node is **indistinguishable from a healthy DR whose production is simply idle**. **Verify at runtime, then decide:** `recovery_target_timeline = 'latest'` (HA-2), or document mirror failover as "rebuild that DR node". | NEW | **GAP** |
| **HA-1b** | **Detecting** the HA-1 stall from the shipped observability | **Second gap: you cannot.** `rpo_seconds` grows — but it grows identically when production is idle, because it is `now() - min(pg_last_xact_replay_timestamp())`. `replay_lsn` freezes — same ambiguity. **Neither `gg_stat_dr_replica` nor `gg_stat_dr_replica_summary` exposes a timeline id**, and `pg_last_wal_replay_lsn()` does not reveal one either; you have to reach `pg_control_checkpoint()` or the archive directory listing to see that production has forked onto TLI 2 while the DR is stuck on TLI 1. Consider adding a `timeline` column to `gg_stat_dr_replica` regardless of how HA-1 is resolved. | NEW | **GAP** |
| **HA-2** | Same as HA-1 with `recovery_target_timeline = 'latest'` and the `.history` file archived | Should work, and the archive-only case is genuinely reachable: the WAL-source state machine advances to `XLOG_FROM_STREAM` **even with no `primary_conninfo`** ("Move to XLOG_FROM_STREAM state in either case… immediate failure if we didn't launch walreceiver", `xlog.c:13337`), which is where the rescan hook lives. So a slot-less archive-only DR does reach `rescanLatestTimeLine()`, reads `00000002.history` via `restore_command`, and follows the fork. Note `recovery_target_timeline` is **`PGC_POSTMASTER`** — flipping it costs a restart of each DR node, though a restart *after* a fork still recovers (startup runs `findNewestTimeLine()`, `xlog.c:5693`). **Also check the last pre-fork segment:** the old timeline's final partial segment is archived as `….partial`, which a plain `cp`-based `restore_command` will not find. Expect either clean continuation or a stall precisely at the fork LSN — determine which, and whether `restore_command` needs `.partial` handling. | NEW `PREDICTED` | to determine |
| **HA-3** | The failover's `gp_segment_configuration` churn (role `p`↔`m` swap, mode/status updates, `gp_configuration_history` inserts) reaching the DR | **Filtered.** All those records reference only protected blocks, so the DR's topology stays DR-local through a production HA event. This half of the scenario should pass even when HA-1 fails — worth asserting separately so the two failure modes don't mask each other. | NEW | PASS |
| **HA-4** | `gprecoverseg` rebuilds the old primary as a mirror; later `gprecoverseg -r` rebalances back | The rebalance is a **second promotion** → another timeline increment for that content. Different contents legitimately sit on different timelines — harmless, because each content has its own independent WAL/LSN space. But it multiplies HA-1's exposure. | NEW | see HA-1/HA-2 |
| **HA-5** | Production **coordinator** activated from its standby coordinator (`gpactivatestandby`) | Content `-1` forks its timeline — the HA-1/HA-2 analysis applies to the DR coordinator. The standby coordinator must also be archiving. | NEW `PREDICTED` | see HA-1 |
| **HA-6** | `gpexpand` adds segments to production | The DR is **invalidated** — the content count no longer matches and the expansion redistributes data cluster-wide. Expect: rebuild the DR from scratch after the expansion completes. Demonstrate that it fails clearly rather than serving wrong results. | NEW | REFUSE / rebuild |
| **HA-7** | A production segment crashes and restarts (no failover) | No timeline change; the DR simply continues. Baseline control for HA-1 — isolates "crash" from "failover". | NEW | PASS |

---

## Group G — Archive and transport failures

| ID | Scenario | Expected behaviour & why | Status | Verdict |
|---|---|---|---|---|
| **G-1** | Archive becomes unreachable (unmount / network partition) | DR stops advancing; `restore_command` retries; already-served reads are unaffected while paused. `rpo_seconds` grows. Recovers on its own when the archive returns. | NEW | DEGRADE |
| **G-2** | A WAL segment is **missing** from the archive (retention deleted it, or archiving failed) | That node stalls permanently at the gap. There is no cross-WAN `pg_rewind` — **recovery is a full re-`create-replica` of that node.** Demonstrate the detection path (stall + growing RPO) and the rebuild. | NEW | HALT → rebuild |
| **G-3** | A WAL segment in the archive is **corrupt** | Replay raises a CRC / invalid-record error and stops at that LSN. Same rebuild recovery. Demonstrate the DR does not silently skip it. | NEW | HALT |
| **G-4** | Production `archive_command` fails for a while, then succeeds | Production's `pg_wal` grows (that is the point of archive transport with **no replication slot** — the DR can never pin production WAL); the DR catches up once archiving resumes. | NEW | PASS |
| **G-5** | DR is offline for a long window, then restarted with the archive intact | Catches up from where it stopped. No production-side impact whatsoever — the key operational advantage over a slot-based standby on PG 12 (no `max_slot_wal_keep_size`). | NEW | PASS |
| **G-6** | Object-store archive with eventual consistency (a segment briefly not visible) | `restore_command` retries; the DR tolerates it. Worth an explicit scenario because it is the most likely production deployment. | NEW (MANUAL) | PASS |

---

## Group H — DR-side failures and restarts

| ID | Scenario | Expected behaviour & why | Status | Verdict |
|---|---|---|---|---|
| **H-1** | Kill (`SIGKILL`) a DR node and restart it | Crash recovery re-applies from the last checkpoint; **the redo filter re-applies identically**, so the DR-local topology survives. Verify `gp_segment_configuration` still reads `dr` and the node returns to its pause target. | NEW | PASS |
| **H-2** | Restart the whole DR cluster | Comes back in recovery, still read-only, `hot_standby` still on, pause target preserved (it lives in `postgresql.conf` / `postgresql.auto.conf`). | NEW | PASS |
| **H-3** | Start a node with `hot_standby = off` in recovery | `IsDRReplicaMode()` is false, so it is an ordinary (unqueryable) standby: no topology filter, no read-only enforcement. This is what keeps mirrors and the standby coordinator unaffected by the fold. | NEW | PASS |
| **H-4** | **Footgun check:** set `hot_standby = on` on an in-cluster **mirror**, let production change topology, then fail over to it | The mirror's `gp_segment_configuration` is frozen by the topology filter, so after FTS promotes it the cluster describes a stale topology. This is a documented misconfiguration, not a supported mode — the scenario exists to show the damage is real and to keep the warning honest. | NEW | documented footgun |
| **H-5** | A DR **segment** dies while the coordinator serves | The distributed query fails (no DR-local mirrors, no DR-local FTS — a DR node never leaves `PM_HOT_STANDBY`). **Mirrorless DR is a deliberate PoC scope decision**; the recovery is to rebuild that node. Demonstrate the failure is clean and diagnosable. | NEW | DEGRADE |
| **H-6** | FTS / autovacuum on the DR | Neither ever runs — both are gated on `PM_RUN`, which a DR node never reaches. Assert no probe writes to `gp_segment_configuration` and no vacuum activity over a long run. Structural, not guarded — worth asserting so a future postmaster change cannot silently break it. | NEW | PASS |
| **H-7** | Disk full on a DR node during an advance | Replay stops with a clear error; reads at the last cut continue where possible. | NEW | DEGRADE |

---

## Group I — Promotion and switchover

| ID | Scenario | Expected behaviour & why | Status | Verdict |
|---|---|---|---|---|
| **P-1** | `switch <rp>` to a consistent cut, then promote there | `pg_promote(false)` + `pg_wal_replay_resume()` on **segments first, then the coordinator**, each polled out of recovery — recovery ends exactly after N. **No second phase and no restart:** DR behaviour is keyed on "in recovery with hot standby", so it lifts with recovery itself. | AUTO | PASS |
| **P-2** | After promotion: `gg_stat_dr_replica.dr_replica = f` **without any restart**, all nodes out of recovery, distributed **writes** succeed | `IsDRReplicaMode()` false → the DR gate, the standby-reader context and the redo filter all disengage; FTS and DTX run normally. Poll for the first FTS cycle before asserting writes. | AUTO | PASS |
| **P-3** | After promotion: data equals production **as of the promote cut** | The promoted cluster is a real point-in-time copy — verify per-table row counts *and* content, not just counts. | AUTO (commented) | PASS |
| **P-4** | `promote` when nodes are **not all paused at one restore point** (e.g. after `pause`, or mid-advance) | Refused by the precondition guard. `pause` is not a consistent cut and must not be promotable. | NEW | REFUSE |
| **P-5** | `promote --at <rp>` where `<rp>` ≠ the cut the cluster is actually at | Refused. | NEW | REFUSE |
| **P-6** | **Promote exactly at a straddle cut** (the C-3 fault-injected state) | The highest-value untested case. In-doubt prepared transactions exist on segments at the cut; DTX recovery on the promoted coordinator must resolve them **atomically** — committed everywhere (its `DISTRIBUTED_COMMIT` was replayed) or aborted everywhere. Assert the post-promotion row count is *exactly* 10 or *exactly* 30, never a torn value, and that no orphaned prepared transactions remain (`pg_prepared_xacts` empty on every segment). | NEW | PASS (to prove) |
| **P-7** | `gg_dr_promote('wrong_rp')` — a restore point the cluster is not at | Refused with the point it *is* at. Guards against promoting the wrong cut by typo. | AUTO | REFUSE |
| **P-8** | Attempt to resume DR mode after promotion | Not possible — recovery only moves forward. Promotion is irreversible; assert the utility says so rather than half-working. | NEW | REFUSE |
| **P-9** | Planned switchover drill, end to end: quiesce production → `gp_create_restore_point('cutover')` → `ggdr switch cutover` → `promote --at cutover` → redirect clients | Zero data loss (production quiesced before the restore point). The full operational rehearsal. | NEW (MANUAL) | PASS |
| **P-10** | Unplanned failover: production lost mid-flight, segments archived unevenly | Promote from the **latest verified common restore point** (`ggdr stat` → `consistent serve point`). Computing that automatically when some segments archived past N and others did not **is not implemented** — the scenario documents the manual procedure and its RPO. | NEW (MANUAL) | DEGRADE |

---

## Group J — Observability, and what operators must be able to see

| ID | Scenario | Expected behaviour & why | Status | Verdict |
|---|---|---|---|---|
| **O-1** | `gg_stat_dr_replica` with all nodes paused at one restore point | One row per node; all report `dr_replica = t`, `in_recovery = t`, and the same `restore_point`. | AUTO | PASS |
| **O-2** | `gg_stat_dr_replica_summary` rollup | `consistent_restore_point` non-NULL **only** when every node is paused at the same point; `rpo_seconds` finite and non-negative. | AUTO | PASS |
| **O-3** | **Served-N is faithful.** Re-point `gp_pause_on_restore_point_replay` to a name never reached, *without* resuming | The view still reports the **actually-reached** restore point — it reads `XLogCtlData.pausedRestorePointName` captured from the replayed WAL record, not the configured GUC. A GUC-based implementation would lie here. | AUTO | PASS |
| **O-4** | Summary while one node lags (mixed restore points) | `consistent_restore_point` is `NULL`, `all_paused = false`. The signal an operator must trust before serving a report. | NEW | PASS |
| **O-5** | No cross-node LSN comparison is offered anywhere | Deliberate: each node has its own WAL/LSN space, so LSNs are not comparable across nodes. Assert that no view or tool invites a `min(replay_lsn)` "cut" — that approximation is exactly what the straddle breaks. | NEW | PASS |
| **O-6** | `rpo_seconds` tracks reality during a controlled production stall | Grows with the stall, shrinks on catch-up. Validates the number operators will alert on. | NEW | PASS |

---

## Group K — `ggdr` and `gg_walfilter` tooling

| ID | Scenario | Expected behaviour | Status |
|---|---|---|---|
| **U-1** | `ggdr stat` reports the consistent serve point | AUTO | PASS |
| **U-2** | `ggdr switch <rp>` — all nodes advance and pause at `<rp>`; new data visible | AUTO | PASS |
| **U-3** | `gg_dr_switch()` / `gg_dr_promote()` — the SQL equivalents, dispatched cluster-wide from the coordinator | Same effect as the utility's `switch` / `promote`, reachable by a client that can only connect to the coordinator. | AUTO | PASS |
| **U-4** | `ggdr pause` — every node halts immediately; summary shows no consistent point | NEW | PASS |
| **U-5** | `ggdr` against a **multi-host** DR | **Not supported in the PoC** — it reaches every node by local socket. Document the failure and the per-host workaround. | NEW (MANUAL) | DEGRADE |
| **U-6** | `gg_walfilter` black-box suite (`src/test/dr/test_gg_walfilter.py`) against the installed binary | AUTO (gates the DR build) | PASS |
| **U-7** | `gg_walfilter inspect --gp-dr-topology <segment>` on a real archived segment containing topology changes | Reports exactly the records the in-backend filter would skip — **both** rules: all-blocks-match (`DRRedoShouldFilter`) and the `XLOG_SMGR_TRUNCATE` payload target (`DRRedoShouldFilterRelFileNode`). Verified on the dense fixture's own archived WAL: the aligned tool flags `Storage` at the exact LSN the engine's guard logged, one more record than the pre-alignment binary. Covered by 7 cases in `test_gg_walfilter.py` (40/40), incl. `SMGR_CREATE` must *not* be filtered — the engine guards only the truncate branch. | AUTO | PASS |
| **U-8** | `gg_walfilter restore` used as the `restore_command` (opt-in out-of-engine filtering, ADR-0002 path) | Filtered segments install atomically; the DR reaches the same state as with the in-backend filter. Keeps the alternative path from bit-rotting. | NEW | PASS |

---

## Priority: what to run first

1. **HA-1 / HA-1b / HA-2** — production mirror switchover. The stall is **confirmed by code
   reading** (see the appendix); what needs a runtime run is its exact shape and the
   `….partial` question. This is the worst failure shape there is — no error, no log line at
   default verbosity, and no shipped view that can distinguish it from an idle production —
   and it decides whether the feature survives contact with a normal production HA event.
2. **V-20** — the `XLOG_SMGR_TRUNCATE` hole in the topology filter. **Closed, fixed, and
   verified end to end.** The dense fixture reproduces it (unguarded build: DR left with 0
   topology rows after a routine production `VACUUM`) and confirms the guard
   (`storage.c:698-715`) stops it. Unit tests 17/17, V-21 now runs automatically at the end of
   every `create-replica`, and `gg_walfilter` has been aligned with the engine's second rule
   (40/40). What remains is putting the dense fixture into CI.
3. **P-1 / P-2 / P-3** — re-enable the promotion block that is currently commented out in
   `dr-entrypoint.sh:378-436`. Promotion is the whole point of a DR replica and is presently
   not asserted by CI.
4. **P-6** — promote at a straddle cut. The in-recovery straddle is proven; the
   *promotion-time* resolution of those in-doubt transactions is not.
5. **V-13** — the protected-catalog `VACUUM FULL` halt. It is the sharpest operator-facing
   contract; demonstrating the `FATAL` (and the rebuild) is how it becomes real.
6. **Q-26 / Q-28 / Q-33** — temp tables, role management, and `COPY … TO`. These decide
   whether real reporting workloads can run on the DR unmodified, and the answers belong in
   user-facing documentation, not in a code reading.
7. **C-5 / V-2** — recovery-conflict cancellation during an advance. Now confined to advance
   bursts rather than being the steady state, but still what a long report will hit.

---

## Appendix — HA-1 verified against the code

The prediction that a production mirror failover silently stalls the DR was traced link by
link. Each step is a direct code reference, not an inference:

1. **The DR is armed with `'current'`, unconditionally.** `cmd_create_replica` writes
   `recovery_target_timeline = 'current'` into every node's `postgresql.conf`
   (`gpMgmt/bin/ggdr:586`). There is no CLI flag to override it.
2. **`'current'` means "do not follow forks".** `check_recovery_target_timeline()` maps the
   string to `RECOVERY_TARGET_TIMELINE_CONTROLFILE` (`guc.c:12212-12213`).
3. **Startup pins the TLI from `pg_control`.** `findNewestTimeLine()` is called only for
   `RECOVERY_TARGET_TIMELINE_LATEST` (`xlog.c:5690-5693`); the `CONTROLFILE` branch just
   keeps `recoveryTargetTLI` as read from the control file (`xlog.c:5695-5702`).
4. **The only runtime rescan is gated on `LATEST`.** `rescanLatestTimeLine()` has exactly one
   call site in the tree, inside `WaitForWALToBecomeAvailable()`, wrapped in
   `if (recoveryTargetTimeLineGoal == RECOVERY_TARGET_TIMELINE_LATEST)`
   (`xlog.c:13386-13392`). With `CONTROLFILE` it never runs, so `00000002.history` is never
   read and the node never learns TLI 2 exists.
5. **Failure to fetch is not an error.** `RestoreArchivedFile()` reports a non-signal
   command failure at **DEBUG2** (`xlogarchive.c:320-322`) and falls through to
   `not_available`. At default `log_min_messages = warning` the server log shows nothing
   from the backend — only the raw `cp: cannot stat …` on stderr, which is exactly what a
   healthy DR at the end of the archive also produces.
6. **The loop is infinite.** After the archive and `pg_wal` both miss, the state machine
   advances to `XLOG_FROM_STREAM`, fails immediately (no walreceiver), skips the rescan,
   sleeps `wal_retrieve_retry_interval`, and loops back to the archive for the same
   now-impossible TLI-1 filename (`xlog.c:13337-13420`).
7. **GP mirror promotion really does fork the timeline.** FTS's promote handler calls
   `SignalPromote()` for a mirror in `DB_IN_ARCHIVE_RECOVERY`
   (`ftsmessagehandler.c:389-403`), which takes the standard promotion path:
   `ThisTimeLineID = findNewestTimeLine(recoveryTargetTLI) + 1` plus a new history file
   (`xlog.c:8048-8050`).

What remains genuinely unverified is only the *runtime* shape: exactly which segment the DR
stalls on (it depends on how much the dying primary had archived), and whether the promoted
mirror's `….partial` segment leaves a data gap even once HA-2's `'latest'` is in place.
That is what the HA-1/HA-2 test run has to establish.

---

## Appendix — V-20 verified against the code

The mechanism is confirmed; only the blast radius depends on runtime state.

1. **The record carries no block references.** `RelationTruncate()` builds `xl_smgr_truncate`
   and calls `XLogBeginInsert()` → `XLogRegisterData()` → `XLogInsert()` with **no
   `XLogRegisterBuffer()` anywhere** (`storage.c:310-326`). The rnode travels in the record
   payload, not as a block tag.
2. **The filter therefore applies it — by design.** `DRRedoShouldFilter()` tracks
   `any_block` and returns it, so a record with zero block refs yields `false` = apply
   (`dr_redo_filter.c:196-225`). This is deliberate and unit-tested:
   `test_dr_mode_no_blocks_applies` in `dr_redo_filter_test.c:191-204` asserts it explicitly.
   **The predicate is not the bug** — a commit record must be applied. The gap is that this
   record class needed its own guard downstream.
3. **`smgr_redo` has no guard.** The `XLOG_SMGR_TRUNCATE` branch goes
   `smgropen(xlrec->rnode)` → `smgrcreate()` → `XLogFlush()` → `smgrtruncate(reln,
   MAIN_FORKNUM, xlrec->blkno)` → `XLogTruncateRelation()` (`storage.c:691-732`), with no
   `DRRelfilenodeIsProtected()` check. Grepping the tree confirms that helper is referenced
   only from `dr_redo_filter.c` and its unit test — `relmap_redo` is the only redo path that
   was hardened.
4. **The DR shares production's relfilenode for this relation.** `ggseed_dr_topology` runs
   `DELETE FROM gp_segment_configuration` → the generated `INSERT`s → `VACUUM FREEZE` →
   `CHECKPOINT` (lines 107-110). No `VACUUM FULL`, no `CLUSTER`, no `TRUNCATE` — nothing that
   allocates a new relfilenode. So the rnode in production's truncate record names the very
   file holding the DR's frozen topology.
5. **The truncation threshold is trivially low.** `should_attempt_truncation()` fires when
   `possibly_freeable > 0 && (possibly_freeable >= REL_TRUNCATE_MINIMUM ||
   possibly_freeable >= rel_pages / REL_TRUNCATE_FRACTION)` with `REL_TRUNCATE_FRACTION = 16`
   (`vacuumlazy.c:85-86, 1866-1875`). For a small relation `rel_pages / 16` is **0**, so
   **any single trailing empty page is enough** — the 1000-page minimum is an alternative
   branch, not a floor. `gp_segment_configuration` is a shared catalog, so ordinary
   autovacuum in any database — and anti-wraparound vacuum in particular — will process it.
6. **How dense the catalog must be, measured rather than assumed.** Greengage builds with a
   **32 KB block size**, not 8 KB, so a page holds far more than a first estimate suggests:
   a scratch table of the same rowtype took **390 rows on block 0** with compact datadir
   strings, and the fixture's own rows are ~132 bytes each (≈240/page). So the catalog stays
   single-page until a few hundred *tuple versions* accumulate — a very large cluster, or any
   cluster with enough FTS churn between vacuums. Not every deployment, but not exotic
   either.

**What the test had to decide** — where the seed's `INSERT`s physically land — is now
measured; see the V-21 appendix below.

---

## Appendix — V-21 measured on the fixture

Run against `src/test/dr/docker-compose.yml` (production: coordinator + 2 segments, no
mirrors; DR built by `ggdr create-replica`). The rest of the suite passed 28/28 on
the same run, so the DR was in its normal, healthy state when measured.

**Same relfilenode — confirmed in a live system, not just from reading the seed script:**

| | relfilenode | pages | block_size | live rows |
|---|---|---|---|---|
| production | 5036 | 1 | 32768 | 3 |
| DR | 5036 | 1 | 32768 | 3 |

Production's truncate record would name exactly the file holding the DR's topology.

**This fixture is in the benign branch.** The DR's live frozen rows landed on **block 0**,
appended after production's now-dead tuples:

```
 ctid  | blk | dbid | content | hostname        production, for comparison:
-------+-----+------+---------+----------        (0,1) dbid 1  primary
 (0,4) |   0 |    1 |      -1 | dr               (0,3) dbid 3  primary
 (0,5) |   0 |    2 |       0 | dr               (0,4) dbid 2  prod-seg0-CHANGED
 (0,6) |   0 |    3 |       1 | dr
```

`max_live_blk = 0`, DR relation = 1 page = production's 1 page. Production can never
truncate below one page while its own live rows sit there, so **nothing here can be
truncated away**. V-20 cannot fire on a cluster this small.

**But the fixture is the easy case by construction**, and that is the important caveat: 3
rows in a page with ~390-row capacity means the seed had abundant free space and never had
to extend the relation. So the run does *not* clear V-20 — it only shows the fixture is not
exposed.

**The dangerous branch, constructed directly.** Replaying the seed's exact DML pattern
(`DELETE` all → `INSERT` N → `VACUUM FREEZE`) against a **full** page 0, on a scratch table
of the same rowtype:

| step | result |
|---|---|
| 390 rows loaded | relation = **1 page** |
| `DELETE FROM …` (all 390) | — |
| `INSERT` 3 rows | relation = **2 pages** — the inserts **extended** rather than reusing page 0 |
| `VACUUM FREEZE` | still **2 pages**; live rows at `(1,1) (1,2) (1,3)` — **all on block 1** |

`VACUUM FREEZE` does not bring them back: block 0 is empty but not *trailing*, so there is
nothing to truncate, and vacuum never moves tuples between pages.

That completes a concrete data-loss chain for a large cluster: production's catalog is dense
(so the seed's rows land on a trailing new block) → production's FTS churn is later vacuumed
away, emptying its own trailing pages → production truncates → the DR replays the unguarded
`XLOG_SMGR_TRUNCATE` against the shared relfilenode → **the block holding the DR's entire
DR-local topology is discarded**.

**Verdict:** V-20 was a real defect with a demonstrated trigger, not a theoretical one — but
**size-dependent**, which is why the default fixture shows nothing. It is now **fixed and
verified end to end**; see the next appendix.

---

## Appendix — V-20 reproduced and fixed, end to end

`src/test/dr/docker-compose.dense.yml` + `scripts/dense-{primary,dr}-entrypoint.sh` build the
state the default fixture cannot reach, and were run against an unguarded and a guarded build
of the same tree.

Both arms were re-run on a **from-scratch image build of the working tree** (clean
git-derived context, so no host-built objects leak in — the host was configured with
different flags), not a hand-patched binary. On that image the default fixture also passes
28/28 and the `gg_walfilter` suite passes, so the guard costs nothing elsewhere. The
unguarded arm is preserved as `greengage-dr-test:v20-unguarded` to keep the comparison
reproducible.

**How the dense state is built.** Production inserts 5000 filler rows into
`gp_segment_configuration` and deletes them **in one transaction**, so they are dead the
moment they become visible — no other backend, FTS included, ever sees a phantom segment —
but they still occupy their pages, and `DELETE` does not update the free-space map. The base
backup is taken in that state, with `autovacuum = off` so vacuum timing stays the
experiment's alone. The DR inherits full pages and an FSM that reports them full, so the
seed's `INSERT`s extend the relation instead of reusing block 0. Afterwards production
vacuums, the fillers go, the trailing pages empty, and the relation truncates.

Measured identically on both runs:

| | |
|---|---|
| production catalog before densify | 1 page |
| after 5000 filler rows inserted + deleted in one txn | **12 pages**, 3 live rows still on block 0 |
| production still dispatches distributed queries | yes |
| DR catalog after `create-replica` | 12 pages, 3 live rows, **highest live block 11** |
| production after `VACUUM` | **1 page** — `XLOG_SMGR_TRUNCATE … to 1 block` emitted |

The DR's entire topology therefore sits 10 blocks above production's truncation target.

**Arm A — unguarded build (`greengage-dr-test:latest`):**

```
DR catalog now: 0 live row(s), 1 page(s), highest live block -1
FAIL  topology DESTROYED: 0 live row(s) left (was 3 on block 11); DR relation 1 page(s)
V-20 VERDICT: TOPOLOGY DESTROYED -- production's truncation discarded the DR's own rows
```

A routine production `VACUUM` erased the DR cluster's identity. This is the defect, observed
through real archived WAL rather than inferred.

**Arm B — guarded build (`greengage-dr-test:v20fix`, same tree + the `smgr_redo` guard):**

```
DR catalog now: 3 live row(s), 12 page(s), highest live block 11
PASS  topology SURVIVED: 3 rows intact, DR still 12 page(s) while production is 1
PASS  smgr_redo guard fired on the real record (1 log line(s))
  guard: skipped truncation of protected topology catalog (relfilenode 5036) to 1 block(s)
         WAL redo at 0/1800A6E8 for Storage/TRUNCATE: global/5036 to 1 blocks flags 7
V-20 VERDICT: TOPOLOGY SURVIVED -- production's truncation was not applied to the DR
dense fixture: 5 passed, 0 failed
```

The log line carries the record it intercepted, so there is no ambiguity about *what* was
skipped. And the replica is not merely retaining rows — it is still fully operational:

```
   ctid   | dbid | content | role | hostname | port        select gp_segment_id, count(*)
----------+------+---------+------+----------+------         from dense_marker group by 1;
 (11,276) |    1 |      -1 | p    | dr       | 7000       -> gp_segment_id 1 | count 1
 (11,277) |    2 |       0 | p    | dr       | 7002
 (11,278) |    3 |       1 | p    | dr       | 7003
```

DR-local hostnames intact on block 11, and distributed dispatch still works.

**The fixture fails closed.** If the catalog is not dense enough and the seed's rows land on
block 0, the precondition check aborts rather than passing — an inconclusive run cannot
masquerade as a green one, which is the failure mode that would make this regression test
worthless as the code evolves.

**`gg_walfilter` has been aligned** with the engine's second rule — see U-7 below. Verified
against the very segment this run produced: on the archived coordinator WAL containing the
truncation, the pre-alignment binary reports `58 record(s), 48 would be rewritten` and lists
no `Storage` record, while the aligned binary reports `49` plus

```
FILTER 0/1800A6E0 Storage  info=0x21 len=46 xid=1078  1664/0/5036 truncate to 1 blk (payload)
```

— the same LSN and relation the engine's guard logged. Exactly one record's difference, and
it is the right one.

**Still outstanding**

- The dense fixture is **not wired into CI**; it is run on demand against two images.

---

## Appendix — C-7 / V-10 measured: `VACUUM FULL` under a live reader

The question this answers: a DR paused at `rp1` has an open transaction reading a heap
table; production `VACUUM FULL`s that table and creates `rp2`; the DR is then switched to
`rp2`. Does the reader fail, or does it silently observe a state that exists at neither
restore point?

**Setup.** `test_h`, 100 000 rows, on a DR paused at `rp1`. Production then
`DELETE`s 10 000 rows, `VACUUM FULL`s the table (relfilenode 16394 → **16397**, a full
rewrite) and creates `rp2`. So `rp1` = 100 000 rows, `rp2` = 90 000 rows, and the two cuts
are distinguishable. `max_standby_archive_delay` is the default 30s.

**Result — the reader is cancelled; it is never shown a torn view.**

```
begin;
select count(*) as first_read from test_h;     ->  100000        (rp1, as expected)
select pg_sleep(75);
    ERROR:  canceling statement due to conflict with recovery
    DETAIL:  User was holding a relation lock for too long.
select count(*) as second_read from test_h;
    ERROR:  current transaction is aborted, commands ignored until end of transaction block
commit;                                        ->  ROLLBACK
select count(*) as after_commit from test_h;   ->  90000         (rp2, new transaction)
```

The first `SELECT` takes `AccessShareLock` on `test_h` and holds it for the transaction.
Replaying production's rewrite requires `AccessExclusiveLock`, so the startup process waits
`max_standby_archive_delay` and then cancels the holder. The transaction aborts as a unit —
the second read never runs, so there is no way to observe half of `rp1` and half of `rp2`
inside one transaction. Only after `ROLLBACK`, in a *new* transaction, does the session see
`rp2`.

**The reader also delays the whole cluster.** The same `gg_dr_switch('rp2')` took:

| | wall time |
|---|---|
| with the live reader holding its lock | **31s** (= `max_standby_archive_delay` + change) |
| with no long-held lock (fresh short queries only) | **1s** |

That is the operationally important half of the finding: one long-running report does not
merely risk itself, it stalls every other consumer's advance by up to
`max_standby_archive_delay` — and with `max_standby_archive_delay = -1` it would stall the
advance indefinitely.

**What this run does *not* establish.** A second run polled `test_h` with short queries
throughout the advance and saw only `100000` then `90000` — no intermediate value, no
errors. That is weak evidence, not proof: the advance lasted ~1s, and in this workload the
intermediate cut (after the `DELETE`, before the rewrite) happens to have the same row count
as `rp2`. The general exposure stands and is untested: **nothing prevents queries while an
advance is in flight**, and mid-advance the cluster is at an arbitrary LSN, not a restore
point — precisely the position §D2 says carries no cross-node guarantee. Demonstrating a
genuinely torn mid-advance read needs a distributed transaction straddling the two restore
points, i.e. the C-3 fault-injection machinery pointed at an advance window rather than at a
cut. Until that is run, "do not query during an advance" is a convention, not something the
implementation enforces.

---

## Summary: the supported surface in one paragraph

A DR replica serves **read-only distributed SELECTs** — joins, aggregates, window
functions, motions, cursors, prepared statements, spilling sorts — against a
transactionally consistent as-of-N image, at READ COMMITTED or REPEATABLE READ. It
refuses **every** write path (DML, DDL, 2PC, sequences, `SELECT … FOR UPDATE`,
data-modifying CTEs, CTAS, `REFRESH MATVIEW`), and also refuses three things that
otherwise look read-ish: **temp tables**, **role/privilege changes**, and **SERIALIZABLE**.
Statistics and configuration are inherited from production and cannot be tuned locally.
Consistency and freshness are opposed knobs: paused at a restore point you get a stable,
conflict-free window, and that is the only serving state there is — free-running was removed
because no other cut has a cross-node guarantee to offer. Advancing between windows inherits
standard hot-standby cancellation. Production's routine `VACUUM`/`ANALYZE` are safe; rewriting maintenance
(`VACUUM FULL`, `CLUSTER`, `REINDEX`) is safe for user tables but expensive in WAL and
lock conflicts — and **fatal for the DR if aimed at a topology catalog**. Production HA
events that fork a timeline are the least-tested boundary and the first thing to verify.
