# ADR-0003: The In-Backend Redo Filter Is the Default Again; `gg_walfilter` Becomes C Tooling in src/bin

- **Status:** Accepted — implemented on branch `7.x-dr`
- **Date:** 2026-08-03
- **Supersedes:** the *default-mechanism* decision of ADR-0002 (restore-time filtering
  via `gg_walfilter` in `restore_command` as the topology-protection mechanism).
  ADR-0002's tool itself is retained — see D2. ADR-0001 D3 is thereby restored as
  written (with one fix, D1.2).
- **Note:** the gate this ADR refers to (`gp_dr_replica` + `dr_replica.signal`) is now
  `EnableHotStandby && RecoveryInProgress()` — see ADR-0004. The decision itself stands.
- **Note:** `gg_recovery` is now named `ggdr` (ADR-0004 D5); references below are pre-rename.

---

## Context

ADR-0002 replaced the in-backend apply-time topology redo filter (`dr_redo_filter.c`,
ADR-0001 D3) with `gg_walfilter`, a Python utility invoked from `restore_command`, to
get the redo loop stock, gain offline observability, and eliminate a latent
consistency-check hazard. ADR-0002 itself named its principal accepted cost:

> **Protection moved from engine invariant to configuration.** … A hand-edited
> `restore_command` bypassing `gg_walfilter` silently loses topology protection. …
> This is the principal trade-off.

Re-weighing the trade-offs led to reversing the default:

- **Configuration is the wrong place for a correctness invariant.** The DR replica's
  defining property — production's WAL never overwrites the DR-local topology — held
  only as long as three separately-configured things stayed wired: `gg_walfilter` in
  `restore_command`, the create-time `pg_wal` pre-filter pass, and the engine-side
  streaming refusal. Any gap (operator edit, tooling drift, a segment dropped into
  `pg_wal` by hand) failed *open*. The in-backend filter fails *closed* by
  construction: it keys off `gp_dr_replica` + `dr_replica.signal` and holds for every
  WAL source — archive fetch, `pg_wal` contents, streaming — with nothing to wire.
- **The engine surface saved was smaller than it looked.** Removing the filter still
  left an engine-side DR-WAL branch (the streaming refusal) that upstream merges had
  to carry; the redo-loop hook is one predicate call and `relmapper.c` carries two
  small guard calls. Meanwhile the restore path gained real complexity of its own
  (boundary-continuation state, archive lookback, a state file, sticky HALT) that the
  in-backend filter simply never needed.
- **Fetch-path cost was real.** 0.5–2 s of pure-Python parsing per 64 MiB segment made
  bulk catch-up measurably slower than the ~free in-backend filter.
- **DR `pg_wal` stopped being byte-identical to production's**, weakening forensics
  and the "one faithful stream" property ADR-0001 valued.

What ADR-0002 got right and must not be lost: the **consistency-check hazard** it
found in the old filter (a filtered record carrying `XLR_CHECK_CONSISTENCY` would
compare production's page image against the deliberately untouched DR page and
`FATAL`), and the **offline observability / generic filtering tool** it built, with
its 26-scenario test suite. ADR-0002 had also already marked the exit: *"A C
implementation (e.g. under `src/bin`, reusing xlogreader)… Revisit if catch-up
throughput ever matters."*

## Decision

1. **Restore the in-backend redo filter as the default and only required
   topology-protection mechanism** (ADR-0001 D3 as written): `dr_redo_filter.c`/`.h`
   back in the tree, the guarded `rm_redo` dispatch back in `StartupXLOG()`, the
   `DRRejectForbiddenRemap()` / `DRInvalidateProtectedRelfilenodes()` guards back in
   `relmap_redo()`, and the cmockery unit tests back in
   `src/backend/access/transam/test/`. No new GUC: the filter remains keyed off
   `IsDRReplicaMode()` (`gp_dr_replica` + `dr_replica.signal`), an engine invariant,
   not configuration.

   1. `gg_recovery create-replica` reverts to the plain default
      `restore_command = 'cp <archive>/seg%c/%f %p'`; the hard dependency on
      `gg_walfilter`, the create-time `pg_wal` pre-filter pass, and the
      override warning are gone. DR `pg_wal` is byte-identical to production again.
   2. **Fix ADR-0002's Context hazard while restoring:** the redo-loop skip now
      covers `checkXLogConsistency()` too — a filtered record's consistency check is
      skipped together with its redo (`xlog.c`, the `DRRedoShouldFilter` block). This
      removes the one *bug* ADR-0002 held against the in-backend filter.
   3. **Remove the engine-side streaming-replication refusal** ADR-0002 added to
      `WaitForWALToBecomeAvailable()`. Its only rationale was that `gg_walfilter`
      cannot see streamed WAL; the apply-time filter protects streamed WAL exactly
      like archived WAL, so the branch (and its upstream-merge burden) is unnecessary.

