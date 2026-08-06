# Greengage 7 DR — SR‑2 Analysis
## Feeding a writer‑less reader gang a coherent snapshot (the riskiest piece of standby distributed read)

*Deep‑dive on SR‑2 of the Standby Distributed Read Mode milestone (M2′). SR‑2 is where subtle, intermittent visibility bugs across motion and cursors will appear if we get it wrong. Grounded in the GG7 `7.x` code; the key functions are `updateSharedLocalSnapshot` (`procarray.c:1647`), `readerFillLocalSnapshot` (`procarray.c:1753`), `copyLocalSnapshot` (`:1718`), and `readSharedLocalSnapshot_forCursor` (`sharedsnapshot.c:666`).*

---

## 1. How snapshot synchronization works **today** (and why it assumes a writer)

In a normal Greengage gang, the QEs on a segment share **one** local snapshot so that all slices of a query (and all readers behind a cursor) see identical visibility. The mechanism is a per‑session **`SharedLocalSnapshotSlot`** in shared memory (`sharedsnapshot.h`), filled by the **writer** QE and consumed by the **reader** QEs:

**Writer side — `updateSharedLocalSnapshot` (`procarray.c:1647‑1696`):** the writer takes its own `GetSnapshotData` result and publishes it into the slot:
```
slot->snapshot.xmin/xmax/xcnt/xip[]  = writer's snapshot      (:1649‑1661)
slot->snapshot.curcid                = writer's curcid        (:1664)
slot->distributedXid                 = dtxContextInfo->distributedXid   (:1672)
slot->segmateSync                    = dtxContextInfo->segmateSync      (:1673)
slot->ready                          = true                   (:1674)
```
It also stamps `writer_proc = MyProc`, `writer_xact = MyPgXact` (set when the slot is created, `sharedsnapshot.c:462‑463`).

**Reader side — `readerFillLocalSnapshot` (`procarray.c:1753`):** a reader QE does **not** call `GetSnapshotData`; it **spins waiting for the writer** to publish a matching slot:
```
for (;;) {
    if (QEDtxContextInfo.segmateSync == slot->segmateSync && slot->ready) {
        if (QEDtxContextInfo.distributedXid != slot->distributedXid) elog(ERROR, …);
        copyLocalSnapshot(snapshot);          // copy xmin/xmax/xip/curcid out of the slot
        SetSharedTransactionId_reader(slot->fullXid, …);   // adopt the writer's local xid
        return;
    }
    if (timed out) elog(ERROR, "… timed out waiting for Writer to set the shared snapshot");
    pg_usleep(1ms); CHECK_FOR_INTERRUPTS();
}
```

Three things to notice, because each is a landmine for an all‑reader gang:
1. **The reader blocks until `slot->ready` and a `segmateSync` match.** With **no writer**, nobody ever sets `ready`/`segmateSync`, so every reader would spin and then `ERROR: timed out waiting for Writer`. *This is the first thing that breaks.*
2. **The reader adopts the writer's local xid** (`SetSharedTransactionId_reader`, via `slot->fullXid`). The whole point of the writer is to pin a single local xid so `TransactionIdIsCurrentTransactionId()` is coherent across the gang. A standby reader must instead behave as a **hot‑standby backend with no transaction xid at all** — so the "adopt writer's xid" step is both impossible and unnecessary, but the code path expects it.
3. **`segmateSync` exists to disambiguate multiple statements that share one distributed‑xid/cid but use different local‑xids (MPP‑3228).** On a replica there are no local‑xid changes to disambiguate (we assign none), but the **counter still has to be threaded** so cursor readers don't grab a stale slot (see §4).

**Cursors take a different path — `readSharedLocalSnapshot_forCursor` (`sharedsnapshot.c:666`):** to stop the writer racing ahead of cursor readers, the writer **dumps** its snapshot to a side structure keyed by `segmateSync` (the `SnapshotDump` array / a tempfile), and cursor readers fetch the dump matching their `segmateSync` rather than the live slot. Again writer‑centric.

---

## 2. The core problem, stated precisely

On the DR, **every QE is a reader** (a writer, by definition, could write). So:
- there is no process to run `updateSharedLocalSnapshot` and set `ready`/`segmateSync`/`fullXid`;
- there is no writer local xid for readers to adopt;
- the *authoritative* visibility is **not** "the writer's local snapshot" but **"the published distributed snapshot `D_N` (from the horizon) + each segment's own hot‑standby recovery snapshot `S_i,N`"** (per the horizon design §5.5 and D‑1).

So SR‑2 must make the reader gang obtain a coherent `{local recovery snapshot + `D_N`}` **without** a writer, while keeping all slices of one query — and all readers behind a cursor — on the **same** local snapshot.

---

## 3. Two routes (and which to pick)

