#!/usr/bin/env bash
#
# Build the pinned DuckDB release (duckdb.version) into PREFIX so that the
# Greengage tree can be configured with --with-duckdb=PREFIX.
#
#   build.sh PREFIX [BUILD_DIR]
#
# Environment: JOBS (default nproc), GEN (ninja|make, auto), DUCKDB_JEMALLOC
# (1 default, 0 drops the bundled allocator), DUCKDB_SHELL (1 default: also
# build the duckdb CLI, used by bench/ scripts), DUCKDB_CORE_EXTENSIONS
# (default "icu;json", linked statically; core_functions and parquet are always
# built in), DUCKDB_REMOTE_EXTENSIONS (1: also link httpfs, avro and iceberg
# from extensions.cmake, fetched from GitHub at the pinned commits) and
# DUCKDB_VCPKG (the vcpkg checkout their native dependencies come from:
# OpenSSL, curl, the AWS SDK, avro-c, roaring; DuckDB 1.5.4 expects vcpkg at
# tag 2025.12.12, bootstrapped, and the build then takes long).
#
# DUCKDB_HDFS_DIR (a duckdb-hdfs checkout, with DUCKDB_HDFS_HADOOP_JOBS
# bounding the Hadoop native build's parallelism) links that extension in too: the
# hdfs:// file system through JNI libhdfs and the Hive Metastore catalog, with
# Arenadata's duckdb-iceberg fork inside it instead of the upstream iceberg
# extension (see extensions.cmake).  It needs DUCKDB_REMOTE_EXTENSIONS=1, two
# JDKs (JAVA_HOME at 11 builds Hadoop and matches libhdfs.so's ABI,
# DUCKDB_HDFS_BUNDLE_JAVA_HOME at 21 builds the Hive client and jlinks the
# shipped JRE), maven and patchelf.  build.sh drives duckdb-hdfs's own Makefile
# for the parts DuckDB's build cannot produce -- the Hadoop dist (libhdfs.so and
# the client jars, about 30 min the first time), the Hive metastore client jars,
# the static avro-c/jansson prefix the avro extension's configure looks for, and
# the patched avro checkout -- then packages hdfs-runtime/ (jars, libhdfs.so,
# jlinked JRE) into PREFIX/lib, where libduckdb.so finds it by $ORIGIN.
# Ozone (ofs://) and Paimon are off: Paimon would force C++17 on the whole
# DuckDB build and ship shared libraries of its own.
#
# What is deliberately off: unit tests, sanitizers (they cannot run inside a
# backend), runtime extension loading (DISABLE_EXTENSION_LOAD: nothing to
# package, no external-access surface) and OVERRIDE_NEW_DELETE (would replace
# malloc for the whole backend).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
prefix=${1:?usage: build.sh PREFIX [BUILD_DIR]}
builddir=${2:-$HOME/duckdb-build}
# shellcheck source=duckdb.version
. "$here/duckdb.version"
jobs=${JOBS:-$(nproc)}
if [ -z "${GEN:-}" ]; then
	if command -v ninja >/dev/null 2>&1; then GEN=ninja; else GEN=make; fi
fi
mkdir -p "$builddir"
tarball="$builddir/duckdb-${DUCKDB_VERSION}.tar.gz"
url="https://github.com/duckdb/duckdb/archive/refs/tags/${DUCKDB_VERSION}.tar.gz"
[ -s "$tarball" ] || curl -fsSL -o "$tarball" "$url"
sum=$(sha256sum "$tarball" | cut -d' ' -f1)
if [ -n "${DUCKDB_SHA256:-}" ] && [ "$sum" != "$DUCKDB_SHA256" ]; then
	echo "build.sh: sha256 mismatch for $tarball: $sum != $DUCKDB_SHA256" >&2
	exit 1