2. **Keep `gg_walfilter`, rewritten in C at `src/bin/gg_walfilter/`,** built and
   installed with the server (`$GPHOME/bin/gg_walfilter`); the Python script in
   `gpMgmt/bin` is deleted. Feature parity with ADR-0002's specification: subcommands
   `restore` / `filter` / `inspect`; rules `--exclude-relfilenode`,
   `--exclude-database`, `--exclude-tablespace`, `--protect-mapped-oid`,
   `--gp-dr-topology`; exit codes 0/1/2/3 with the sticky `HALT` sentinel; same-length
   `XLOG_NOOP` rewrite with CRC recomputation; fail-closed parsing; boundary-spill
   state + deterministic archive lookback. Its **role changes**: auxiliary tooling —
   offline inspection and filtering (partial replicas, redaction, forensics) and
   *opt-in* restore-path use — not the DR default.

   Implementation notes (the load-bearing choices):
   - **Hand-written segment walker, not an `xlogreader.c` symlink.** The filter must
     decide and rewrite a record that spills into the next, *not-yet-archived* segment
     from its in-segment prefix alone; `XLogReadRecord` requires the whole record, and
     `DecodeXLogRecord` on a zero-padded prefix buffer fails *open* (phantom `0/0/0`
     block reference ⇒ record wrongly kept). The decoder mirrors `DecodeXLogRecord`
     check-for-check; the installed `pg_waldump` serves as an independent
     xlogreader-based oracle in the test suite instead.
   - **Runtime WAL geometry** (seg size / block size from the long page header), as in
     the Python tool — the C tool handles segments from any compatible build, unlike
     `pg_waldump`, whose page size is compile-time (`XLOG_BLCKSZ`, 32 KiB in Greengage
     builds).
   - **Stricter than the Python tool where the Python was lax:** `xlp_info` flag bits
     are validated; `XLP_FIRST_IS_OVERWRITE_CONTRECORD` (an aborted contrecord
     overwritten after a crash-failover) is handled correctly — the aborted record's
     dead bytes are left untouched, the walk restarts at the overwrite page, and the
     record found there must be `XLOG_OVERWRITE_CONTRECORD` (fail closed otherwise).
     Usage errors uniformly exit 1 (Python's argparse exited 2, colliding with the
     "fetch failed" polling signal).
   - **State file** moves from JSON (`state.json`) to a small versioned binary format
     (`<state-dir>/boundaries`, CRC-32C-protected), same semantics (keyed by next
     segment, keep 64, `length 0` = known-clean, missing = lookback). The old
     Python state file is simply ignored — the lookback re-derives any boundary
     deterministically, so the upgrade is self-healing. `HALT` is unchanged.
   - `RelMapping`/`RelMapFile`/`RELMAPPER_FILEMAGIC`/`MAX_MAPPINGS` moved from
     `relmapper.c` into `utils/relmapper.h` so the tool (frontend) shares the
     `pg_filenode.map` layout instead of duplicating it.