### Route A — Synthesize the slot: elect a "snapshot leader" QE that fills the slot like a writer would
One QE in the gang (e.g. the first/`gangMember 0`, or a designated "standby leader") runs a thin analog of `updateSharedLocalSnapshot`: it builds the **local hot‑standby snapshot** (the `S_i,N` capture path — `KnownAssignedXidsGetAndSetXmin`, horizon §5.2), publishes it into `SharedLocalSnapshotSlot` with `ready=true` and the dispatched `segmateSync`, and the other readers consume it through the **existing, unmodified `readerFillLocalSnapshot`**.

- **Pro:** maximum reuse — `readerFillLocalSnapshot`, `copyLocalSnapshot`, cursor dump path all work unchanged; lowest risk of *new* visibility logic; the segmate handshake keeps slices/cursors coherent exactly as today.
- **Con:** the leader (gang member 0, §below) must handle `SetSharedTransactionId_reader` (the "adopt writer xid" step) gracefully when there is **no** xid — a standby snapshot has no current transaction xid, so `fullXid` must be a benign `InvalidFullTransactionId` and `TransactionIdIsCurrentTransactionId()` must return false for everything (true on a replica — the backend owns no xid). The leader is *not* a writer (assigns no xid, writes no WAL); it is just the snapshot publisher. Need to ensure nothing else keys off `writer_proc`/`writer_xact` in a way that assumes writability (audit: `writer_xact` is read for xid‑in‑progress checks; on a replica it should be `MyPgXact` of the leader with no xid → harmless, but verify).

### Route B — Bypass the slot for standby reads: teach the reader path to build its own snapshot directly
Under standby mode, `readerFillLocalSnapshot` returns a snapshot built **locally** from the segment's `KnownAssignedXids` (the same `S_i,N` machinery) instead of waiting on the slot, and visibility uses the dispatched `D_N` for the distributed dimension.

- **Pro:** no leader election; conceptually cleaner ("each QE just takes the recovery snapshot"); no dependence on the writer‑centric handshake at all.
- **Con — the coherence trap:** if each reader independently calls the recovery‑snapshot builder, **two slices on the same segment can get *different* `{xmin,xmax,xip}`** because replay advanced between the two calls. That breaks the invariant that all slices of one query share one snapshot — exactly the bug class SR‑2 exists to prevent. To use Route B safely you must still **compute the local snapshot once per (segment, statement) and share it**, which re‑introduces a shared structure and a sync point… i.e. you back into Route A's slot anyway, just reinvented.

**Recommendation: Route A.** The slot + `segmateSync` handshake is *precisely* the "compute once, share across slices and cursor readers" primitive we need; the only thing wrong with it for us is that the filler is normally a writer. Replace the *filler* (a non‑writing snapshot leader) and reuse everything else. Route B's apparent simplicity evaporates the moment you need slice/cursor coherence, and it duplicates logic.

> Net: **keep the shared‑snapshot slot; change who fills it and what it contains** — a leader QE publishes the local recovery snapshot (no writer xid), readers consume via the unchanged handshake, and the distributed dimension comes from the dispatched `D_N`.

