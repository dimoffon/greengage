#!/usr/bin/env bash

## ==================================================================
## Required: A fresh gpdemo cluster with mirrors sourced.
##
## This script tests and showcases a very simple Point-In-Time
## Recovery scenario by utilizing WAL Archiving and restore
## points. This test also demonstrates the commit blocking during
## distributed restore point creation during concurrent transactions
## to guarantee cluster consistency.
##
## Note: After successfully running this test, the PITR cluster will
## still be up and running from the temp_test directory. Run the
## `clean` Makefile target to go back to the gpdemo cluster.
## ==================================================================

# Store gpdemo master and primary segment data directories.
# This assumes default settings for the ports and data directories.
DATADIR="${COORDINATOR_DATA_DIRECTORY%*/*/*}"
MASTER=${DATADIR}/qddir/demoDataDir-1
PRIMARY1=${DATADIR}/dbfast1/demoDataDir0
PRIMARY2=${DATADIR}/dbfast2/demoDataDir1
PRIMARY3=${DATADIR}/dbfast3/demoDataDir2
MASTER_PORT=7000
PRIMARY1_PORT=7002
PRIMARY2_PORT=7003
PRIMARY3_PORT=7004

# Set up temporary directories to store the basebackups and the WAL
# archives that will be used for Point-In-Time Recovery later.
TEMP_DIR=$PWD/temp_test
REPLICA_MASTER=$TEMP_DIR/replica_m
REPLICA_PRIMARY1=$TEMP_DIR/replica_p1
REPLICA_PRIMARY2=$TEMP_DIR/replica_p2
REPLICA_PRIMARY3=$TEMP_DIR/replica_p3

ARCHIVE_PREFIX=$TEMP_DIR/archive_seg

REPLICA_MASTER_DBID=10
REPLICA_PRIMARY1_DBID=11
REPLICA_PRIMARY2_DBID=12
REPLICA_PRIMARY3_DBID=13

# The options for pg_regress and pg_isolation2_regress.
REGRESS_OPTS="--dbname=gpdb_pitr_database --use-existing --init-file=../regress/init_file --init-file=./init_file_gpdb_pitr --load-extension=gp_inject_fault"
ISOLATION2_REGRESS_OPTS="${REGRESS_OPTS} --init-file=../isolation2/init_file_isolation2"

# Run test via pg_regress with given test name.
run_test()
{
    ../regress/pg_regress $REGRESS_OPTS $1
    if [ $? != 0 ]; then
        exit 1
    fi
}

# Run test via pg_isolation2_regress with given test name. The
# isolation2 framework is mainly used to demonstrate the commit
# blocking scenario.
run_test_isolation2()
{
    ../isolation2/pg_isolation2_regress $ISOLATION2_REGRESS_OPTS $1
    if [ $? != 0 ]; then
        exit 1
    fi
}

# Remove temporary test directory if it already exists.
[ -d $TEMP_DIR ] && rm -rf $TEMP_DIR

# Create our test database.
createdb gpdb_pitr_database

# Test gp_create_restore_point()
run_test test_gp_create_restore_point

# Test output of gp_switch_wal()
run_test_isolation2 test_gp_switch_wal

# Set up WAL Archiving by updating the postgresql.conf files of the
# master and primary segments. Afterwards, restart the cluster to load
# the new settings.
echo "Setting up WAL Archiving configurations..."
for segment_role in MASTER PRIMARY1 PRIMARY2 PRIMARY3; do
  DATADIR_VAR=$segment_role
  echo "wal_level = replica
archive_mode = on
archive_command = 'cp %p ${ARCHIVE_PREFIX}%c/%f'" >> ${!DATADIR_VAR}/postgresql.conf
done
mkdir -p ${ARCHIVE_PREFIX}{-1,0,1,2}
gpstop -ar -q

# Create the basebackups which will be our replicas for Point-In-Time
# Recovery later.
echo "Creating basebackups..."
for segment_role in MASTER PRIMARY1 PRIMARY2 PRIMARY3; do
  PORT_VAR=${segment_role}_PORT
  REPLICA_VAR=REPLICA_$segment_role
  REPLICA_DBID_VAR=REPLICA_${segment_role}_DBID
  pg_basebackup -h localhost -p ${!PORT_VAR} -X stream -D ${!REPLICA_VAR} --target-gp-dbid ${!REPLICA_DBID_VAR}
done

# Build the cluster topology the PITR cluster will run with.
#
# A restored cluster is a different cluster: same contents and ports, but its own
# dbids and data directories.  Something has to say so before it can dispatch.
#
# This used to be done after the fact, by UPDATEing gp_segment_configuration on
# the restored coordinator under allow_system_table_mods -- which only worked
# because that catalog happened to be where topology lived.  It is now written to
# each node's own $PGDATA/gg_topology before the node ever starts, so the cluster
# describes itself from its first boot rather than being corrected afterwards.
# This is the first non-DR user of the file topology provider, and the case it
# was designed for.
#
# Derived from the source cluster while it is still up: same content, role, mode,
# status, port, hostname and address, with the replica's dbid and datadir
# substituted.  Mirrors are dropped -- the PITR cluster has none.
declare -A PITR_DBID PITR_DATADIR
PITR_DBID["-1"]=$REPLICA_MASTER_DBID;  PITR_DATADIR["-1"]=$REPLICA_MASTER
PITR_DBID["0"]=$REPLICA_PRIMARY1_DBID; PITR_DATADIR["0"]=$REPLICA_PRIMARY1
PITR_DBID["1"]=$REPLICA_PRIMARY2_DBID; PITR_DATADIR["1"]=$REPLICA_PRIMARY2
PITR_DBID["2"]=$REPLICA_PRIMARY3_DBID; PITR_DATADIR["2"]=$REPLICA_PRIMARY3

