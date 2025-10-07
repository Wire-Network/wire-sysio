FROM ubuntu:focal
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
                       g++-10               \
                       cmake                \
                       git                  \
                       jq                   \
                       libcurl4-openssl-dev \
                       libgmp-dev           \
                       llvm-11-dev          \
                       ninja-build          \
                       golang               \
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

RUN  update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100 --slave \
                                   /usr/bin/g++ g++ /usr/bin/g++-10 --slave \
                                   /usr/bin/gcov gcov /usr/bin/gcov-10

RUN mkdir -p /opt/llvm && chmod 777 /opt/llvm
