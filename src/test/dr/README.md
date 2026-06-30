# Greengage DR — docker test environment

A minimal, dependency-light test for the disaster-recovery (DR) read-replica
feature — milestone **M1** (the apply-time topology redo filter) and **M2**
(read-only enforcement on a live, in-recovery DR coordinator). Two single-host
Greengage demo clusters run in separate containers that share a `/archive`
volume — **no pgBackRest or WAL-G**: the primary writes WAL to the volume with
`archive_command`, the DR cluster reads it back with `restore_command`.

```
              shared docker volume  wal_archive  ->  /archive  (in both containers)
 ┌─ primary ───────────────┐                         ┌─ dr ──────────────────────────┐
 │ demo cluster            │   archive_command  cp    │ restored from primary's base  │
 │ (coordinator + 1 seg)   │ ───────────────────────▶ │ backup; gp_dr_replica=on +    │
 │ archive_mode=on         │   /archive/wal/seg%c/    │ dr_replica.signal; frozen-seed│
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
   (bumps the segment's port by 1000), records the change's WAL LSN, and
   force-archives it — generating, and shipping, WAL for the protected topology
   catalog.
2. The **dr** container restores the coordinator's base backup, arms
   `gp_dr_replica` + `dr_replica.signal` + `standby.signal` + `restore_command`,
   and starts the coordinator as a **live, continuous hot-standby**. It does
   *not* set `hot_standby` — DR mode auto-enables it in the postmaster (M2) — so
   the coordinator accepts read connections **while in recovery**. It then waits
   to replay past the recorded change LSN.
3. **Assertions** (against the live coordinator, utility mode):
   - **M1** — `gp_segment_configuration` still shows the pre-change port: the
     redo filter skipped production's change.
   - **M2 reads** — a coordinator-only catalog `SELECT` works in recovery.
   - **M2 writes refused** — `SELECT … FOR UPDATE`, `INSERT`, and `CREATE TABLE`
     all fail (the first two with the DR-specific "read-only disaster-recovery
     replica" error). → `DR M1+M2 TEST: PASS`.

   The DR coordinator is left **running**, so you can connect and read it:
   `… exec dr env PGOPTIONS='-c gp_role=utility' psql -p 7000 postgres`.

This end-to-end test is what caught two silent-no-op bugs that unit tests missed:
the M1.3 protected-set resolution (the shared relmap isn't loaded during redo)
and the M2 `IsDRReplicaMode()` flag (set only in the startup process, invisible
to the backends where the enforcement runs).

## Files

| File | Purpose |
|---|---|
| `Dockerfile` | Ubuntu 24.04 + build deps; builds Greengage (`--disable-orca --without-python`) to `/usr/local/greengage-db-devel`; gpadmin/sshd handled at runtime. |
| `docker-compose.yml` | `primary` + `dr` services, shared `wal_archive` volume, `privileged`/`sysctls`/`ulimits`/`init`. |
| `scripts/lib.sh` | Shared helpers + container (gpadmin/sshd) setup. |
| `scripts/primary-entrypoint.sh` | Build cluster, archive, base-backup, change topology, record change LSN. |
| `scripts/dr-entrypoint.sh` | Restore, arm DR mode, start the coordinator as a live hot-standby, run the M1+M2 assertions. |
| `scripts/run-dr-test.sh` | Earlier standalone M1 assertion (superseded; assertions now inline in `dr-entrypoint.sh`). |

## Notes / status

- This is a PoC fixture. It verifies **M1** (topology redo filter) and **M2**
  (read-only enforcement + coordinator-only reads in recovery). The DR
  coordinator serves only *coordinator-only* (catalog / entry-DB-singleton)
  reads; dispatching a distributed read-only query to reader-only gangs is the
  separate **M2′** milestone, and consistent as-of reads are **M3** — neither is
  exercised here. Only the DR coordinator is started; the DR segment is not.
- Reference recipe for the archive / basebackup / restore mechanics:
  `src/test/gpdb_pitr/test_gpdb_pitr.sh`.
- **`DR_SEED`** (dr service env) defaults to `0`. With `DR_SEED=1` the DR
  coordinator is also frozen-seeded with DR-local topology (`gpseed_dr_topology`,
  M1.5) before recovery — the seed itself runs, but the standalone seed advances
  the data dir's WAL, which then diverges from the production archive on resume.
  Reconciling the frozen seed with continuous restore is a create-utility (M4)
  problem; the filter test runs cleanly with the seed off.
