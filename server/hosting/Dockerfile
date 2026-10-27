# syntax=docker/dockerfile:1.7
# ARM64/multiarch adaptation: Ronny Stiftel; original Docker build: Kaniel/lKaniel.
# Source: https://github.com/lKaniel/bloodborne-server/tree/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1/server/hosting
# The one shadNet image: docker-compose here, the Helm chart in deploy/helm/yharnam, CI.
#
# Build context is the repository's root (Dockerfile.dockerignore keeps it to server/ and the
# ss.info template), with the protobuf submodule checked out:
#   git submodule update --init server/externals/protobuf
#   docker build -f server/hosting/Dockerfile -t yharnam-shadnet:local .
# CMake fetches abseil with git while configuring (protobuf_FORCE_FETCH_DEPENDENCIES), so the
# build needs network: github.com, conda.anaconda.org (or download.qt.io for QT_FROM=aqt).
#
# Qt: shadNet needs Qt >= 6.8 (src/webapi_server.cpp uses the 6.8 QHttpServer API); Ubuntu 24.04's
# apt Qt is 6.4 and does not build it. Two sources, picked with --build-arg QT_FROM:
#   conda (default, built and run 2026-10-23): qt6-main 6.8.4 from conda-forge, QtHttpServer
#         6.8.3 built from github.com/qt/qthttpserver (conda-forge has no qt6-httpserver).
#   aqt:  Qt 6.8.3 from download.qt.io with aqtinstall (not built: download.qt.io was blocked
#         where this was written).
# Behind a TLS-intercepting proxy (build boxes, some CI): pass its CA as a build secret and the
# proxy as a build arg; neither ends up in the image:
#   docker build --network host --secret id=proxy_ca,src=/path/ca.crt \
#       --build-arg HTTPS_PROXY=$HTTPS_PROXY -f server/hosting/Dockerfile .
# BUILD_JOBS=N caps the compile's parallel jobs (default: every core).
#
# The runtime image is Ubuntu 24.04 with shadnet, the Qt and C++ libraries it loads (found with
# ldd) and tini; no compiler, no package manager, user 10001, data in /data.

ARG QT_FROM=conda

