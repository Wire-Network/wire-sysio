FROM ubuntu:22.04 as tester

ARG BRANCH=master
ARG REPO=https://github.com/Wire-Network/wire-sysio

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
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
        python3-pip \
        zlib1g-dev \
        libbz2-dev \
        liblzma-dev \
        libncurses5-dev \
        libzstd-dev \
    && rm -rf /var/lib/apt/lists/*

RUN add-apt-repository ppa:deadsnakes/ppa -y
RUN apt-get update && apt-get install -y python3.12
RUN update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.12 1

RUN curl -sS https://bootstrap.pypa.io/get-pip.py | python3.12

RUN python3.12 -m pip install --upgrade pip
RUN python3.12 -m pip install numpy

RUN mkdir /wire
WORKDIR /wire

RUN git clone -b ${BRANCH} ${REPO} /wire/wire-sysio
WORKDIR /wire/wire-sysio

RUN git submodule update --init --recursive

RUN mkdir build
WORKDIR /wire/wire-sysio/build

RUN cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/lib/llvm-11 .. && make -j$(nproc)