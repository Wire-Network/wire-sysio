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
    clang-18             \
    clang-tools-18       \
    libc++-18-dev        \
    libc++-dev           \
    libbz2-dev           \
    libstdc++-14-dev     \
    g++-10               \
    g++-12               \
    gcc-10               \
    gcc-12               \
    autoconf automake libtool libltdl-dev

ENV CC=/usr/bin/clang-18
ENV CXX=/usr/bin/clang++-18
ENV CMAKE_MAKE_PROGRAM=/usr/bin/ninja

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
