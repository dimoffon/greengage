# Greengage 7 DR test image.
#
# Builds Greengage (with the disaster-recovery feature on this branch) and
# prepares a container that can run a single-host demo cluster: a gpadmin user,
# sshd, and passwordless localhost ssh (Greengage's cluster tools need ssh even
# to localhost). The same image runs both the "primary" and "dr" services in
# docker-compose.yml; the service entrypoint scripts do the cluster work.
#
# Build context is the repo root (see docker-compose.yml). ORCA and PL/Python
# are disabled to keep the build fast; the gpMgmt utilities (gpstart/gpstop/
# gpinitsystem/...) only need the system python3.
FROM ubuntu:24.04

ENV LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8 \
    DEBIAN_FRONTEND=noninteractive \
    GPHOME=/usr/local/greengage-db-devel

# Build + runtime dependencies (subset of README.Ubuntu.bash, minus ORCA/xerces).
RUN apt-get update && apt-get install -y --no-install-recommends \
        bison build-essential cmake flex gcc g++ git ca-certificates \
        libapr1-dev libbz2-dev libcurl4-gnutls-dev libevent-dev libkrb5-dev \
        libldap-dev libpam-dev libperl-dev libreadline-dev libssl-dev libuv1-dev \
        libxml2-dev libyaml-dev libzstd-dev uuid-dev zlib1g-dev \
        locales lsof net-tools iputils-ping procps \
        openssh-client openssh-server pkg-config rsync sudo \
        python3 python3-dev python3-pip python3-psutil python3-psycopg2 python3-yaml \
    && locale-gen en_US.UTF-8 \
    && mkdir -p /run/sshd \
    && rm -rf /var/lib/apt/lists/*

# Build Greengage from this source tree.
COPY . /home/gpadmin/gpdb_src
WORKDIR /home/gpadmin/gpdb_src
# The host working tree may carry build artifacts from a host build: absolute
# symlinks (e.g. src/include/utils/errcodes.h -> /home/.../errcodes.h) and
# *-stamp files. Copied in, the symlinks dangle and the stamps make `make` think
# generated headers are up-to-date, so it never regenerates them. Strip them so
# the in-image build regenerates everything cleanly.
RUN find src/include -xtype l -delete 2>/dev/null || true ; \
    find . -name '*-stamp' -delete 2>/dev/null || true ; \
    ./configure --prefix=${GPHOME} --disable-orca --without-python \
        --with-libxml --with-perl --with-uuid=e2fs --with-zstd \
    && make -j"$(nproc)" -s -C src/backend generated-headers \
    && make -j"$(nproc)" -s \
    && make -j"$(nproc)" -s install \
    && make -C gpMgmt -s install

# gpMgmt's python deps that aren't packaged.
RUN python3 -m pip install --break-system-packages --no-cache-dir pygresql || true

# DR test scripts (also bind-mounted in compose for fast iteration, but baked in
# so the image is self-contained).
COPY src/test/dr/scripts/ /dr/scripts/
RUN chmod +x /dr/scripts/*.sh || true

# Keep the container alive; the service entrypoint drives cluster setup.
CMD ["sleep", "infinity"]
