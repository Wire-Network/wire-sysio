# Simpler version without LLVM build
FROM ubuntu:noble
ENV TZ="America/New_York"
ENV DEBIAN_FRONTEND=noninteractive

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
    python3-numpy        \
    file                 \
    zlib1g-dev           \
    zstd                 \
    curl                 \
    zip                  \
    unzip                \
    tar                  \
    sudo                 \
    golang               \
    python3-dev          \
    libffi-dev           \
    libedit-dev          \
    libxml2-dev          \
    libncurses-dev       \
    libtinfo-dev         \
    libzstd-dev          \
    libssl-dev           \
    ccache               \
    libbz2-dev           \
    libstdc++-14-dev     \
    gcc-10               \
    g++-10               \
    gcc-14               \
    g++-14               \
    autoconf automake libtool libltdl-dev

RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 && \
    update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100

RUN mkdir -p /opt/llvm && chmod 777 /opt/llvm

ENV CC=/usr/bin/gcc-14
ENV CXX=/usr/bin/g++-14
ENV CMAKE_MAKE_PROGRAM=/usr/bin/ninja


COPY <<-EOF /extras.cmake
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
  set(CMAKE_C_COMPILER "gcc-14" CACHE STRING "")
  set(CMAKE_CXX_COMPILER "g++-14" CACHE STRING "")
  set(CMAKE_C_FLAGS "-Wall -Wextra -fdiagnostics-color=always" CACHE STRING "")
  set(CMAKE_CXX_FLAGS "-Wall -Wextra -fdiagnostics-color=always" CACHE STRING "")
EOF

ENV SYSIO_PLATFORM_HAS_EXTRAS_CMAKE=1

COPY scripts /scripts
RUN chmod +x /scripts/llvm-11/llvm-11-ubuntu-build-source.sh
RUN BASE_DIR=/opt/llvm /scripts/llvm-11/llvm-11-ubuntu-build-source.sh

ENV CMAKE_PREFIX_PATH=/opt/llvm/llvm-11