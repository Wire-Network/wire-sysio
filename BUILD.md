# Build from Source Instructions

> For ubuntu 24.04,
> use [scripts/llvm-11/llvm-11-ubuntu-build-source.sh](scripts/llvm-11/llvm-11-ubuntu-build-source.sh).
> additionally this script should enable support for most major distros, like Arch, Fedora, etc. to be built on
> regardless of version.

# Get the source code

Clone this repo as follows::

```bash
# Clone with HTTP
git clone --recursive https://github.com/Wire-Network/wire-sysio.git

# Clone with SSH
# Uncomment the following line if you have SSH keys set up
# git clone --recursive git@github.com:Wire-Network/wire-sysio.git

# Enter the repo directory
cd wire-sysio
```

# On Host Machine 

> ONLY SUPPORTS: Ubuntu `20.04,22.04,24.04` on `x86_64`

## Prerequisites

You will need to build on a [supported operating system](README.md#supported-operating-systems).

**Requirements to build:**

- C++20 compiler and standard library
- CMake 3.16+
- LLVM 11 - for Linux only
  - Ubuntu 24.04 requires building from source.
    - [scripts/llvm-11/llvm-11-ubuntu-build-source.sh](scripts/llvm-11/llvm-11-ubuntu-build-source.sh)
  - newer versions do not work
- libcurl 7.40.0+
- git
- GMP
- Python 3
- python3-numpy
- zlib


## Step 1 - Install Dependencies

### 1a. Ubuntu (20.04, 22.04, 24.04)

On any supported Ubuntu version (20.04, 22.04, 24.04), install the following dependencies:

```shell
# First install `software-properties-common` for the `add-apt-repository` command
sudo apt update 
sudo apt install -y \
        lsb-release \
        wget \
        software-properties-common

# Enable Python 3.12 PPA and refresh indexes
sudo add-apt-repository ppa:deadsnakes/ppa -y
sudo apt update

# Build toolchain and development libraries needed by the application
sudo apt install -y \
    	build-essential \
      cmake \
      curl \
      doxygen \
      git \
      gnupg \
      libboost-all-dev \
      libbz2-dev \
      libcurl4-openssl-dev \
      libgmp-dev \
      liblzma-dev \
      libncurses5-dev \
      libssl-dev \
      libusb-1.0-0-dev \
      libzstd-dev \
      ninja-build \
      pkg-config \
      python3.12 \
      tar \
      unzip \
      zip \
      zlib1g-dev

```
### 1b. Ubuntu (20.04, 22.04)

On Ubuntu 20.04 and 22.04, install LLVM 11 from the official repositories:

```shell
sudo apt install -y llvm-11
```
### 1c. Ubuntu (24.04)

On Ubuntu 24.04, build/install LLVM 11 from source.
You can use our script to do this, or build from source yourself.

```shell
# Set the base directory for LLVM 11 installation.
# Source code will be downloaded to $BASE_DIR/llvm-project
# Binary packages will be installed in the prefix $BASE_DIR/llvm-11
export BASE_DIR=/opt/llvm

# Run this script to build and install LLVM 11 from source
./scripts/llvm-11/llvm-11-ubuntu-build-source.sh
```

> *IMPORTANT* Remember the path for the purpose of using with CMake.
> Example `-DCMAKE_PREFIX_PATH=${BASE_DIR}/llvm-11`

### 1d. Bootstrap vcpkg

Wire Sysio, while being a derivative/fork of `leap v5`, it uses a different dependency management system in-line with modern tooling, specifically, `vcpkg`.  

After installing the required system dependencies, you must bootstrap vcpkg from the root of the cloned `wire-sysio`:

```sh
# From wire-sysio repo root, run
./vcpkg/bootstrap-vcpkg.sh
```

You are now ready to build.

## Step 2 - Build

Select build instructions below based on OS.

> ⚠️ **A Warning On Parallel Compilation Jobs (`-j` flag)** ⚠️  
> When building C/C++ software, often the build is performed in parallel via a command such as `make -j "$(nproc)"` which
> uses all available CPU threads. However, be aware that some compilation units (`*.cpp` files) in Wire Sysion will
> consume nearly 4GB of memory. Failures due to memory exhaustion will typically, but not always, manifest as compiler
> crashes. Using all available CPU threads may also prevent you from doing other things on your computer during
> compilation. For these reasons, consider reducing this value.

### Build

To build, make sure you are in the root of the `wire-sysio` repo, then run the following commands:

```bash
mkdir -p build

## on Ubuntu 20 & 22
cmake -B build -S . \
  -DCMAKE_C_COMPILER=gcc-10 \
  -DCMAKE_CXX_COMPILER=g++-10 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/usr/lib/llvm-11 \
  ..

## on Ubuntu 24, you must set the prefix path 
## to the location of your LLVM 11 build completed in step 1c
cmake -B build -S . \
  -DCMAKE_C_COMPILER=gcc-10 \
  -DCMAKE_CXX_COMPILER=g++-10 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=<LLVM_PREFIX_PATH_FROM_STEP_1C> \
  .. 

## Build for development
## If you want to build with debug symbols, and with access to the LLVM tool suite, add this:
## -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=<LLVM_PREFIX_PATH_FROM_STEP_1C>
# cmake  -B build -S . --build
```

Now you can optionally [test](./README.md#Testing) your build, or [install](#step-3---install) the `*.deb` binary packages,
which will be in the root of your build directory.

## Step 3 - Install

Once you have [built](#build) Wire Sysio and [tested](#step-4---test) your build, you can install it on your system.

We recommend installing the binary package you just built. Navigate to your build directory in a terminal and run this
command:

```bash
# Create a distro package
cd build
make -j package

# Install the package
sudo apt update
sudo apt install -y ./wire-sysio[-_][0-9]*.deb
```

It is also possible to install using `make` instead:

```bash
sudo make install
```

# Docker Container

> Supports any x86_64,arm64,aarch64 Linux, or Apple Silicon arm64,aarch64.
> As long as you have Docker installed.

This is the easiest way to build Wire Sysio, as it contains all dependencies & configuration required.

```shell
## From the root of the repo
./scripts/docker-build.sh

## To build a specific docker target/stage
## Look at `./etc/docker/Dockerfile` for available targets/stages
# ./scripts/docker-build.sh --target=app-build-repo
# ./scripts/docker-build.sh --target=app-build-local

## To tag the image with a specific docker tag (default is `wire/sysio`)
# ./scripts/docker-build.sh --target=app-build-repo --tag=wire/sysio:repo-src
# ./scripts/docker-build.sh --target=app-build-local --tag=wire/sysio:local-src
```
