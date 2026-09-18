#!/usr/bin/env bash
#
# Remove what up.sh started, and nothing else: the compose project is ours
# (gg-adh by default), so a stand someone else is running from the same
# directory under its own project name is untouched.  Volumes go too -- the
# seed is recreated on the next up.sh.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
adh=${ADH_SHOW_DIR:-$HOME/ws/adh-show}
project=${GG_HDFS_PROJECT:-gg-adh}

docker compose -p "$project" -f "$adh/docker-compose.yml" -f "$here/docker-compose.hdfs.yml" down -v --remove-orphans
rm -rf "$here/.conf-nn" "$here/.run"
echo "down.sh: project $project removed"
