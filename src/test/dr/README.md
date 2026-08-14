# Greengage DR — docker test environment

A minimal, dependency-light test for the disaster-recovery (DR) read-replica
feature — **T1–T6** (the replica's topology is its own, and its replay of
production's WAL is unrestricted), **M2** (read-only enforcement on a live,
in-recovery DR coordinator), and **M4** (building the replica with
`ggdr create`). Two single-host
Greengage demo clusters run in separate containers that share a `/archive`
volume — **no pgBackRest or WAL-G**: the primary writes WAL to the volume with
`archive_command`, the DR cluster reads it back with `restore_command`.

```
              shared docker volume  wal_archive  ->  /archive  (in both containers)
 ┌─ primary ───────────────┐                         ┌─ dr ──────────────────────────┐
 │ demo cluster            │   archive_command  cp    │ restored from primary's base  │
 │ (coordinator + 1 seg)   │ ───────────────────────▶ │ backup; hot_standby=on;       │
 │ archive_mode=on         │   /archive/wal/seg%c/    │ own gg_topology store;        │
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
2. The **dr** container restores each instance's base backup; **writes the
   DR-local topology** into every node's own `$PGDATA/gg_topology` (via
   `gg_topology`, overwriting the copy of production's store that came in the base
   backup); arms `gg_topology_source = file` + `hot_standby` + `standby.signal` +
   `restore_command`; and starts the coordinator as a **live, continuous
   hot-standby**. `hot_standby = on` *is* what puts a node in DR mode — there is
   no separate GUC or marker file. It then waits to replay past the recorded
   change LSN.
3. **Assertions** (against the live coordinator, utility mode):
   - **M1** — `gp_segment_configuration` does **not** show production's change.
   - **M4 topology store** — seg0 has the DR-local hostname `dr` (genuine
     topology independence, not merely "ignored production's change"), and
     `gg_topology_source` really is `file`.
   - **P6 independence** — `gp_segment_configuration_internal` still holds
     *production's* rows, because nothing seeds it any more, while the replica
     serves `dr`. This is the assertion that shows the replica does not read the
     catalog for its topology at all.
   - **M1 selective** — the post-backup user table `dr_wal_applied` **is** present
     (the filter applies non-topology changes; it only skips protected catalogs).
   - **M2 reads** — a coordinator-only catalog `SELECT` works in recovery.
   - **M2 writes refused** — `SELECT … FOR UPDATE`, `INSERT`, and `CREATE TABLE`
     all fail (the first two with the DR-specific "read-only disaster-recovery
     replica" error). → `DR M1+M2 TEST: PASS (7/7)`.

   The DR coordinator is left **running**, so you can connect and read it:
   `… exec dr psql -p 7000 postgres`.

   > **Connect in dispatch mode — a plain `psql`.** Do **not** add
   > `PGOPTIONS='-c gp_role=utility'` to read *data*: utility mode does not
   > dispatch, so a distributed table is read on the coordinator alone and comes
   > back **empty** — at every restore point, no matter what the replica has
   > replayed or is serving. It looks exactly like a replica that never received
   > the data, and every status surface will disagree with it, because the rows
   > are on the segments. (This README said `gp_role=utility` until M2′ shipped,
   > when the coordinator-only read was all a DR could serve; it now serves
   > distributed reads, which is what you want.) Utility mode remains the right
   > tool for the other job — inspecting **one node's own** state
   > (`pg_is_in_recovery()`, `pg_last_served_restore_point()`, that node's slice
   > of a table): `… exec dr env PGOPTIONS='-c gp_role=utility' psql -p 7002 postgres`.

4. **M3 skew** — the regression test for the mid-advance torn read (scenario C-12,
   [ADR-0006](../../../doc/architecture/adr/0006-dr-read-replica.md) D8).
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

## Opt-in fixtures

Two further fixtures cover things the default one deliberately does not do to its
production cluster. Both run against the image the default fixture builds, and both take
their own compose project so they leave a running default stand alone:

```bash
docker-compose -f src/test/dr/docker-compose.yml build        # base image, once
```

### V-13′ — production maintenance on the topology catalog

`docker-compose.maint.yml`. Production runs `VACUUM`, `VACUUM FULL`, `REINDEX` and
`TRUNCATE` on `gp_segment_configuration_internal` with a replica attached, each with its own
restore point so the replica advances across them one at a time. The last three rewrite a
mapped shared catalog's relfilenode and emit a shared relmap update — the record the old
design halted the replica on. Production publishes each post-op relfilenode so the DR side
asserts the rewrite really happened; a fixture that quietly did nothing fails instead.
Verdict line: `V-13' VERDICT:`.

