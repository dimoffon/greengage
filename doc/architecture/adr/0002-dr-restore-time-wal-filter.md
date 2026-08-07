# ADR-0002: Restore-Time WAL Filtering (`gg_walfilter`) Replaces the In-Backend Redo Filter

- **Status:** **Superseded in part by ADR-0003 (2026-08-03)** — the *default-mechanism*
  decision (restore-time filtering replacing the in-backend redo filter) was reversed
  the same day after re-weighing the principal trade-off recorded below ("protection
  moved from engine invariant to configuration"). The in-backend filter is the default
  again. The **tool survives**: `gg_walfilter` was kept, ported from Python to C at
  `src/bin/gg_walfilter/` (this ADR's own "C implementation" alternative), with the
  mechanism, rule semantics, exit codes and boundary-state design specified here.
- **Date:** 2026-08-03
- **Supersedes:** *(historical)* the *apply-time redo filter* mechanism of ADR-0001 D3.
  The other half of D3 — the frozen-tuple topology seed — stands unchanged, as does
  every other ADR-0001 decision (D1, D2, D4–D10).
- **Reference implementation studied:**
  [cybertec-postgresql/walbouncer](https://github.com/cybertec-postgresql/walbouncer) —
  a streaming-replication proxy that filters a WAL stream per standby (by tablespace /
  database) by replacing unwanted records with no-ops.
- **Note:** `gg_recovery` is now named `ggdr` (ADR-0004 D5); references below are pre-rename.

---

## Context

ADR-0001 D3 made the DR cluster self-describing with an **apply-time redo filter**
compiled into the backend: a hook in the redo loop skipped `rm_redo` for records whose
block references all fell in the protected topology-catalog set
(`dr_redo_filter.c`), and a guard in `relmap_redo` `FATAL`ed on a forbidden remap.
It worked (validated end-to-end), but it had structural costs:

- **Engine surface.** The hook sat on the hottest recovery path (every record of every
  replayed segment), plus a second hook and an `#include` inside `relmapper.c`. Every
  upstream merge of `xlog.c`/`relmapper.c` had to carry it.
- **Single-purpose.** The mechanism could serve no other filtering need
  (walbouncer-style partial replicas, thin dev copies, offline redaction), although the
  underlying problem — "neutralize selected records of a physical WAL stream" — is
  generic.
- **Opaque.** Deciding *what would be filtered* required a running DR backend; there was
  no way to examine a segment offline.
- **A latent hazard.** The skip left `checkXLogConsistency()` running: a filtered record
  carrying `XLR_CHECK_CONSISTENCY` (production running `wal_consistency_checking`) would
  compare production's page image against the deliberately untouched DR page and `FATAL`.

ADR-0001 D3 had *rejected* a "WAL proxy / rewriter" on the grounds that bytes cannot be
deleted from physical WAL and that rewriting "corrupts the canonical archive or forces
maintaining a second, divergent WAL stream". Re-examination showed the objection does
not apply to **same-length record rewriting on the fetch path**:

- walbouncer demonstrates that a record can be replaced by an equal-length `XLOG_NOOP`
  without disturbing LSN geometry, the `xl_prev` chain, or the standby's reader;
- performing the rewrite in `restore_command` — *after* the archive, *per replica* —
  leaves the archive canonical. Only the copies inside a DR node's `pg_wal` differ,
  and those are private to that node.

## Decision

Replace the in-backend filter with **`gg_walfilter`** (`gpMgmt/bin/gg_walfilter`), a
standalone, dependency-free Python utility invoked by `restore_command`, and remove the
backend hooks entirely.

1. **Mechanism — same-length `XLOG_NOOP` rewrite.** A record is neutralized iff it has
   at least one block reference and *every* block reference matches the filter rules
   (identical semantics to the old `DRRedoShouldFilter`). The rewrite keeps
   `xl_tot_len` / `xl_prev` / `xl_xid` (framing and standby xid tracking intact), sets
   `rmgr=XLOG, info=XLOG_NOOP`, replaces the block references with an all-zeros
   main-data payload (`xlog_redo` asserts non-FPI XLOG records carry none; the swap also
   clears `XLR_CHECK_CONSISTENCY`, eliminating the consistency-check hazard), and
   recomputes the CRC. Records spilling across a segment boundary carry their
   replacement tail to the next fetch via a small state file, re-derived from the
   archive when missing (restartpoint refetch, rebuilt node) — deterministically, so a
   refetch always reproduces the same bytes.

2. **Generic rules, DR preset.** `--exclude-relfilenode SPC/DB/REL`,
   `--exclude-database OID`, `--exclude-tablespace OID` (walbouncer's dimensions), and
   `--protect-mapped-oid OID` resolved through the node's own
   `global/pg_filenode.map`. `--gp-dr-topology` is the DR preset: the eight protected
   topology-catalog OIDs plus the forbidden-remap guard. Subcommands: `restore`
   (restore_command entry), `filter` (offline/in-place), `inspect` (dry-run forensics).

3. **The relmap guard moves into the tool.** `gg_walfilter` decodes shared
   `XLOG_RELMAP_UPDATE` records; a change to a protected catalog's relfilenode refuses
   the segment (exit 3) and writes a sticky `HALT` sentinel. Replay stalls at the end
   of the previous segment and the node **stays up serving its last state** (the old
   guard killed the postmaster with `FATAL`); the operator action is unchanged:
   re-create + re-seed the node. Benign shared remaps pass through and update the
   filter's resolution mid-stream.

4. **The two filter-bypassing WAL paths are closed:**
   - *pg_wal contents* (base-backup `-X stream` WAL, archive re-fetches) are read by
     recovery without consulting `restore_command` — `gg_recovery create-replica` now
     runs `gg_walfilter filter` over every segment in each node's `pg_wal` before
     arming DR mode (also seeding the boundary state);
   - *streaming replication* — the one remaining engine-side piece: in DR mode the
     startup process refuses to launch a walreceiver even if `primary_conninfo` is set
     (warn-once in `WaitForWALToBecomeAvailable`), keeping the node on the filtered
     archive path.

5. **Fail closed.** A segment that does not parse (structural damage, wrong page magic,
   broken `xl_prev` chain, a to-be-rewritten record failing its CRC) is *never* passed
   through: `restore_command` fails and recovery waits, loudly.

**Alternatives considered:**

- *Keep the in-backend filter* — rejected for the reasons in Context; the restore-time
  filter provides the same protection semantics with the redo loop stock.
- *A walbouncer-style streaming proxy* — DR transport is archive-based by design
  (ADR-0001 D2, no replication slots); a proxy would reintroduce a live connection and
  a new daemon for no gain at archive-cadence freshness.
- *A C implementation (e.g. under `src/bin`, reusing `xlogreader`)* — faster and
  format-proof, but heavier to build/ship and not requested; the Python utility lives
  in gpMgmt beside `gg_recovery`, needs only the stdlib, and reads the WAL geometry
  from segment headers rather than hardcoding it. Revisit if catch-up throughput ever
  matters (see Costs).

## Consequences

### Positive

- **The engine's WAL path is stock again.** `dr_redo_filter.c`/`.h`, the redo-loop
  hook, the `relmapper.c` guards and the mock unit test are gone; the only remaining
  engine-side DR-WAL logic is the ~15-line streaming refusal. Upstream merges of
  `xlog.c`/`relmapper.c` no longer carry filter logic.
- **Reusable tooling.** The same utility does partial-replica-style exclusion by
  database/tablespace, offline neutralization of local segments, and read-only
  `inspect` — usable for forensics and tests without any server.
- **The consistency-check hazard is gone** (`XLR_CHECK_CONSISTENCY` is cleared by the
  rewrite; the old skip-redo left the check running against untouched pages).
- **Decisions are observable offline**: `gg_walfilter inspect <segment>` shows exactly
  what would be rewritten and why; unit tests (`src/test/dr/test_gg_walfilter.py`)
  validate the rewrite byte-level with no cluster.
- **Halt keeps reads available.** On a forbidden remap the node stalls but keeps
  serving its last consistent state, instead of dying with `FATAL`.

### Negative / accepted costs

- **Protection moved from engine invariant to configuration.** The old filter held for
  any WAL source; the new one holds because `gg_recovery` wires `gg_walfilter` into
  `restore_command`, filters `pg_wal` at create time, and the engine refuses streaming.
  A hand-edited `restore_command` bypassing `gg_walfilter` silently loses topology
  protection (`gg_recovery` prints a loud warning if you override it that way; manually
  dropped-in segments are on the operator). This is the principal trade-off.
- **DR `pg_wal` is no longer byte-identical to production's.** ADR-0001 listed
  byte-identical `pg_wal` as a positive; it now holds only for the *archive* (which is
  the copy that matters for PITR and forensics). `pg_waldump` on a DR node shows
  `XLOG/NOOP` where production wrote topology updates.
- **Fetch-path CPU.** Pure-Python record walking costs roughly 0.5–2 s per 64 MiB
  segment (a few µs/record; CRC work only on rewritten records). Irrelevant at
  archive-cadence trickle, but bulk catch-up over hundreds of segments is measurably
  slower than the in-backend filter (which was ~free). A C rewrite is the escape hatch.
- **Boundary-continuation state is new complexity** the in-backend filter simply did
  not need (a spilled rewrite's CRC covers bytes in the next segment). It is small,
  deterministic, covered by unit tests, and self-healing via archive lookback — but it
  exists.
- **Slightly earlier stop on a forbidden remap:** the whole segment containing the
  offending relmap update is refused, so replay stops at the previous segment boundary
  rather than at the exact prior record. Operationally identical (the node was about to
  be re-created either way).

### Neutral / notes

- Filter semantics (block-reference matching, "any non-protected block ⇒ apply") are
  intentionally identical to the old filter, so ADR-0001's M1 validation scenarios carry
  over; the end-to-end fixture (`src/test/dr/`) exercises the new path unchanged.
- The old FATAL-on-remap behavior stopped replay *mid-segment right at the record*; both
  behaviors leave the replica strictly before the forbidden change.
- AO-table WAL (`RM_APPEND_ONLY_ID`) carries its relfilenode in rmgr-specific data, not
  block references — invisible to generic database/tablespace exclusion rules. The DR
  preset is unaffected (the protected catalogs are heap), but reuse for AO-aware
  filtering would need an AO decoder in the utility.

## Validation

- `src/test/dr/test_gg_walfilter.py` — 26 unit tests over synthetic segments:
  CRC-32C vectors, selective rewrite + reparse + CRC round-trip, framing preservation,
  page-crossing records, boundary spill (state chaining, stateless archive lookback,
  archive-gap passthrough), relmap guard (benign / forbidden / non-shared), stream
  ends (XLOG_SWITCH, recycled page, zeroed tail), fail-closed on broken `xl_prev` and
  on pre-rewrite CRC mismatch, and the `restore`/`filter` CLIs end-to-end including
  the sticky HALT. Run by the DR fixture before it builds the cluster.
- The `filter → restore` boundary-state handoff was additionally verified with the
  previous segment absent from the archive (lookback impossible), proving the state
  chain alone produces the zeroed continuation.
- The end-to-end docker fixture (`src/test/dr/`) validates the DR behavior unchanged:
  production's topology change invisible on DR, post-backup user tables visible,
  DR-local hostnames survive replay.

## References

- ADR-0001 (`0001-dr-read-replica.md`) — D3 for the superseded mechanism and the
  frozen-tuple seed that remains.
- Architecture description: `doc/architecture/greengage-dr-read-replica.md` §4.2.
- walbouncer: <https://github.com/cybertec-postgresql/walbouncer> — WAL-stream
  filtering by replacing records with no-ops (streaming-proxy form).
