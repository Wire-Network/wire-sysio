# Multi-stage Dockerfile
# Stages:
#   1) distro-deps   - Install OS-level toolchain and libraries (base for all other stages)
#   2) source-deps   - Install source-built dependencies (LLVM 11) on top of distro-deps
#   3) app-source-*  - Prepare application sources (from local context or remote repo)
#   4) app-build-*   - Build the application from the chosen app-source-* stage
#
# Use `--target` at build time to select which final build stage to produce:
#   docker build --target=app-build-local -t wire-sysio:local .
#   docker build --target=app-build-repo  -t wire-sysio:repo  .

# Stage 1: distro-deps
# Base image with system packages required for building (compilers, cmake, Python, etc.)
FROM ubuntu:24.04 AS distro-deps

ENV DEBIAN_FRONTEND=noninteractive
ENV PLATFORM_DIR=/wire
ENV APP_DIR="${PLATFORM_DIR}/wire-sysio"
ENV CDT_DIR="${PLATFORM_DIR}/wire-cdt"

ENV BUILD_TYPE=Release
ENV APP_BUILD_DIR="${APP_DIR}/build/${BUILD_TYPE}"
ENV CDT_BUILD_DIR="${CDT_DIR}/build/${BUILD_TYPE}"

ENV APP_DEV_BUILD_DIR="${APP_DIR}/build/debug-docker"
ENV CDT_DEV_BUILD_DIR="${CDT_DIR}/build/debug-docker"

ENV LLVM_11_DIR=/opt/llvm/llvm-11

# Core package tools and repository helpers
RUN apt-get update
RUN apt-get install -y \
        lsb-release \
        wget \
        software-properties-common

# Enable Python 3.12 PPA and refresh indexes
RUN add-apt-repository ppa:deadsnakes/ppa -y
RUN apt-get update

# Build toolchain and development libraries needed by the application
RUN apt-get install -y \
        gnupg \
        build-essential \
        ccache \
        ninja-build \
        cmake \
        curl \
        zip \
        unzip \
        tar \
        git \
        vim \
        sudo \
        doxygen \
        libboost-all-dev \
        libcurl4-openssl-dev \
        libgmp-dev \
        libssl-dev \
        libusb-1.0-0-dev \
        pkg-config \
        python3.12 \
        python3-pip \
        zlib1g-dev \
        libbz2-dev \
        liblzma-dev \
        libncurses5-dev \
        libzstd-dev
    	

# Make Python 3.12 the default python3
RUN update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.12 1
RUN mkdir -p /opt/ccache-storage & ccache -M 40GB -d /opt/ccache-storage


# Stage 2: source-deps
# Install dependencies built from source (LLVM 11) on top of distro-deps.
FROM distro-deps AS source-deps

# Install LLVM 11 from provided scripts into /opt/llvm/llvm-11
RUN mkdir -p /opt/llvm/scripts
RUN mkdir -p LLVM_11_DIR

# Build and install LLVM 11 using the helper script
RUN --mount=type=bind,from=llvm-11-scripts,target=/opt/llvm/scripts \
    BASE_DIR=/opt/llvm /opt/llvm/scripts/llvm-11-ubuntu-build-source.sh

# Stage 3a: app-source-local
# Use local build context as the application source.
FROM source-deps AS app-source-local
RUN mkdir -p ${APP_DIR}
WORKDIR ${APP_DIR}
COPY --from=app-root / ${APP_DIR}/
RUN ./vcpkg/bootstrap-vcpkg.sh

# Stage 3b: app-source-repo
# Fetch application source from a remote repository (branch configurable via ARGs).
FROM source-deps AS app-source-repo
ARG BRANCH=master
ARG REPO=https://github.com/Wire-Network/wire-sysio
RUN mkdir -p /wire
WORKDIR /wire

# Clone the selected branch with submodules, then ensure full submodule initialization
RUN git clone --recursive -b ${BRANCH} ${REPO} ${APP_DIR}
WORKDIR ${APP_DIR}
RUN git submodule update --init --recursive
RUN ./vcpkg/bootstrap-vcpkg.sh

# Stage 4a: app-build-local
# Configure and build the application from local sources.
## BUILD WITH LOCAL SOURCE CODE
FROM app-source-local AS app-build-local
RUN mkdir -p ${APP_BUILD_DIR}
WORKDIR ${APP_BUILD_DIR}
RUN cmake \
    -S $(realpath --relative-to=$PWD ${APP_DIR}) \
    -B . \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
    -DCMAKE_TOOLCHAIN_FILE=${APP_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_C_COMPILER=gcc-10 \
    -DCMAKE_CXX_COMPILER=g++-10 \
    -DCMAKE_PREFIX_PATH=${LLVM_11_DIR} \
    -DENABLE_CCACHE=ON

RUN ninja -j$([[ "$(nproc)" -gt 8 ]] && echo 8 || nproc)

# Ensure application binaries are in PATH
ENV PATH="${APP_BUILD_DIR}/bin:${PATH}"

# Stage 4b: app-build-repo
# Configure and build the application from sources cloned from the repository.
## BUILD WITH REPO SOURCE CODE
FROM app-source-repo AS app-build-repo
RUN mkdir -p ${APP_BUILD_DIR}
WORKDIR ${APP_BUILD_DIR}
RUN apt install -y golang
RUN cmake \
    -S $(realpath --relative-to=$PWD ${APP_DIR}) \
    -B . \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
    -DCMAKE_TOOLCHAIN_FILE=${APP_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_C_COMPILER=gcc-10 \
    -DCMAKE_CXX_COMPILER=g++-10 \
    -DCMAKE_PREFIX_PATH=${LLVM_11_DIR} \
    -DENABLE_CCACHE=ON


RUN ninja -j$([[ "$(nproc)" -gt 8 ]] && echo 8 || nproc)

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
RUN cd /wire && git clone --recursive https://github.com/Wire-Network/wire-cdt.git
WORKDIR /wire/wire-cdt
RUN ./vcpkg/bootstrap-vcpkg.sh

# Stage 6: cdt-build-repo
# Build the Wire CDT from source (for local development purposes).
## BUILD WITH LOCAL SOURCE CODE
FROM cdt-source-repo AS cdt-build-repo
RUN mkdir -p ${CDT_BUILD_DIR}
RUN cd ${CDT_BUILD_DIR} && cmake -S $(realpath --relative-to=$PWD ${APP_DIR}) \
      -B . \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -DCMAKE_C_COMPILER=gcc-10 \
      -DCMAKE_CXX_COMPILER=g++-10 \
      -DCMAKE_PREFIX_PATH=${LLVM_11_DIR}

RUN cd ${CDT_BUILD_DIR} && ninja -j$([[ "$(nproc)" -gt 8 ]] && echo 8 || nproc)