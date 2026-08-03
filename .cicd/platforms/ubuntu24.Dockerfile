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
