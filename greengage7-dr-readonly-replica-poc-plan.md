# Greengage 7 — Read‑Only Disaster‑Recovery Cluster Replica
## Analysis & PoC Development Plan  *(rev. 2 — transport & topology decisions locked)*

*Target: Greengage 7.x (PostgreSQL 12 base, GPORCA, "coordinator/segment" terminology). Scope: a second, independent Greengage cluster — its own coordinator and primary segments — kept up to date from a production cluster, serving read‑only queries, and promotable on switchover.*

> **Decisions locked in this revision (were open in rev. 1):**
> 1. **Transport = WAL archiving / continuous restore (PITR model), NOT live streaming.** Rationale in §0.1. This matches the proven GPDR design and is the right fit for a geo‑remote DR on a high‑WAL production system that already streams to local mirrors.
> 2. **Topology independence = apply‑time redo filter inside the engine, NOT a WAL‑stream proxy and NOT an offline WAL rewriter.** Rationale in §5.1. A standalone WAL tool, if built, is repositioned as an *archive‑management/validation* utility (§5.8), not the topology filter.
> 3. A new **§10** reviews the problems this specific combination (archive + redo filter) introduces, for discussion before development starts.

---

## 0. TL;DR / orientation

**This feature already exists as a shipping product**: VMware/Broadcom built it for Greenplum 7 as **Greenplum Disaster Recovery (GPDR) "Read Replica Mode"** (Read Replica needs GP **7.3.0+**) — a *recovery cluster* doing **continuous WAL restore** that serves read‑only queries, built on a `gp_pitr` extension + the `gpdr` utility. We are building an **open‑source equivalent**. The foundational primitives are already in Greengage 7 (`gp_create_restore_point()`, WAL‑logged distributed commits, hot‑standby, `wal_level=replica`); the orchestration/control layer is the gap.

