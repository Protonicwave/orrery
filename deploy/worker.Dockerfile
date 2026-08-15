# The worker: the C++ simulator, and the Python that feeds it.
#
# Two stages, because the toolchain that builds the simulator is several hundred
# megabytes and none of it is needed to run one. The build context is the
# repository root, since this image contains the one thing in this repository
# that is not the service.
#
# The simulator is built from source here rather than downloaded. There is no
# published binary and there should not be: what the worker runs has to be the
# revision the rest of the image came from, and building it in the image is the
# only way to say that without a second thing to keep in step.

FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install --yes --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake/ cmake/
COPY include/ include/
COPY src/ src/
COPY apps/ apps/

# Tests and benchmarks are off. A worker runs `orrery run` and nothing else, and
# building the suite here would fetch Catch2 and compile several hundred cases
# that continuous integration has already run against this revision.
RUN cmake --preset release \
        -DORRERY_BUILD_TESTS=OFF \
        -DORRERY_BUILD_BENCHMARKS=OFF \
    && cmake --build --preset release --target orrery


FROM python:3.12-slim-bookworm

# libstdc++ and libgomp, which the simulator links and the slim image does not
# carry. Named rather than pulled in with a compiler, which is the whole point
# of the second stage.
RUN apt-get update && apt-get install --yes --no-install-recommends \
        libstdc++6 libgomp1 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/release/apps/orrery /usr/local/bin/orrery

WORKDIR /service
COPY service/pyproject.toml ./
COPY service/orrery_service/ orrery_service/
RUN pip install --no-cache-dir .

# Not root. A process that runs a program over a document a stranger sent should
# not be able to write to its own image, and this costs one line.
RUN useradd --create-home --uid 10001 worker

# Where a run works, and the only place this image expects to be able to write.
# Created here, owned by the worker, so that the volume mounted over it in
# deploy/compose.yaml is initialised with that ownership: a fresh named volume
# takes the permissions of the directory it covers, and one created empty by the
# daemon would belong to root and be unwritable by the process that needs it.
RUN mkdir /work && chown worker:worker /work

USER worker

ENV ORRERY_BINARY=/usr/local/bin/orrery
ENTRYPOINT ["python", "-m", "orrery_service.worker"]
