ARG DEBIAN_BASE=debian@sha256:3783cc01769c7b2b1b83a5c5ad96c815348e28ed7da68e2e3687004faa906251

FROM ${DEBIAN_BASE} AS builder

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        cmake \
        g++ \
        git \
        libsqlite3-dev \
        libssl-dev \
        make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake/ cmake/
COPY include/ include/
COPY src/ src/
COPY docs/*.txt docs/
COPY requirements/*.json requirements/

ARG SOURCE_REVISION=unknown
RUN cmake -S . -B /build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/moq-interop \
        -DMOQ_INTEROP_BUILD_TESTS=OFF \
        -DMOQ_INTEROP_SOURCE_REVISION=${SOURCE_REVISION} \
    && cmake --build /build --parallel 2 \
    && cmake --install /build --strip

FROM ${DEBIAN_BASE} AS runtime

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        curl \
        libsqlite3-0 \
        libssl3 \
    && rm -rf /var/lib/apt/lists/* \
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
