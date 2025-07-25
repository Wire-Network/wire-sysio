FROM ubuntu:22.04 as tester

ARG BRANCH=master
ARG REPO=https://github.com/Wire-Network/wire-sysio

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update
RUN apt-get install -y \
        lsb-release \
        wget \
        software-properties-common \
        gnupg \
        build-essential \
        cmake \
        curl \
        git \
        vim \
        sudo \
        doxygen \
        llvm-11 \
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
        libzstd-dev
RUN add-apt-repository ppa:deadsnakes/ppa -y
RUN DEBIAN_FRONTEND=noninteractive apt-get install python3.12 -y
RUN update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.12 1

RUN mkdir /wire
WORKDIR /wire

RUN git clone -b ${BRANCH} ${REPO} /wire/wire-sysio
WORKDIR /wire/wire-sysio

RUN git submodule update --init --recursive

RUN mkdir build
WORKDIR /wire/wire-sysio/build

RUN cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/lib/llvm-11 .. && make -j4