# Greengage DR — docker test environment

A minimal, dependency-light test for the disaster-recovery (DR) read-replica
feature (milestone M1, the apply-time topology redo filter). Two single-host
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
   (bumps the segment's port by 1000) and creates a restore point after it —
   generating, and archiving, WAL for the protected topology catalog.
2. The **dr** container restores the coordinator's base backup, arms
   `gp_dr_replica` + `dr_replica.signal` + `restore_command`, and — in
   **single-user mode** — recovers up to the restore point (so the change is
   replayed, with the filter active) and reads `gp_segment_configuration`.
   Single-user mode avoids the interconnect / FTS / dispatch that a *live* DR
   coordinator would need: a DR coordinator holding production's topology can't
   bind its interconnect, and serving reads from a coordinator still *in
   recovery* is the separate M2′ "standby distributed read" milestone.
3. **Assertion**: the DR coordinator replays *past* the change, but
   `gp_segment_configuration` still shows the pre-change port — the redo filter
   skipped production's change → `FILTER TEST: PASS`.

This end-to-end test is what caught the original M1.3 bug: the protected-set
resolution returned nothing during redo (the shared relmap isn't loaded in the
startup process yet), so the filter was a silent no-op until fixed.

## Files

| File | Purpose |
|---|---|
| `Dockerfile` | Ubuntu 24.04 + build deps; builds Greengage (`--disable-orca --without-python`) to `/usr/local/greengage-db-devel`; gpadmin/sshd handled at runtime. |
| `docker-compose.yml` | `primary` + `dr` services, shared `wal_archive` volume, `privileged`/`sysctls`/`ulimits`/`init`. |
| `scripts/lib.sh` | Shared helpers + container (gpadmin/sshd) setup. |
| `scripts/primary-entrypoint.sh` | Build cluster, archive, base-backup, change topology. |
| `scripts/dr-entrypoint.sh` | Restore, frozen-seed, arm DR mode + restore, start in recovery. |
| `scripts/run-dr-test.sh` | The M1 filter assertion. |

## Notes / status

- This is a PoC fixture. It verifies the **M1 redo filter only** (topology
  independence). Bringing the DR cluster fully online as a queryable replica is
  M2′/M3 (standby distributed read + consistent reads), not exercised here.
- Reference recipe for the archive / basebackup / restore mechanics:
  `src/test/gpdb_pitr/test_gpdb_pitr.sh`.
- **`DR_SEED`** (dr service env) defaults to `0`. With `DR_SEED=1` the DR
  coordinator is also frozen-seeded with DR-local topology (`gpseed_dr_topology`,
  M1.5) before recovery — the seed itself runs, but the standalone seed advances
  the data dir's WAL, which then diverges from the production archive on resume.
  Reconciling the frozen seed with continuous restore is a create-utility (M4)
  problem; the filter test runs cleanly with the seed off.
