FROM ubuntu:noble
ENV TZ="America/New_York"
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update
RUN apt-get install -y \
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
RUN mkdir -p /opt/llvm && chmod 777 /opt/llvm