```bash
docker-compose -p maint -f src/test/dr/docker-compose.maint.yml up -d
docker-compose -p maint -f src/test/dr/docker-compose.maint.yml logs -f dr
docker-compose -p maint -f src/test/dr/docker-compose.maint.yml down -v
```

### Mirror failover — production's WAL forks under the replica

`docker-compose.failover.yml`. The only fixture whose production cluster has **mirrors**.
A segment's primary is killed between two restore points, FTS promotes its mirror, and the
promoted node starts a new timeline: everything after the promotion exists only there. The
replica is stopped at the earlier point when it happens, so the advance to the later one has
to cross the fork — which it can only do because `ggdr create` arms
`recovery_target_timeline = 'latest'`. Verdict line: `FAILOVER VERDICT:`.

The failure mode being guarded against is silence, not error: a replica that will not follow
the switch simply waits for WAL nobody will write again. The DR side wraps its advance in a
`timeout` so that presents as a failed check rather than a hung container.

```bash
docker-compose -p fo -f src/test/dr/docker-compose.failover.yml up -d
docker-compose -p fo -f src/test/dr/docker-compose.failover.yml logs -f dr
docker-compose -p fo -f src/test/dr/docker-compose.failover.yml down -v
```

### Standby coordinator activation — the topology row disappears

`docker-compose.costandby.yml`. The same fork, on content −1, where there is no FTS: an
operator stops the coordinator and runs `gpactivatestandby`. It differs from the segment case
in what it does to the topology — FTS flips a segment's role in place, while
`segment_config_activate_standby()` (`segadmin.c`) **deletes** the old coordinator's row and
promotes the standby's. So production's WAL carries the removal of the row for content −1,
the content the replica's own coordinator occupies.

That makes the two assertions at the centre of the run a matched pair, taken at the same
instant: the replica's **catalog** loses that row exactly as production's did, and the
topology it **serves** still has it — same dbid, still `dr`. Verdict line:
`STANDBY-ACTIVATION VERDICT:`.

Production runs **without** segment mirrors here, so content −1 is the only thing in the
cluster that can fork and the segments are an untouched control group. Getting that shape
needs `WITH_MIRRORS=false WITH_STANDBY=true` on the command line, since gpdemo ties the two
together — and it needs `update_pg_hba_on_segments_for_standby()` to tolerate a pair with no
mirror, which it did not until this fixture asked for it.

One environment note, encoded in the fixture: `gpactivatestandby` reads the standby's port
from `$PGPORT` and nothing else, which is right when the standby is another host on the same
port and wrong on a single-host demo — unset, it promotes the standby and then waits to
connect on the port the coordinator it replaced used to own.

```bash
docker-compose -p co -f src/test/dr/docker-compose.costandby.yml up -d
docker-compose -p co -f src/test/dr/docker-compose.costandby.yml logs -f dr
docker-compose -p co -f src/test/dr/docker-compose.costandby.yml down -v
```

## Files

