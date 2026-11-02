# Multi-stage Dockerfile
# Stages:
#   1) distro-deps   - Install OS-level toolchain and libraries (base for all other stages)
#   2) source-deps   - Install LLVM 23.1.3 and its C++ runtime
#   3) app-source-*  - Prepare application sources (from local context or remote repo)
#   4) app-build-*   - Build the application from the chosen app-source-* stage
#
# Use `--target` at build time to select which final build stage to produce:
#   docker build --target=app-build-local -t wire-sysio:local .
#   docker build --target=app-build-repo  -t wire-sysio:repo  .

ARG UBUNTU_TAG=24.04
FROM ubuntu:${UBUNTU_TAG} AS distro-deps
ENV DEBIAN_FRONTEND=noninteractive

# --- Stage 1: distro-deps ----------------------------------------------------
# Base image with system packages required for building (compilers, cmake, Python, etc.)

ENV DEBIAN_FRONTEND=noninteractive
ENV PLATFORM_DIR=/wire
ENV APP_DIR="${PLATFORM_DIR}/wire-sysio"
ENV CDT_DIR="${PLATFORM_DIR}/wire-cdt"

ENV BUILD_TYPE=Release
ENV APP_BUILD_DIR="${APP_DIR}/build/${BUILD_TYPE}"
ENV CDT_BUILD_DIR="${CDT_DIR}/build/${BUILD_TYPE}"

ENV APP_DEV_BUILD_DIR="${APP_DIR}/build/debug-docker"
ENV CDT_DEV_BUILD_DIR="${CDT_DIR}/build/debug-docker"

ENV WIRE_LLVM_ROOT=/opt/llvm-23.1.3

# vcpkg deterministic behavior (manifests/registries/versions + compiler tracking)
ENV VCPKG_FEATURE_FLAGS=manifests,registries,versions,compilertracking
ENV VCPKG_DEFAULT_TRIPLET=x64-linux-release
ENV VCPKG_OVERLAY_TRIPLETS=/wire/wire-sysio/.github/vcpkg-triplets

# Core package tools and repository helpers
RUN apt-get update && \
    apt-get install -y \
        lsb-release \
        wget \
        software-properties-common

# Build toolchain and development libraries (binutils intentionally omitted). The Java runtime
# drives the ANTLR parser generator of the query engine plugin at build time.
RUN apt-get install -y \
        build-essential \
        openjdk-21-jre-headless \
        binutils \
        ccache \
        cmake \
        curl \
        doxygen \
        git \
        gnupg \
        golang \
        libbz2-dev \
        libcurl4-openssl-dev \
        libgmp-dev \
        liblzma-dev \
        libncurses5-dev \
        libstdc++-14-dev \
        libusb-1.0-0-dev \
        libzstd-dev \
        ninja-build \
        pkg-config \
        python3 \
        python3-pip \
        python3-venv \
        python3-dev \
        autoconf \
        autoconf-archive \
        automake \
        libtool \
        sudo \
        tar \
        unzip \
        vim \
        zip \
        zlib1g-dev

RUN mkdir -p /opt/ccache-storage && ccache -M 40GB -d /opt/ccache-storage

# Stage 2: source-deps
FROM distro-deps AS source-deps
# The same verified host distribution is used by local builds and CI.
RUN --mount=type=bind,from=llvm-scripts,target=/opt/llvm-scripts \
    bash /opt/llvm-scripts/install-toolchain.sh "${WIRE_LLVM_ROOT}"
ENV PATH="${WIRE_LLVM_ROOT}/bin:${PATH}"
ENV CC=${WIRE_LLVM_ROOT}/bin/clang \
    CXX=${WIRE_LLVM_ROOT}/bin/clang++

ENV MAX_JOBS=16

# Stage 3a: app-source-local
# Use local build context as the application source.
FROM source-deps AS app-source-local
RUN mkdir -p ${APP_DIR}
WORKDIR ${APP_DIR}
COPY --from=app-root / ${APP_DIR}/
RUN if [ -f ./vcpkg/bootstrap-vcpkg.sh ]; then ./vcpkg/bootstrap-vcpkg.sh; else echo "vcpkg bootstrap script not found, skipping"; fi

# Stage 3b: app-source-repo
# Fetch application source from a remote repository (branch configurable via ARGs).
FROM source-deps AS app-source-repo
ARG SYSIO_BRANCH=master
ARG REPO=https://github.com/Wire-Network/wire-sysio
RUN mkdir -p /wire
WORKDIR /wire

# Clone the selected branch with submodules, then ensure full submodule initialization
RUN git clone --recursive -b ${SYSIO_BRANCH} ${REPO} ${APP_DIR}
WORKDIR ${APP_DIR}
RUN git submodule update --init --recursive
RUN if [ -f ./vcpkg/bootstrap-vcpkg.sh ]; then ./vcpkg/bootstrap-vcpkg.sh; else echo "vcpkg bootstrap script not found, skipping"; fi

