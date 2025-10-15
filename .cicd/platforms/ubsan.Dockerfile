# syntax=docker/dockerfile:1
FROM ubuntu:jammy
ENV TZ="America/New_York"
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get upgrade -y && \
    apt-get install -y build-essential      \
                       cmake                \
                       git                  \
                       jq                   \
                       libcurl4-openssl-dev \
                       libgmp-dev           \
                       llvm-11-dev          \
                       lsb-release          \
                       ninja-build          \
                       python3-numpy        \
                       software-properties-common \
                       file                 \
                       wget                 \
                       zlib1g-dev           \
                       zstd                 \
                       gnupg                \
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
                       libboost-all-dev     \
                       libssl-dev           \
                       libusb-1.0-0-dev     \
                       pkg-config           \
                       python3-dev          \
                       libffi-dev           \
                       libedit-dev          \
                       libxml2-dev          \
                       libncurses-dev       \
                       libtinfo-dev         \
                       libzstd-dev          \
                       libbz2-dev           \
                       liblzma-dev          \
                       libncurses5-dev

RUN add-apt-repository ppa:deadsnakes/ppa -y && apt-get update

RUN apt-get install -y python3.12 python3-pip

RUN update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.12 1

RUN yes | bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)" llvm.sh 18


RUN rm -rf /usr/lib/llvm-18/lib/cmake

COPY <<-EOF /ubsan.supp
  vptr:wasm_sysio_validation.hpp
  vptr:wasm_sysio_injection.hpp
EOF

ENV LEAP_PLATFORM_HAS_EXTRAS_CMAKE=1
COPY <<-EOF /extras.cmake
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)

  set(CMAKE_C_COMPILER "clang-18" CACHE STRING "")
  set(CMAKE_CXX_COMPILER "clang++-18" CACHE STRING "")
  set(CMAKE_C_FLAGS "-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" CACHE STRING "")
  set(CMAKE_CXX_FLAGS "-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" CACHE STRING "")
EOF

ENV UBSAN_OPTIONS=print_stacktrace=1,suppressions=/ubsan.supp