# Greengage 7 DR — Standby Distributed Read Mode
## Design for "coordinator in recovery that still dispatches read‑only distributed queries"

*Fills the gap you identified: the main plan's §5.3 covered what the DR coordinator must **refuse** (writes/DDL/2PC), but not the **mechanics** of a coordinator that is itself in recovery yet accepts a client `SELECT`, plans it, builds a gang, and dispatches to segments that are also in recovery. That mode does not exist in Greengage today and is genuinely new core work. Grounded in the GG7 `7.x` code; file:line references are concrete.*

---

## 0. Why "just turn on hot_standby" is only half the story

Single‑node PostgreSQL hot standby gives you a backend that serves reads while the startup process replays WAL. Greengage needs more, because on a Greengage coordinator **dispatching even a pure `SELECT` goes through the write path of the distributed transaction manager**:

1. **The QD allocates a distributed xid (gxid) by *writing shared memory*.** `currentDtxActivate()` does `MyTmGxact->gxid = ShmemVariableCache->nextGxid++` under a spinlock (`cdbtm.c:306`), after possibly `bumpGxid()`. Incrementing `nextGxid` is a **write to shared state** — illegal for a process in recovery, and meaningless on a replica (gxids belong to production's sequence, and we *replay* them).
2. **The QD opens a *writer* gang.** QEs come up and, when `Gp_is_writer`, run as writer QEs that `StartTransaction` in a writable mode and can themselves lazily assign local xids. A segment in recovery can't.
3. **The DTM may write WAL** (distributed commit/abort/“forget” records — the very `xact_redo_distributed_commit` records the horizon design relies on). Recovery forbids WAL writes entirely.

So the missing mode is a **standby distributed transaction**: the QD runs read‑only, **never** calls `currentDtxActivate()`, allocates **no** gxid, writes **no** WAL, and dispatches a query to **reader‑only** gangs — feeding them the **pre‑captured `D_N`** (from the published horizon, per the horizon design) instead of a freshly created snapshot.

**The good news from reading the code:** most of the executor‑side machinery already exists. There is already a `DTX_CONTEXT_QE_READER` (a reader QE that takes its snapshot from elsewhere and assigns no xid), a `Gp_is_writer` flag distinguishing writer from reader gangs, and a `DTX_CONTEXT_LOCAL_ONLY` for non‑distributed work. What's missing is (a) a **QD‑side standby context** that skips gxid allocation and uses `D_N`, and (b) the ability to dispatch to an **all‑reader gang** (no writer). This milestone builds those two things and wires them to the read‑only enforcement already specced in main‑plan §5.3.

---

## 1. The distributed‑transaction contexts today (and the one we add)

From `src/include/cdb/cdbtm.h` (`DtxContext`):
- `DTX_CONTEXT_LOCAL_ONLY` — no distributed transaction (utility statements, single‑node work).
- `DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE` — the normal QD: allocates a gxid, may write WAL, manages 2PC. **This is the write path we must avoid.**
- `DTX_CONTEXT_QD_RETRY_PHASE_2` — QD recovery/retry of 2PC.
- `DTX_CONTEXT_QE_*` — executor contexts; notably **`DTX_CONTEXT_QE_READER`** (reader QE: uses the writer's/dispatched snapshot, **assigns no xid**), plus writer/2PC variants and `DTX_CONTEXT_QE_ENTRY_DB_SINGLETON`.

**We add `DTX_CONTEXT_QD_STANDBY_READER`** (name TBD): a QD context that
- never calls `currentDtxActivate()` → **no gxid**, no `nextGxid++`, no `GxactLockTableInsert`;
- never enters prepare/commit/abort distributed paths (they early‑out, like they already do for non‑`QD_DISTRIBUTED_CAPABLE` contexts — see the guards at `cdbtm.c:334/852/902`);
- carries the **published `D_N`** as its distributed snapshot (horizon design §5.4) rather than building one from `nextGxid`;
- dispatches to **reader‑only** gangs.

Because the existing commit/prepare/rollback functions already begin with `if (DistributedTransactionContext != DTX_CONTEXT_QD_DISTRIBUTED_CAPABLE) return;` style guards, a new QD context that is *not* `QD_DISTRIBUTED_CAPABLE` is **inert** in all those write paths almost for free — we mainly add the new case where contexts are *set up* and where snapshots are *built/dispatched*.

---

## 2. End‑to‑end flow of a standby distributed read

```
client ──SELECT──▶ DR coordinator backend (process is in hot-standby; RecoveryInProgress())
  │
  ├─ classify statement: read-only? (main plan §5.3)  ── if not → ERROR (read-only DR replica)
  │
  ├─ enter DTX_CONTEXT_QD_STANDBY_READER
  │     - DO NOT currentDtxActivate(); gxid stays InvalidDistributedTransactionId
  │     - distributed snapshot = published D_N  (from horizon registry; horizon design §5.4)
  │     - local MVCC: this backend is a standard hot-standby snapshot backend
  │
  ├─ plan (planner/ORCA) — read-only plan; no DML/result-relation nodes (enforced)
  │
  ├─ assign a READER-ONLY gang (no writer QE) on DR segments
  │     - segments are in recovery; their QEs come up as standby backends
  │     - QD packs DtxContextInfo with D_N (cdbdtxcontextinfo.c:79 override, horizon §5.4)
  │
  └─ dispatch ▶ each DR segment QE:
        - setupQEDtxContext → DTX_CONTEXT_QE_READER  (no xid; uses dispatched D_N)
        - QE runs in hot-standby; tuple visibility = D_N (distributed) + S_i,N (local)   <- horizon §5.5
        - returns rows ▶ QD ▶ client
```

The two genuinely new pieces are the **QD‑side context** (left, "enter DTX_CONTEXT_QD_STANDBY_READER") and the **reader‑only gang** (no writer). Everything to the right of the dispatch arrow is the existing reader‑QE path plus the visibility work already designed in the horizon doc.

---

## 3. The hard sub‑problems

### 3.1 No gxid on the QD — and why that's OK
Normally the gxid is the QD's handle for 2PC and for stamping the distributed snapshot. In standby read mode we need **neither**:
- No 2PC: the query writes nothing, so there is no distributed commit to coordinate.
- The distributed snapshot is **not** built from our own gxid; it **is** `D_N`, captured on production at the barrier and shipped to us. The QD's job is to *relay* `D_N`, not to mint a new distributed transaction.

So `MyTmGxact->gxid` stays `InvalidDistributedTransactionId` for the whole statement. We must audit the dispatch path for any assumption that the QD has a valid gxid (e.g. `dtxFormGid` at `cdbtm.c:438/1243`, gang‑loss checks at `:419/1408`) and ensure the standby‑reader path either skips them or supplies a benign sentinel. The reader‑QE path already tolerates "no local xid"; the new audit is on the **QD** side, which historically always had one.

### 3.2 Reader‑only gang (no writer QE)
Today a gang generally has a **writer** QE (`Gp_is_writer`) plus readers; readers can lean on the writer's `SharedLocalSnapshotSlot`. In standby mode there is **no writer** anywhere (a writer would, by definition, be allowed to write). Two implications:
- **Gang creation** must support an *all‑reader* gang. The dispatcher sets `Gp_is_writer=false` for every QE in the gang. The QE path at `setupQEDtxContext` (`cdbtm.c:1781`) already branches on `Gp_is_writer`; with no writer, every QE takes the reader branch (`isReaderQE = true`).
- **Snapshot source for readers.** Readers normally synchronize to the writer's snapshot via `SharedLocalSnapshotSlot` (segmate sync). With no writer, the **dispatched `D_N` + the segment's own hot‑standby local snapshot** must be the authority. We either (a) populate a `SharedLocalSnapshotSlot` from the dispatched context so the existing reader code path is reused unchanged, or (b) teach the reader path to take the local component directly from the segment's recovery snapshot under standby mode. Option (a) is less invasive (reuses `updateSharedLocalSnapshot`, `procarray.c`), and is the recommended PoC route. This is the **fiddliest** part of the milestone — segmate snapshot synchronization is historically subtle.

### 3.3 Setting up the context: `setupRegularDtxContext` / `setupQEDtxContext`
- **QD** (`setupRegularDtxContext`, `cdbtm.c:1658`): today it moves `LOCAL_ONLY → QD_DISTRIBUTED_CAPABLE` for a distributed‑capable QD. Add: when `gp_dr_replica` (and `RecoveryInProgress()`), a read‑only statement enters `DTX_CONTEXT_QD_STANDBY_READER` instead, and the distributed snapshot is sourced from the published horizon, not created.
- **QE** (`setupQEDtxContext`, `cdbtm.c:1718`): already selects `DTX_CONTEXT_QE_READER` for non‑writer executors with a dispatched snapshot. The main change is ensuring the **all‑reader** case (no writer present) is well‑formed — i.e. the `SharedLocalSnapshotSlot == NULL` / "not expecting distributed snapshot" guard (`cdbtm.c:1795`) is satisfied via option 3.2(a).

### 3.4 Local MVCC on each node = standard hot standby
The **local** visibility component is exactly PostgreSQL hot standby: the DR coordinator and each DR segment serve reads while the startup process replays, using a recovery snapshot built from `KnownAssignedXids`. The horizon work pins the *distributed* dimension to `D_N` and holds the *retention* floor (D‑1); the *local* dimension is stock hot standby. This is the part the main plan correctly assumed "already works" — it does, **per node**; the new work is making the *coordinator dispatch* on top of it.

### 3.5 Catalog reads during planning
Planning reads catalogs (pg_class, pg_statistic, …). On the DR these are replayed from production and are visible under the node's hot‑standby snapshot; planning is read‑only. The one wrinkle (already noted in the horizon doc §9 item 8): the coordinator should read catalogs consistently with the **as‑of‑`N`** image so plans match the segment data; treat the coordinator symmetrically (it holds `S_(-1),N`). No new mechanism beyond what the horizon design already specifies.

### 3.6 Connection / session setup must not write
Normal session startup can touch writable state (e.g. updating `pg_stat`, temp schemas, `nextval` for some defaults). In standby read mode all of these must be inert or rejected. This dovetails with main‑plan §5.3's read‑only enforcement, but the **session‑establishment** path specifically needs auditing (authentication, role setup, `search_path`, GUC assignments) so that merely *connecting* to the DR coordinator writes nothing. `default_transaction_read_only`‑style enforcement plus the existing hot‑standby write barriers cover most of it; the GP‑specific additions are temp‑table creation, sequence access, and any FTS/probe‑on‑connect behavior.

### 3.7 What still uses an entry‑DB singleton
`IS_QUERY_DISPATCHER() && !Gp_is_writer` maps to `isEntryDbSingleton` (`cdbtm.c:1783`) — the coordinator’s own "segment" for catalog‑only queries. Standby reads that touch only the coordinator (e.g. `SELECT * FROM gp_segment_configuration`, catalog‑only queries) run there and never dispatch — these should work early and are a good first demo (they exercise the QD standby context without needing the reader‑gang work).

---

## 4. Interaction with the rest of the feature
- **Read‑only enforcement (main plan §5.3)** is the gate *in front of* this mode: statements are classified; only read‑only ones enter `DTX_CONTEXT_QD_STANDBY_READER`. Writes/DDL/2PC/`nextval` are rejected before any dispatch.
- **Consistency / `D_N` (horizon design)** supplies the distributed snapshot this mode dispatches, and the holder (D‑1) protects the tuples it reads.
- **Topology (main plan M1)** supplies the DR‑local `gp_segment_configuration` the dispatcher uses to find DR segments (not production's).
- **FTS neutralized (main plan §5.5)**: FTS must not run its normal probe/update loop on the DR (it writes the catalog and assumes a writable coordinator). A DR‑local health monitor (read‑only) may track segment replay instead.
- **Gang lifecycle**: standby reader gangs are created/destroyed like normal reader gangs but never carry a writer; ensure gang‑reuse/caching logic doesn't expect a writer to be present.

---

## 5. Development sub‑plan (new milestone — call it **M2′ "Standby distributed read"**, sequenced between main‑plan M2 and M3)

This is a prerequisite for M3's "money demo": you cannot demonstrate a consistent distributed read on the DR until the DR coordinator can dispatch a distributed read at all. Suggested 4–6 weeks.

### SR‑0 — Spike: coordinator‑only standby reads (3–5 days)
- Bring up a single DR coordinator in hot standby with `gp_dr_replica`; make catalog‑only / entry‑DB‑singleton `SELECT`s work (`IS_QUERY_DISPATCHER() && !Gp_is_writer` path, §3.7) — **no segment dispatch yet**.
- **Exit:** `SELECT * FROM gp_segment_configuration`, `SELECT version()`, catalog queries succeed on the DR coordinator while it is in recovery; any write fails cleanly.

### SR‑1 — QD standby context, no gxid (1–1.5 weeks)
- Add `DTX_CONTEXT_QD_STANDBY_READER`; wire `setupRegularDtxContext` (`cdbtm.c:1658`) to enter it for read‑only statements under `gp_dr_replica`+recovery; assert `MyTmGxact->gxid == InvalidDistributedTransactionId` throughout; confirm commit/prepare/rollback paths are inert (existing guards).
- Audit and neutralize QD‑side assumptions of a valid gxid in the dispatch path (§3.1).
- **Exit:** the DR coordinator enters the new context for a `SELECT`, allocates no gxid, writes no WAL (verify with `pg_current_wal_lsn` unchanged across the statement), and errors on any write.

### SR‑2 — Reader‑only gang + dispatch of `D_N` (1.5–2 weeks; the fiddly core)
- Dispatcher: create an **all‑reader** gang (`Gp_is_writer=false` for every QE); pack `DtxContextInfo` with the published `D_N` (reuse the horizon §5.4 override at `cdbdtxcontextinfo.c:79`); choose the snapshot‑sync route from §3.2 (recommend populating `SharedLocalSnapshotSlot` from the dispatched context so the reader‑QE path is reused).
- QE: confirm `setupQEDtxContext` takes the `DTX_CONTEXT_QE_READER` branch for every segment with no writer present (§3.3), and the "no snapshot slot"/"not expecting distributed snapshot" guards (`cdbtm.c:1795`) are satisfied.
- **Exit:** a single‑table `SELECT` on the DR coordinator **dispatches to all DR segments** (in recovery), returns correct rows aggregated at the coordinator, with **no writer QE anywhere** and no WAL written on any node.

### SR‑3 — Real query coverage (1 week)
- Multi‑table joins, aggregates, motion (redistribute/broadcast), cursors — all read‑only — across the reader‑only gang; verify ORCA and the postgres planner both produce dispatchable read‑only plans; verify resource‑group/queue accounting works read‑only.
- **Exit:** a representative analytic query (joins + aggregation + motion) runs end‑to‑end on the DR and matches the result the same query gives on production at the corresponding horizon.

### SR‑4 — Session hardening (0.5–1 week)
- Audit session establishment (§3.6): connecting + setting GUCs + `search_path` writes nothing; temp tables and `nextval` rejected; no FTS‑on‑connect writes.
- **Exit:** a fresh connection that only ever reads leaves the cluster byte‑identical (no WAL, no catalog change); the read‑only error surface is consistent and friendly.

> After M2′, M3 (consistent read horizon) layers the *correctness* of the dispatched `D_N` on top of a dispatch path that already works; D‑1 (the holder) protects the tuples; the result is the consistent‑read money demo.

---

## 6. Risks specific to this milestone
| Risk | Severity | Mitigation |
|---|---|---|
| **Reader gang with no writer** breaks segmate snapshot synchronization (readers historically sync to a writer) | **High** | Populate `SharedLocalSnapshotSlot` from the dispatched `D_N`+local recovery snapshot so the existing reader path is reused (§3.2a); heavy testing of cursors/motion; this is the part most likely to surprise. |
| QD‑side code assumes a valid gxid (gid formatting, gang‑loss, dispatch bookkeeping) | **High** | Systematic audit (§3.1); benign sentinel or skip under standby context; assert‑heavy build. |
| A statement that *looks* read‑only still triggers a write (catalog side‑effects, `nextval`, temp, hint‑bit setting that dirties pages) | Medium→High | Hot‑standby already defers hint‑bit WAL; classify aggressively (§5.3); reject the rest; test the "no WAL written" invariant per statement. |
| Planner/ORCA emits a plan node requiring write (CTAS, INSERT…SELECT, `SELECT … FOR UPDATE`) | Medium | Reject at classification; ensure `SELECT … FOR UPDATE/SHARE` (row locks = writes) is refused on DR. |
| Gang caching/reuse expects a writer to persist between statements | Medium | Adjust gang lifecycle for all‑reader gangs; don't cache a writer that doesn't exist. |
| Catalog image skew between coordinator plan and segment data | Medium | Coordinator holds `S_(-1),N`; plan as‑of `N` (horizon §9.8). |
| Entry‑DB‑singleton vs dispatched‑read confusion | Low | Clear split (§3.7): coordinator‑only queries never dispatch; covered first in SR‑0. |

---

## 7. Exact touch points (GG7 `7.x`)
- **New context enum:** `src/include/cdb/cdbtm.h` (`DtxContext` — add `DTX_CONTEXT_QD_STANDBY_READER`).
- **QD context setup:** `src/backend/cdb/cdbtm.c` `setupRegularDtxContext` (`:1658`); ensure `currentDtxActivate` (`:~270`, gxid bump at `:306`) is **not** called in the new context; commit/prepare/rollback guards already at `:334/852/902` make those inert.
- **QE context setup:** `setupQEDtxContext` (`:1718`), reader branch + `Gp_is_writer` checks (`:1781‑1810`), entry‑DB‑singleton (`:1783`), snapshot‑slot guards (`:1795`).
- **Snapshot dispatch (use `D_N`):** `src/backend/cdb/cdbdtxcontextinfo.c:76‑81` (`createDtxSnapshot` override), `:297` (`DtxContextInfo_Deserialize`); shared local snapshot via `updateSharedLocalSnapshot` (`procarray.c`).
- **Reader‑only gang:** dispatcher/gang code (`src/backend/cdb/dispatcher/…`, `cdbgang*.c`) — set `Gp_is_writer=false` for all QEs; gang lifecycle for writer‑less gangs.
- **Local visibility (reuse, hot standby):** `GetSnapshotData` recovery branch (`procarray.c:2343+`), `KnownAssignedXids*`.
- **Read‑only gate:** the `gp_dr_replica` GUC + `dr_replica.signal` (main plan §5.3) + statement classification in the exec entry path.

---

## 8. Summary
The missing mode — a coordinator in recovery that dispatches read‑only distributed queries — is real and is **not** delivered by hot standby alone, because Greengage dispatch normally allocates a gxid (a shared‑memory write at `cdbtm.c:306`) and opens a writer gang. The design adds **one new QD distributed context** (`DTX_CONTEXT_QD_STANDBY_READER`) that allocates no gxid, writes no WAL, and dispatches the **pre‑captured `D_N`** to **reader‑only gangs**, reusing the already‑existing `DTX_CONTEXT_QE_READER` executor path and stock per‑node hot standby for local visibility. It slots in as **M2′**, between read‑only enforcement (M2) and consistent reads (M3): M2 decides *what may run*, M2′ makes the distributed read *physically dispatch* on an all‑recovery cluster, and M3 + D‑1 make the result *consistent and stable*. The riskiest piece is the **writer‑less reader gang's snapshot synchronization** (§3.2) — that is where SR‑2 should concentrate.