FROM ubuntu:24.04 AS toolchain
ENV DEBIAN_FRONTEND=noninteractive
RUN --mount=type=secret,id=proxy_ca,required=false \
    set -eu; \
    if [ -s /run/secrets/proxy_ca ]; then \
        install -m 0644 /run/secrets/proxy_ca /tmp/proxy_ca.crt; \
        echo 'Acquire::https::CaInfo "/tmp/proxy_ca.crt";' > /etc/apt/apt.conf.d/99proxy-ca; \
        sed -i 's|http://|https://|g' /etc/apt/sources.list.d/ubuntu.sources; \
    fi; \
    apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates git cmake ninja-build g++ curl \
        bzip2 python3 python3-venv tini; \
    rm -rf /var/lib/apt/lists/* /etc/apt/apt.conf.d/99proxy-ca; \
    if [ -s /run/secrets/proxy_ca ]; then \
        mv /tmp/proxy_ca.crt /usr/local/share/ca-certificates/build-proxy.crt; \
        update-ca-certificates; \
    fi
# apt runs as _apt, which cannot read the secret: hence the copy. git, curl and micromamba
# read the system store (with the proxy's CA, if one was given).
ENV SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt \
    GIT_SSL_CAINFO=/etc/ssl/certs/ca-certificates.crt

# --- Qt from conda-forge -------------------------------------------------------------------------
FROM toolchain AS qt-conda
ARG QT_VERSION=6.8.4
ARG QTHTTPSERVER_VERSION=6.8.3
ARG MICROMAMBA_VERSION=2.3.3-0
ARG TARGETARCH
ARG BUILD_JOBS=0
RUN case "$TARGETARCH" in arm64) MAMBA_ARCH=aarch64 ;; amd64) MAMBA_ARCH=64 ;; *) exit 1 ;; esac \
    && curl -fsSL -o /usr/local/bin/micromamba \
        "https://github.com/mamba-org/micromamba-releases/releases/download/${MICROMAMBA_VERSION}/micromamba-linux-${MAMBA_ARCH}" \
    && chmod +x /usr/local/bin/micromamba \
    && micromamba create -y -r /tmp/mamba -p /opt/qt -c conda-forge "qt6-main=${QT_VERSION}" \
    && rm -rf /tmp/mamba
RUN git clone --depth 1 -b "v${QTHTTPSERVER_VERSION}" https://github.com/qt/qthttpserver /tmp/qhs \
    && cmake -S /tmp/qhs -B /tmp/qhs/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/opt/qt -DCMAKE_INSTALL_PREFIX=/opt/qt \
        -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DQT_BUILD_DOCS=OFF \
    && cmake --build /tmp/qhs/build $( [ "$BUILD_JOBS" -gt 0 ] && echo -j"$BUILD_JOBS" ) \
    && cmake --install /tmp/qhs/build \
    && rm -rf /tmp/qhs

# --- Qt from download.qt.io (aqtinstall) ---------------------------------------------------------
FROM toolchain AS qt-aqt
ARG AQT_QT_VERSION=6.8.3
RUN apt-get update && apt-get install -y --no-install-recommends libglib2.0-dev libgl-dev \
        libssl-dev && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /opt/aqt && /opt/aqt/bin/pip install --no-cache-dir aqtinstall==3.2.1 \
    && /opt/aqt/bin/aqt install-qt linux desktop "${AQT_QT_VERSION}" linux_gcc_64 \
         -m qthttpserver qtwebsockets -O /tmp/aqt \
    && mv "/tmp/aqt/${AQT_QT_VERSION}/gcc_64" /opt/qt && rm -rf /tmp/aqt /opt/qt/doc /opt/qt/examples

FROM qt-${QT_FROM} AS qt

# --- shadnet -------------------------------------------------------------------------------------
FROM qt AS build
ARG BUILD_JOBS=0
ARG SOURCE_REVISION=9e60cec70e589a199cf86771de4844fe556562d4
WORKDIR /src/server
# What CMakeLists.txt reads; not hosting/, so editing the ops files does not rebuild shadnet.
COPY server/CMakeLists.txt /src/server/
COPY server/externals /src/server/externals
COPY server/coop /src/server/coop
COPY server/src /src/server/src
COPY server/tests /src/server/tests
COPY server/loadtest /src/server/loadtest
COPY server/worlds.cfg server/scoreboards.cfg /src/server/
COPY backend/ss_info_template.xml /src/backend/
RUN test -f externals/protobuf/CMakeLists.txt \
        || (echo "externals/protobuf is empty: run git submodule update --init" && exit 1)
# The build folder is a BuildKit cache: a rebuild after a source change compiles only that.
RUN --mount=type=cache,id=shadnet-build,target=/src/server/build \
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/qt \
    && printf '#pragma once\n#define BB_SCM_REV "%s"\n' "$SOURCE_REVISION" > build/include/bbcoop_revision.h \
    && cmake --build build --target shadnet bbload test_server_link test_build_fingerprint \
        test_packet_routes test_rpc_tunnel test_admission test_http_gate test_team_lobby test_bloodborne_trade test_bloodborne_world test_world_writer $( [ "$BUILD_JOBS" -gt 0 ] && echo -j"$BUILD_JOBS" ) \
    && ctest --test-dir build --output-on-failure -R '^(server_link|build_fingerprint|packet_routes|rpc_tunnel|admission|http_gate|team_lobby|bloodborne_trade|bloodborne_world|world_writer)$' \
    && mkdir -p /bin-out && strip -o /bin-out/shadnet build/shadnet \
    && strip -o /bin-out/bbload build/bbload
COPY server/hosting/collect-runtime.sh /usr/local/bin/
RUN sh /usr/local/bin/collect-runtime.sh /bin-out/shadnet /opt/qt /out \
    && cp /bin-out/bbload /out/bbload

# Simulated players (loadtest/bbload.cpp) for smoke tests against a deployed shard:
#   docker build --target loadtest -t yharnam-bbload:local -f server/hosting/Dockerfile .
FROM build AS loadtest-build
ARG BUILD_JOBS=0
RUN --mount=type=cache,id=shadnet-build,target=/src/server/build \
    cmake --build build --target bbload $( [ "$BUILD_JOBS" -gt 0 ] && echo -j"$BUILD_JOBS" ) \
    && strip -o /bin-out/bbload build/bbload \
    && sh /usr/local/bin/collect-runtime.sh /bin-out/bbload /opt/qt /out-bbload

FROM ubuntu:24.04 AS loadtest
COPY --from=loadtest-build /out-bbload/ /opt/shadnet/
ENV LD_LIBRARY_PATH=/opt/shadnet/lib QT_PLUGIN_PATH=/opt/shadnet/plugins
USER 10001
ENTRYPOINT ["/opt/shadnet/bbload"]

# --- runtime (the default target) ----------------------------------------------------------------
FROM ubuntu:24.04 AS runtime
ARG SHADNET_UID=10001
RUN groupadd --system --gid "${SHADNET_UID}" shadnet \
    && useradd --system --uid "${SHADNET_UID}" --gid "${SHADNET_UID}" --home-dir /data \
        --shell /usr/sbin/nologin shadnet \
    && mkdir -p /data && chown shadnet:shadnet /data && chmod 0750 /data
COPY --from=build /usr/bin/tini /usr/bin/tini
COPY --from=build /out/ /opt/shadnet/
COPY server/worlds.cfg server/scoreboards.cfg /opt/shadnet/defaults/
COPY server/hosting/entrypoint.sh server/hosting/healthcheck.sh /opt/shadnet/
ENV LD_LIBRARY_PATH=/opt/shadnet/lib QT_PLUGIN_PATH=/opt/shadnet/plugins SHADNET_HOME=/data \
    LANG=C.UTF-8 \
    PATH=/opt/shadnet:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
VOLUME /data
WORKDIR /data
USER ${SHADNET_UID}:${SHADNET_UID}
# 31314/udp the games' link (public). 20443/tcp the game's services and 31315/tcp summons/WebAPI
# (public, plain HTTP; the games call them at their link's address). 31313/tcp stays on 127.0.0.1
# (TcpLocalOnly: the games' calls come through the link). 31320 stats: keep private.
EXPOSE 31314/udp 20443/tcp 31315/tcp
HEALTHCHECK --interval=30s --timeout=10s --start-period=30s --retries=3 \
    CMD ["/opt/shadnet/healthcheck.sh"]
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/bin/tini", "--", "/opt/shadnet/entrypoint.sh"]
