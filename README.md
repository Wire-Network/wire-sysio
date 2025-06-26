# Wire Sysio

Wire Sysio is a fork of Leap, a C++ implementation of the [Antelope](https://github.com/AntelopeIO) protocol. It contains blockchain node software and supporting tools for developers and node operators.

## Branches

The `master` branch is the latest stable branch.

## Supported Operating Systems

We currently support the following operating systems.
- Ubuntu 22.04 Jammy
- Ubuntu 20.04 Focal

<!-- TODO: needs to add and test build on unsupported environments -->

## Installation 

In the future, we plan to support downloading Debian packages directly from our [release page](https://github.com/Wire-Network/wire-sysio/releases), providing a more streamlined and convenient setup process. However, for the time being, installation requires *building the software from source*.

Finally, verify Leap was installed correctly:
```bash
nodeop --full-version
```
You should see a [semantic version](https://semver.org) string followed by a `git` commit hash with no errors. For example:
```
v3.1.2-0b64f879e3ebe2e4df09d2e62f1fc164cc1125d1
```


## Building from source

### Prerequisites

You will need to build on a [supported operating system](#supported-operating-systems).

**Requirements to build:**

- C++20 compiler and standard library
- CMake 3.16+
- LLVM 7 - 11 - for Linux only
  - newer versions do not work
- libcurl 7.40.0+
- git
- GMP
- Python 3
- python3-numpy
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

### Step 3 - Build
Select build instructions below for a [pinned build](#pinned-build) (preferred) or an [unpinned build](#unpinned-build).

> ℹ️ **Pinned vs. Unpinned Build** ℹ️  
We have two types of builds for Leap: "pinned" and "unpinned." A pinned build is a reproducible build with the build environment and dependency versions fixed by the development team. In contrast, unpinned builds use the dependency versions provided by the build platform. Unpinned builds tend to be quicker because the pinned build environment must be built from scratch. Pinned builds, in addition to being reproducible, ensure the compiler remains the same between builds of different Leap major versions. Leap requires the compiler version to remain the same, otherwise its state might need to be recovered from a portable snapshot or the chain needs to be replayed.

> ⚠️ **A Warning On Parallel Compilation Jobs (`-j` flag)** ⚠️  
When building C/C++ software, often the build is performed in parallel via a command such as `make -j "$(nproc)"` which uses all available CPU threads. However, be aware that some compilation units (`*.cpp` files) in Wire Sysion will consume nearly 4GB of memory. Failures due to memory exhaustion will typically, but not always, manifest as compiler crashes. Using all available CPU threads may also prevent you from doing other things on your computer during compilation. For these reasons, consider reducing this value.

> 🐋 **Docker and `sudo`** 🐋  
If you are in an Ubuntu docker container, omit `sudo` from all commands because you run as `root` by default. Most other docker containers also exclude `sudo`, especially Debian-family containers. If your shell prompt is a hash tag (`#`), omit `sudo`.

#### Pinned Reproducible Build
The pinned reproducible build requires Docker. Make sure you are in the root of the `leap` repo and then run
```bash
DOCKER_BUILDKIT=1 docker build -f tools/reproducible.Dockerfile -o . .
```
This command will take a substantial amount of time because a toolchain is built from scratch. Upon completion, the current directory will contain a built `.deb` and `.tar.gz` (you can change the `-o .` argument to place the output in a different directory). If needing to reduce the number of parallel jobs as warned above, run the command as,
```bash
DOCKER_BUILDKIT=1 docker build --build-arg LEAP_BUILD_JOBS=4 -f tools/reproducible.Dockerfile -o . .
```

#### Unpinned Build
The following instructions are valid for this branch. Other release branches may have different requirements, so ensure you follow the directions in the branch or release you intend to build. If you are in an Ubuntu docker container, omit `sudo` because you run as `root` by default.

Install dependencies:

```bash
sudo apt-get update
sudo apt-get install -y \
        build-essential \
        cmake \
        git \
        libcurl4-openssl-dev \
        libgmp-dev \
        llvm-11-dev \
        python3-numpy \
        file \
        zlib1g-dev
```


On Ubuntu 20.04, install gcc-10 which has C++20 support:
```bash
sudo apt-get install -y g++-10
```

To build, make sure you are in the root of the `wire-sysio` repo, then run the following command:
```bash
mkdir -p build
cd build

## on Ubuntu 20, specify the gcc-10 compiler
cmake -DCMAKE_C_COMPILER=gcc-10 -DCMAKE_CXX_COMPILER=g++-10 -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/lib/llvm-11 ..

## on Ubuntu 22, the default gcc version is 11, using the default compiler is fine
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/lib/llvm-11 ..

make -j "$(nproc)" package
```

Now you can optionally [test](#step-4---test) your build, or [install](#step-3---install) the `*.deb` binary packages, which will be in the root of your build directory.

### Step 3 - Install

Once you have [built](#build) Wire Sysio and [tested](#step-4---test) your build, you can install it on your system. Don't forget to omit `sudo` if you are running in a docker container.

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
[WASM spec tests](#wasm-spec-tests) | Unit tests | Small | Unit tests for our WASM runtime, each short but *very* CPU-intensive
[Serial tests](#serial-tests) | Component/Integration | Medium
[Long-running tests](#long-running-tests) | Integration | Medium-to-Large | Tests which take an extraordinarily long amount of time to run

When building from source, we recommended running at least the [parallelizable tests](#parallelizable-tests).

#### Parallelizable Tests

This test suite consists of any test that does not require shared resources, such as file descriptors, specific folders, or ports, and can therefore be run concurrently in different threads without side effects (hence, easily parallelized). These are mostly unit tests and [small tests](https://testing.googleblog.com/2010/12/test-sizes.html) which complete in a short amount of time.

You can invoke them by running `ctest` from a terminal in your build directory and specifying the following arguments:

```bash
ctest -j "$(nproc)" -LE _tests
```

Since Wire resource handling changes caused considerable changes for unit test setup, some tests have been turned off
till they can be fixed, so that the core set of tests can be successfully validated. To include these tests in running
the above ctest command, the following flag must be passed to cmake before compiling and running: "-DDONT_SKIP_TESTS=TRUE"

#### WASM Spec Tests

The WASM spec tests verify that our WASM execution engine is compliant with the web assembly standard. These are very [small](https://testing.googleblog.com/2010/12/test-sizes.html), very fast unit tests. However, there are over a thousand of them so the suite can take a little time to run. These tests are extremely CPU-intensive.

You can invoke them by running `ctest` from a terminal in your Wire Sysio build directory and specifying the following arguments:

```bash
ctest -j "$(nproc)" -L wasm_spec_tests
```

We have observed severe performance issues when multiple virtual machines are running this test suite on the same physical host at the same time, for example in a CICD system. This can be resolved by disabling hyperthreading on the host.

#### Serial Tests

The serial test suite consists of [medium](https://testing.googleblog.com/2010/12/test-sizes.html) component or integration tests that use specific paths, ports, rely on process names, or similar, and cannot be run concurrently with other tests. Serial tests can be sensitive to other software running on the same host and they may `SIGKILL` other `nodeop` processes. These tests take a moderate amount of time to complete, but we recommend running them.

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

---

<!-- <!-- markdownlint-disable MD033 -->
<table>
  <tr>
    <td><img src="https://wire.foundation/favicon.ico" alt="Wire Network" width="50"/></td>
    <td>
      <strong>Wire Network</strong><br>
      <a href="https://www.wire.network/">Website</a> |
      <a href="https://x.com/wire_blockchain">Twitter</a> |
      <a href="https://www.linkedin.com/company/wire-network-blockchain/">LinkedIn</a><br>
      © 2024 Wire Network. All rights reserved.
    </td>
  </tr>
</table>
