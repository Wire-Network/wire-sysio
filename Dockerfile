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
        cmake \
        curl \
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
        python3 \
        zlib1g-dev \
        libbz2-dev \
        liblzma-dev \
        libncurses5-dev \
        libzstd-dev \
    		python3.12

# Make Python 3.12 the default python3
RUN update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.12 1

# Stage 2: source-deps
# Install dependencies built from source (LLVM 11) on top of distro-deps.
FROM distro-deps AS source-deps

# Install LLVM 11 from provided scripts into /opt/llvm/llvm-11
RUN mkdir -p /opt/llvm/scripts
RUN mkdir -p /opt/llvm/llvm-11
COPY scripts/llvm-11/* /opt/llvm/scripts/

# Build and install LLVM 11 using the helper script
RUN BASE_DIR=/opt/llvm /opt/llvm/scripts/llvm-11-ubuntu-build-source.sh

# Stage 3a: app-source-local
# Use local build context as the application source.
FROM source-deps AS app-source-local

RUN mkdir -p /wire/wire-sysio
WORKDIR /wire/wire-sysio
COPY . /wire/wire-sysio/

# Stage 3b: app-source-repo
# Fetch application source from a remote repository (branch configurable via ARGs).
FROM source-deps AS app-source-repo
ARG BRANCH=master
ARG REPO=https://github.com/Wire-Network/wire-sysio

RUN mkdir -p /wire
WORKDIR /wire

# Clone the selected branch with submodules, then ensure full submodule initialization
RUN git clone --recursive -b ${BRANCH} ${REPO} /wire/wire-sysio
WORKDIR /wire/wire-sysio
RUN git submodule update --init --recursive

# Stage 4a: app-build-local
# Configure and build the application from local sources.
## BUILD WITH LOCAL SOURCE CODE
FROM app-source-local AS app-build-local
WORKDIR /wire/wire-sysio
RUN mkdir build
WORKDIR /wire/wire-sysio/build
RUN cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc-10 \
    -DCMAKE_CXX_COMPILER=g++-10 \
    -DCMAKE_PREFIX_PATH=/opt/llvm/llvm-11 .. \
    && \
    make -j$([[ "$(nproc)" -gt 16 ]] && echo 16 || nproc)

# Stage 4b: app-build-repo
# Configure and build the application from sources cloned from the repository.
## BUILD WITH LOCAL SOURCE CODE
FROM app-source-repo AS app-build-repo
WORKDIR /wire/wire-sysio
RUN mkdir build
WORKDIR /wire/wire-sysio/build
RUN cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc-10 \
    -DCMAKE_CXX_COMPILER=g++-10 \
    -DCMAKE_PREFIX_PATH=/opt/llvm/llvm-11 .. \
    && \
    make -j$([[ "$(nproc)" -gt 8 ]] && echo 8 || nproc)