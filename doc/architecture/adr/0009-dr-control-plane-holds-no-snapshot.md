# ADR-0009: The DR Control Plane Holds No Snapshot

- **Status:** Accepted — implemented and validated on branch `7.x-dr`
- **Date:** 2026-09-02
- **Relates to:** [ADR-0006](0006-dr-read-replica.md) — the feature; and
  [ADR-0005](0005-dr-served-restore-point-snapshot.md) D6, whose "conflicts are the safety
  net" is what turned the switch into a casualty.

---

## Context

A DR replica answers every read from the image frozen at the served restore point
(ADR-0006 D8). That image is a local MVCC snapshot, and like any snapshot it is published as
the backend's `xmin` in the proc array. Hot standby's conflict machinery reads that `xmin`:
every replayed prune, `VACUUM` cleanup, btree delete or freeze that removes a row version
behind it cancels the backend, and `ggdr create` sets `max_standby_archive_delay = 0` so
the cancel is immediate. For a *reader* that is the agreed policy — the right answer or
none. During an advance the served `xmin` is old by construction, so readers overlapping
the advance are cancelled routinely.

`gg_dr_switch()` was a *function*, called as `SELECT gg_dr_switch('rpN')`. A function runs
inside its caller's executor, whose snapshot is registered for the statement's whole life —
and on a replica that snapshot is the served image. So the switch itself was a reader of the
image it was replacing, for the entire time it waited for production to create and archive
the next point, and the first replayed prune cancelled it.

Measured on a host lab (production with two segments, replica built by `ggdr create`):

| production activity between the two points | `SELECT gg_dr_switch()` |
|---|---|
| none (`CREATE TABLE`, `INSERT` only) | 8/8 ok |
| updates, deletes, `ANALYZE`, `VACUUM` of user and catalog tables | 0/8 ok |
| user-table updates and `VACUUM` only | 3/5 ok |

Every failure was `canceling statement due to conflict with recovery`, and the coordinator's
`pg_stat_database_conflicts.confl_snapshot` moved by one per failure. The per-node poll the
coordinator dispatched (`SELECT pg_last_paused_restore_point() WHERE …`) had the same
exposure for the milliseconds it ran on each segment: 4 of 6072 back-to-back polls on one
segment were cancelled during a churn replay. The fixture's scenario C-9 passed only because
production did nothing but `CREATE TABLE` and `INSERT` while the switch waited.

The same shape was reached independently upstream: PostgreSQL 17's `pg_wal_replay_wait()`
is a procedure precisely so that it can wait for replay without holding a snapshot that
replay would conflict with.

## Decision

### D1 — `gg_dr_switch()` is a procedure, and drops its snapshot before it waits

`CALL gg_dr_switch('rpN')`. At entry the procedure pops the snapshot its portal pushed,
invalidates the catalog snapshot parse analysis registered, and refuses to continue if any
snapshot is still held (`REPEATABLE READ`/`SERIALIZABLE` register the transaction snapshot
for the whole transaction; a snapshot from an enclosing function outlives the call). With
nothing held, `MyPgXact->xmin` is invalid and the conflict machinery has nothing to target.
A function cannot do this: it runs under an executor whose registered snapshot cannot be
released from underneath it. `PortalRunUtility` tolerates a procedure popping the snapshot
it pushed — it is how procedures that `COMMIT` work — so no portal change is needed.

Each poll in the wait loop dispatches, and a dispatch may look a catalog up (creating a gang
checks that the user is a superuser), which registers a catalog snapshot — the frozen image.
The loop invalidates it again before sleeping, so nothing old is pinned across the second
between polls. The microseconds inside a dispatch are the residual exposure, and they are
the same microseconds any `CALL` spends between its portal pushing a snapshot and the
procedure popping it.

### D2 — Nothing dispatched to a segment goes through the executor

The executor is where a segment takes the frozen image (`GetSnapshotData`) and where it
refuses a query while it has nothing to serve (`ExecutorStart`). Both are right for reads
and wrong for the control plane, so the per-node statements are utility statements:

- the per-node primitive is `CALL gg_dr_switch_node(target, publish)`, a procedure that
  runs on the segment through `ProcessUtility` and drops its snapshot on entry;