PITR_TOPOLOGY=$TEMP_DIR/gg_topology.txt
echo "Building the PITR cluster's topology..."
psql -p $MASTER_PORT -d postgres -Atc \
  "select content||' '||role||' '||preferred_role||' '||mode||' '||status||' '||port||' '||hostname||' '||address
     from gp_segment_configuration where role = 'p' order by content;" \
| while read -r content rest; do
    echo "${PITR_DBID[$content]} $content $rest ${PITR_DATADIR[$content]}"
  done > $PITR_TOPOLOGY

if [ "$(wc -l < $PITR_TOPOLOGY)" != 4 ]; then
  echo "FAIL: expected 4 primaries in the source topology, got:"
  cat $PITR_TOPOLOGY
  exit 1
fi
cat $PITR_TOPOLOGY | sed 's/^/  topology: /'

# Run setup test. This will create the tables, create the restore
# points, and demonstrate the commit blocking.
run_test_isolation2 gpdb_pitr_setup

# Stop the gpdemo cluster. We'll be focusing on the PITR cluster from
# now onwards.
echo "Stopping gpdemo cluster to now focus on PITR cluster..."
gpstop -a -q

# Appending recovery settings to postgresql.conf in all the replicas to setup
# for Point-In-Time Recovery. Specifically, we need to have the restore_command
# and recovery_target_name set up properly. We'll also need to empty out the
# postgresql.auto.conf file to disable synchronous replication on the PITR
# cluster since it won't have mirrors to replicate to.
# Also touch a recovery_finished file in the datadirs to demonstrate that the
# recovery_end_command GUC is functional.
echo "Appending recovery settings to postgresql.conf files in the replicas and starting them up..."
for segment_role in MASTER PRIMARY1 PRIMARY2 PRIMARY3; do
  REPLICA_VAR=REPLICA_$segment_role
echo "restore_command = 'cp ${ARCHIVE_PREFIX}%c/%f %p'
recovery_target_name = 'test_restore_point'
recovery_target_action = 'promote'
recovery_end_command = 'touch ${!REPLICA_VAR}/recovery_finished'" >> ${!REPLICA_VAR}/postgresql.conf
echo "" > ${!REPLICA_VAR}/postgresql.auto.conf

  # Point this node at its own topology store and write it, before it starts.
  # Both halves have to happen here: gg_topology_source is PGC_POSTMASTER, and
  # gg_topology refuses to write a store while a postmaster owns the directory.
  # The base backup brought production's copy of the file along; this overwrites
  # it, which is exactly what it is there for.
  echo "gg_topology_source = file" >> ${!REPLICA_VAR}/postgresql.conf
  gg_topology write -D ${!REPLICA_VAR} -f $PITR_TOPOLOGY
  if [ $? != 0 ]; then
    echo "FAIL: could not write the topology store for ${!REPLICA_VAR}"
    exit 1
  fi

  touch ${!REPLICA_VAR}/recovery.signal
  pg_ctl start -D ${!REPLICA_VAR} -l /dev/null
done

# Wait up to 30 seconds for new master to accept connections.
RETRY=60
while true; do
  pg_isready > /dev/null
  if [ $? == 0 ]; then
    break
  fi

  sleep 0.5s
  RETRY=$[$RETRY - 1]
  if [ $RETRY -le 0 ]; then
    echo "FAIL: Timed out waiting for new master to accept connections."
    exit 1
  fi
done

# No catalog surgery here any more.  The topology was written into each node's
# store before it started, so the restored cluster has described itself correctly
# from its first boot -- check that it does, since a wrong answer here is the one
# failure that would send queries to the cluster this one was restored from.
echo "Verifying the restored cluster describes itself..."
PGOPTIONS="-c gp_role=utility" psql postgres -Atc \
  "select dbid||':'||content||':'||datadir from gp_segment_configuration order by content;" \
  | sed 's/^/  serving: /'
PITR_SERVED=$(PGOPTIONS="-c gp_role=utility" psql postgres -Atc \
  "select count(*) from gp_segment_configuration where dbid = ${REPLICA_MASTER_DBID} and datadir = '${REPLICA_MASTER}';")
if [ "$PITR_SERVED" != 1 ]; then
  echo "FAIL: the restored coordinator does not describe itself (dbid ${REPLICA_MASTER_DBID}, ${REPLICA_MASTER})."
  exit 1
fi

# Restart the cluster to get the MPP parts working.
echo "Restarting cluster now that the new cluster is properly configured..."
export COORDINATOR_DATA_DIRECTORY=$REPLICA_MASTER
gpstop -ar

# Run validation test to confirm we have gone back in time.
run_test gpdb_pitr_validate

# Print unnecessary success output.
echo "============================================="
echo "SUCCESS! GPDB Point-In-Time Recovery worked."
echo "============================================="
