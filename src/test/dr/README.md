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

1. The **primary** comes up, enables archiving, base-backs up each instance into
   the shared volume, publishes a DR-local topology, then **changes
   `gp_segment_configuration`** (bumps the segment's port by 1000) — generating
   WAL for the protected topology catalog.
2. The **dr** cluster restores the base backups, frozen-seeds DR-local topology
   (`gpseed_dr_topology`, M1.5), arms `gp_dr_replica` + `dr_replica.signal` +
   `restore_command`, and starts in continuous recovery.
3. **Assertion** (`run-dr-test.sh`): after replaying the primary's WAL, the DR
   coordinator's `gp_segment_configuration` must **not** reflect the primary's
   port change — the redo filter skipped it — and (if seeded) the coordinator
   hostname is the DR-local `dr`.

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

- This is a PoC fixture. The DR cluster runs its instances individually in
  recovery (`pg_ctl`), and the test reads `gp_segment_configuration` in **utility
  mode** — a full distributed query on the DR coordinator-in-recovery is the
  separate M2′ "standby distributed read" milestone, not exercised here.
- Reference recipe for the archive/basebackup/restore mechanics:
  `src/test/gpdb_pitr/test_gpdb_pitr.sh`.
- `DR_SEED=0` in the `dr` service env runs the test without the frozen seed
  (still a valid filter test: production's post-backup change must not appear).
