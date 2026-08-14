#!/bin/bash
# costandby-primary-entrypoint.sh -- production side of the coordinator-standby
# activation fixture.
#
# The mirror fixture next door fails a SEGMENT over.  This one fails the
# COORDINATOR over, which is a different mechanism with a sharper consequence:
#
#   * no FTS.  A standby coordinator is activated by an operator running
#     gpactivatestandby, not by a probe noticing a silence.
#   * the activated node DELETES the old coordinator's row.  FTS flips a
#     segment's role and status in place; segment_config_activate_standby()
#     (segadmin.c) removes the coordinator row outright and promotes the
#     standby's, which keeps its own dbid.
#
# That deletion is the point of running this at all.  Production's WAL now
# carries the removal of the row describing content -1, replayed into a replica
# whose own coordinator row lives at that same content -- and under the design
# this feature replaced, that catalog WAS what the replica read.  Here it must
# arrive in the replica's catalog and change nothing about what the replica
# serves.
#
#   co_rp_pre --> [replica builds, pauses] --> co_rp_a --> ACTIVATE --> co_rp_b
#
# The coordinator's WAL forks at the activation, so co_rp_b and everything with
# it exists only on timeline 2 of content -1.
set -euo pipefail
source /dr/scripts/lib.sh
ensure_gpadmin
reset_archive    # this run's /archive starts empty; the volume outlives `down`

ROWS_PRE=${ROWS_PRE:-100}
ROWS_POST=${ROWS_POST:-200}
STATE=$ARCHIVE/costandby_prod_state.env

# The port production answers on.  It moves when the standby is activated: the
# activated node keeps its own port, so everything after that step has to ask a
# different one.  Every psql below goes through $CPORT for that reason.
CPORT=$PORT_BASE

cd "$DEMO"
rm -f "$DEMO/gpdemo-env.sh"
# Segment mirrors are on, and not because this fixture wants them.
#
# The clean shape for this test is a mirrorless cluster WITH a standby
# coordinator, so that content -1 is the only thing in the cluster that can fork
# and the segments are an untouched control group.  That shape cannot be built:
# gpinitstandby dies on it, in update_pg_hba_on_segments_for_standby()
# (gppylib/operations/update_pg_hba_on_segments.py:87), which reads
# segmentPair.mirrorDB.getSegmentHostName() for every pair without checking
# whether there is a mirror -- so a mirrorless cluster raises "'NoneType' object
# has no attribute 'getSegmentHostName'" and the standby is rolled back.  That
# is upstream code untouched by this branch, and gpdemo's own default (mirrors
# imply a standby) is the configuration it is exercised in.
#
# It costs this fixture nothing: no segment ever fails over here, so the
# segments remain the control group either way -- they just have mirrors sitting
# behind them while they do it.
log "costandby-primary: creating demo cluster (coordinator + STANDBY COORDINATOR + 2 segments + mirrors) ..."
LANG=en_US.UTF-8 make create-demo-cluster \
	PORT_BASE="$PORT_BASE" NUM_PRIMARY_MIRROR_PAIRS=2 \
	WITH_MIRRORS=true WITH_STANDBY=true 2>&1 | tail -12
source "$DEMO/gpdemo-env.sh"

log "costandby-primary: instances:"
psql -p "$CPORT" -d postgres -Atc \
	"select 'dbid '||dbid||' content '||content||' role '||role||' port '||port||' '||datadir
	   from gp_segment_configuration order by content, role desc;" | sed 's/^/    /'

