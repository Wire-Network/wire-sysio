FROM ubuntu:resolute
ENV TZ="America/New_York"
ENV DEBIAN_FRONTEND=noninteractive

# Builds with the Ubuntu 26.04 default toolchain: clang 21 over GCC 15's libstdc++.
RUN apt-get update && apt-get upgrade -y && \
    apt-get install -y \
    lsb-release          \
    wget                 \
    gnupg                \
    build-essential      \
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
    libzstd-dev          \
    libssl-dev           \
    ccache               \
    clang-21             \
    clang-tools-21       \
    libc++-21-dev        \
    libbz2-dev           \
    autoconf automake libtool libltdl-dev

ENV CC=/usr/bin/clang-21
ENV CXX=/usr/bin/clang++-21
ENV CMAKE_MAKE_PROGRAM=/usr/bin/ninja

# GitHub CLI from the official apt repository, as in every platform image.
RUN mkdir -p -m 755 /etc/apt/keyrings \
    && wget -nv -O /etc/apt/keyrings/githubcli-archive-keyring.gpg https://cli.github.com/packages/githubcli-archive-keyring.gpg \
    && chmod go+r /etc/apt/keyrings/githubcli-archive-keyring.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" \
       > /etc/apt/sources.list.d/github-cli.list \
    && apt-get update \
    && apt-get install -y gh
