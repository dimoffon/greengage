# ADR-0008: A DR Replica Follows Production Through Its HA Events

- **Status:** Accepted — implemented and validated on branch `7.x-dr`
- **Date:** 2026-08-15
- **Relates to:** [ADR-0006](0006-dr-read-replica.md) — the feature this constrains, and
  [ADR-0007](0007-pluggable-cluster-topology.md) — which is what makes production's topology
  churn harmless to replay in the first place.

---

## Context

A DR replica reads an archive that production fills. Production is a highly available
cluster, so sooner or later it uses that HA: FTS promotes a mirror after a segment primary
dies, or an operator activates the standby coordinator with `gpactivatestandby`. Both are
routine on the production side and neither is announced to anyone — the transport is an
archive, so production cannot know a replica is attached.

Both events **fork a timeline**. The promoted node writes a `.history` file and continues on
timeline *N+1*; every record after the promotion exists only there. The archive is keyed on
**content** (the GP `%c` escape), and a promotion does not change a content id, so the new
timeline lands in the same per-content directory the replica is already reading. What it
does change is which timeline that content's WAL is on.

Two things about the topology also differ between the two events, and the difference matters
to a replica:

- FTS **flips a segment's role and status in place**;
- `segment_config_activate_standby()` (`segadmin.c`) **deletes the old coordinator's row**
  and promotes the standby's, which keeps its own dbid.

Under ADR-0007 neither is dangerous — the replica's own topology is `$PGDATA/gg_topology`
and production's rows replay into a catalog it does not read — but the deletion is the
sharpest available demonstration that this is so, because it removes the row for content
`-1`, the content the replica's own coordinator occupies.

Until this record, `ggdr create` pinned every node to `recovery_target_timeline = 'current'`.
That setting dates from the M4 era, when the replica's topology was installed by a
single-user *seed* that wrote WAL of its own, and the hazard being guarded against was
recovery wandering onto that fork. ADR-0007's file store deleted the seed, and with it the
only fork a replica could ever meet that was not production's.

---

## Decision

### D1 — A replica follows the newest timeline in its archive

`ggdr create` arms every node with **`recovery_target_timeline = 'latest'`**. A replica
crosses a production promotion by reading the archived `.history` file and continuing on the
new timeline, for the affected content only; contents that did not fail over stay where they
are, which is correct because each content has its own independent WAL/LSN space.

**Rejected:** *keeping `'current'` and documenting mirror failover as "rebuild that DR
node"*. The cost is not the rebuild, it is the detection. A replica pinned to `'current'`
does not fail when production forks — it requests the next segment of a timeline that has
stopped, misses, and retries forever. The failed restore logs at `DEBUG2`, invisible at
default `log_min_messages`, so the node is **indistinguishable from a healthy replica whose
production is idle**: `replay_lsn` freezes and `rpo_seconds` grows exactly as they would on a
quiet Sunday. A DR feature whose failure mode is silence is worse than one that cannot
survive failover at all, because the operator learns about it at promotion time.

**Rejected:** *teaching `restore_command` to fetch the old timeline's `….partial` segment.*
Measured and unnecessary: promotion copies the pre-fork bytes of the partial segment into the
new timeline's first segment, so a replica that follows the fork gets everything by asking
for `00000002…` and never needs the `.partial` at all.

### D2 — A replica does not archive

`ggdr create` arms every node with **`archive_mode = off`**.

The base backup carries production's `archive_mode` and its `archive_command`, and that
command names production's archive. A node in recovery archives nothing, so this is inert
until the replica is **promoted** — at which point it becomes a cluster that archives, and
the only archive it is configured for is production's.

Under D1 that is not untidy but unsafe. A replica follows the *newest* timeline in the
archive; a history file left behind by a promoted replica is newer than production's, so the
next replica built from that archive would follow the **old replica's** timeline instead of
production's, replaying a promoted replica's writes into what is supposed to be a copy of
production. One promotion would quietly poison every rebuild after it.

**Rejected:** *leaving archiving on so a promoted replica keeps its own PITR history.* A
promoted replica that should archive is an operator decision with an archive location of its
own, taken after promotion. Inheriting production's location by accident is not that
decision.

### D3 — The production-side precondition is that mirrors archive

This is a requirement on **production**, and it cannot be repaired later: with
`archive_mode = on` a node in recovery archives nothing, so a mirror stays silent until FTS
promotes it, and it can only start archiving then if it was configured for it beforehand. By
that point the node that would have configured it is gone.

Production's mirrors and its standby coordinator must therefore carry the same
`archive_mode`/`archive_command` as the primaries. Both fixtures configure archiving on every
instance for this reason.

---

## Consequences

- **Production HA is no longer a reason to rebuild a replica.** A segment failover or a
  coordinator activation is replayed and followed; the replica keeps serving.
- **The replica's catalog tracks production's topology, including a deleted coordinator
  row.** Nothing reads it, and the fixtures assert both halves in the same run.
- **A promoted replica archives nothing until an operator says so.** If the intent is for a
  promoted replica to become the new production, archiving is part of that promotion runbook.
- **Detection is still weak.** Neither `gg_stat_dr_replica` nor its summary exposes a
  timeline, so an operator cannot see from SQL that production has forked (scenario HA-1b).
  The fixtures read the restored `.history` file and the startup process's `new target
  timeline is N` log line instead. A `timeline` column remains the obvious addition.

---

## Validation

Two fixtures, each a production cluster with a real HA topology, run against the same image:

- **`src/test/dr/docker-compose.failover.yml` — 23/23.** Production runs *with mirrors* and
  archives on them; a segment primary is killed `-m immediate` between two restore points
  while the replica is paused at the earlier one, so the advance to the later one must cross
  the fork. Asserted: the advance completes (wrapped in a timeout, because the failure mode
  under `'current'` is waiting, not erroring); the forked content restored
  `00000002.history` *and* logged `new target timeline is 2`; the other two nodes did not
  switch; the rows written through the promoted mirror are served; no node restarted or
  logged a FATAL; and the promoted replica left production's archive without a timeline of
  its own (D2).
- **`src/test/dr/docker-compose.costandby.yml` — 25/25.** Production runs *with a standby
  coordinator*, mirrorless, so content `-1` is the only thing that can fork; the coordinator
  is stopped and the standby activated between two restore points. Asserted, beyond the
  timeline checks: the replica's catalog lost the old coordinator's row exactly as
  production's did, while the topology it serves still has that dbid with hostname `dr`; a
  table created *after* the activation reads back, which only WAL on the new timeline can
  supply; and promotion still works afterwards — the state in which `StartupXLOG`'s
  DR exclusion for `needToPromoteCatalog` matters most, since the replayed catalog no longer
  holds a row for this node's own dbid at content `-1`.

Both were run with the other suites unchanged on the same image: default 57/57, maintenance
35/35.

---

## References

- `gpMgmt/bin/ggdr` — `cmd_create`, the arming block (D1, D2).
- `src/test/dr/docker-compose.failover.yml`, `docker-compose.costandby.yml` and their
  entrypoints.
- `doc/architecture/greengage-dr-read-replica.md` §4.2.1 — how the mechanism works.
- `doc/architecture/greengage-dr-test-scenarios.md` Group F — HA-1, HA-1b, HA-2, HA-3′, HA-5,
  and the HA-1 appendix that predicted the defect D1 fixes.
