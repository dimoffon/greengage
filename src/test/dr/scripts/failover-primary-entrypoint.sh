#!/bin/bash
# failover-primary-entrypoint.sh -- production side of the mirror-failover fixture.
#
# This is the only fixture whose production cluster has MIRRORS, and the only one
# where a content's WAL forks mid-run.  The sequence is:
#
#   1. build a mirrored cluster, archive per content (%c), base-backup the primaries
#   2. fo_rp_pre  -- the replica builds and stops here
#   3. fo_rp_a    -- rows written while the original primary still owns content N
#   4. FAILOVER   -- kill that primary; FTS promotes its mirror, which starts
#                    timeline 2 and archives its history file
#   5. fo_rp_b    -- rows written THROUGH the promoted mirror, on the new timeline
#
# Everything after step 4 exists only on timeline 2.  A replica that will not
# follow a timeline switch stops advancing at the fork -- silently, since there is
# no error to raise: it is simply waiting for WAL that nobody will write again.
# Step 5 is what a passing DR side has to be able to see.
#
# Only the primaries are base-backed up and only they go in the published
# topology: the DR replica is mirrorless (decision #6, whole-cluster failover
# only), so it restores one node per content.  Which node of the pair produced
# the WAL is not its business -- the archive is keyed on content, and a promotion
# does not change that.
set -euo pipefail
source /dr/scripts/lib.sh
ensure_gpadmin
reset_archive    # this run's /archive starts empty; the volume outlives `down`

FO_C=${FAILOVER_CONTENT:-0}
ROWS_PRE=${ROWS_PRE:-100}
ROWS_POST=${ROWS_POST:-200}
STATE=$ARCHIVE/failover_prod_state.env

cd "$DEMO"
rm -f "$DEMO/gpdemo-env.sh"
# WITH_STANDBY=false is not decoration: gpdemo's Makefile turns the standby
# coordinator ON whenever mirrors are on (an unconditional assignment inside
# `ifeq ($(WITH_MIRRORS), true)`), and a command-line override is the only way
# back out.  A standby coordinator is a different feature from segment mirrors
# and this fixture wants neither its instance nor its failover: the replica is
# mirrorless by design, and gg_topology refuses a topology that contains one.
log "fo-primary: creating demo cluster (coordinator + 2 primaries + 2 MIRRORS, no standby coordinator) ..."
LANG=en_US.UTF-8 make create-demo-cluster \
	PORT_BASE="$PORT_BASE" NUM_PRIMARY_MIRROR_PAIRS=2 \
	WITH_MIRRORS=true WITH_STANDBY=false 2>&1 | tail -12
source "$DEMO/gpdemo-env.sh"

log "fo-primary: instances (primaries AND mirrors):"
psql -p "$PORT_BASE" -d postgres -Atc \
	"select 'dbid '||dbid||' content '||content||' role '||role||' port '||port||' '||datadir
	   from gp_segment_configuration order by content, role desc;" | sed 's/^/    /'