3. **Tests.** `src/test/dr/test_gg_walfilter.py` is rewritten as a **black-box** suite
   driving the installed binary (33 tests): the `WalWriter` synthetic-WAL generator is
   kept, the old Python parser is inlined as a read-only verification oracle, all 26
   ADR-0002 scenarios carry over, plus new cases for the overwrite-contrecord paths,
   undefined page flags, and a `pg_waldump` end-to-end decode of filtered segments.
   During the port, the C tool was additionally cross-checked against the then-extant
   Python tool for byte-identical `inspect`/`filter`/`restore` behavior. The DR docker
   fixture keeps the test gate (now `GG_WALFILTER="$(command -v gg_walfilter)"`).

## Consequences

### Positive

- **Topology protection is an engine invariant again** — holds for any WAL source with
  nothing to configure; a bare `cp` restore_command is safe. The principal ADR-0002
  trade-off is undone.
- **The consistency-check hazard is fixed in the in-backend filter** (not merely
  avoided by rewriting records): skip covers `checkXLogConsistency()`.
- **DR `pg_wal` is byte-identical to production again**; the archive remains canonical
  as before. `pg_waldump` output on a DR node matches production's WAL.
- **No fetch-path CPU cost** in the default path; and where the tool *is* used, the C
  implementation removes the Python throughput cost ADR-0002 accepted.
- **The reusable tool survives, better**: compiled, shipped in `$GPHOME/bin`,
  format-strict, with the overwrite-contrecord gap closed.
- Engine loses the streaming-refusal branch ADR-0002 added.

### Negative / accepted costs

- **The engine hooks are back**: the redo-loop predicate, two `relmapper.c` guard
  calls, and `dr_redo_filter.c` must be carried through upstream merges of
  `xlog.c`/`relmapper.c` again — the cost ADR-0002 removed, consciously re-accepted as
  smaller than invariant-by-configuration.
- **Forbidden remap kills the postmaster again** (`FATAL` in `relmap_redo`) instead of
  ADR-0002's stall-but-keep-serving HALT. The reactive contract is unchanged
  (re-create + re-seed the node); availability during that window is worse. A
  deployment that prefers the stall behavior can still get it by opting into
  `gg_walfilter restore` in `restore_command` — both guards coexist.
- **Two implementations of the same filter semantics** (engine predicate + tool rules)
  must be kept in sync — mitigated by the shared protected-OID list being small and
  the semantics deliberately simple (block-reference matching).
- The C tool is ~2000 lines replacing ~1260 lines of Python, and its decoder mirrors
  (rather than links) `DecodeXLogRecord` — a format change inside the GG7 major would
  need a manual port (guarded by the `XLOG_PAGE_MAGIC` hard stop and the `pg_waldump`
  oracle test).

### Neutral / notes

- Filter semantics are unchanged across all three ADRs: rewrite/skip iff ≥1 block
  reference and every block reference is protected.
- The M1 validation scenarios and the DR docker fixture carry over unchanged (the
  fixture asserts the *behavior* — production's topology change invisible on DR —
  which is mechanism-agnostic).
- `gg_walfilter`'s `restore` mode remains fully supported for walbouncer-style
  partial-replica use cases, which the in-backend filter (DR-specific by design)
  cannot serve.

## Validation

- `make -C src/backend/access/transam` + the restored cmockery suite
  (`dr_redo_filter.t`: 13 tests) pass.
- `src/test/dr/test_gg_walfilter.py`: 33 black-box tests against the C binary pass,
  including the `pg_waldump` oracle decode.
- Cross-check during the port: `inspect` output, `filter` bytes, `restore` chains
  (state-present and lookback paths) and HALT behavior byte-identical between the
  Python and C implementations over `WalWriter`-generated scenarios.
- The end-to-end docker fixture (`src/test/dr/`) validates M1 via the restored redo
  filter: production's topology change invisible on DR, post-backup user tables
  visible, DR-local hostnames survive replay.

## References

- ADR-0001 (`0001-dr-read-replica.md`) — D3, the restored mechanism.
- ADR-0002 (`0002-dr-restore-time-wal-filter.md`) — the superseded default; still the
  authoritative specification of `gg_walfilter`'s mechanism and rule semantics.
- Architecture description: `doc/architecture/greengage-dr-read-replica.md` §4.2
  (filter), §4.2.4 (the tool).
- walbouncer: <https://github.com/cybertec-postgresql/walbouncer>.
