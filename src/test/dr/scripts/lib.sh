#!/bin/bash
# lib.sh -- shared helpers for the Greengage DR docker test.
#
# Two single-host demo clusters run in separate containers ("primary" and
# "dr") sharing the /archive volume. The primary archives WAL there per
# segment (using the GP %c = content-id escape); the dr cluster restores from
# it. Layout on the shared volume:
#   /archive/wal/seg<content>/      per-instance WAL archive (content -1 = QD)
#   /archive/basebackup/seg<content>/  per-instance base backup the DR restores
#   /archive/dr_topology.tsv        DR-local topology (written by primary)
#   /archive/primary_ready          marker: primary is up and base-backed up

export GPHOME=${GPHOME:-/usr/local/greengage-db-devel}
export ARCHIVE=/archive
export WAL_ARCHIVE=$ARCHIVE/wal
export BASEBACKUP=$ARCHIVE/basebackup
export READY_MARKER=$ARCHIVE/primary_ready
export TOPO_FILE=$ARCHIVE/dr_topology.tsv
export SRC=/home/gpadmin/gpdb_src
export DEMO=$SRC/gpAux/gpdemo

# Demo cluster identity (single segment, no mirrors):
#   coordinator: content -1, dbid 1, port 7000, datadir datadirs/qddir/demoDataDir-1
#   segment 0  : content  0, dbid 2, port 7002, datadir datadirs/dbfast1/demoDataDir0
export PORT_BASE=7000

log() { echo "[$(date +%H:%M:%S)] [$(hostname)] $*"; }
die() { echo "[$(date +%H:%M:%S)] [$(hostname)] ERROR: $*" >&2; exit 1; }

# Run the rest of the calling entrypoint as gpadmin (re-exec pattern).
ensure_gpadmin() {
	if [ "$(id -un)" = root ]; then
		setup_container
		# su - starts a login shell with a clean environment, so anything the
		# entrypoints read has to be listed here or it silently arrives unset.
		exec su - gpadmin -c "exec env GPHOME=$GPHOME \
			WAL_CONSISTENCY_CHECKING='${WAL_CONSISTENCY_CHECKING:-all}' bash $0"
	fi
	# As gpadmin from here on.
	source "$GPHOME/greengage_path.sh"
}

# Root-only: gpadmin user + passwordless localhost ssh + sshd + /archive perms.
setup_container() {
	log "setup_container: preparing gpadmin user, sshd, and /archive"

	# gpinitsystem/gpstop connect to the coordinator over the loopback, but the
	# demo pg_hba.conf only lists IPv4 -- a connection over the IPv6 loopback
	# (::1) is rejected ("no pg_hba.conf entry for host ::1"). Force IPv4: drop
	# the ::1 localhost line from /etc/hosts (rewrite in place; it is a docker
	# bind-mount) and prefer IPv4 in gai.conf.
	grep -q '::ffff:0:0/96' /etc/gai.conf 2>/dev/null || \
		echo 'precedence ::ffff:0:0/96 100' >> /etc/gai.conf
	if grep -q '^::1' /etc/hosts 2>/dev/null; then
		grep -v '^::1' /etc/hosts > /etc/hosts.new && cat /etc/hosts.new > /etc/hosts && rm -f /etc/hosts.new
	fi

	if ! id gpadmin >/dev/null 2>&1; then
		useradd -m -s /bin/bash -G sudo gpadmin
		echo 'gpadmin ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/gpadmin
		echo 'gpadmin:password' | chpasswd
	fi
	# The image baked /home/gpadmin (and gpdb_src) as root via WORKDIR; give the
	# home dir to gpadmin so it can create ~/.ssh etc.
	mkdir -p /home/gpadmin && chown gpadmin:gpadmin /home/gpadmin

	# Passwordless ssh to localhost (gpinitsystem/gpstart/gpstop need it).
	su - gpadmin -c '
		set -e
		mkdir -p ~/.ssh && chmod 700 ~/.ssh
		[ -f ~/.ssh/id_rsa ] || ssh-keygen -t rsa -N "" -f ~/.ssh/id_rsa -q
		cat ~/.ssh/id_rsa.pub >> ~/.ssh/authorized_keys
		chmod 600 ~/.ssh/authorized_keys
		printf "Host *\n\tStrictHostKeyChecking no\n\tUserKnownHostsFile /dev/null\n\tLogLevel ERROR\n" > ~/.ssh/config
		chmod 600 ~/.ssh/config
	'

	# sshd
	ssh-keygen -A >/dev/null 2>&1 || true
	mkdir -p /run/sshd
	sed -i 's/^#\?UsePAM.*/UsePAM no/' /etc/ssh/sshd_config || true
	pgrep -x sshd >/dev/null || /usr/sbin/sshd

	# gpinitsystem host checks ping; needs suid in a container.
	chmod u+s "$(command -v ping)" 2>/dev/null || true

	# The demo cluster writes under the (image-baked) source tree; make it ours.
	chown -R gpadmin:gpadmin "$DEMO" 2>/dev/null || true
	mkdir -p "$WAL_ARCHIVE" "$BASEBACKUP"
	chown -R gpadmin:gpadmin "$ARCHIVE"
}

# Echo "datadir<TAB>port<TAB>dbid<TAB>content" for every instance, ordered by content.
list_instances() {
	psql -p "$PORT_BASE" -d postgres -Atc \
		"select datadir||E'\t'||port||E'\t'||dbid||E'\t'||content
		   from gp_segment_configuration order by content;"
}
