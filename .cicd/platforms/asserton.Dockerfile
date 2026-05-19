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
                       mono-complete        \
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

ENV CC=/usr/bin/clang-18
ENV CXX=/usr/bin/clang++-18
ENV CMAKE_MAKE_PROGRAM=/usr/bin/ninja

COPY <<-EOF /extras.cmake
  # reset the build type to empty to disable any cmake default flags
  set(CMAKE_BUILD_TYPE "" CACHE STRING "" FORCE)
  set(CMAKE_C_FLAGS "-O3" CACHE STRING "")
  set(CMAKE_CXX_FLAGS "-O3" CACHE STRING "")
  # Workflow's command-line -DCMAKE_BUILD_TYPE=Release wins over the FORCE above,
  # bringing back -DNDEBUG via CMAKE_*_FLAGS_RELEASE.  Force the Release flags too
  # so asserts stay compiled regardless of which BUILD_TYPE wins.
  set(CMAKE_C_FLAGS_RELEASE   "-O3" CACHE STRING "" FORCE)
  set(CMAKE_CXX_FLAGS_RELEASE "-O3" CACHE STRING "" FORCE)
  set(SYSIO_ENABLE_RELEASE_BUILD_TEST "Off" CACHE BOOL "")
EOF

ENV SYSIO_PLATFORM_HAS_EXTRAS_CMAKE=1