fi
echo "build.sh: $tarball sha256 $sum"
src="$builddir/duckdb-${DUCKDB_VERSION#v}"
[ -d "$src" ] || tar -xzf "$tarball" -C "$builddir"
skip=""
[ "${DUCKDB_JEMALLOC:-1}" = 1 ] || skip="jemalloc"
cmake_vars="-DBUILD_SHELL=${DUCKDB_SHELL:-1} -DBUILD_UNITTESTS=0 -DBUILD_BENCHMARKS=0"
cmake_vars="$cmake_vars -DENABLE_SANITIZER=0 -DENABLE_UBSAN=0 -DDISABLE_EXTENSION_LOAD=1"
cmake_vars="$cmake_vars -DCMAKE_INSTALL_PREFIX=$prefix"
export CMAKE_BUILD_PARALLEL_LEVEL="$jobs"
# DUCKDB_EXTENSIONS is what DuckDB's Makefile turns into -DBUILD_EXTENSIONS;
# core_functions and parquet are always built in.  Everything goes through the
# environment, not the make command line: a command-line CMAKE_VARS would
# override the Makefile's own additions to it (-DBUILD_EXTENSIONS included).
# OVERRIDE_GIT_DESCRIBE stamps the library version (the tarball has no git
# metadata); the Makefile appends its own -DOVERRIDE_GIT_DESCRIBE from this
# variable, so it must come from the environment too.
ext_configs=""
vcpkg_toolchain=""
merged_manifest=""
hdfs_dir=""
# One -Wl token, comma separated: CMAKE_VARS travels through make unquoted, so a
# space in any of its values would split into another variable.
ld_opts=""
if [ "${DUCKDB_REMOTE_EXTENSIONS:-0}" = 1 ]; then
	ext_configs="$here/extensions.cmake"
	if [ -n "${DUCKDB_VCPKG:-}" ]; then
		vcpkg_toolchain="$DUCKDB_VCPKG/scripts/buildsystems/vcpkg.cmake"
		merged_manifest=1
		[ -f "$vcpkg_toolchain" ] || { echo "build.sh: $vcpkg_toolchain not found" >&2; exit 1; }
		# The dependencies are static archives; without this every OpenSSL,
		# curl and AWS symbol they contain would be exported by libduckdb.so
		# and interpose on the backend's own libssl/libcrypto (or be
		# interposed by them).  Hidden, they bind inside the library.
		ld_opts="$ld_opts,--exclude-libs,ALL"
	fi
	if [ -n "${DUCKDB_HDFS_DIR:-}" ]; then
		hdfs_dir=$(cd "$DUCKDB_HDFS_DIR" && pwd)
		bundle_java_home=${DUCKDB_HDFS_BUNDLE_JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}
		[ -x "$bundle_java_home/bin/jlink" ] ||
			{ echo "build.sh: $bundle_java_home/bin/jlink not found; set DUCKDB_HDFS_BUNDLE_JAVA_HOME to a JDK 21" >&2; exit 1; }
		# duckdb-hdfs's own Makefile owns these four: the Hadoop dist
		# (libhdfs.so + client jars), the Hive metastore client closure, the
		# static avro-c/jansson prefix the avro extension's configure looks
		# for, and the patched avro checkout.  WITH_OZONE/WITH_PAIMON are off
		# here as they are in the DuckDB configure below.
		#
		# The Hadoop build needs apt's libtirpc-dev (hadoop-pipes, which the
		# dist profile builds and hadoop-tools-dist depends on); without root,
		# extract the package anywhere and point PKG_CONFIG_PATH and
		# CMAKE_PREFIX_PATH at it.  It also needs Arenadata's Maven artifacts,
		# whose repository id "arenadata" points at GitHub Packages and needs
		# either credentials or a ~/.m2/settings.xml mirror of the public
		# https://maven.arenadata.io/arenadata.
		#
		# Two memory guards, for a host with strict overcommit
		# (vm.overcommit_memory=2), where a reservation is charged whether or
		# not it is touched: Maven's JVM otherwise reserves a quarter of RAM,
		# and javadoc forks one JVM per module.  Hadoop's cmake step runs
		# make -j<online CPUs> with no knob of its own, so taskset is what
		# bounds it -- it reads Runtime.availableProcessors().
		export MAVEN_OPTS=${MAVEN_OPTS:--Xms256m -Xmx2g -XX:MaxMetaspaceSize=512m}
		hadoop_jobs=${DUCKDB_HDFS_HADOOP_JOBS:-4}
		env -u MAKELEVEL -u MAKEFLAGS BUNDLE_JAVA_HOME="$bundle_java_home" \
			taskset -c "0-$((hadoop_jobs - 1))" \
			make -C "$hdfs_dir" WITH_OZONE=0 WITH_PAIMON=0 \
			HADOOP_MVN_ARGS="-B -V -e -ntp install -DskipTests -Pdist,native -Dmaven.javadoc.skip=true -Dmaven.source.skip=true" \
			hadoop hive-metastore-client avro-deps avro-ext-src
		libhdfs=$(echo "$hdfs_dir"/hadoop/hadoop-dist/target/hadoop-*/lib/native/libhdfs.so)
		[ -f "$libhdfs" ] || { echo "build.sh: no libhdfs.so under $hdfs_dir/hadoop/hadoop-dist/target" >&2; exit 1; }
		hadoop_dist=${libhdfs%/lib/native/libhdfs.so}
		hive_deps=$(echo "$hdfs_dir"/hive/standalone-metastore/metastore-client/target/duckdb-hms-deps)
		[ -d "$hive_deps" ] || { echo "build.sh: $hive_deps not built" >&2; exit 1; }
		# The extension's configure reads these; WITH_ICEBERG=ON compiles the
		# vendored duckdb-iceberg fork into it (extensions.cmake drops the
		# upstream iceberg extension in this build), and its avro extension
		# finds avro-c/jansson through CMAKE_PREFIX_PATH.
		cmake_vars="$cmake_vars -DHADOOP_DIST=$hadoop_dist"
		cmake_vars="$cmake_vars -DHDFS_INCLUDE_DIR=$hdfs_dir/third_party/hdfs/include"
		cmake_vars="$cmake_vars -DJNI_INCLUDE_DIR=$bundle_java_home/include"
		cmake_vars="$cmake_vars -DWITH_ICEBERG=ON -DWITH_PAIMON=OFF"
		cmake_vars="$cmake_vars -DCMAKE_PREFIX_PATH=$hdfs_dir/build/avro-deps-install"
		export DUCKDB_AVRO_DIRECTORY="$hdfs_dir/build/avro-ext-src"
		# libduckdb.so links libhdfs.so, which is packaged under
		# PREFIX/lib/hdfs-runtime/ next to it; its rpath is set with patchelf
		# after the install, not at link time.  A literal $ORIGIN cannot be
		# passed through here: the flags travel to cmake as a make variable and
		# are expanded twice on the way (make eats $O, and the recipe's shell
		# eats what is left), which yields a silently useless rpath.
	fi