- the poll is `SHOW gg_dr_paused_restore_point`, a read-only GUC whose show hook reads the
  startup process's shared state — `SHOW` is one of the utility statements
  `PlannedStmtRequiresSnapshot` exempts, so it takes no snapshot at all.

A raw dispatched command runs on a segment through `exec_simple_query`, the same path a
client's statement takes, so both are ordinary statements there. No dispatch flag, no
per-session mode, no GUC that has to be synced to the gang: nothing the control plane does
can leave a segment's session in a state a later read would inherit.

### D3 — Arming and publishing are two calls, because only the coordinator knows when

The old per-node primitive armed *and* published in one call, on the reasoning that a node
has no image for the target on the first pass so the publish is a no-op. That is false for
a node that got there ahead of the switch — one that restarted and replayed to its armed
target, or one an operator drove by hand — which would have served the new point while a
lagging peer still served the old one: the torn read ADR-0006 D8 exists to prevent. Now
`gg_dr_switch_node(target, false)` arms (write the target, reload, resume unless already
there) and `gg_dr_switch_node(target, true)` publishes; the coordinator dispatches the
second only after `dr_all_paused_at()` has seen every node stopped at the target.

## Consequences

- **`CALL`, not `SELECT`.** `ggdr switch`, the fixtures and the documents use the procedure
  form; a procedure returns nothing, so `ggdr` judges the call by psql's exit status.
  `gg_dr_promote()` stays a function: it runs only when every node is paused at one point,
  so nothing is replaying while it waits.
- **Catalog change** (`prokind`, the new procedure, the new GUC): `CATALOG_VERSION_NO`
  bumped, an initdb is required. ADR-0005's "binary-only upgrade" no longer holds from that
  build.
- **Refused where it cannot be safe:** inside a `REPEATABLE READ` or `SERIALIZABLE`
  transaction, and from inside a function or another procedure.
- **`ggdr stat` and the progress line retry a cancelled node read.** The utility-mode reads
  `ggdr` makes per node are still frozen-image reads and can still be cancelled while a
  node replays; everything they ask for is in shared memory, so asking again is always
  right. A bounded retry replaces the spurious `UNREACHABLE`.
- **Not addressed here, and still open:** a read *started* after replay has pruned a row
  version the served image needs is not cancelled — the conflict machinery only sees
  snapshots that exist when the record replays — and returns a wrong answer, not a stale
  one. Measured on the same lab: a table with one row at the served point answered
  `count(*) = 0` after production updated and vacuumed it, with no error, until the next
  point was published. That contradicts ADR-0005 D2 ("stale, but never wrong") and is
  recorded as scenario C-13 in the test catalogue; the candidate fix (mark the served
  image stale in `ResolveRecoveryConflictWithSnapshot()` and refuse reads until the next
  publish) is a policy decision this record does not take.

## Validation

- Host lab, same churn as the table above, `CALL gg_dr_switch()` armed before the point
  existed: see the measured rows in
  [`../greengage-dr-test-scenarios.md`](../greengage-dr-test-scenarios.md) (C-9).
- `src/test/dr/docker-compose.yml`: the SQL recovery-control test now arms the switch to
  `dr_rp_promote` *before* production creates it, and production runs three rounds of
  update/delete/`ANALYZE`/`VACUUM` on a user table plus `VACUUM` of `pg_class`,
  `pg_statistic` and `pg_attribute` before creating the point. With the `SELECT` form
  that switch is cancelled; with the procedure it returns cleanly and the cluster serves
  `dr_rp_promote`.

## References

- `src/backend/access/transam/xlogfuncs_gp.c` — `dr_release_snapshots()`,
  `dr_arm_everywhere()`, `dr_publish_everywhere()`, `dr_all_paused_at()`,
  `gg_dr_switch()`, `gg_dr_switch_node()`.
- `src/backend/utils/misc/guc_gp.c` — `gg_dr_paused_restore_point` and its show hook.
- `src/backend/tcop/pquery.c` — `PlannedStmtRequiresSnapshot()`, `PortalRunUtility()`.
- PostgreSQL 17, `src/backend/commands/waitlsn.c` — `pg_wal_replay_wait()`, the upstream
  precedent for a replay-waiting procedure that holds no snapshot.
