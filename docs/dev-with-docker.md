# Developing with docker

This document describes how to set up a development environment for the project using Docker. This approach ensures that all developers have a consistent environment, regardless of their host operating system.

This document covers creating a local container with a mapped volume to the platform root directory.
To be clear, this is the parent directory where you have cloned this repo, as well as `wire-system-contracts`, `wire-cdt`, and any other related repositories.
This directory will be referred to as `<PLATFORM_DIR>` in the instructions below.

> NOTE: You may need to configure Docker Trust as well as allocate more RAM & CPU to Docker. Building the project alone requires 32GB of RAM.

> NOTE: If you want to see how the Docker build process works, checkout [scripts/docker-build.sh](../scripts/docker-build.sh).

## Prerequisites

1. Install [Docker](https://docs.docker.com/get-docker/) on your machine.
2. Install [Docker Compose](https://docs.docker.com/compose/install/) if it's not included with your Docker installation.
3. You have cloned `wire-sysio`,`wire-cdt`, and `wire-system-contracts` repositories into the `<PLATFORM_DIR>` directory.

## Setting Up the Development Environment

> This doc assumes you have already cloned the repo and are in the root directory of the project.

### 1. Create the docker image for the development environment

Run the docker build script to create the docker image.

```shell
./scripts/docker-build.sh --target=platform-dev --tag=wire/sysio:platform-dev
```

### 2. Create the docker container

Create the docker container using the image created in the previous step.

```shell
# Replace <PLATFORM_DIR> with the path to your platform root directory
docker create --name wire-platform-dev  -v <PLATFORM_DIR>:/wire -it wire/sysio:platform-dev /bin/bash
```

### 3. Start the docker container & attach

Start the docker container and open a bash shell inside it.

```shell
docker start wire-platform-dev && \
  docker exec -it wire-platform-dev /bin/bash
```

### 4. Build the project inside the container

Once inside the container, navigate to the `/wire` directory and build the project.

#### Step 1: Build `wire-sysio` in Debug mode

```shell
cd /wire/wire-sysio
mkdir -p build/debug-docker
cmake -S . \
  -B build/debug-docker \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=/opt/clang/clang-18/bin/clang-18 \
  -DCMAKE_CXX_COMPILER=/opt/clang/clang-18/bin/clang++ \
  -DCMAKE_INSTALL_PREFIX=/opt/llvm/llvm-11 \
  -DCMAKE_PREFIX_PATH="/opt/llvm/llvm-11;/opt/clang/clang-18"

cmake --build build/debug-docker -- -j"$(nproc)"

# Add the built sysio binaries to the PATH
export PATH=/wire/wire-sysio/build/debug-docker/bin:$PATH
```

#### Step 2: Build `wire-cdt` in Debug mode

Go to the `wire-cdt` directory and build it in Debug mode.

```shell
cd /wire/wire-cdt
mkdir -p build/debug-docker
cmake -S . \
  -B build/debug-docker \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=/opt/clang/clang-18/bin/clang-18 \
  -DCMAKE_CXX_COMPILER=/opt/clang/clang-18/bin/clang++ \
  -DCMAKE_INSTALL_PREFIX=/opt/llvm/llvm-11 \
  -DCMAKE_PREFIX_PATH="/opt/llvm/llvm-11;/opt/clang/clang-18"

cmake --build build/debug-docker -- -j"$(nproc)"

# Add the built CDT binaries to the PATH
export PATH=/wire/wire-cdt/build/debug-docker/bin:$PATH
```
#### Step 3: Build `wire-system-contracts` in Debug mode

Go to the `wire-system-contracts` directory and build it in Debug mode, using the `wire-cdt` build from the previous step.

```shell
cd /wire/wire-system-contracts
./build.sh -c /wire/wire-cdt/build/debug-docker
```