fi
[ -z "$ld_opts" ] || cmake_vars="$cmake_vars -DCMAKE_SHARED_LINKER_FLAGS=-Wl${ld_opts}"
env GEN="$GEN" DUCKDB_EXTENSIONS="${DUCKDB_CORE_EXTENSIONS:-icu;json}" \
	SKIP_EXTENSIONS="$skip" DISABLE_SANITIZER=1 CMAKE_VARS="$cmake_vars" \
	EXTENSION_CONFIGS="$ext_configs" \
	VCPKG_TOOLCHAIN_PATH="$vcpkg_toolchain" USE_MERGED_VCPKG_MANIFEST="$merged_manifest" \
	OVERRIDE_GIT_DESCRIBE="$DUCKDB_VERSION" \
	make -C "$src" release
rel="$src/build/release"
mkdir -p "$prefix/lib" "$prefix/include" "$prefix/bin"
cmake --install "$rel" --prefix "$prefix" >/dev/null 2>&1 || true
install -m 644 "$src/src/include/duckdb.h" "$src/src/include/duckdb.hpp" "$prefix/include/"
[ -f "$prefix/lib/libduckdb.so" ] || install -m 755 "$rel/src/libduckdb.so" "$prefix/lib/"
[ -f "$rel/duckdb" ] && install -m 755 "$rel/duckdb" "$prefix/bin/"
if [ -n "$hdfs_dir" ]; then
	# hdfs-runtime/ goes beside libduckdb.so: the extension locates its jars by
	# dladdr() on its own code, which lives in libduckdb.so, and the linker
	# resolves libhdfs.so through the $ORIGIN rpath above.  The Greengage
	# Makefile's install-duckdb-lib copies the directory on to $libdir.
	"$hdfs_dir/scripts/package-jni-runtime.sh" --ext-dir "$prefix/lib" \
		--hadoop-dist "$hadoop_dist" --hive-client-dir "$hive_deps" \
		--java-home "$bundle_java_home"
	# DT_RPATH, not DT_RUNPATH: an rpath is inherited down the dependency
	# chain, so it is also a fallback for libhdfs.so's own libjvm.so (whose
	# packaged copy the bundler already repointed at the bundled JRE).
	patchelf --force-rpath \
		--set-rpath '$ORIGIN/hdfs-runtime/lib:$ORIGIN/hdfs-runtime/jre/lib/server' \
		"$prefix/lib/libduckdb.so"
	ldd "$prefix/lib/libduckdb.so" | grep -q "libhdfs.so.*not found" &&
		{ echo "build.sh: libduckdb.so cannot resolve libhdfs.so from $prefix/lib/hdfs-runtime" >&2; exit 1; }
	ls -la "$prefix/lib/hdfs-runtime/lib/libhdfs.so"
fi
echo "build.sh: installed DuckDB ${DUCKDB_VERSION} into $prefix"
ls -la "$prefix/lib/libduckdb.so" "$prefix/include/duckdb.h"