# Asserted, not assumed: gpinitsystem reports a standby that failed to
# initialize as a WARNING and finishes with a working cluster otherwise, so the
# only thing that distinguishes "standby ready" from "standby silently absent"
# is this query.  It is how the mirrorless attempt above was diagnosed.
SHAPE=$(psql -p "$CPORT" -d postgres -Atc \
	"select count(*) filter (where content = -1 and role = 'p')||' '||
	        count(*) filter (where content = -1 and role = 'm')||' '||
	        count(*) filter (where content >= 0 and role = 'p')
	   from gp_segment_configuration;")
[ "$SHAPE" = "1 1 2" ] || \
	die "expected one coordinator, one standby coordinator and two segment primaries; got '$SHAPE'"

list_primaries() {
	psql -p "$CPORT" -d postgres -Atc \
		"select datadir||E'\t'||port||E'\t'||dbid||E'\t'||content
		   from gp_segment_configuration where role = 'p' order by content;"
}

COORD_DATADIR=$(psql -p "$CPORT" -d postgres -Atc \
	"select datadir from gp_segment_configuration where content = -1 and role = 'p';")
grep -q 'replication.*trust' "$COORD_DATADIR/pg_hba.conf" || \
	echo 'host replication all 0.0.0.0/0 trust' >> "$COORD_DATADIR/pg_hba.conf"

# Archiving on every instance, the standby coordinator included, and for the
# same reason the mirror fixture configures mirrors: archive_mode = 'on' means a
# node in recovery archives nothing, so the standby stays silent until it is
# activated -- and then it is the only node that can archive content -1, with
# nobody left to configure it if it was not configured now.
log "costandby-primary: enabling per-instance WAL archiving to $WAL_ARCHIVE (standby too)"
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

gpstop -ar -q

psql -p "$CPORT" -d postgres -q -c \
	"drop table if exists co_marker;
	 create table co_marker(id int, note text) distributed by (id);
	 insert into co_marker select g, 'before the base backup' from generate_series(1,$ROWS_PRE) g;" || true

log "costandby-primary: base-backup each PRIMARY to $BASEBACKUP (the standby is not one)"
while IFS=$'\t' read -r datadir port dbid content; do
	rm -rf "$BASEBACKUP/seg$content"
	pg_basebackup -h localhost -p "$port" -X stream --no-sync \
		-D "$BASEBACKUP/seg$content" --target-gp-dbid "$dbid"
	log "costandby-primary:   content $content (dbid $dbid, port $port) -> $BASEBACKUP/seg$content"
done < <(list_primaries)

# role = 'p' drops the standby coordinator from the published topology, which is
# not merely tidy: gg_topology refuses a store containing a standby coordinator,
# so a replica describing one could not be built at all.
psql -p "$CPORT" -d postgres -Atc \
	"select dbid||E'\t'||content||E'\t'||role||E'\t'||preferred_role||E'\t'||mode||E'\t'||status||E'\t'||port||E'\tdr\tdr\t'||datadir
	   from gp_segment_configuration where role = 'p' order by content;" > "$TOPO_FILE"

archive_and_rp() {
	psql -p "$CPORT" -d postgres -q -c "select gp_create_restore_point('$1');" >/dev/null 2>&1 || true
	psql -p "$CPORT" -d postgres -q -c "checkpoint;" >/dev/null 2>&1 || true
	for _ in 1 2 3; do
		psql -p "$CPORT" -d postgres -q -c "select pg_switch_wal();" >/dev/null 2>&1 || true
		sleep 2
	done
	log "costandby-primary: restore point '$1' created and archived"
}

OLD_DBID=$(psql -p "$CPORT" -d postgres -Atc \
	"select dbid from gp_segment_configuration where content = -1 and role = 'p';")
SB_DATADIR= SB_PORT= SB_DBID=
IFS=$'\t' read -r SB_DATADIR SB_PORT SB_DBID < <(psql -p "$CPORT" -d postgres -Atc \
	"select datadir||E'\t'||port||E'\t'||dbid from gp_segment_configuration
	  where content = -1 and role = 'm';") || true
[ -n "${SB_DBID:-}" ] || die "no standby coordinator; the cluster was built without one"
log "costandby-primary: coordinator dbid $OLD_DBID (port $CPORT), standby dbid $SB_DBID (port $SB_PORT)"

{
	echo "CO_OLD_DBID=$OLD_DBID"
	echo "CO_NEW_DBID=$SB_DBID"
	echo "CO_NEW_PORT=$SB_PORT"
	echo "ROWS_PRE=$ROWS_PRE"
	echo "ROWS_POST=$ROWS_POST"
} > "$STATE"

archive_and_rp co_rp_pre
touch "$READY_MARKER"
log "costandby-primary: ready (base backups + topology published, archiving live)"

log "costandby-primary: waiting for the DR to finish building ..."
for _ in $(seq 1 900); do [ -f "$ARCHIVE/costandby_dr_built" ] && break; sleep 2; done
[ -f "$ARCHIVE/costandby_dr_built" ] || die "timed out waiting for the DR to build"

# --- co_rp_a: the last point before the coordinator moves --------------------
psql -p "$CPORT" -d postgres -q -c \
	"create table co_after(id int, note text) distributed by (id);
	 insert into co_after select g, 'before the activation' from generate_series(1,$ROWS_PRE) g;" || true
TOTAL_A=$(psql -p "$CPORT" -d postgres -Atc "select count(*) from co_after;")
echo "TOTAL_A=$TOTAL_A" >> "$STATE"
archive_and_rp co_rp_a

# --- the activation -----------------------------------------------------------
log "costandby-primary: waiting for the standby coordinator to report synchronised ..."
synced=
for _ in $(seq 1 120); do
	m=$(psql -p "$CPORT" -d postgres -Atc \
		"select mode from gp_segment_configuration where dbid = $SB_DBID;" 2>/dev/null || true)
	if [ "$m" = s ]; then synced=1; break; fi
	sleep 2
done
[ -n "$synced" ] || die "the standby coordinator never reached mode 's'"

log "costandby-primary: stopping the coordinator (dbid $OLD_DBID) with -m immediate ..."
pg_ctl -D "$COORD_DATADIR" stop -m immediate -W || true
# gpactivatestandby refuses while anything answers on the old coordinator's port,
# and says so about a socket file as readily as about a live postmaster.  An
# immediate stop is the case worth testing and it is also the one most likely to
# leave those behind, so clear them: nothing of ours is listening there now.
rm -f /tmp/.s.PGSQL."$PORT_BASE" /tmp/.s.PGSQL."$PORT_BASE".lock 2>/dev/null || true

# PGPORT has to be the STANDBY's port, and gpdemo-env.sh set it to the
# coordinator's.  gpactivatestandby takes the standby's port from the
# environment and nowhere else -- it prints str(os.getenv('PGPORT')) as "Standby
# port" and builds its connection with a bare dbconn.DbURL() -- which is right
# for a real deployment, where the standby is another host using the same port.
# On a single-host demo the two ports differ, so without this the utility
# promotes the standby and then waits to connect on 7000, where the coordinator
# it just replaced used to be, until it times out.
log "costandby-primary: activating the standby coordinator (dbid $SB_DBID) on port $SB_PORT ..."
set +e
PGPORT="$SB_PORT" \
COORDINATOR_DATA_DIRECTORY="$SB_DATADIR" MASTER_DATA_DIRECTORY="$SB_DATADIR" \
	gpactivatestandby -a -d "$SB_DATADIR" 2>&1 | tail -25 | sed 's/^/    gpactivatestandby: /'
act_rc=${PIPESTATUS[0]}
set -e
# Exit 1 is "activated, with warnings" (the utility's own convention); only 2 is
# a failure.  Either way the assertions below decide, not this code.
log "costandby-primary: gpactivatestandby exited $act_rc"

CPORT=$SB_PORT
export COORDINATOR_DATA_DIRECTORY="$SB_DATADIR" MASTER_DATA_DIRECTORY="$SB_DATADIR" PGPORT="$SB_PORT"

log "costandby-primary: waiting for the new coordinator on port $CPORT ..."
up=
for _ in $(seq 1 120); do
	[ "$(psql -p "$CPORT" -d postgres -Atc "select not pg_is_in_recovery();" 2>/dev/null)" = t ] && { up=1; break; }
	sleep 2
done
[ -n "$up" ] || die "the activated standby never came up out of recovery on port $CPORT"

# Same reason as the mirror fixture: read the timeline from the WAL file being
# written, not from pg_control_checkpoint(), which still reports the last
# checkpoint's timeline for the first seconds after a promotion.
NEW_TLI=
for _ in $(seq 1 60); do
	NEW_TLI=$(psql -p "$CPORT" -d postgres -Atc \
		"select ('x'||substring(pg_walfile_name(pg_current_wal_lsn()) from 1 for 8))::bit(32)::int
		  where not pg_is_in_recovery();" 2>/dev/null || true)
	[ -n "$NEW_TLI" ] && break
	sleep 2
done
log "costandby-primary: the new coordinator is writing timeline ${NEW_TLI:-<unknown>}"
[ "${NEW_TLI:-1}" -gt 1 ] 2>/dev/null || \
	die "the activated coordinator is on timeline ${NEW_TLI:-?}; no fork happened and the fixture would prove nothing"

# The catalog rewrite the activation performs, asserted here so the DR side's
# claim about replaying it means something.
GONE=$(psql -p "$CPORT" -d postgres -Atc \
	"select count(*) from gp_segment_configuration where dbid = $OLD_DBID;")
NOWCO=$(psql -p "$CPORT" -d postgres -Atc \
	"select count(*) from gp_segment_configuration where content = -1 and role = 'p' and dbid = $SB_DBID;")
[ "$GONE" = 0 ]  || die "the old coordinator row (dbid $OLD_DBID) is still in production's topology"
[ "$NOWCO" = 1 ] || die "dbid $SB_DBID is not production's coordinator after activation"
log "costandby-primary: production's topology dropped dbid $OLD_DBID and made dbid $SB_DBID the coordinator"
{
	echo "CO_NEW_TLI=$NEW_TLI"
} >> "$STATE"

# --- co_rp_b: written through the activated coordinator ----------------------
# The table is created AFTER the activation, so its pg_class row exists only in
# WAL the new coordinator wrote on the new timeline.  A replica that did not
# follow the fork cannot answer a query naming it at all -- which is what makes
# one dispatched SELECT on the DR side proof that it did.
psql -p "$CPORT" -d postgres -q -c \
	"create table co_after_activation(id int, note text) distributed by (id);
	 insert into co_after_activation select g, 'after the activation' from generate_series(1,$ROWS_POST) g;" || true
psql -p "$CPORT" -d postgres -q -c \
	"insert into co_after select g, 'after the activation' from generate_series($((ROWS_PRE+1)),$((ROWS_PRE+ROWS_POST))) g;" || true
TOTAL_B=$(psql -p "$CPORT" -d postgres -Atc "select count(*) from co_after;")
NEWTAB=$(psql -p "$CPORT" -d postgres -Atc "select count(*) from co_after_activation;")
{
	echo "TOTAL_B=$TOTAL_B"
	echo "NEWTAB=$NEWTAB"
} >> "$STATE"
log "costandby-primary: co_after now has $TOTAL_B row(s); co_after_activation ($NEWTAB rows) exists only past the fork"

archive_and_rp co_rp_b
touch "$ARCHIVE/costandby_ops_done"
log "costandby-primary: activation complete and archived; idling so WAL keeps flowing"
exec sleep infinity
