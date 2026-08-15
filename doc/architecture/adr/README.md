# Architecture decision records

## Start here

**[ADR-0006 — Disaster-Recovery Read Replica for Greengage 7](0006-dr-read-replica.md)**

One self-contained document: what the feature is, every decision that is live, what was
rejected and why, the costs accepted, and the invariants a developer must not break. It is
the only ADR you need to understand or review the implementation.

**[ADR-0007 — Cluster Topology Behind a Pluggable Store](0007-pluggable-cluster-topology.md)**

Current and complete (P0–P8). Topology moved out of the replicated catalog and behind a
provider, which is why several of ADR-0006's mechanisms were deleted rather than maintained
— **including D4, its apply-time topology redo filter and frozen-tuple seed**, which no
longer exist in the tree. Read it before changing anything that reads or writes
`gp_segment_configuration`.

**[ADR-0008 — A DR Replica Follows Production Through Its HA Events](0008-dr-follows-production-ha.md)**

Current. What happens to a replica when production fails a segment over to its mirror or
activates its standby coordinator: both fork a timeline, the replica follows it
(`recovery_target_timeline = 'latest'`), and it must not archive into production's archive
while doing so. Read it before changing what `ggdr create` writes into `postgresql.conf`.

Companions, both current:

- [`../greengage-dr-read-replica.md`](../greengage-dr-read-replica.md) — how the mechanisms
  work, with the code map and operational workflows.
- [`../greengage-dr-test-scenarios.md`](../greengage-dr-test-scenarios.md) — the scenario
  catalogue: expected behaviour per case, what is automated, and measured results.

## Proof-of-concept record

ADR-0001 … ADR-0005 are kept for the history of how the design was reached — including two
mechanisms that were adopted and then reversed. They are **superseded in full** by ADR-0006
and do not describe the current implementation. Read one only when you want the rationale
behind a specific turn.

| ADR | Subject | Outcome |
|---|---|---|
| [0001](0001-dr-read-replica.md) | The original PoC design, end to end | Mostly carried forward; D2 (mode gate) and D8 (as-of-N distributed snapshot) replaced |
| [0002](0002-dr-restore-time-wal-filter.md) | Topology protection moved out of the engine into `gg_walfilter` on the restore path | Reversed by 0003 — but still the authoritative spec of `gg_walfilter`'s rules, which survives as tooling |
| [0003](0003-dr-in-backend-filter-restored.md) | The in-backend apply-time redo filter restored as the default | Stands (ADR-0006 D4) |
| [0004](0004-hot-standby-is-dr-mode.md) | `hot_standby` *is* DR mode; restore-point-only recovery; SQL recovery control | Stands except D3 (ADR-0006 D3, D7, D13) |
| [0005](0005-dr-served-restore-point-snapshot.md) | Serve the local snapshot frozen at the restore point; drop the distributed snapshot | Stands (ADR-0006 D8 – D11) |
