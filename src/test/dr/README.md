# Greengage DR — docker test environment

A minimal, dependency-light test for the disaster-recovery (DR) read-replica
feature — milestones **M1** (apply-time topology redo filter), **M2** (read-only
enforcement on a live, in-recovery DR coordinator), and **M4** (frozen seed of
DR-local topology that survives resuming production's WAL). Two single-host
Greengage demo clusters run in separate containers that share a `/archive`
volume — **no pgBackRest or WAL-G**: the primary writes WAL to the volume with
`archive_command`, the DR cluster reads it back with `restore_command`.

```
              shared docker volume  wal_archive  ->  /archive  (in both containers)
 ┌─ primary ───────────────┐                         ┌─ dr ──────────────────────────┐
 │ demo cluster            │   archive_command  cp    │ restored from primary's base  │
 │ (coordinator + 1 seg)   │ ───────────────────────▶ │ backup; hot_standby=on;       │
 │ archive_mode=on         │   /archive/wal/seg%c/    │ frozen-seeded topology;       │
 │ base-backups itself ───▶│   /archive/basebackup/   │ restore_command  cp  ◀────────│
 │ then changes topology   │                          │ continuous recovery           │
 └─────────────────────────┘                          └───────────────────────────────┘
```

Greengage archives WAL per segment and the `%c` archive escape expands to the
segment **content id**, so a single shared archive directory is safe across all
instances (each writes/reads under `/archive/wal/seg<content>/`).

## Run

```bash
docker compose -f src/test/dr/docker-compose.yml up --build      # builds image, runs both
docker compose -f src/test/dr/docker-compose.yml logs -f dr      # watch the assertion
docker compose -f src/test/dr/docker-compose.yml down -v         # tear down + drop volume
```

The first run builds Greengage (with the DR feature) into the image — that takes
a while; subsequent runs reuse the image.

## What it asserts

1. The **primary** comes up, enables per-segment archiving, base-backs up each
   instance into the shared volume, then **changes `gp_segment_configuration`**
   — it rewrites seg0's `hostname` (a *non-breaking* field: the coordinator
   dispatches by `address`+`port`, so production stays fully queryable; an
   earlier version bumped the port and broke production's own dispatch) — records
   the change's WAL LSN, and force-archives it, shipping WAL for the protected
   topology catalog.
   It also creates a user table (`dr_wal_applied`) *after* the base backup, so
   that table exists only in the WAL (not the backup).
2. The **dr** container restores the coordinator's base backup; **frozen-seeds
   DR-local topology** (`ggseed_dr_topology`, `DR_SEED=1`) while preserving the
   recovery-start state (save/restore `backup_label` + `pg_control`, the M4 fix);
   arms `hot_standby` + `standby.signal` +
   `restore_command`; and starts the coordinator as a **live, continuous
   hot-standby**. `hot_standby = on` *is* what puts a node in DR mode — there is
   no separate GUC or marker file. It then waits to replay past the recorded
   change LSN.
3. **Assertions** (against the live coordinator, utility mode):
   - **M1** — `gp_segment_configuration` does **not** show production's change.
   - **M4 seed** — seg0 has the DR-local hostname `dr` (genuine topology
     independence, not merely "ignored production's change").
   - **M1 selective** — the post-backup user table `dr_wal_applied` **is** present
     (the filter applies non-topology changes; it only skips protected catalogs).
   - **M2 reads** — a coordinator-only catalog `SELECT` works in recovery.
   - **M2 writes refused** — `SELECT … FOR UPDATE`, `INSERT`, and `CREATE TABLE`
     all fail (the first two with the DR-specific "read-only disaster-recovery
     replica" error). → `DR M1+M2 TEST: PASS (7/7)`.

   The DR coordinator is left **running**, so you can connect and read it:
   `… exec dr env PGOPTIONS='-c gp_role=utility' psql -p 7000 postgres`.

4. **M3 skew** — the regression test for the mid-advance torn read (scenario C-12,
   [ADR-0005](../../../doc/architecture/adr/0005-dr-served-restore-point-snapshot.md)).
   Rather than racing a real advance, it *holds* the dangerous state still: the
   **segments** are driven to `dr_rp2` by hand while the **coordinator** stays at
   `dr_rp1`, so `dr_m3`'s second row is replayed and locally committed on a segment
   throughout. A read must still return the `dr_rp1` answer (`1`, not `2`), because
   every node answers from the image it froze at the point the cluster published —
   not from wherever its replay has got to. It also forces an index-only scan over
   `dr_ios`, whose post-`dr_rp1` pages production VACUUMed (so the segments have
   replayed `XLOG_HEAP2_VISIBLE` for them): that is the one path a frozen snapshot
   cannot cover by itself, and it must still give the `dr_rp1` answer.

This end-to-end test is what caught two silent-no-op bugs that unit tests missed:
the M1.3 protected-set resolution (the shared relmap isn't loaded during redo)
and the M2 `IsDRReplicaMode()` flag (set only in the startup process, invisible
to the backends where the enforcement runs).

## V-20 regression fixture (dense topology catalog)

`docker-compose.dense.yml` is a second, opt-in fixture for scenario **V-20**: production's
`VACUUM` truncating trailing pages of a protected topology catalog must not truncate the
DR's copy of it. The default fixture cannot show this — with 3 segment rows in a 32 KB page
the DR's seed has ample room on block 0, so production's truncation has nothing of the DR's
to discard. The dense fixture fills the catalog with filler rows that are inserted **and
deleted in one transaction** (dead on arrival, never visible to FTS, but still occupying
their pages), base-backs up in that state, and vacuums only after the DR is built.

It runs against a prebuilt image so the same fixture can be pointed at a guarded and an
unguarded build:

```bash
docker-compose -f src/test/dr/docker-compose.yml build        # base image, once

DR_IMAGE=greengage-dr-test:latest \
  docker-compose -p dense -f src/test/dr/docker-compose.dense.yml up -d
docker-compose -p dense -f src/test/dr/docker-compose.dense.yml logs -f dr
docker-compose -p dense -f src/test/dr/docker-compose.dense.yml down -v
```

The DR container prints a `V-20 VERDICT:` line — `TOPOLOGY SURVIVED` on a build with the
`smgr_redo` guard, `TOPOLOGY DESTROYED` without it. It **fails closed**: if the catalog is
not dense enough and the seed's rows land on block 0, the precondition check aborts instead
of reporting a pass, so an inconclusive run can never look green.

## Files

| File | Purpose |
|---|---|
| `Dockerfile` | Ubuntu 24.04 + build deps; builds Greengage (`--disable-orca --without-python`) to `/usr/local/greengage-db-devel`; gpadmin/sshd handled at runtime. |
| `docker-compose.yml` | `primary` + `dr` services, shared `wal_archive` volume, `privileged`/`sysctls`/`ulimits`/`init`. |
| `scripts/lib.sh` | Shared helpers + container (gpadmin/sshd) setup. |
| `scripts/primary-entrypoint.sh` | Build cluster, archive, base-backup, change topology, record change LSN. |
| `scripts/dr-entrypoint.sh` | Restore, arm DR mode, start the coordinator as a live hot-standby, run the M1+M2 assertions. |
| `scripts/run-dr-test.sh` | Earlier standalone M1 assertion (superseded; assertions now inline in `dr-entrypoint.sh`). |
| `docker-compose.dense.yml` | V-20 regression fixture (dense topology catalog); uses a prebuilt image via `$DR_IMAGE`. |
| `scripts/dense-primary-entrypoint.sh` | Densify `gp_segment_configuration`, base-backup in that state, then vacuum to trigger the truncation. |
| `scripts/dense-dr-entrypoint.sh` | Build the replica, assert it is exposed (live rows on a trailing block), replay past the truncation, report the verdict. |

## Notes / status

- This is a PoC fixture. It verifies **M1** (topology redo filter), **M2**
  (read-only enforcement + coordinator-only reads in recovery), and **M4** (the
  frozen seed surviving WAL resume). The DR coordinator serves only
  *coordinator-only* (catalog / entry-DB-singleton) reads; dispatching a
  distributed read-only query to reader-only gangs is the separate **M2′**
  milestone, and consistent as-of reads are **M3** — neither is exercised here.
  The DR segment (content 0) is now also started in recovery (M2′ I-0) and serves
  segment-local reads; dispatching a query from the coordinator to it is M2′
  SR-1/SR-2, not yet implemented.
- Reference recipe for the archive / basebackup / restore mechanics:
  `src/test/gpdb_pitr/test_gpdb_pitr.sh`.
- **`DR_SEED`** (dr service env) defaults to `1`: the DR coordinator is
  frozen-seeded with DR-local topology (`ggseed_dr_topology`) *and* its
  recovery-start state (`backup_label` + `pg_control`) is preserved around the
  seed (the M4 fix), so it resumes production's WAL from the archive instead of
  forking the timeline. Set `DR_SEED=0` to skip the seed (the DR then mirrors
  production's topology and the test verifies the filter via production's change
  alone). One documented caveat remains in M4: `VACUUM FREEZE` bumps
  `pg_class.relfrozenxid` for `gp_segment_configuration`, LSN-shadowing that one
  page in the sub-second `(backup-LSN, seed-LSN]` window.
