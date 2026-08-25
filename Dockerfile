# syntax=docker/dockerfile:1.7
#
# safeedge runtime image.
#
# Two stages: a full toolchain to build, and `scratch` to ship. The runtime
# stage contains exactly one file -- the statically linked binary -- so there is
# no shell, no package manager, no libc, and nothing for a scanner to find or a
# CVE to apply to. An image with no userland cannot have a userland
# vulnerability.
#
# The cost of that is stated rather than glossed over: `scratch` means no
# debugging tools inside the container. `docker exec` is not available because
# there is nothing to exec. That is the right trade for a safety-adjacent
# runtime -- diagnostics come out through the metrics endpoint and structured
# logs, both designed for it -- but it is a trade, and a team that expects to
# shell in will find it painful. See ADR-0009.

ARG UBUNTU_VERSION=24.04

# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ \
      cmake \
      ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copied separately from the sources so that a change to a .cpp does not
# invalidate the configure step's cache layer.
COPY CMakeLists.txt ./
COPY cmake ./cmake

COPY include ./include
COPY src ./src

# Static linking is what makes the scratch stage possible. Tests, benchmarks and
# examples are off: tests would need network access to fetch GoogleTest, which
# has no business in an image build.
RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DSAFEEDGE_BUILD_TESTS=OFF \
      -DSAFEEDGE_BUILD_BENCHMARKS=OFF \
      -DSAFEEDGE_BUILD_EXAMPLES=OFF \
      -DCMAKE_EXE_LINKER_FLAGS="-static" \
 && cmake --build build --target safeedged -j "$(nproc)" \
 && strip build/safeedged

# ---------------------------------------------------------------------------
# @satisfies REQ-EDGE-005
FROM scratch

COPY --from=build /src/build/safeedged /safeedged

# Numeric because there is no /etc/passwd to resolve a name against. 65532 is
# the conventional "nonroot" id used by distroless images.
# @satisfies REQ-EDGE-006
USER 65532:65532

EXPOSE 9100

ENV SAFEEDGE_FREQUENCY_HZ=1000 \
    SAFEEDGE_METRICS_PORT=9100 \
    SAFEEDGE_RT_PRIORITY=80

# The binary probes itself, because there is no curl in here to do it. See the
# --healthcheck mode in safeedged.cpp.
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
  CMD ["/safeedged", "--healthcheck"]

ENTRYPOINT ["/safeedged"]