# Assert the shape rather than assume it.  Everything below reads "the primary of
# content N" and "the mirror of content N" as single rows, and the first version
# of this script learned the hard way what a second content=-1 row does to that:
# the coordinator lookup returned two datadirs and the failure surfaced as a grep
# complaining about a filename with a newline in it.
SHAPE=$(psql -p "$PORT_BASE" -d postgres -Atc \
	"select count(*) filter (where content = -1)||' '||
	        count(*) filter (where content >= 0 and role = 'p')||' '||
	        count(*) filter (where content >= 0 and role = 'm')
	   from gp_segment_configuration;")
[ "$SHAPE" = "1 2 2" ] || \
	die "expected one coordinator, two primaries and two mirrors; got '$SHAPE' (coordinators primaries mirrors)"

# Primaries only: what gets base-backed up, and what the DR replica will be.
list_primaries() {
	psql -p "$PORT_BASE" -d postgres -Atc \
		"select datadir||E'\t'||port||E'\t'||dbid||E'\t'||content
		   from gp_segment_configuration where role = 'p' order by content;"
}

COORD_DATADIR=$(psql -p "$PORT_BASE" -d postgres -Atc \
	"select datadir from gp_segment_configuration where content = -1 and role = 'p';")
grep -q 'replication.*trust' "$COORD_DATADIR/pg_hba.conf" || \
	echo 'host replication all 0.0.0.0/0 trust' >> "$COORD_DATADIR/pg_hba.conf"

# Archiving goes on EVERY instance, mirrors included.  A mirror archives nothing
# while it is a mirror -- archive_mode = 'on' (not 'always') means WAL is archived
# by a node that is out of recovery -- so there is no double-archiving and no
# collision with the primary's files.  The moment FTS promotes it, it becomes the
# node that archives this content, and it can only do that if it was already
# configured for it: there is nobody left to add the setting afterwards.
log "fo-primary: enabling per-instance WAL archiving to $WAL_ARCHIVE (mirrors too)"
while IFS=$'\t' read -r datadir port dbid content; do
	mkdir -p "$WAL_ARCHIVE/seg$content"
	if ! grep -q 'GREENGAGE DR ARCHIVE' "$datadir/postgresql.conf"; then
		cat >> "$datadir/postgresql.conf" <<EOF

# --- GREENGAGE DR ARCHIVE ---
wal_level = replica
archive_mode = on
archive_timeout = 30
archive_command = 'test ! -f $WAL_ARCHIVE/seg%c/%f && cp %p $WAL_ARCHIVE/seg%c/%f'
wal_consistency_checking = '${WAL_CONSISTENCY_CHECKING:-all}'
EOF
	fi
done < <(list_instances)

# FTS has to notice the dead primary inside this fixture's patience.  At the
# 60 s default probe interval a failover takes minutes of wall clock for no
# added coverage; 10 s is the GUC's floor.  All three are PGC_SIGHUP, and the
# coordinator is the only node running FTS.
if ! grep -q 'GREENGAGE DR FTS' "$COORD_DATADIR/postgresql.conf"; then
	cat >> "$COORD_DATADIR/postgresql.conf" <<EOF

# --- GREENGAGE DR FTS (fixture: fail over promptly) ---
gp_fts_probe_interval = 10
gp_fts_probe_timeout = 5
gp_fts_probe_retries = 2
EOF
fi

# archive_mode requires a full restart.
gpstop -ar -q

# --- user data ---------------------------------------------------------------
# fo_marker exists before the base backup; fo_after is created after it, so every
# one of its rows reaches the replica through the WAL -- and the ones written in
# step 5 reach it only across the timeline fork.
psql -p "$PORT_BASE" -d postgres -q -c \
	"drop table if exists fo_marker;
	 create table fo_marker(id int, note text) distributed by (id);
	 insert into fo_marker select g, 'before the base backup' from generate_series(1,$ROWS_PRE) g;" || true

log "fo-primary: base-backup each PRIMARY to $BASEBACKUP (the DR restores these)"
while IFS=$'\t' read -r datadir port dbid content; do
	rm -rf "$BASEBACKUP/seg$content"
	pg_basebackup -h localhost -p "$port" -X stream --no-sync \
		-D "$BASEBACKUP/seg$content" --target-gp-dbid "$dbid"
	log "fo-primary:   content $content (dbid $dbid, port $port) -> $BASEBACKUP/seg$content"
done < <(list_primaries)

# The DR-local topology: primaries only (the replica has no mirrors), hostname and
# address forced to 'dr'.  Taken now, before the failover, which is the point --
# after it production's catalog says content $FO_C is served by a different dbid,
# and the replica's topology must not care.
psql -p "$PORT_BASE" -d postgres -Atc \
	"select dbid||E'\t'||content||E'\t'||role||E'\t'||preferred_role||E'\t'||mode||E'\t'||status||E'\t'||port||E'\tdr\tdr\t'||datadir
	   from gp_segment_configuration where role = 'p' order by content;" > "$TOPO_FILE"

# archive_and_rp <restore point name> -- create it and force it to the archive.
# The coordinator's own switch is explicit; the segments' WAL rides archive_timeout,
# which is why this waits rather than returning immediately.
archive_and_rp() {
	psql -p "$PORT_BASE" -d postgres -q -c "select gp_create_restore_point('$1');" >/dev/null 2>&1 || true
	psql -p "$PORT_BASE" -d postgres -q -c "checkpoint;" >/dev/null 2>&1 || true
	for _ in 1 2 3; do
		psql -p "$PORT_BASE" -d postgres -q -c "select pg_switch_wal();" >/dev/null 2>&1 || true
		sleep 2
	done
	log "fo-primary: restore point '$1' created and archived"
}

# Identify the pair that will fail over, while both halves are still in their
# original roles.
P_DATADIR= P_PORT= P_DBID= M_DATADIR= M_PORT= M_DBID=
IFS=$'\t' read -r P_DATADIR P_PORT P_DBID < <(psql -p "$PORT_BASE" -d postgres -Atc \
	"select datadir||E'\t'||port||E'\t'||dbid from gp_segment_configuration
	  where content = $FO_C and role = 'p';") || true
IFS=$'\t' read -r M_DATADIR M_PORT M_DBID < <(psql -p "$PORT_BASE" -d postgres -Atc \
	"select datadir||E'\t'||port||E'\t'||dbid from gp_segment_configuration
	  where content = $FO_C and role = 'm';") || true
[ -n "${M_DBID:-}" ] || die "content $FO_C has no mirror; the cluster was built without them"
log "fo-primary: content $FO_C -- primary dbid $P_DBID (port $P_PORT), mirror dbid $M_DBID (port $M_PORT)"

{
	echo "FO_CONTENT=$FO_C"
	echo "FO_OLD_DBID=$P_DBID"
	echo "FO_NEW_DBID=$M_DBID"
	echo "ROWS_PRE=$ROWS_PRE"
	echo "ROWS_POST=$ROWS_POST"
} > "$STATE"

archive_and_rp fo_rp_pre
touch "$READY_MARKER"
log "fo-primary: ready (base backups + topology published, archiving live)"

log "fo-primary: waiting for the DR to finish building ..."
for _ in $(seq 1 900); do [ -f "$ARCHIVE/failover_dr_built" ] && break; sleep 2; done
[ -f "$ARCHIVE/failover_dr_built" ] || die "timed out waiting for the DR to build"

# --- fo_rp_a: the last point on the original timeline -------------------------
psql -p "$PORT_BASE" -d postgres -q -c \
	"create table fo_after(id int, note text) distributed by (id);
	 insert into fo_after select g, 'before the failover' from generate_series(1,$ROWS_PRE) g;" || true
SEG_A=$(psql -p "$PORT_BASE" -d postgres -Atc \
	"select count(*) from fo_after where gp_segment_id = $FO_C;")
TOTAL_A=$(psql -p "$PORT_BASE" -d postgres -Atc "select count(*) from fo_after;")
{
	echo "TOTAL_A=$TOTAL_A"
	echo "SEG_A=$SEG_A"
} >> "$STATE"
archive_and_rp fo_rp_a
log "fo-primary: fo_after has $TOTAL_A row(s), $SEG_A of them on content $FO_C"

# --- the failover -------------------------------------------------------------
# -m immediate, not a clean stop: a clean shutdown is a graceful handover and the
# case worth testing is the one where the primary simply stops existing.  What it
# leaves behind is bounded -- WAL is archived a completed segment at a time, and
# the mirror is synchronous, so no archived segment can hold WAL the mirror never
# received.  That bound is what lets the replica cross the fork at all: had
# production archived records past the promotion point on the old timeline, a
# replica replaying them would be beyond the fork and unable to follow it.
# FTS only promotes a mirror it believes is caught up: a pair in mode 'n' means
# the mirror is not synchronised, and killing the primary then costs the content
# outright instead of failing it over.  It is also the bound this test leans on --
# a synchronous mirror cannot be behind archived WAL, which is what keeps the
# replica's replay position at or before the fork.
log "fo-primary: waiting for the content-$FO_C pair to report synchronised ..."
synced=
for _ in $(seq 1 120); do
	m=$(psql -p "$PORT_BASE" -d postgres -Atc \
		"select mode from gp_segment_configuration where dbid = $P_DBID;" 2>/dev/null || true)
	if [ "$m" = s ]; then synced=1; break; fi
	sleep 2
done
[ -n "$synced" ] || die "content $FO_C never reached mode 's'; FTS would not promote its mirror"

log "fo-primary: killing the content-$FO_C primary (dbid $P_DBID) with -m immediate ..."
pg_ctl -D "$P_DATADIR" stop -m immediate -W || true

log "fo-primary: waiting for FTS to promote the mirror (dbid $M_DBID) ..."
promoted=
for _ in $(seq 1 120); do
	role=$(psql -p "$PORT_BASE" -d postgres -Atc \
		"select role from gp_segment_configuration where dbid = $M_DBID;" 2>/dev/null || true)
	if [ "$role" = p ]; then promoted=1; break; fi
	sleep 2
done
[ -n "$promoted" ] || die "FTS did not promote dbid $M_DBID within 240s"
log "fo-primary: FTS promoted dbid $M_DBID -- content $FO_C is now served by the old mirror"
psql -p "$PORT_BASE" -d postgres -Atc \
	"select 'dbid '||dbid||' content '||content||' role '||role||' mode '||mode||' status '||status
	   from gp_segment_configuration where content = $FO_C order by dbid;" | sed 's/^/    /'

# The promotion is only interesting to this fixture if it forked the timeline.
# Say so out loud: a promotion that somehow kept timeline 1 would make the DR
# side pass while testing nothing.
#
# From the name of the WAL file it is writing, whose first eight hex digits ARE
# the timeline, and not from pg_control_checkpoint(): that reports the timeline
# of the last completed CHECKPOINT, which for the first seconds after a promotion
# is still the old one.  Read too early it says 1 about a node that has already
# forked -- which is exactly how this check first failed, against an archive that
# had 00000002.history in it at the time.
tli_of() {
	PGOPTIONS='-c gp_role=utility' psql -p "$1" -d postgres -Atc \
		"select ('x'||substring(pg_walfile_name(pg_current_wal_lsn()) from 1 for 8))::bit(32)::int
		  where not pg_is_in_recovery();" 2>/dev/null || true
}
NEW_TLI=
for _ in $(seq 1 60); do
	NEW_TLI=$(tli_of "$M_PORT")
	[ -n "$NEW_TLI" ] && break
	sleep 2
done
log "fo-primary: promoted node is writing timeline ${NEW_TLI:-<unknown>}"
[ "${NEW_TLI:-1}" -gt 1 ] 2>/dev/null || \
	die "promoted node is on timeline $NEW_TLI; no fork happened and the fixture would prove nothing"
echo "FO_NEW_TLI=$NEW_TLI" >> "$STATE"

# --- fo_rp_b: written through the promoted mirror, on the new timeline --------
psql -p "$PORT_BASE" -d postgres -q -c \
	"insert into fo_after select g, 'after the failover' from generate_series($((ROWS_PRE+1)),$((ROWS_PRE+ROWS_POST))) g;" || true
SEG_B=$(psql -p "$PORT_BASE" -d postgres -Atc \
	"select count(*) from fo_after where gp_segment_id = $FO_C;")
TOTAL_B=$(psql -p "$PORT_BASE" -d postgres -Atc "select count(*) from fo_after;")
[ "$SEG_B" -gt "$SEG_A" ] || \
	die "content $FO_C gained no rows after the failover ($SEG_A -> $SEG_B); nothing would be proved by reading it"
{
	echo "TOTAL_B=$TOTAL_B"
	echo "SEG_B=$SEG_B"
} >> "$STATE"
log "fo-primary: fo_after now has $TOTAL_B row(s), $SEG_B on content $FO_C ($((SEG_B-SEG_A)) written through the promoted mirror)"

archive_and_rp fo_rp_b
touch "$ARCHIVE/failover_ops_done"
log "fo-primary: failover complete and archived; idling so WAL keeps flowing"
exec sleep infinity