| File | Purpose |
|---|---|
| `Dockerfile` | Ubuntu 24.04 + build deps; builds Greengage (`--disable-orca --without-python`) to `/usr/local/greengage-db-devel`; gpadmin/sshd handled at runtime. |
| `docker-compose.yml` | `primary` + `dr` services, shared `wal_archive` volume, `privileged`/`sysctls`/`ulimits`/`init`. Bind-mounts `scripts/` **and `gpMgmt/bin/ggdr`** (the fixture runs ggdr from the source tree, and gpMgmt does not install it), so editing either costs an `up`, not a rebuild. |
| `scripts/lib.sh` | Shared helpers + container (gpadmin/sshd) setup. |
| `scripts/primary-entrypoint.sh` | Build cluster, archive, base-backup, change topology, record change LSN. |
| `scripts/dr-entrypoint.sh` | Restore, arm DR mode, start the coordinator as a live hot-standby, run the M1+M2 assertions. |
| `scripts/run-dr-test.sh` | Earlier standalone M1 assertion (superseded; assertions now inline in `dr-entrypoint.sh`). |
| `docker-compose.maint.yml` | V-13′ fixture: production maintenance on the topology catalog; uses a prebuilt image via `$DR_IMAGE`. |
| `scripts/maint-primary-entrypoint.sh` | Densify the topology catalog, base-backup, then `VACUUM` / `VACUUM FULL` / `REINDEX` / `TRUNCATE`, publishing each relfilenode. |
| `scripts/maint-dr-entrypoint.sh` | Advance the replica across each operation and assert it did not notice; promote at the end. |
| `docker-compose.failover.yml` | Mirror-failover fixture: production runs **with mirrors** and fails one content over mid-run, forking its WAL. |
| `scripts/failover-primary-entrypoint.sh` | Build a mirrored cluster, archive on mirrors too, kill a primary between two restore points, write more rows through the promoted mirror. |
| `scripts/failover-dr-entrypoint.sh` | Advance the replica across the timeline fork and assert it followed it — and that only the failed-over content did. |
| `docker-compose.costandby.yml` | Standby-coordinator activation fixture: production activates its standby, deleting the old coordinator's topology row. |
| `scripts/costandby-primary-entrypoint.sh` | Build a cluster with a standby coordinator, stop the coordinator between two restore points, run `gpactivatestandby`, then write past the fork. |
| `scripts/costandby-dr-entrypoint.sh` | Advance the replica across content −1's fork; assert its catalog lost the coordinator row and its served topology did not. |

## Notes / status

- This is a PoC fixture. It verifies **T1–T6** (topology independence and
  unrestricted replay), **M2** (read-only enforcement + coordinator-only reads in
  recovery), and **M4** (building the replica). The DR coordinator serves only
  *coordinator-only* (catalog / entry-DB-singleton) reads; dispatching a
  distributed read-only query to reader-only gangs is the separate **M2′**
  milestone, and consistent as-of reads are **M3** — neither is exercised here.
  The DR segment (content 0) is now also started in recovery (M2′ I-0) and serves
  segment-local reads; dispatching a query from the coordinator to it is M2′
  SR-1/SR-2, not yet implemented.
- Reference recipe for the archive / basebackup / restore mechanics:
  `src/test/gpdb_pitr/test_gpdb_pitr.sh`.
- **The frozen seed is gone** (P6). The replica's topology lives in
  `$PGDATA/gg_topology`, which production's WAL cannot reach, so there is nothing
  to freeze and nothing to protect. With it went `ggseed_dr_topology`, the
  `DR_SEED` knob, the `backup_label`/`pg_control` save-restore, the backup-tail
  WAL re-fetch, the permanent-timeline-fork hazard those existed to contain, and
  the `VACUUM FREEZE` `pg_class` LSN-shadowing caveat.
- **The apply-time redo filter is gone too** (P7). `dr_redo_filter.{c,h}`, the
  redo hook, the `relmapper.c` remap guard and the `storage.c` truncate guard are
  deleted: with the topology outside the WAL there is nothing left to protect.
  The replica now replays production's WAL in full, including
  `gp_segment_configuration_internal` and `gp_configuration_history` — T3 and T5
  assert exactly that. `gg_walfilter` keeps its generic rules and its remap guard,
  now reachable as `--halt-on-remap` rather than through the deleted
  `--gp-dr-topology` preset.
- **T6 is the acceptance test for that deletion.** Production runs with
  `wal_consistency_checking` (env `WAL_CONSISTENCY_CHECKING`, default `all`), so
  every record carries a full-page image and the replica compares its own page
  against production's after redo. It could not be run before: the filter skipped
  the consistency check alongside redo, so agreement proved nothing about the
  records being skipped.
