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
                       ninja-build          \
                       python3-numpy        \
                       file                 \
                       zlib1g-dev           \
                       zstd                 \
                       curl                 \
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
                       clang-11             \
                       clang++-11           \
                       clang-18             \
                       clang++-18           \
                       sudo

RUN mkdir -p /opt/llvm && chmod 777 /opt/llvm
