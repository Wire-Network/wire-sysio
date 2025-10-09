FROM ubuntu:focal

ENV TZ="America/New_York"
ENV DEBIAN_FRONTEND=noninteractive
ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8

RUN apt-get update && \
    apt-get install -y \
        lsb-release \
        wget \
        software-properties-common \
        gnupg

# add golang backports PPA for Go >= 1.19
RUN add-apt-repository ppa:deadsnakes/ppa -y && \
    add-apt-repository ppa:longsleep/golang-backports -y && \
    apt-get update

RUN apt-get update && apt-get upgrade -y && \
    apt-get install -y build-essential      \
                       g++-10               \
                       cmake                \
                       git                  \
                       jq                   \
                       libcurl4-openssl-dev \
                       libgmp-dev           \
                       llvm-11-dev          \
                       ninja-build          \
                       golang-go            \
                       python3-numpy        \
                       file                 \
                       zlib1g-dev           \
                       zstd                 \
                       curl                 \
                       sudo                 \
                       zip                  \
                       unzip                \
                       tar                  \
                       python3-dev          \
                       libffi-dev           \
                       libedit-dev          \
                       libxml2-dev          \
                       libncurses-dev       \
                       libtinfo-dev         \
                       libzstd-dev          \
                       libssl-dev           \
                       ccache               \
                       gcc-10               \
                       g++-10               \
                       clang-18             \
                       clang++-18           \
                       autoconf automake libtool \
                       sudo

RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100 --slave \
                         /usr/bin/g++ g++ /usr/bin/g++-10 --slave \
                         /usr/bin/gcov gcov /usr/bin/gcov-10
# RUN apt-get remove -y pkg-config && \
#     wget http://archive.ubuntu.com/ubuntu/pool/main/p/pkg-config/pkg-config_0.29.2-1ubuntu3_amd64.deb && \
#     apt-get install -y ./pkg-config_0.29.2-1ubuntu3_amd64.deb && \
#     rm pkg-config_0.29.2-1ubuntu3_amd64.deb

RUN apt-get remove -y pkg-config && \
    apt-get install -y pkgconf && \
    ln -sf /usr/bin/pkgconf /usr/bin/pkg-config && \
    pkg-config --version
RUN pkg-config --version

RUN mkdir -p /opt/llvm && chmod 777 /opt/llvm

RUN go version