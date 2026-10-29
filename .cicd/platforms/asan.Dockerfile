# syntax=docker/dockerfile:1
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
                       openjdk-21-jre-headless \
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
                       autoconf             \
                       automake             \
                       libtool              \
                       htop

RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 && \
    update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100

# Set compiler environment variables
ENV CC=/usr/bin/clang-18
ENV CXX=/usr/bin/clang++-18
ENV CMAKE_MAKE_PROGRAM=/usr/bin/ninja

COPY <<-EOF /extras.cmake
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
  set(CMAKE_C_COMPILER "clang-18" CACHE STRING "")
  set(CMAKE_CXX_COMPILER "clang++-18" CACHE STRING "")
  set(CMAKE_C_FLAGS "-fsanitize=address -fno-omit-frame-pointer" CACHE STRING "")
  set(CMAKE_CXX_FLAGS "-fsanitize=address -fno-omit-frame-pointer" CACHE STRING "")
EOF

ENV SYSIO_PLATFORM_HAS_EXTRAS_CMAKE=1
ENV ASAN_OPTIONS=detect_leaks=0

# GitHub CLI from the official apt repository. The release workflows use `gh`
# inside these images (linux_amd64_build.yaml downloads the pinned wire-cdt
# release asset with `gh release download`), and it is installed in EVERY
# platform image rather than just the packaging one so the images stay uniform.
RUN mkdir -p -m 755 /etc/apt/keyrings \
    && wget -nv -O /etc/apt/keyrings/githubcli-archive-keyring.gpg https://cli.github.com/packages/githubcli-archive-keyring.gpg \
    && chmod go+r /etc/apt/keyrings/githubcli-archive-keyring.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" \
       > /etc/apt/sources.list.d/github-cli.list \
    && apt-get update \
    && apt-get install -y gh

# go-ethereum, for the Ethereum client's conformance suite
# (plugins/outpost_ethereum_client_plugin/test/test_geth_dev_conformance.cpp): the
# case starts `geth --dev` and drives the client through a real node's parsing
# and refusals, and it fails rather than skips when no geth is found. Installed
# in EVERY platform image, like gh, so the images stay uniform. Pinned by release
# and checksum so the image is reproducible; bump both together.
ARG GETH_RELEASE=1.17.10-1df30b18
ARG GETH_SHA256=e016aca8cc391c8d5d14dc17c9af589488d99c37ed71d892213c80757935ba64
RUN wget -nv -O /tmp/geth.tar.gz "https://gethstore.blob.core.windows.net/builds/geth-linux-amd64-${GETH_RELEASE}.tar.gz" \
    && echo "${GETH_SHA256}  /tmp/geth.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/geth.tar.gz -C /tmp \
    && install -m 755 "/tmp/geth-linux-amd64-${GETH_RELEASE}/geth" /usr/local/bin/geth \
    && rm -rf /tmp/geth.tar.gz "/tmp/geth-linux-amd64-${GETH_RELEASE}" \
    && geth version