> **Decided — the leader is gang member 0 (per segment).** Rather than "first arriver fills," a **fixed** filler: on each segment, gang member 0 occupies "the writer's seat" as a *non‑writing* publisher; members 1..N are pure consumers via the unchanged `readerFillLocalSnapshot`. Properties:
> - **Two layers, two leaders (do not conflate).** Gang member 0 leads only the **local `segmateSync` layer** — it publishes *that segment's* `CaptureRecoveryHorizonSnapshot()` into *that segment's* slot. The **distributed** layer is still `D_N`, relayed by the **QD** and dispatched to every segment. Per segment: one member‑0 filler; across the cluster: one QD supplying `D_N`.
> - **No fill race** → we do **not** need a CAS/`ready`‑check‑under‑lock guard. One QE fills deterministically; the rest wait deterministically (the existing handshake's model). This is the main reason to prefer a fixed leader over first‑arriver.
> - **Ordering dependency (the wrinkle):** members 1..N must block until member 0 publishes for the current `segmateSync`. The followers' path is already exactly this spin (`readerFillLocalSnapshot`); the new work is a **branch keyed on gang position** at gang setup so member 0 runs the publish path and 1..N run the consume path.
> - **Hung/crashed leader fails the statement, not hangs it:** followers hit the existing `segmate_timeout_us` and error (reworded for standby). A fixed leader is thus a tiny per‑statement SPOF — acceptable for a read‑only replica (the statement retries); first‑arriver would have let a surviving QE fill, but at the cost of the race guard.
> - **Lifecycle:** leadership is **per‑gang and stable for the gang's life**. Cached reader gangs keep member 0 as leader across statements; member 0 **republishes for each new `segmateSync`** (every dispatched statement carries a fresh one from the QD). Gang torn down/rebuilt → the new member 0 leads. No re‑election; this disposes of the cached‑gang lifecycle concern.

---

## 4. The coherence requirements SR‑2 must preserve (the test checklist)

These are the invariants that, if violated, produce the "subtle, intermittent" bugs:

1. **All slices of one statement on one segment see one local snapshot.** Guaranteed by Route A's single fill + `segmateSync` match. Test: a multi‑slice plan (redistribute/broadcast motion) where a concurrently‑replaying tuple must be uniformly visible or invisible across slices.
2. **Cursor readers don't see the gang "race ahead."** The cursor path (`readSharedLocalSnapshot_forCursor`, dump keyed by `segmateSync`) must still work: the leader must **dump** the snapshot per `segmateSync` for cursor readers, exactly as the writer does today. Test: `DECLARE … CURSOR; FETCH` interleaved with continued WAL replay; the cursor must keep its original `D_N`+local snapshot for its whole life (which also means the **horizon holder must pin** that cursor's horizon — ties to D‑1 §4 pinning).
3. **`segmateSync` still increments per dispatched statement** so a new statement on the same gang doesn't match a stale slot. The dispatcher already generates `segmateSync`; standby mode must keep threading it even though there are no local‑xid changes to disambiguate. Test: two statements on the same cached reader gang in the same session — the second must not consume the first's slot.
4. **`distributedXid` consistency check still passes.** `readerFillLocalSnapshot` errors if `QEDtxContextInfo.distributedXid != slot->distributedXid` (`:1808`). On a replica the "distributed xid" of the *read* is not a freshly minted gxid — it's tied to `D_N`. Decide what goes in `dtxContextInfo->distributedXid` for a standby read (likely a sentinel/`Invalid`, since we mint no gxid) and ensure the leader stamps the **same** value so the check passes. Test: assert no spurious "transaction ID doesn't match between reader and writer gang" errors.
5. **No reader adopts a transaction xid.** `SetSharedTransactionId_reader` must result in the reader owning **no** xid (`InvalidFullTransactionId`), and `TransactionIdIsCurrentTransactionId()` must be false for all xids (correct on a replica — the backend writes nothing). Test: a read that touches a tuple written by the (replayed) transaction whose xid would, on a primary, be "current" — it must be judged by `D_N`, not treated as self.
6. **Timeout path doesn't fire spuriously.** Since the leader fills the slot promptly, readers should match on the first or second spin. Test under load that `GetSnapshotData timed out waiting for Writer` never appears in standby mode (and if the leader is slow, the error message should be reworded for standby — "waiting for snapshot leader," not "Writer").

---

