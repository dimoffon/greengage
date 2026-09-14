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
# OpenSSL, curl, the AWS SDK, avro-c, roaring; DuckDB 1.5.5 expects vcpkg at
# tag 2025.12.12, bootstrapped, and the build then takes long).
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
		cmake_vars="$cmake_vars -DCMAKE_SHARED_LINKER_FLAGS=-Wl,--exclude-libs,ALL"
	fi
fi
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
echo "build.sh: installed DuckDB ${DUCKDB_VERSION} into $prefix"
ls -la "$prefix/lib/libduckdb.so" "$prefix/include/duckdb.h"
