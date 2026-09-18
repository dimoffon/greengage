#!/usr/bin/env bash
#
# Bring up HDFS and the Hive Metastore from the ADH demo stand, seed Iceberg
# tables into them, and print what a Greengage cluster on this host needs to
# reach them.  Only seven of the stand's services run: kdc, openbao, pg,
# namenode, datanode, hdfs-init and metastore (docker-compose.hdfs.yml says
# why).  down.sh removes them again.
#
#   ADH_SHOW_DIR=~/ws/adh-show ./up.sh
#
# Prerequisites, none of which this script can do for you:
#   * Docker Compose v2 (the stand's compose file is v2 syntax; a v1
#     docker-compose will not read it);
#   * the adh-base image, built from the stand over the VPN:
#         cd $ADH_SHOW_DIR && cp -n .env.example .env && docker compose build kdc
#     ADSW_IP in .env must point at downloads.adsw.io;
#
# No root is needed.  The container names the Kerberos principals are tied to
# (nn/dn1/ms.adh.local) are resolved by the backend's JVM from the hosts file
# this script writes, through -Djdk.net.hosts.file in gg_duckdb.jvm_options
# (run.sh sets it), so /etc/hosts stays untouched.
#
# Environment: ADH_SHOW_DIR (default ~/ws/adh-show), GG_HDFS_PROJECT (compose
# project, default gg-adh), GG_HDFS_KEEP_RANGER=1 (leave the NameNode's Ranger
# authorizer in place; by default it is stripped, since the stand's Ranger is
# not one of the services we run).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
adh=${ADH_SHOW_DIR:-$HOME/ws/adh-show}
project=${GG_HDFS_PROJECT:-gg-adh}
services="kdc openbao pg namenode datanode hdfs-init metastore"

docker compose version >/dev/null 2>&1 ||
	{ echo "up.sh: Docker Compose v2 is required (install the compose plugin into ~/.docker/cli-plugins)" >&2; exit 1; }
[ -f "$adh/docker-compose.yml" ] ||
	{ echo "up.sh: no ADH stand at $adh; set ADH_SHOW_DIR" >&2; exit 1; }
[ -f "$adh/.env" ] || cp "$adh/.env.example" "$adh/.env"

# The NameNode's conf, with the Ranger authorizer taken out: the stand wires
# RangerHdfsAuthorizer into hdfs-site.xml, and we do not run Ranger.  The
# plugin is meant to fall back to HDFS permissions when the admin is absent,
# but a fallback that has to time out first is not what a test should wait on.
rm -rf "$here/.conf-nn"
cp -a "$adh/conf" "$here/.conf-nn"
if [ "${GG_HDFS_KEEP_RANGER:-0}" != 1 ]; then
	python3 - "$here/.conf-nn/hadoop/hdfs-site.xml" <<'PY'
import re
import sys

path = sys.argv[1]
xml = open(path).read()
out = re.sub(r"\n\s*<property>\s*<name>dfs\.namenode\.inode\.attributes\.provider\.class</name>.*?</property>",
             "", xml, flags=re.S)
if out == xml:
    print("up.sh: no Ranger authorizer in hdfs-site.xml; nothing stripped")
open(path, "w").write(out)
PY
fi

# The override mounts this by absolute path (see the comment there).
export GG_HDFS_CONF_NN="$here/.conf-nn"

compose() { docker compose -p "$project" -f "$adh/docker-compose.yml" -f "$here/docker-compose.hdfs.yml" "$@"; }

echo "up.sh: starting $services"
# shellcheck disable=SC2086
compose up -d $services

echo "up.sh: waiting for the metastore"
for _ in $(seq 1 120); do
	state=$(compose ps --format '{{.Service}} {{.Health}}' 2>/dev/null | awk '$1 == "metastore" {print $2}')
	[ "$state" = healthy ] && break
	sleep 5
done
[ "${state:-}" = healthy ] ||
	{ echo "up.sh: the metastore did not come up; see: docker compose -p $project logs metastore" >&2; exit 1; }

mkdir -p "$here/.run"
# What the backend's JVM resolves instead of the system resolver: the stand's
# fixed addresses, which its Kerberos principals and its Iceberg metadata name.
{
	echo "127.0.0.1 localhost"
	echo "172.30.77.10 kdc.adh.local"
	echo "172.30.77.20 nn.adh.local"
	echo "172.30.77.21 dn1.adh.local"
	echo "172.30.77.22 ms.adh.local"
} > "$here/.run/jvm-hosts"
cp "$adh/conf/krb5-host.conf" "$here/.run/krb5.conf"
docker cp "$(compose ps -q kdc)":/secrets/keytabs/_users/adh.keytab "$here/.run/adh.keytab" >/dev/null
chmod 600 "$here/.run/adh.keytab"

# Seeding runs inside the stand's image: it has Spark, the Iceberg runtime and
# the cluster's own configuration.  demo_hdfs comes from the shipped tarball
# (the stand's own demo-init would drag in Ozone, YARN and Ranger); gg.events
# and gg.events_mor are ours, with the shapes the tests need.
echo "up.sh: seeding demo_hdfs and gg"
docker cp "$here/seed_gg.py" "$(compose ps -q metastore)":/tmp/seed_gg.py >/dev/null
compose exec -T metastore bash -lc '
set -euo pipefail
export VAULT_TOKEN=$(cat /secrets/openbao/token)
kinit -kt /secrets/keytabs/_users/adh.keytab adh
mkdir -p /tmp/seed && cd /tmp/seed && tar xzf /opt/adh/demo/seed/demo_hdfs.tgz
spark3-submit --master "local[2]" --conf spark.kerberos.access.hadoopFileSystems= \
	/opt/adh/demo/register_seed.py --src=/tmp/seed demo_hdfs=hdfs:///warehouse
spark3-submit --master "local[2]" --conf spark.kerberos.access.hadoopFileSystems= \
	/tmp/seed_gg.py hdfs:///warehouse' | tee "$here/.run/seed.log"
sed -n '/^GGREF /s/^GGREF //p' "$here/.run/seed.log" > "$here/expected_ref.txt"

# A ticket for the backends.  It comes from the KDC container, because the host
# needs no Kerberos tools of its own: the cache is just a file, and what
# duckdb-hdfs needs is a FILE cache owned by the server's OS user, mode 0600.
# Nothing here renews it -- it lasts a day, and up.sh is how you get another.
ccache=${GG_HDFS_CCACHE:-/tmp/krb5cc_$(id -u)}
compose exec -T kdc kinit -kt /secrets/keytabs/_users/adh.keytab -c /tmp/cc_host adh
docker cp "$(compose ps -q kdc)":/tmp/cc_host "$ccache" >/dev/null
chmod 600 "$ccache"

cat <<EOF

up.sh: HDFS and the Hive Metastore are up (project $project),
       seeded with demo_hdfs and gg, and a ticket for adh@ADH.LOCAL is in
       $ccache (valid 24h).

Run the checks with:  $here/run.sh
EOF
