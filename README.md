# Wire Sysio
Wire Sysio is a fork of Leap, a C++ implementation of the [Antelope](https://github.com/AntelopeIO) protocol. It contains blockchain node software and supporting tools for developers and node operators.

## Branches
The `main` branch is the latest stable branch. 


## Supported Operating Systems
We currently support the following operating systems.

| **Operating Systems**           |
|---------------------------------|
| Ubuntu 22.04 Jammy              |
| Ubuntu 20.04 Focal              |
| Ubuntu 18.04 Bionic             |


<!-- TODO: needs to add and test build on unsupported environments -->

## Installation 
In the future, we plan to support the installation of Debian packages directly from our release page, providing a more streamlined and convenient setup process. However, for the time being, installation requires *building the software from source*.

### Prerequisites
You will need to build on a [supported operating system](#supported-operating-systems).

**Requirements to build:**
- C++17 compiler and standard library
- boost 1.67+
- CMake 3.8+
- LLVM 7 - 11 - for Linux only
  - newer versions do not work
- openssl 1.1+
- libcurl
- curl
- libusb
- git
- GMP
- Python 3
- zlib

### Step 1 - Clone
If you don't have the `wire-sysio` repo cloned to your computer yet, [open a terminal](https://itsfoss.com/open-terminal-ubuntu) and navigate to the folder where you want to clone it:

```bash
cd ~/Downloads
```

Clone this repo using either HTTPS:

```bash
git clone --recursive https://github.com/Wire-Network/wire-sysio.git
```
or SSH:

```bash
git clone --recursive git@github.com:Wire-Network/wire-sysio.git
```

Upon cloning it, you should have a local copy of `wire-sysio`, containing our source code.

Navigate into that folder:


```bash
cd wire-sysio
```

<!-- TODO: should add libraries/fc as github submodule and include prior to build step -->

### Step 2 - Build
Select build instructions below for to [build](#build).

> ⚠️ **A Warning On Parallel Compilation Jobs (`-j` flag)** ⚠️  
When building C/C++ software, often the build is performed in parallel via a command such as `make -j "$(nproc)"` which uses all available CPU threads. However, be aware that some compilation units (`*.cpp` files) in Wire Sysion will consume nearly 4GB of memory. Failures due to memory exhaustion will typically, but not always, manifest as compiler crashes. Using all available CPU threads may also prevent you from doing other things on your computer during compilation. For these reasons, consider reducing this value.

> 🐋 **Docker and `sudo`** 🐋  
If you are in an Ubuntu docker container, omit `sudo` from all commands because you run as `root` by default. Most other docker containers also exclude `sudo`, especially Debian-family containers. If your shell prompt is a hash tag (`#`), omit `sudo`.

####  Build
Note: If you are in an Ubuntu docker container, omit `sudo` because you run as `root` by default.

**Ubuntu 22.04 Jammy & Ubuntu 20.04 Focal**

Install dependencies:
```bash
sudo apt-get update
sudo apt-get install -y \
        build-essential \
        cmake \
        curl \
        git \
        libboost-all-dev \
        libcurl4-openssl-dev \
        libgmp-dev \
        libssl-dev \
        libusb-1.0-0-dev \
        llvm-11-dev \
        pkg-config
```
To build, make sure you are in the root of the `wire-sysio` repo, then run the following command:

```bash
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/lib/llvm-11 ..
make -j $(nproc) package
```


</details>

<details> <summary>**Ubuntu 18.04 Bionic**</summary>

Install dependencies:
```bash
sudo apt-get update
sudo apt-get install -y \
        build-essential \
        cmake \
        curl \
        g++-8 \
        git \
        libcurl4-openssl-dev \
        libgmp-dev \
        libssl-dev \
        libusb-1.0-0-dev \
        llvm-7-dev \
        pkg-config \
        python3 \
        zlib1g-dev
```
You need to build Boost from source on this distribution:
```bash
curl -fL https://boostorg.jfrog.io/artifactory/main/release/1.79.0/source/boost_1_79_0.tar.bz2 -o ~/Downloads/boost_1_79_0.tar.bz2
tar -jvxf ~/Downloads/boost_1_79_0.tar.bz2 -C ~/Downloads/
pushd ~/Downloads/boost_1_79_0
./bootstrap.sh --prefix="$HOME/boost1.79"
./b2 --with-iostreams --with-date_time --with-filesystem --with-system --with-program_options --with-chrono --with-test -j "$(nproc)" install
popd
```
The Boost `*.tar.bz2` download and `boost_1_79_0` folder can be removed now if you want more space.
```bash
rm -r ~/Downloads/boost_1_79_0.tar.bz2 ~/Downloads/boost_1_79_0
```
From a terminal in the root of the `wire-sysio` repo, build.
```bash
mkdir -p build
cd build
cmake -DCMAKE_C_COMPILER=gcc-8 -DCMAKE_CXX_COMPILER=g++-8 -DCMAKE_PREFIX_PATH="$HOME/boost1.79;/usr/lib/llvm-7/" -DCMAKE_BUILD_TYPE=Release ..
make -j "$(nproc)" package
```
After building, you may remove the `~/boost1.79` directory or you may keep it around for your next build.
</details>

Now you can optionally [test](#step-4---test) your build, or [install](#step-5---install) the `*.deb` binary packages, which will be in the root of your build directory.


### Step 3 - Install
Once you have [built](#step-3---build-the-source-code) Wire Sysio and [tested](#step-4---test) your build, you can install it on your system. Don't forget to omit `sudo` if you are running in a docker container.

We recommend installing the binary package you just built. Navigate to your build directory in a terminal and run this command:
```bash
sudo apt-get update
sudo apt-get install -y ./wire-sysio[-_][0-9]*.deb
```

It is also possible to install using `make` instead:
```bash
sudo make install
```

### Step 4 - Test
Wire Sysio supports the following test suites:

Test Suite | Test Type | [Test Size](https://testing.googleblog.com/2010/12/test-sizes.html) | Notes
---|:---:|:---:|---
[Parallelizable tests](#parallelizable-tests) | Unit tests | Small
[WASM spec tests](#wasm-spec-tests) | Unit tests | Small | Unit tests for our WASM runtime, each short but _very_ CPU-intensive
[Serial tests](#serial-tests) | Component/Integration | Medium
[Long-running tests](#long-running-tests) | Integration | Medium-to-Large | Tests which take an extraordinarily long amount of time to run

When building from source, we recommended running at least the [parallelizable tests](#parallelizable-tests).

#### Parallelizable Tests
This test suite consists of any test that does not require shared resources, such as file descriptors, specific folders, or ports, and can therefore be run concurrently in different threads without side effects (hence, easily parallelized). These are mostly unit tests and [small tests](https://testing.googleblog.com/2010/12/test-sizes.html) which complete in a short amount of time.

You can invoke them by running `ctest` from a terminal in your build directory and specifying the following arguments:
```bash
ctest -j "$(nproc)" -LE _tests
```

#### WASM Spec Tests
The WASM spec tests verify that our WASM execution engine is compliant with the web assembly standard. These are very [small](https://testing.googleblog.com/2010/12/test-sizes.html), very fast unit tests. However, there are over a thousand of them so the suite can take a little time to run. These tests are extremely CPU-intensive.

You can invoke them by running `ctest` from a terminal in your Wire Sysio build directory and specifying the following arguments:
```bash
ctest -j "$(nproc)" -L wasm_spec_tests
```
We have observed severe performance issues when multiple virtual machines are running this test suite on the same physical host at the same time, for example in a CICD system. This can be resolved by disabling hyperthreading on the host.

#### Serial Tests
The serial test suite consists of [medium](https://testing.googleblog.com/2010/12/test-sizes.html) component or integration tests that use specific paths, ports, rely on process names, or similar, and cannot be run concurrently with other tests. Serial tests can be sensitive to other software running on the same host and they may `SIGKILL` other `nodeos` processes. These tests take a moderate amount of time to complete, but we recommend running them.

You can invoke them by running `ctest` from a terminal in your build directory and specifying the following arguments:
```bash
ctest -L "nonparallelizable_tests"
```

#### Long-Running Tests
The long-running tests are [medium-to-large](https://testing.googleblog.com/2010/12/test-sizes.html) integration tests that rely on shared resources and take a very long time to run.

You can invoke them by running `ctest` from a terminal in your `build` directory and specifying the following arguments:
```bash
ctest -L "long_running_tests"
```
