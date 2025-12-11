# syntax=docker/dockerfile:1
FROM ubuntu:noble
ENV TZ="America/New_York"
ENV DEBIAN_FRONTEND=noninteractive

ENV CLANG_18_DIR=/opt/clang/clang-18
ENV LLVM_11_DIR=/opt/llvm/llvm-11

RUN apt-get update && apt-get install -y \
    lsb-release \
    wget \
    software-properties-common \
    gnupg

RUN add-apt-repository ppa:deadsnakes/ppa -y
RUN apt-get update

RUN apt-get update && apt-get upgrade -y && \
    apt-get install -y build-essential      \
                       cmake                \
                       git                  \
                       jq                   \
                       libcurl4-openssl-dev \
                       libgmp-dev           \
                       ninja-build          \
                       python3              \
                       python3-dev          \
                       python3-pip          \
                       python3-numpy        \
                       file                 \
                       zlib1g-dev           \
                       zstd                 \
                       ccache               \
                       curl                 \
                       zip                  \
                       unzip                \
                       tar                  \
                       vim                  \
                       sudo                 \
                       doxygen              \
                       golang               \
                       gcc-10               \
                       g++-10               \
                       gcc-12               \
                       g++-12               \
                       gcc-14               \
                       g++-14               \
                       libstdc++-14-dev     \
                       libboost-all-dev     \
                       libssl-dev           \
                       libusb-1.0-0-dev     \
                       pkg-config           \
                       libffi-dev           \
                       libedit-dev          \
                       libxml2-dev          \
                       libncurses-dev       \
                       libtinfo-dev         \
                       libzstd-dev          \
                       libbz2-dev           \
                       liblzma-dev          \
                       clang-18             \
                       clang-tools-18       \
                       libc++-18-dev        \
                       libc++-dev           \
                       autoconf automake libtool libltdl-dev \
                       python3-venv         \
                       htop

RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 && \
    update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100

RUN mkdir -p /opt/llvm && chmod 777 /opt/llvm
RUN mkdir -p /opt/clang && chmod 777 /opt/clang

COPY <<-EOF /ubsan.supp
  vptr:wasm_sysio_validation.hpp
  vptr:wasm_sysio_injection.hpp
EOF

ENV CC=/usr/bin/clang-18
ENV CXX=/usr/bin/clang++-18
ENV CMAKE_MAKE_PROGRAM=/usr/bin/ninja
ENV CMAKE_PREFIX_PATH="${LLVM_11_DIR}"

COPY <<-EOF /extras.cmake
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
  set(CMAKE_C_COMPILER "clang-18" CACHE STRING "")
  set(CMAKE_CXX_COMPILER "clang++-18" CACHE STRING "")
  set(CMAKE_C_FLAGS "-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" CACHE STRING "")
  set(CMAKE_CXX_FLAGS "-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" CACHE STRING "")
EOF

ENV SYSIO_PLATFORM_HAS_EXTRAS_CMAKE=1
ENV UBSAN_OPTIONS=print_stacktrace=1,suppressions=/ubsan.supp

# Copy scripts directory and run LLVM build script
COPY scripts /scripts

RUN mkdir -p /opt/clang/scripts && \
    ln -sf /scripts/clang-18/clang-18-ubuntu-build-source.sh \
           /opt/clang/scripts/clang-18-ubuntu-build-source.sh

RUN chmod +x /scripts/clang-18/clang-18-ubuntu-build-source.sh && \
    BASE_DIR=/opt/clang /scripts/clang-18/clang-18-ubuntu-build-source.sh

RUN chmod +x /scripts/llvm-11/llvm-11-ubuntu-build-source.sh && \
    BASE_DIR=/opt/llvm /scripts/llvm-11/llvm-11-ubuntu-build-source.sh

ENV PATH="${CLANG_18_DIR}/bin:${PATH}"
ENV CC=${CLANG_18_DIR}/bin/clang
ENV CXX=${CLANG_18_DIR}/bin/clang++