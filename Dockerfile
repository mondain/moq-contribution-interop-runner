ARG DEBIAN_BASE=debian@sha256:3783cc01769c7b2b1b83a5c5ad96c815348e28ed7da68e2e3687004faa906251
ARG DEBIAN_SNAPSHOT=20260927T000000Z
ARG SOURCE_DATE_EPOCH=1790467200

FROM ${DEBIAN_BASE} AS apt-base

ARG DEBIAN_SNAPSHOT
RUN rm -f /etc/apt/sources.list.d/debian.sources \
    && printf '%s\n' \
        "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} bookworm main" \
        "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/${DEBIAN_SNAPSHOT} bookworm-security main" \
        > /etc/apt/sources.list

FROM apt-base AS builder

ARG SOURCE_REVISION
ARG SOURCE_DATE_EPOCH
RUN if ! printf '%s\n' "${SOURCE_REVISION}" | grep -Eq '^[0-9a-f]{40}$'; then \
        echo 'SOURCE_REVISION must be a full lowercase Git object ID' >&2; exit 2; fi

ENV SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH}"
RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        cmake \
        g++ \
        git \
        libsqlite3-dev \
        libssl-dev \
        make \
        perl \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake/ cmake/
COPY include/ include/
COPY src/ src/
COPY docs/*.txt docs/
COPY requirements/*.json requirements/

RUN cmake -S . -B /build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXE_LINKER_FLAGS=-Wl,--build-id=none \
        -DCMAKE_INSTALL_PREFIX=/opt/moq-interop \
        -DMOQ_INTEROP_BUILD_TESTS=OFF \
        -DMOQ_INTEROP_BUILD_QUICHE_TEST_PEER=OFF \
        -DMOQ_INTEROP_SOURCE_REVISION=${SOURCE_REVISION} \
    && cmake --build /build --parallel 2 \
    && cmake --install /build --strip

FROM apt-base AS runtime

ARG DEBIAN_SNAPSHOT
ARG SOURCE_DATE_EPOCH
ARG SOURCE_REVISION
LABEL org.moq-interop.debian-snapshot="${DEBIAN_SNAPSHOT}" \
      org.moq-interop.source-date-epoch="${SOURCE_DATE_EPOCH}" \
      org.opencontainers.image.revision="${SOURCE_REVISION}"

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        curl \
        libsqlite3-0 \
        libssl3 \
    && rm -rf /var/lib/apt/lists/* /var/log/apt/* \
    && rm -f /var/log/dpkg.log /var/cache/ldconfig/aux-cache \
    && groupadd --gid 10001 moq-interop \
    && useradd --uid 10001 --gid 10001 --no-create-home \
        --home-dir /nonexistent --shell /usr/sbin/nologin moq-interop \
    && install -d --owner=10001 --group=10001 --mode=0750 /var/lib/moq-interop

COPY --from=builder /opt/moq-interop/bin/moq-interop-runner /usr/local/bin/moq-interop-runner
COPY --from=builder /opt/moq-interop/share/moq-interop/ /usr/share/moq-interop/

USER 10001:10001
WORKDIR /var/lib/moq-interop
EXPOSE 8080/tcp
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=2s --timeout=2s --start-period=2s --retries=15 \
    CMD ["curl", "--fail", "--silent", "--show-error", "--max-time", "1", "http://127.0.0.1:8080/healthz"]
ENTRYPOINT ["/usr/local/bin/moq-interop-runner"]
CMD ["--bind", "0.0.0.0", "--port", "8080", "--database", "/var/lib/moq-interop/runs.sqlite3", "--docs", "/usr/share/moq-interop/docs", "--requirements", "/usr/share/moq-interop/requirements"]