The closest conceptual analog outside the Greenplum family is **Oracle Active Data Guard** (a physical standby open read‑only *while* redo apply continues, kept transactionally consistent). The two MPP‑specific problems beyond single‑node Active Data Guard are **(A) cluster‑topology independence** (your #1) and **(B) a cluster‑wide consistent snapshot across segments that replay independently** (your #4). Both have code‑grounded solutions below.

### 0.1 Why archive transport (the locked decision)
Live per‑segment streaming to a geo‑remote DR was rejected for three reinforcing reasons:
- **Production self‑protection.** Streaming needs a replication slot per remote consumer; a slot retains WAL on production until acked. Over a WAN, with very large WAL volume, a lagging/disconnected DR would pin WAL in production's `pg_wal` and can fill the disk — and **PG12 has no `max_slot_wal_keep_size`** to bound it. Archiving has **no slot**: production writes each completed segment to the archive and is done; retention moves to the archive repository (pruned independently). *The single largest production‑safety risk from rev. 1 is retired by this decision.*
- **No egress doubling.** Each primary already runs a walsender to its **local mirror**. A remote stream would add a second walsender + slot per primary, governed by WAN‑lagged acks. Archiving adds only the `archive_command` write per segment (often already running for backup/PITR).
- **Latency tolerance.** Archive shipping is store‑and‑forward; it tolerates WAN latency and intermittent connectivity by design. Streaming does not.

A welcome side effect: **archive transport simplifies consistency (your #4)** — continuous restore to named *distributed restore points* makes the restore points themselves the consistency barriers, the exact GPDR model.

---

## 1. Relevant Greengage internals (grounding; verified on the `7.x` branch)

File paths are given so the team can jump straight in.

### 1.1 Cluster topology: `gp_segment_configuration`
- `src/include/catalog/gp_segment_configuration.h`. A **`BKI_SHARED_RELATION`** (OID **5036**) — a *shared* catalog in the **global** tablespace, present on coordinator + every segment, replicated **block‑for‑block by physical WAL**. It has **two indexes (OIDs 7139, 7140)** and a **TOAST relation (6092/6093)** (`indexing.h`, `toasting.h`).
- Columns: `dbid, content, role('p'/'m'), preferred_role, mode, status, port, hostname, address, datadir`.
- Read two ways (`src/backend/cdb/cdbutil.c`): `readGpSegConfigFromCatalog()` (in‑transaction) and **`readGpSegConfigFromFTSFiles()`** — a flat dump file (`GPSEGCONFIGDUMPFILE`) FTS maintains because catalog lookups can't run outside a transaction. *Useful as a DR‑local topology injection point.*
- Sibling shared catalogs that also encode production identity/topology: **`gp_configuration_history`** (the "history" table), **`gp_id`**, **`gp_version_at_initdb`**.

**Consequence (your #1):** under physical replication the DR coordinator's `gp_segment_configuration` will contain **production's** hosts/ports/dbids/datadirs. In normal Greenplum this never bites (the standby coordinator is in the *same* cluster). It is unique to the cross‑cluster case → genuinely new work.

### 1.2 Distributed MVCC — basis for consistent reads
- The QD builds a **`DistributedSnapshot`** (`cdbdistributedsnapshot.h`: `xminAllDistributedSnapshots, distribSnapshotId, xmin, xmax, count, inProgressXidArray[]`) and ships it to every QE inside **`DtxContextInfo`** (`cdbdtxcontextinfo.h`) with each statement.
- Segments map **gxid → local XID** via the **distributed log** SLRU (`access/distributedlog.h`).
- **Distributed commits are WAL‑logged and replayed**: `xact.c` has `xact_redo_distributed_commit()` calling `DistributedLog_SetCommittedTree(... isRedo=true)`. So on a replica, after replay, the gxid↔localxid map is correctly reconstructed — the visibility *machinery* already works on a replica; what's missing is a coordinator that hands out a globally consistent snapshot bounded by what every DR segment has replayed.

### 1.3 The cluster‑wide consistency barrier — `gp_create_restore_point()`
`src/backend/access/transam/xlogfuncs_gp.c`: runs only on the QD; **acquires `TwophaseCommitLock` `LW_EXCLUSIVE`** ("to ensure cluster‑wide restore point consistency by blocking distributed commit prepared broadcasts"), `CdbDispatchCommand("SELECT pg_catalog.pg_create_restore_point(...)", DF_NEED_TWO_PHASE | DF_CANCEL_ON_ERROR, …)` to all segments, creates the restore point on the QD, releases the lock, returns `{gp_segment_id, restore_lsn}` per segment + coordinator. **That `{content→LSN}` vector at a quiesced 2PC boundary is the consistency point** — and a restore point is an `XLOG_RESTORE_POINT` WAL record, so each segment archives and later replays it. This is the barrier we reuse.

### 1.4 How replicas are built (base backup) and how recovery is configured (PG12 model)
- Nodes are seeded with **`pg_basebackup`** + GP flags (`src/bin/pg_basebackup/pg_basebackup.c`, driven from `gpMgmt/bin/gppylib/commands/pg.py`): **`--target-gp-dbid <dbid>`** (GP tablespaces use **per‑dbid paths** → every DR node gets its **own dbid**), `--tablespace-mapping`, and `--write-recovery-conf`.
- **PG12 recovery model (no `recovery.conf`):** recovery settings live in **`postgresql.auto.conf`** + a **`standby.signal`** marker. For our archive transport the relevant knobs are **`restore_command`** (pull a segment from the archive), optional **`recovery_target_name` / `recovery_target_action`**, **`hot_standby=on`**, and **`archive_mode`/`archive_command`** on the production side. GP also uses an extra **`internal.auto.conf`**.
- Redo dispatch is a **single point**: `xlog.c:7628` → `RmgrTable[record->xl_rmid].rm_redo(xlogreader)` (the surgical hook for the apply‑time filter). Per‑record `RelFileNode` is available via `XLogRecGetBlockTag` / `XLogRecHasBlockRef`. `XLOG_NOOP` exists (`pg_control.h` 0x20). Shared‑catalog relfilenodes are tracked by the **relmapper** (`relmapper.c`, `pg_filenode.map`), and relmap updates are WAL‑logged.
- Utilities: `gpinitstandby`, `gprecoverseg` (incremental via `pg_rewind`), `gpactivatestandby` (promotion = `pg_ctl promote` + catalog fix‑up), `gpaddmirrors`, `gpmovemirrors`, `gpexpand`.

### 1.5 FTS
`src/backend/fts/fts.c` etc.: runs in the coordinator, probes segments, **writes `gp_segment_configuration`**, drives failover. On the DR cluster FTS must be **neutralized** (it writes the catalog and promotes — neither valid in recovery) and optionally replaced by a DR‑local monitor tracking restore progress (§5.7).

### 1.6 Replication/recovery status surface
`gp_stat_replication` (`system_views.sql`) is **primary‑side** (walsenders) — under archive transport it's largely irrelevant for DR; the DR‑side surface must be **archive/restore‑progress** oriented (§5.6). New GP GUCs go in `guc_gp.c`. `wal_level=replica` is already the GG7 default.

---

## 2. How other MPP / distributed databases solve this

| System | Mechanism | Topology independence | Consistent reads on replica | What we borrow |
|---|---|---|---|---|
| **VMware Greenplum (GPDR)** | Recovery cluster does **continuous WAL restore** to the **latest distributed restore point**; Read Replica mode opens it read‑only. `gp_pitr` + `gpdr`. Segment **count** must match; **host layout may differ**. **Archive‑based.** | Recovery cluster keeps its **own** topology; restore is per‑content. | **Restore‑point barrier**: consistent as of the last fully‑applied distributed restore point. Hot‑standby recovery conflicts cancel queries. | The whole shape of the feature, the transport, and restore‑point‑gated consistency. |
| **Oracle Active Data Guard** | Physical standby open read‑only **while** redo apply runs; transactionally consistent; `STANDBY_MAX_DATA_DELAY` bounds staleness; **DML Redirect**; **Far Sync** (SYNC‑local/ASYNC‑remote relay); **Fast‑Start Failover** + observer. | N/A (single instance/RAC). | Read SCN bounded by applied SCN — never partial txns. | Read‑consistency contract; staleness‑bound GUC; **Far Sync = local relay/staging** for our WAN archive; witness for safe auto‑switchover; (later) write‑redirect. |
| **Citus** | Each node has its own physical standby; failover promotes all + **rewrites `pg_dist_node`**. Standby usually warm. | Topology table rewritten at failover. | Mostly warm‑standby. | "Promote all nodes, then rewrite the topology table" failover recipe (your #5). |
| **Snowflake** | Logical, **metadata‑driven** replication; failover groups. | Fully topology‑independent. | Metadata commit = consistent version. | Validates the external/metadata‑authoritative topology direction (aspirational). |

**Synthesis.** Mainstream answer to "consistent reads on a physical replica" = *advance a cluster‑wide consistent point and serve reads as of the last fully‑applied point.* Greengage already has the barrier and the per‑segment visibility machinery. Novel work: (1) DR coordinator **owns a topology** despite physical replication; (2) the barrier becomes a **DR‑published read horizon** + a **pinned distributed snapshot** the DR coordinator dispatches.

---

## 3. Target architecture (archive / continuous‑restore)

```
        PRODUCTION (read/write)                 ARCHIVE REPOSITORY            DR cluster (read-only, in recovery)
        ┌─────────────────────────┐             (S3 / NFS / pgBackRest /      ┌─────────────────────────────┐
        │ Coordinator  archive_cmd │── push ──▶   WAL-G / DD), per-segment ──▶ │ DR Coordinator restore_cmd  │
        │ Seg c0       archive_cmd │── push ──▶   WAL + base backups +    ──▶ │ DR Seg c0   restore_cmd      │
        │ Seg c1       archive_cmd │── push ──▶   restore-point markers   ──▶ │ DR Seg c1   restore_cmd      │
        │  ...   (+ local mirrors) │                     ▲                    │  ...  hot_standby + RO        │
        └─────────────┬───────────┘                     │                    │ DR-local topology (own dbids) │
                      │                                  │                    │ apply-time REDO FILTER on     │
   periodic distributed restore point  ── (optional local relay / Far Sync ──┘  topology catalogs            │
   (gp_create_restore_point-like): each   staging archives prod-local-fast,     └──────────────┬──────────────┘
   segment's WAL gets a marker + the       relay async-ships to remote repo)                   │
   distributed-xid horizon for barrier N                                       DR control plane publishes the
                                                                               latest restore point that ALL DR
                                                                               segments have completely restored
```

Properties:
- **Per‑content continuous restore.** DR coordinator restores the production coordinator's archived WAL; DR seg *cN* restores production primary *cN*'s archived WAL. Segment **count must match**; **host layout free**. DR nodes seeded by **base‑backup restore from the repository** (so DR creation puts ~no load on production).
- **DR‑local topology**, protected from replayed WAL by the **apply‑time redo filter** (§5.1).
- **Restore‑point‑gated reads** (§5.4): production emits periodic distributed restore points; the DR control plane publishes the newest restore point **every DR segment has completely restored**; read‑only queries get a distributed snapshot pinned to it.
- **Read‑only enforcement** end‑to‑end on the DR coordinator (§5.3).
- **Switchover** = promote all DR nodes + reconcile DR topology + re‑enable FTS (§5.5).
- **Optional local relay/staging** (Far Sync analog) so production archives locally‑fast and the WAN transfer is decoupled (§5.8).

---

## 4. Compatibility contract (GPDR enforces the same)
- **Segment (content) count identical** on production and DR. DR mirrors are independent (DR may be mirrorless, or have its own mirrors for HA‑within‑DR).
- **Same major version & `CATALOG_VERSION_NO`, block size, checksum/`wal_log_hints` setting, encoding.** Physical replication is byte‑level; mismatches are fatal.
- **Host layout may differ** (host count, segments‑per‑host, addresses). Tablespaces remapped via per‑dbid paths (`--target-gp-dbid`, `--tablespace-mapping`).
- DR is **read‑only**: no writes/DDL/`nextval`/2PC/autovacuum on DR (recovery enforces; we add guardrails, §5.3).

---

## 5. Detailed design — addressing your 8 points

### 5.1 Topology independence — apply‑time redo filter (your #1) **[decision locked]**

**Why not a WAL proxy or offline rewriter.** Physical WAL is a contiguous byte stream where the LSN *is* the byte offset, each record carries `xl_prev` (backward chain) + `xl_crc`, and each page stores `pd_lsn` with an apply interlock. You **cannot remove bytes** (shifts LSNs, breaks the chain and page interlock). You *can* rewrite a record in place into a same‑length `XLOG_NOOP` + recomputed CRC, but that's a stateful, format‑coupled transform on the most safety‑critical byte stream, with silent‑corruption blast radius — and crucially it would either **corrupt the canonical archive** production needs for its own PITR (if done on the push side) or force a **second, rewritten archive copy** (if done on the DR pull side). At high WAL volume a second archive is exactly the cost we're avoiding. Since we own the replay engine and are modifying core anyway, filtering at apply time is strictly cleaner and keeps **one faithful archive** reusable for production PITR and N DR replicas.

**The mechanism.** At `xlog.c:7628`, before dispatching a record to its rmgr, check whether **all** of the record's block references point into a small **protected set** of relfilenodes; if so, **skip `rm_redo`** (the replay LSN still advances — it's driven by the record reader, not by redo), leaving the DR node's own pages untouched.
- **Protected set** (resolved via the **shared relmapper at startup**, not hardcoded): `gp_segment_configuration` (5036) **+ its indexes 7139/7140 + its TOAST 6092/6093**, `gp_configuration_history` + indexes, `gp_id`, `gp_version_at_initdb`.
- **Matching** is per‑block via `XLogRecGetBlockTag`/`XLogRecHasBlockRef`, scoped to data‑modifying rmgrs (Heap/Heap2/Btree/etc.) in the **global** tablespace. This **naturally catches full‑page images** (FPIs) — which we *must* skip, since with `full_page_writes`/checksums/`wal_log_hints` production emits FPIs for these pages and an FPI overwrites the whole page on replay. The match reads the (uncompressed) block header, so it works even with `wal_compression=on`.
- **Gating** behind a `gp_dr_replica` GUC + a `dr_replica.signal` marker; production and normal mirrors unaffected.
- DR's `pg_wal` stays **byte‑identical to production's** → preserves cascading DR replicas, faithful `pg_waldump`/PITR, and deterministic DR crash recovery (the filter re‑applies identically on restart).

**Seeding DR's own topology rows (the real work).** After base‑backup restore, DR's `gp_segment_configuration` holds *production's* topology; we need DR‑local content, but DR is **always in recovery** in steady state (no read‑write window). Two viable approaches:
- **(primary) Frozen‑tuple seeding at create time.** The create utility, *before* placing `standby.signal`, starts the DR coordinator **standalone** (read‑write, not yet applying production WAL), deletes the production rows, inserts DR rows, `VACUUM FREEZE gp_segment_configuration` (xmin = `FrozenTransactionId` → unconditionally visible, no CLOG dependency), checkpoints to flush the page, shuts down; then configures continuous restore with the filter active. Because the rows are frozen and the filter blocks production's records from overwriting that page, the DR rows persist regardless of the replayed XID timeline (avoids the XID‑reuse hazard a non‑frozen seed would have).
- **(alternative) DR‑local read redirection.** Teach the topology‑read path (`cdbutil.c`, FTS dump path) to prefer a DR‑local source under `gp_dr_replica`, making the catalog content irrelevant for dispatch. Cleaner re: replication, but touches every topology‑consumer site; reserve as the path toward the etcd/external‑store end state.

**Known edge — relfilenode change / `VACUUM FULL`.** If production ever `VACUUM FULL`/`CLUSTER`/`REINDEX`es a topology catalog, its relfilenode changes and a WAL‑logged relmap update follows; not filtering it would drift DR's mapper, filtering it would orphan DR's pages. Affects any relfilenode‑based scheme. Mitigation: **forbid rewriting these (tiny) catalogs on production while a DR replica is attached** (event trigger / pre‑flight check) + document.

**Strategic direction.** The external‑store/etcd approach (your option b) remains the clean long‑term answer (read redirection generalized) but is a large cross‑cutting epic ("cluster coordination via consensus"); scope after the PoC.

### 5.2 Utility to create the DR cluster (your #2) — `gpcreatedrcluster` (working name)
Built on base‑backup restore from the **repository** (not from production), then continuous‑restore config:
1. **Validate the contract** (§4): content count, version/catalog version, checksum/blocksize/encoding; reachable DR hosts; archive‑repo reachability; `pg_hba.conf`.
2. **Allocate DR identity**: fresh dbids for DR coordinator + each DR segment; build the **DR‑local topology**.
3. **Restore each DR node's base backup from the repository** with **`--target-gp-dbid <dr_dbid>`** + `--tablespace-mapping`; parallelize across hosts (reuse `clsRecoverSegment*` worker patterns).
4. **Seed DR topology** (frozen‑tuple step, §5.1) and **arm the redo filter / DR mode** (`gp_dr_replica`, `dr_replica.signal`).
5. **Write recovery config** into each node's `postgresql.auto.conf`: **`restore_command`** (pull from repo), `hot_standby=on`, the new DR GUCs (§5.8); create `standby.signal`. **No replication slot anywhere.**
6. **Start the DR cluster in DR mode**; register with the DR control plane (which begins continuous restore + horizon publication, §5.4/§5.6).

Ship `gpdestroydrcluster` and `gpdr status` too. Architecturally: "`gpinitstandby` generalized to a whole standby cluster with DR identity remapping, fed by continuous restore."

### 5.3 Read‑only DR coordinator mode (your #3)
This has **two halves**: (a) what the DR coordinator must **refuse**, and (b) the **mechanics** of a coordinator that is itself in recovery yet still dispatches read‑only distributed queries. Half (b) is substantial, genuinely new core work — it is **not** delivered by hot standby alone — and is specified in its own document (**Standby Distributed Read Mode**) and milestone (**M2′**, §6).

**(a) Refusal / hardening (this section):**
- **Hot‑standby on** (DR serves reads while applying WAL — the Active Data Guard model).
- **Refuse non‑read‑only at dispatch**: block `DF_NEED_TWO_PHASE`, writable DML/DDL, `nextval`, temp‑with‑WAL, `SELECT … FOR UPDATE/SHARE` (row locks are writes), distributed‑transaction start that writes. Hook: dispatcher / `cdbtm.c` DTX start + statement classification under the new DR mode. Clear `ERROR: cluster is a read-only DR replica`.
- **Explicit DR mode** (`gp_dr_replica` GUC + `dr_replica.signal`) so backends, FTS, autovacuum, bgworkers branch uniformly (cleaner than scattering `RecoveryInProgress()` checks).
- **Disable on DR**: autovacuum, FTS catalog writes, `gpexpand`, write‑side scheduled jobs. Match GPDR's "only gpstart/gpstop/gpconfig (+ gpbackup) supported on the recovery cluster" posture.

**(b) Standby distributed read (separate doc + M2′):** the obstacle is that a normal distributed `SELECT` allocates a distributed xid by *writing shared memory* (`currentDtxActivate` → `nextGxid++`, `cdbtm.c:306`) and opens a **writer gang** — both illegal in recovery. The fix is a new QD context **`DTX_CONTEXT_QD_STANDBY_READER`** that allocates **no gxid**, writes **no WAL**, and dispatches the **pre‑captured `D_N`** (from the published horizon, §5.4) to **reader‑only gangs**, reusing the already‑existing `DTX_CONTEXT_QE_READER` executor path and stock per‑node hot standby for local visibility. The fiddliest piece is the writer‑less reader gang's snapshot synchronization. See the Standby Distributed Read Mode design for the full treatment.

### 5.4 Consistent reads (your #4) — restore‑point‑gated, transport‑agnostic core


**The invariant.** Each DR segment restores/replays independently; a read‑only distributed snapshot is safe only if its visibility horizon ≤ the slowest segment's fully‑decided horizon. So:

> The DR cluster's **published read horizon** = the newest **distributed restore point** *N* such that **every** DR segment (and the DR coordinator) has restored complete WAL through, and replayed past, its `restore_lsn` for *N*.

**Mechanism (reusing `gp_create_restore_point`):**
1. **Production emits periodic distributed restore points** (a scheduler on the production coordinator) — cadence is the **RPO knob**. Capture the **distributed‑xid horizon** (the QD's distributed snapshot + `latestCompletedGxid`) at the barrier and **carry it in the restore‑point WAL record** (extend `XLOG_RESTORE_POINT`, or add a small GP "DR barrier" record), so the DR side can reconstruct *visibility*, not just position. Each segment archives the marker as part of normal archiving.
2. **DR control plane tracks restore progress** per node (which restore points each has *completely restored*, via the archive + `pg_last_wal_replay_lsn()`), computes `published_barrier = max N restored+replayed by all DR nodes`, and publishes it to the DR coordinator (shared memory + a small DR table; reflected in §5.6).
3. **DR coordinator dispatches a pinned `DistributedSnapshot`** built from barrier *N*'s horizon (`xmax = horizon`, in‑progress list from the marker) via `DtxContextInfo`. Each DR segment already has the correct gxid↔localxid map from replay (§1.2) → globally consistent, equal to "production as of barrier *N*."

**Design fork to decide now — how recovery advances (this is sharpened by the archive choice; see §10‑C):**
- **(i) Stop‑and‑go (GPDR‑faithful):** replay each node exactly to restore point *N* (`recovery_target_name=N`, `recovery_target_action=pause`), open read‑only, then advance to *N+1*. Gives a clean static image, but **PG12's recovery target is essentially one‑shot** — re‑arming to successive named points within one running instance is not natively supported without restart, so this likely needs a **core change** (a repeatable "advance to next restore point and keep serving reads" operation).
- **(ii) Free‑running replay + horizon (recommended):** let each node replay continuously to end‑of‑available‑WAL; the coordinator publishes the latest restore point crossed by **all** segments and pins read snapshots to it. **No re‑targeting needed**, and it reuses the same horizon mechanism for any transport. Cost: segments replay *ahead* of the published read horizon, so MVCC must hide already‑applied‑but‑beyond‑*N* transactions (fine — the pinned distributed snapshot does this) and **recovery‑conflict exposure widens** to the gap between *N* and the replay frontier (tunable via `max_standby_streaming_delay`/`hot_standby_feedback`).

**Why a barrier, not `min(replay_lsn)`.** A naive cross‑segment `min(replay_lsn)` is **not** a transactionally consistent cut — a distributed transaction can be committed on c0 but not c1 at arbitrary LSNs. Only the `TwophaseCommitLock`‑guarded restore point guarantees no distributed transaction straddles the cut.

### 5.5 Switchover / failover (your #5)
1. **Decide RPO**: optionally drive DR to the last *complete common* restore point (§10‑F) for minimal loss; for a hard failover accept the last published barrier.
2. **Promote every DR node** (`pg_ctl promote` on DR coordinator + each DR segment): ends recovery, switches timeline, removes `standby.signal`/`dr_replica.signal`.
3. **Reconcile DR topology** to a normal primary config: clear DR mode, set roles `p`/`u`, fix `gp_id`, re‑enable FTS as an active service, (re)build DR‑local mirrors (`gpaddmirrors`) if wanted, regenerate `pg_hba.conf`. ("Promote all, then rewrite the topology table" — Citus — + GPDR's `gpdr promote`.)
4. **Validate & open read/write** (`gpstate`, `gpcheckcat`); redirect applications.
5. **Failback** (later): reconfigure the old production as a new DR (reverse the flow) — GPDR's failover/failback.

Automated switchover needs a **witness/observer** (Fast‑Start‑Failover analog) to avoid split‑brain; out of scope for PoC, but design the mode flags so an observer can drive them.

### 5.6 DR replication/restore‑status view (your #6)
`gp_stat_replication` is primary/streaming‑oriented and largely irrelevant here. Add an **archive/restore‑progress** surface:
- New SRFs `gp_stat_get_dr_replica()` per DR node: `content, dbid, last_restored_segment, last_replayed_lsn, segments_behind (archive lag), last_restored_restore_point, published_read_horizon_restore_point, restore_command_errors, timeline, conflicts, state (restoring/caught-up/stalled)`.
- **Cluster‑level**: `latest_complete_common_restore_point`, **estimated RPO** (now − wall‑clock of `published_read_horizon`), and **per‑segment archive/replay skew** (the freshness limiter, §10‑A).
- **Production‑side companion**: archive pipeline health per segment (last archived segment, `archive_command` failures, relay/staging lag) — since a single lagging segment gates DR freshness.
- `gpdr status` CLI + a `gpstate --dr` mode (reuse `clsSystemState.py` display patterns).

### 5.7 Restrictions in DR mode (your #7) and minimization
**Inherent (document; GPDR documents the same):**
- Read‑only: no DML/DDL/`nextval`/temp‑with‑WAL/2PC (§5.3).
- **Reads are as‑of the last published restore point**, not real‑time. Lag floor ≈ `archive_timeout` + repo/relay transfer + restore cadence + slowest‑segment skew (§10‑A,E).
- **Recovery conflicts** can cancel long queries or stall replay (`max_standby_streaming_delay`/`hot_standby_feedback`).
- **Config inherited & frozen** (roles, resource groups/queues, GUCs from replicated catalogs); changing on DR unsupported.
- **FTS disabled** → no in‑DR auto‑failover while in replica mode.

**Minimize:**
- **Lower lag**: shorter restore‑point cadence; `wal_compression=on` and a **local relay/staging** to cut archive latency; homogeneous DR hardware to reduce skew.
- **Fewer cancellations**: tune `max_standby_streaming_delay` / `hot_standby_feedback`; or serve heavy reporting against a deliberately older but stable restore point.
- **Relax read‑only later**: Active‑Data‑Guard‑style **DML redirect** (forward incidental writes to production) — large, post‑PoC.
- **In‑DR HA**: give DR its own mirrors + a DR‑local FTS variant that fails over DR mirrors without touching the production‑sourced restore identity.

### 5.8 Archive & recovery configuration (your #8)
- **Production side**: `archive_mode=on`, an `archive_command` pushing each completed segment (per segment **and** coordinator — GPDB archives per‑segment) to the repository (pgBackRest/WAL‑G/S3/NFS/DD). Strongly recommend **`wal_compression=on`** (compresses FPIs — a large fraction of "huge WAL" — at the source, directly cutting WAN volume). Consider a larger WAL segment size at initdb for very high volume. Tune `archive_timeout` to bound RPO vs. overhead.
- **Local relay / staging (Far Sync analog)**: production archives to a *local, fast* archive/relay; the relay async‑ships to the geo‑remote repository — decoupling production's `archive_command` latency from the WAN.
- **DR side**: `restore_command` (pull from repo) + `standby.signal` + `hot_standby=on` in `postgresql.auto.conf`. **No replication slot** (the rev. 1 slot‑bloat risk is gone). Retention is the **repository's** concern (pgBackRest/WAL‑G expiration; GPDR `expire`), decoupled from production `pg_wal`.
- **New GUCs** (in `guc_gp.c`, surfaced via `gpconfig`/`auto.conf`):
  - `gp_dr_replica = on` — marks a DR replica (drives read‑only enforcement + redo filter).
  - `gp_dr_role = coordinator|segment`, `gp_dr_content = N` — DR identity for the control plane.
  - `gp_dr_restore_point_interval` (production) — barrier cadence / RPO.
  - `gp_dr_read_horizon_policy = bounded_lag|bounded_cancel` — recovery‑conflict policy.
  - optional relay/repo metadata.
- Keep human‑edited DR config minimal in `auto.conf` (it's machine‑rewritten by `ALTER SYSTEM`); richer DR config (repo, cadence, pairing) belongs in a **control‑plane YAML** (à la GPDR `recovery_cluster_config_file_gp7.yml`).
- **An offline WAL utility, if built, is for archive *management/validation*** — a `pg_waldump`‑style inspector (confirm contents/restore points), gap detection, pruning, pre‑flight — **not** the topology filter (which is in‑engine, §5.1). If team‑ownership reasons ever demand WAL rewriting, do it **only** on the DR pull side into a DR‑private copy, never on the canonical archive.

---

## 6. PoC development plan (phased)

Each milestone has a demoable **exit criterion**. Sequencing de‑risks topology (M1) and consistency (M3) early.

### M0 — Spike & decisions (1–2 weeks)
- Reproduce GPDR Read Replica on a stock GP7 build as a **reference oracle**.
- Decide: **§10‑C recovery‑advance model** (recommend free‑running + horizon); restore‑point cadence default; conflict policy default; repository tech (pgBackRest vs WAL‑G).
- Stand up CI: two small clusters + a shared archive repository (e.g. MinIO/NFS), automated teardown.
- **Exit:** design ratified; two clusters where DR nodes do continuous restore from a repo and come up in hot‑standby (no consistency gating yet).

### M1 — DR‑local topology via apply‑time redo filter (your #1) (3–5 weeks)
- Implement the `xlog.c:7628` filter for the protected set (incl. indexes/toast), resolved via the shared relmapper; handle FPIs; re‑resolve on relmap redo.
- `gp_dr_replica` GUC + `dr_replica.signal`; frozen‑tuple seeding step in a throwaway init script.
- **Exit:** force a topology change on production (mark a mirror down / `gprecoverseg`), ship/restore the WAL; DR's `gp_segment_configuration` is **unchanged** and still describes the DR cluster; DR coordinator `SELECT`s see *itself*; `pg_waldump` confirms DR `pg_wal` is byte‑identical to production.

### M2 — Read‑only DR coordinator mode (your #3) (2–3 weeks)
- Enforce read‑only at dispatch/DTM; block 2PC/DDL/DML/`nextval`/temp‑with‑WAL/`FOR UPDATE`; neutralize FTS catalog writes + autovacuum on DR. This is the **refusal** half (§5.3a); the dispatch half is M2′.
- **Exit:** **coordinator‑only** `SELECT`s (catalog / entry‑DB‑singleton) work on the DR coordinator while in recovery; every write/DDL/`nextval` fails fast; FTS makes no catalog change. (Cross‑segment dispatch is M2′.)

### M2′ — Standby distributed read (your #3, dispatch half) (4–6 weeks; prerequisite for M3)
*Detailed in the Standby Distributed Read Mode design.* The DR coordinator is itself in recovery, so a normal distributed `SELECT` can't run: it would allocate a gxid via a shared‑memory write (`currentDtxActivate`→`nextGxid++`, `cdbtm.c:306`) and open a writer gang. Build the new `DTX_CONTEXT_QD_STANDBY_READER` (no gxid, no WAL, dispatches `D_N`) and **reader‑only gangs**, reusing the existing `DTX_CONTEXT_QE_READER` path.
- SR‑0 coordinator‑only reads → SR‑1 QD standby context (no gxid) → SR‑2 reader‑only gang + dispatch `D_N` (the fiddly core: writer‑less snapshot sync) → SR‑3 real query coverage (joins/aggregates/motion) → SR‑4 session hardening.
- **Exit:** a representative analytic query (joins + aggregation + motion, read‑only) **dispatches from the DR coordinator to all DR segments while every node is in recovery**, returns correct rows, with **no writer QE and no WAL written anywhere**.

### M3 — Restore‑point‑gated consistent reads (your #4) (5–7 weeks; centerpiece)
*Builds on M2′: M3 makes the dispatched `D_N` **correct/consistent**; M2′ made the distributed read physically dispatch.*
- Production: restore‑point scheduler; **extend the restore‑point WAL record** with barrier id + distributed‑xid horizon.
- DR: control plane tracking per‑segment complete‑restore + replay; **publish** `published_barrier`; DR coordinator builds & dispatches a **pinned `DistributedSnapshot`** from it.
- Implement the chosen recovery‑advance model (M0).
- **Exit (money demo):** a multi‑statement distributed transaction on production becomes visible on DR **atomically** only after all DR segments cross the restore point — never a partial read — verified under concurrent load and artificial per‑segment restore skew.

### M4 — Create/destroy utility (your #2) (4–5 weeks; overlaps M1–M3)
- `gpcreatedrcluster`: validate → allocate DR dbids → parallel base‑backup **restore from repo** with `--target-gp-dbid` → frozen‑tuple seed → write `restore_command` + DR GUCs → start in DR mode. Plus `gpdestroydrcluster`.
- **Exit:** one command turns a fresh set of DR hosts (different layout from production) into a continuously‑restoring, read‑only, consistent DR cluster, with ~no load on production.

### M5 — Observability + restrictions polish (your #6/#7) (3–4 weeks)
- `gp_stat_dr_replica` + cluster‑level RPO / latest‑complete‑common restore point / per‑segment skew; production‑side archive‑health view; `gpdr status` / `gpstate --dr`.
- `gp_stat_database_conflicts` equivalent + conflict‑policy GUC; gap detection; restrictions doc + guardrails.
- **Exit:** one status command shows live RPO/lag/skew/conflicts; an injected archive gap on one segment shows a clear stall + the freshness it caps the cluster at.

### M6 — Switchover/failover (your #5) (3–4 weeks)
- `gpdr promote`: drain option → promote all DR nodes → reconcile topology to primary → re‑enable FTS → open read/write. (Failback documented; automation optional.)
- **Exit:** with production stopped, one command promotes DR to a writable primary serving data up to the last complete common restore point; `gpcheckcat` clean.

**Stretch / post‑PoC:** etcd‑backed topology (option b); DML‑redirect; witness‑driven Fast‑Start‑Failover; in‑DR mirrors + DR‑local FTS; incremental DR refresh without full re‑base‑backup.

---

## 7. Risk register (updated for archive + redo filter)
| Risk | Severity | Mitigation |
|---|---|---|
| ~~Slot‑driven WAL bloat on production~~ **(RETIRED by archive transport)** | — | No slots; retention is on the repository. |
| **Per‑segment archive skew gates DR freshness / consistency** (§10‑A,F) | **High** | Per‑segment archive‑health monitoring; compute *latest complete common* restore point; homogeneous DR + relay. |
| **Timeline switches from production mirror failover** must be followed per segment; missing `.history`/post‑switch WAL stalls DR (§10‑B) | **High** | Ensure archive pipeline archives history files + post‑failover WAL reliably; DR restore follows timelines per segment; test failover‑during‑DR explicitly. |
| **Continuous‑restore advance model** may need core work (§10‑C) | **High** | Decide free‑running + horizon at M0 to avoid PG12 re‑targeting limits. |
| Redo filter misses a path (relfilenode change, relmap redo, toast/index) → topology corrupted | High | Resolve protected set via relmapper incl. indexes/toast; re‑resolve on relmap redo; forbid `VACUUM FULL` on topology catalogs; assertion tests + DR validator. |
| Consistency bug → non‑atomic (partial‑txn) read | High | Barrier under `TwophaseCommitLock` only; never `min(replay_lsn)`; M3 skew/concurrency tests; differential test vs GPDR oracle. |
| **Archive repository as SPOF / object‑store eventual consistency** (transient missing segment) | Medium→High | HA repo; retries/backoff in `restore_command`; gap detection; relay/staging. |
| Recovery conflicts cancel queries / stall replay (worse under free‑running, §10‑C) | Medium | Tunable policy GUC; serve heavy reads at an older stable point; document. |
| Coarser/variable RPO; tail loss if production dies between restore points (§10‑E) | Medium | Tune cadence vs. overhead; surface RPO in status. |
| Re‑sync over WAN when a node falls behind (no easy `pg_rewind`) | Medium | Re‑base‑backup that node from repo + re‑seed topology; document workflow. |
| `relfrozenxid`/wraparound bookkeeping for filtered shared catalogs (§10‑H) | Medium | Frozen seed keeps real oldest‑xid safe; verify `datfrozenxid` interaction in tests. |
| Catalog/version drift between clusters | Medium | Hard pre‑flight checks (§4). |

---

## 8. Open decisions for the team
1. **~~Transport~~ — DECIDED: archive / continuous restore** (§0.1).
2. **~~Topology filtering~~ — DECIDED: apply‑time redo filter** (§5.1); etcd/external store deferred as a separate epic.
3. **Recovery‑advance model** (§5.4 / §10‑C): free‑running + horizon (recommended) vs. stop‑and‑go (needs core re‑targeting).
4. **Restore‑point cadence & conflict policy defaults** (RPO vs. overhead; `bounded_lag` vs `bounded_cancel`).
5. **Repository technology** (pgBackRest vs WAL‑G vs cloud‑native) and whether a local relay/staging is in v1.
6. **In‑DR HA**: DR mirrors in v1, or whole‑cluster failover only?
7. **Naming/UX** aligned with Greengage conventions and (where sensible) GPDR.

---

## 9. First commits to land (week 1–2)
- `gp_dr_replica` GUC in `guc_gp.c` + `dr_replica.signal` handling in `StartupXLOG`.
- Skeleton **redo filter** at `xlog.c:7628`, keyed on relmapper‑resolved relfilenodes of the four topology catalogs (+ indexes/toast), no‑op behind the GUC, with unit tests in `src/backend/access/transam/`.
- A throwaway init script (`gpMgmt/bin/`) that restores a 2‑segment base backup from a repo with `--target-gp-dbid`, does the frozen‑tuple seed, and brings the cluster up in hot‑standby continuous restore.
- A `make`‑able **two‑cluster + repository CI fixture** (extends demo‑cluster) so M1–M6 each have an end‑to‑end test.

---

## 10. Review — problems this design (archive + redo filter) introduces *(for discussion before development)*

These are issues created or sharpened specifically by the locked decisions, beyond the generic risks in §7. Ordered roughly by how much they should shape the design *now*.

**A. Per‑segment archive independence is the real freshness/consistency limiter.** Each segment archives its own WAL independently. The cluster can only advance the read horizon to restore point *N* when **every** segment has the complete archived WAL through its *N*‑LSN *and* has replayed it. The slowest segment's archive pipeline (or restore throughput) therefore gates the entire DR — and unlike streaming's continuous acked connection, archive completeness is discrete and per‑segment. *Discussion:* what's the monitoring + alerting contract, and what skew is acceptable before we alarm? Do we throttle restore‑point creation to the slowest archiver?

**B. Production HA events fork the timeline and complicate restore.** When a production primary fails over to its mirror, that segment's WAL undergoes a **timeline switch**; the new primary archives on a new timeline, and the DR segment (still on the old timeline) must follow via the archived `.history` file. Different segments can switch at different times, so DR segments may be on **different timelines simultaneously**. If history files or post‑failover WAL aren't archived reliably, that segment's restore stalls. GPDR shipped real bugs exactly here ("recovery target timeline does not exist"; WAL failing to archive after primary→mirror failover). *Discussion:* this is arguably the nastiest operational problem — do we require the production archive pipeline to guarantee history‑file archiving, and do we test "mirror failover while DR is attached" as a first‑class scenario in M3/M5?

**C. The continuous‑restore advance model may force a core change.** PG12's `recovery_target_*` is essentially one‑shot: after pausing at a target, `pg_wal_replay_resume()` goes to end‑of‑WAL, not to the next named point, and changing the target needs a restart. So a GPDR‑style "stop at restore point *N*, open read‑only, then advance to *N+1* within the same running instance" likely needs new core support. The **free‑running replay + horizon** model (recommended, §5.4(ii)) avoids re‑targeting entirely and reuses one transport‑agnostic mechanism — but it lets segments replay **ahead** of the published read horizon, which **widens recovery‑conflict exposure** (a cleanup/vacuum record applied at the replay frontier can conflict with a query reading as‑of an older restore point). *Discussion:* confirm free‑running + horizon at M0; agree the conflict‑management policy and whether reads can optionally pin to a further‑back stable point for heavy reporting.

**D. The archive repository becomes a new production‑critical dependency.** Streaming was point‑to‑point; now both clusters depend on a repository whose availability, throughput, **retention/expiration policy**, and (for object stores like S3) **eventual consistency** all matter — a just‑uploaded segment can transiently 404, stalling a `restore_command`. *Discussion:* repo HA expectations; `restore_command` retry/backoff; who owns expiration so we never prune WAL the DR still needs, and never let the repo grow unbounded.

**E. RPO is coarser and more variable, with tail‑loss between restore points.** Freshness floor ≈ `archive_timeout` + repo/relay transfer + restore cadence + slowest‑segment skew — structurally higher than streaming. And restore points are the *only* consistent open points: if production dies between *N* and *N+1*, DR can only open at the last **complete common** restore point, losing everything after it. *Discussion:* target RPO, and the cadence/overhead tradeoff (frequent restore points add dispatch + `TwophaseCommitLock` churn on production).

**F. "Latest complete common restore point" is non‑trivial to compute on failure.** On an unclean production loss, the last restore point may be **partially archived** — some segments past *N*, others not. DR's true openable point is the newest *N* for which **all** segments have complete archived WAL through their *N*‑LSN. This needs a per‑segment archive‑completeness check married to the restore‑point set, both for the status view (§5.6) and for `gpdr promote`'s drain step (§5.5). *Discussion:* define and test this computation explicitly; it's easy to get subtly wrong and silently open at an inconsistent point.

**G. Re‑syncing a fallen‑behind DR node is heavy over a WAN.** If a DR node hits an unrecoverable archive gap or falls too far behind, there's no easy `pg_rewind` across the WAN; the practical fix is a **full base‑backup restore from the repo for that node + re‑seed topology** (frozen‑tuple step again). *Discussion:* accept full re‑base‑backup for the PoC, and design the create utility so a single‑node re‑seed is a first‑class operation, not a special case.

**H. Apply‑time‑filter subtleties to validate (not blockers, but test‑worthy):**
- **`relfrozenxid` / wraparound bookkeeping** for the filtered shared catalogs: production's vacuum/freeze of `gp_segment_configuration` is WAL‑logged and we filter it, so DR's `pg_class.relfrozenxid` for that relation is replayed from production while DR's pages are independently frozen. The frozen seed keeps DR's *real* oldest xid safe, but confirm there's no interaction with `datfrozenxid`/autovacuum‑wraparound logic on DR.
- **The filter reduces WAL *apply*, not WAL *volume*** — topology records are tiny; bandwidth is handled by `wal_compression`/relay, not the filter. (Stated to avoid a misconception.)
- **Determinism across restartpoints**: DR re‑filters identically on restart and on archive re‑fetch; verify with a kill‑during‑restore test.
- **The `VACUUM FULL`/relmap prohibition** (§5.1) must be actually enforced (event trigger or checked at restore‑point creation), not just documented.

---

### Appendix A — Code map
- Topology/dispatch: `src/backend/cdb/cdbutil.c`; `src/include/catalog/gp_segment_configuration.h`, `gp_configuration_history.h`, `gp_id.h`, `gp_version_at_initdb.h`, `indexing.h` (idx 7139/7140), `toasting.h` (toast 6092/6093)
- Distributed MVCC: `src/backend/cdb/cdbtm.c`, `cdbdistributedsnapshot.c`, `cdbdtxcontextinfo.c`; `src/include/cdb/cdbdistributedsnapshot.h`, `cdbdtxcontextinfo.h`; `src/backend/access/transam/distributedlog.c`, `xact.c` (`xact_redo_distributed_commit`)
- Barrier / WAL / **redo filter hook**: `src/backend/access/transam/xlogfuncs_gp.c` (`gp_create_restore_point`); `xlog.c` (**redo dispatch `:7628`**, restore‑point record, `XLOG_NOOP`); `src/include/access/xlogreader.h` (`XLogRecGetBlockTag`/`XLogRecHasBlockRef`); `src/backend/utils/cache/relmapper.c`
- Base‑backup/recovery config: `src/bin/pg_basebackup/pg_basebackup.c`; `gpMgmt/bin/gppylib/commands/pg.py`; `operations/buildMirrorSegments.py`, `initstandby.py`; `go-tools/utils/postgres/update_conf_files.go`
- FTS: `src/backend/fts/fts.c`, `ftsprobe.c`, `ftsmessagehandler.c`
- Status views/CLI: `src/backend/catalog/system_views.sql` (`gp_stat_replication`); `gpMgmt/bin/gppylib/programs/clsSystemState.py`; `gpMgmt/bin/gpstate`
- GUCs: `src/backend/utils/misc/guc_gp.c`, `postgresql.conf.sample`

### Appendix B — Reference reading
- VMware Greenplum Disaster Recovery: *Overview*, *Read Replica Mode*, *Direct Replication*, *Failover and Failback*, *gpdr/gp_pitr* references (Broadcom TechDocs) — the closest existing implementation; **archive/PITR‑based**, validating this transport.
- Oracle Active Data Guard: *Real‑Time Query*, *DML Redirect*, *Far Sync*, *Fast‑Start Failover* — readable‑physical‑standby consistency, the relay pattern, and witness‑driven failover.
- Citus multi‑node DR (per‑node standby + `pg_dist_node` rewrite on failover) — the topology‑rewrite failover recipe.
- Greengage DB blog: *Overview of Greengage cluster backup and restore* (pgBackRest / WAL‑G context; saving `gp_segment_configuration` alongside backups).