# Stage 4a: app-build-local
# Configure and build the application from local sources.
## BUILD WITH LOCAL SOURCE CODE
FROM app-source-local AS app-build-local
RUN mkdir -p ${APP_BUILD_DIR}
WORKDIR ${APP_BUILD_DIR}

RUN set -eux; \
  unset CFLAGS CXXFLAGS LDFLAGS CPPFLAGS || true; \
  rc=0; \
  env \
    VCPKG_MAX_CONCURRENCY=8 \
    CMAKE_BUILD_PARALLEL_LEVEL=8 \
    CC=${CC} \
    CXX=${CXX} \
    cmake \
      -S "$(realpath --relative-to=$PWD ${APP_DIR})" \
      -B . \
      -G Ninja \
      -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
      -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
      -DCMAKE_TOOLCHAIN_FILE="${APP_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" \
      -DCMAKE_C_COMPILER="${CC}" \
      -DCMAKE_CXX_COMPILER="${CXX}" \
      -DCMAKE_AR="${WIRE_LLVM_ROOT}/bin/llvm-ar" \
      -DCMAKE_RANLIB="${WIRE_LLVM_ROOT}/bin/llvm-ranlib" \
      -DENABLE_CCACHE=ON 

RUN cmake --build . -- -j$(( $([[ "$(nproc)" -gt ${MAX_JOBS} ]] && echo ${MAX_JOBS} || nproc) - 1 ))

# Ensure application binaries are in PATH
ENV PATH="${APP_BUILD_DIR}/bin:${PATH}"

# Stage 4b: app-build-repo
# Configure and build the application from sources cloned from the repository.
## BUILD WITH REPO SOURCE CODE
FROM app-source-repo AS app-build-repo
RUN mkdir -p ${APP_BUILD_DIR}
WORKDIR ${APP_BUILD_DIR}
RUN env CC=${CC} CXX=${CXX} \
    cmake \
    -S $(realpath --relative-to=$PWD ${APP_DIR}) \
    -B . \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
    -DCMAKE_TOOLCHAIN_FILE=${APP_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_C_COMPILER=${CC} \
    -DCMAKE_CXX_COMPILER=${CXX} \
    -DENABLE_CCACHE=ON

RUN cmake --build . -- -j$(( $([[ "$(nproc)" -gt ${MAX_JOBS} ]] && echo ${MAX_JOBS} || nproc) - 1 ))

# Ensure application binaries are in PATH
ENV PATH="${APP_BUILD_DIR}/bin:${PATH}"

# Stage 4c: app-dev
# Configure and build the application from sources cloned from the repository.
## BUILD WITH REPO SOURCE CODE
FROM source-deps AS app-dev
RUN mkdir -p ${APP_DIR}
VOLUME ["${APP_DIR}"]
WORKDIR ${APP_DIR}
ENV PATH="${APP_BUILD_DIR}/bin:${PATH}"

# Stage 4d: platform-dev
# Setup development environment with both app and cdt source code volumes.
## DO NOT BUILD, JUST SETUP VOLUMES
FROM source-deps AS platform-dev
RUN mkdir -p ${PLATFORM_DIR}
VOLUME ["${PLATFORM_DIR}"]
WORKDIR ${PLATFORM_DIR}
ENV PATH="${CDT_DEV_BUILD_DIR}/bin:${APP_DEV_BUILD_DIR}/bin:${PATH}"

# Stage 5: cdt-source-repo
# Cloning Wire CDT source from the remote repository.
FROM app-build-repo AS cdt-source-repo
ARG CDT_BRANCH=master
WORKDIR /wire
RUN git clone --recursive -b ${CDT_BRANCH} https://github.com/Wire-Network/wire-cdt.git
WORKDIR /wire/wire-cdt
RUN if [ -f ./vcpkg/bootstrap-vcpkg.sh ]; then ./vcpkg/bootstrap-vcpkg.sh; else echo "vcpkg bootstrap script not found, skipping"; fi

# Stage 6: cdt-build-repo
# Build the Wire CDT from source (for local development purposes).
## BUILD WITH LOCAL SOURCE CODE
FROM cdt-source-repo AS cdt-build-repo
RUN mkdir -p ${CDT_BUILD_DIR}
WORKDIR ${CDT_BUILD_DIR} 
RUN env CC=${CC} CXX=${CXX} \
        cmake -S $(realpath --relative-to=$PWD ${APP_DIR}) \
        -B . \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
        -DCMAKE_TOOLCHAIN_FILE=${APP_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DCMAKE_C_COMPILER=${CC} \
        -DCMAKE_CXX_COMPILER=${CXX} \
          -Dsysio_DIR=${APP_BUILD_DIR} \
        -DENABLE_CCACHE=ON

RUN  cmake --build . -- -j$(( $([[ "$(nproc)" -gt ${MAX_JOBS} ]] && echo ${MAX_JOBS} || nproc) - 1 ))