## 5. Interaction with the rest of the feature
- **`D_N` source (horizon §5.4):** the **distributed** snapshot dispatched in `DtxContextInfo` is `D_N` from the published horizon; SR‑2 handles only the **local** snapshot coherence. Both are needed for a correct read; SR‑2 must not accidentally let a reader build a *distributed* snapshot locally (it must use the dispatched one).
- **`S_i,N` capture (horizon §5.2):** the leader's local snapshot is exactly the recovery snapshot that the barrier capture uses; reuse `CaptureRecoveryHorizonSnapshot()` so the leader and the captured `S_i,N` are built by identical code.
- **Holder / cursors (D‑1 §4):** a cursor pins its horizon for its lifetime; the D‑1 holder must keep `S_i,N`'s xmin retained for that whole time, or the cursor's snapshot could reference vacuumed tuples. SR‑2 (cursor dump) and D‑1 (pin) must be tested *together*.
- **Read‑only enforcement (main plan §5.3a):** the leader is **not** a writer — it must be structurally prevented from assigning an xid or writing WAL (it's a reader that happens to publish the slot). This should fall out of standby mode, but assert it.

---

## 6. Risks specific to SR‑2
| Risk | Severity | Mitigation |
|---|---|---|
| Readers spin → `timed out waiting for Writer` because nobody fills the slot | **Certain without the fix** | Route A leader fills slot with `ready=true` + dispatched `segmateSync`. |
| Two slices get different local snapshots (replay advanced between calls) → non‑uniform visibility | **High** (the core bug class) | Single fill per (segment, statement) via the slot; never let readers build local snapshots independently (reject Route B). |
| Cursor reader sees replay race‑ahead → wrong/missing rows mid‑cursor | High | Keep the `segmateSync`‑keyed dump path; pin the cursor's horizon in the D‑1 holder for the cursor's life; joint test. |
| `distributedXid` mismatch check throws spuriously | Medium | Define the standby read's `distributedXid` sentinel; leader stamps the same value; assert. |
| Something keys off `writer_proc`/`writer_xact` assuming writability | Medium | Audit all readers of `writer_xact` (xid‑in‑progress, `TransactionIdIsCurrentTransactionId`); on a replica the leader owns no xid → must be harmless; assert false‑for‑all. |
| Gang/leader lifecycle on cached reader gangs | **Resolved** | Leader = gang member 0, stable per‑gang for its life; republishes per `segmateSync`; new member 0 on rebuild. No re‑election. Residual: member‑0 crash fails the statement (per‑statement SPOF, acceptable for RO replica; statement retries). |
| Error messages reference "Writer" and confuse standby operators | Low | Reword the standby‑mode timeout/diagnostics. |

---

## 7. SR‑2 sub‑steps (refines M2′ SR‑2)
1. **Gang‑member‑0 leader + non‑writing fill.** Branch at gang setup on gang position: **member 0** runs `updateStandbyLocalSnapshot` (a sibling of `updateSharedLocalSnapshot`) that publishes the **recovery** snapshot (`CaptureRecoveryHorizonSnapshot`) with `ready=true`, the dispatched `segmateSync`, sentinel `distributedXid`, `fullXid=Invalid`; **members 1..N** run the unchanged consume path. Member 0 republishes per `segmateSync`. *Exit:* followers match on first spin; no "waiting for Writer" timeouts; no fill race (no CAS guard needed).
2. **Reader path tolerates no‑xid.** Ensure `readerFillLocalSnapshot`/`SetSharedTransactionId_reader` leave the reader xid‑less and `TransactionIdIsCurrentTransactionId()` false‑for‑all. *Exit:* a single‑segment read returns correct rows using `D_N` for distributed visibility.
3. **Multi‑slice coherence (motion).** Validate that redistribute/broadcast plans keep all slices on one snapshot. *Exit:* the §4.1 motion test passes under concurrent replay.
4. **Cursor coherence.** Wire the `segmateSync` dump path for standby; pin the cursor horizon in the D‑1 holder. *Exit:* the §4.2 cursor test passes; the cursor's view is stable for its whole life while replay advances.
5. **Diagnostics + sentinels.** Reword "Writer" messages; lock the `distributedXid` sentinel and `segmateSync` threading. *Exit:* no spurious mismatch/timeout errors under load; clean diagnostics.

---

## 8. Exact touch points (GG7 `7.x`)
- **Writer/leader fill:** `src/backend/storage/ipc/procarray.c` `updateSharedLocalSnapshot` (`:1647`) → add standby sibling `updateStandbyLocalSnapshot`; slot creation writer back‑refs (`sharedsnapshot.c:462‑463`).
- **Reader consume:** `readerFillLocalSnapshot` (`procarray.c:1753`), `copyLocalSnapshot` (`:1718`), `SetSharedTransactionId_reader` (grep `SetSharedTransactionId_reader`).
- **Cursor path:** `readSharedLocalSnapshot_forCursor` (`sharedsnapshot.c:666`), the `SnapshotDump` array (`sharedsnapshot.h:20‑31`), dump fill (`sharedsnapshot.c:652+`).
- **Slot struct / lifecycle:** `src/include/utils/sharedsnapshot.h` (`SharedSnapshotSlot`), `SharedSnapshotAdd`/`Remove`/`Lookup` (`sharedsnapshot.c`).
- **Local recovery snapshot source:** `CaptureRecoveryHorizonSnapshot` (new, horizon §5.2) reusing `KnownAssignedXidsGetAndSetXmin` (`procarray.c:4961`).
- **Distributed snapshot (dispatched `D_N`):** `cdbdtxcontextinfo.c:79` (createDtxSnapshot override), `:297` (deserialize); `QEDtxContextInfo`.
- **Gang (all‑reader, leader):** `src/backend/cdb/dispatcher/cdbgang*.c` — set `Gp_is_writer=false` for all; designate leader.

---

## 9. Summary
Greengage already has exactly the primitive SR‑2 needs — a per‑session shared snapshot slot that forces all slices and cursor readers onto **one** local snapshot via a `segmateSync` handshake (`readerFillLocalSnapshot`, `procarray.c:1753`). The only thing wrong with it for the DR is that the **filler is a writer**, and a DR gang has no writer. So **keep the slot and the handshake; replace the filler** with a non‑writing **snapshot leader** that publishes the segment's **hot‑standby recovery snapshot** (no xid, sentinel `distributedXid`), while the **distributed** dimension comes from the dispatched **`D_N`**. The tempting "just let each reader build its own snapshot" route (B) silently breaks slice/cursor coherence and ends up reinventing the slot — so we reject it. The real work and the real risk are in **coherence under concurrent replay**: multi‑slice motion (§4.1) and cursors (§4.2, jointly with the D‑1 holder pin) are where SR‑2 must prove itself.
