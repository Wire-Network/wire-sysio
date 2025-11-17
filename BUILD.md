# Build from Source Instructions

> **Supported Platforms:** As of this release, Wire Sysio can be built on **Ubuntu 24.04 LTS (and newer, e.g. Ubuntu 25.x)** on x86_64. Earlier Ubuntu versions (20.04, 22.04) are no longer supported for building from source.

> For Ubuntu 24.04 and above, some dependencies (notably **LLVM 11** and **Clang 18**) are not available as standard packages and must be built from source. We provide helper scripts:
> - [scripts/llvm-11/llvm-11-ubuntu-build-source.sh](scripts/llvm-11/llvm-11-ubuntu-build-source.sh) to build and install LLVM 11.
> - [scripts/clang-18/clang-18-ubuntu-build-source.sh](scripts/clang-18/clang-18-ubuntu-build-source.sh) to build and install Clang 18.
> These scripts can also be used on other major Linux distributions (Arch, Fedora, etc.) to facilitate building Wire Sysio on those systems, since they compile the required toolchain components from source.

# Get the source code

Clone this repo as follows:

```bash
# Clone with HTTP
git clone --recursive https://github.com/Wire-Network/wire-sysio.git

# Clone with SSH (if you have SSH keys set up)
# git clone --recursive git@github.com:Wire-Network/wire-sysio.git

# Enter the repo directory
cd wire-sysio
```

# On Host Machine

> **ONLY SUPPORTS:** Ubuntu 24.04 LTS and Ubuntu 25.x (x86_64).
> (Other Linux distributions may work using the provided build scripts for Clang/LLVM, but Ubuntu 24.04 is the minimum officially supported environment.)

## Prerequisites

You will need to build on a [supported operating system](README.md#supported-operating-systems) with the following tools and libraries available:

- **Clang 18 (C++20 compiler)** – A C++20 capable compiler. Wire Sysio is developed and tested with Clang 18 (LLVM 18).
- **LLVM 11 libraries** – If not available as a package (Ubuntu 24+), LLVM 11 must be built from source (see **Step 1c**).
- **CMake 3.16+** – Build system generator.
- **libcurl 7.40.0+** – HTTP client library.
- **GMP** – Arbitrary precision arithmetic library.
- **Python 3** – Python is needed for some build steps and tools.
- **zlib** – Compression library.
- **git** – Version control, used for fetching submodules.

Other standard development tools and libraries (build essentials, SSL, Boost, etc.) are needed as well. These will be installed in the steps below.

## Step 1 - Install Dependencies

### 1a. Install System Packages (Ubuntu 24.04 / 25.x)

On Ubuntu 24.04 and 25.x, install the following dependencies.

```bash
# Update package lists
sudo apt update

# Install base build tools and libraries (binutils included on 24.04)
sudo apt-get install -y \
        build-essential \
        binutils \
        ccache \
        cmake \
        curl \
        doxygen \
        git \
        gnupg \
        golang \
        libbz2-dev \
        libcurl4-openssl-dev \
        libgmp-dev \
        liblzma-dev \
        libncurses5-dev \
        libstdc++-14-dev \
        libusb-1.0-0-dev \
        libzstd-dev \
        ninja-build \
        pkg-config \
        python3 \
        python3-pip \
        python3-venv \
        python3-dev \
        autoconf \
        autoconf-archive \
        automake \
        libtool \
        sudo \
        tar \
        unzip \
        vim \
        zip \
        zlib1g-dev
```

### 1b. Install Clang 18

- **Build Clang 18 from source using our script** – If you prefer to compile Clang 18 (to match our Docker build environment or if a pre-built package is unavailable for your distro), use the provided script:
  
  ```bash
  # Choose an installation directory for Clang 18 
  export BASE_DIR=/opt/clang    # you can choose a different prefix if desired

  # Build and install Clang 18 from source (this will download LLVM/Clang source and take some time)
  ./scripts/clang-18/clang-18-ubuntu-build-source.sh
  ```
  
  This script will compile Clang 18 and install it under `$BASE_DIR/clang-18`. (Using `/opt/clang/clang-18` as an example location.) 

After installing Clang 18, ensure the `clang-18` and `clang++-18` executables are available in your PATH (or note their full paths for use in the build step). If you built from source in `/opt/clang`, the compiler binaries will be in `/opt/clang/clang-18/bin/`.

**Example (PATH + CC/CXX):**
```bash
# If you built Clang 18 at /opt/clang/clang-18:
export PATH=/opt/clang/clang-18/bin:$PATH

# Verify they’re visible:
command -v clang-18 && clang-18 --version
command -v clang++-18 && clang++-18 --version

# Tell CMake/vcpkg to use them explicitly:
export CC=$(command -v clang-18)
export CXX=$(command -v clang++-18)
```

> **Optional (mirrors Docker’s vcpkg sub-build behavior):**
> Create a tiny chainloaded toolchain so **vcpkg ports** also use Clang/LLVM binutils.
> ```bash
> sudo mkdir -p /opt/clang
> sudo tee /opt/clang/clang18-vcpkg-toolchain.cmake >/dev/null <<'EOF'
> set(CMAKE_C_COMPILER   /opt/clang/clang-18/bin/clang)
> set(CMAKE_CXX_COMPILER /opt/clang/clang-18/bin/clang++)
> set(CMAKE_AR           /opt/clang/clang-18/bin/llvm-ar)
> set(CMAKE_RANLIB       /opt/clang/clang-18/bin/llvm-ranlib)
> set(CMAKE_CXX_STANDARD 20)
> set(CMAKE_CXX_STANDARD_REQUIRED ON)
> EOF
> export VCPKG_CHAINLOAD_TOOLCHAIN_FILE=/opt/clang/clang18-vcpkg-toolchain.cmake
> export VCPKG_KEEP_ENV_VARS="CC;CXX;AR;RANLIB;B2_TOOLSET;VCPKG_CHAINLOAD_TOOLCHAIN_FILE"
> export B2_TOOLSET=clang
> ```

### 1c. Install LLVM 11

Wire Sysio depends on **LLVM 11**. On modern systems (Ubuntu 24.04+), you will need to build LLVM 11 from source, since it’s not available in the apt repositories.

Use our helper script to build and install LLVM 11:

```bash
# Choose an installation base directory for LLVM 11
export BASE_DIR=/opt/llvm    # (you can choose a different directory if desired)

# Build and install LLVM 11 from source
./scripts/llvm-11/llvm-11-ubuntu-build-source.sh
```

This will download the LLVM 11 source code, compile it, and install the libraries and tools to `$BASE_DIR/llvm-11`. (By default, the script places the final installation in `/opt/llvm/llvm-11` if `BASE_DIR=/opt/llvm`.) 

> **IMPORTANT:** Remember the installation path of LLVM 11 – you will need to supply it to CMake in the build step. For example, `-DCMAKE_PREFIX_PATH=${BASE_DIR}/llvm-11`. 

### 1d. Bootstrap vcpkg

Wire Sysio uses the **vcpkg** package manager for managing third-party dependencies. After installing the required system packages above, you must bootstrap vcpkg from the root of the cloned `wire-sysio` repository:

```bash
# From the wire-sysio repository root directory
./vcpkg/bootstrap-vcpkg.sh
```

This will build the vcpkg executable and set up the local vcpkg infrastructure. (It may take a few minutes on first run.)

You are now ready to build Wire Sysio.

## Step 2 - Build

Make sure you are in the root of the `wire-sysio` repo, then perform the build with CMake and your chosen compiler.

> ⚠️ **Memory/Parallelism Warning** ⚠️  
> Building Wire Sysio from source can be resource-intensive. Some source files require **up to 4 GB of RAM** each to compile. If you use all CPU cores for parallel compilation (e.g. `make -j$(nproc)` or Ninja default parallelism), you may exhaust memory and encounter compiler crashes. Consider using a lower parallel job count (`-j`) if you run into memory issues or if you need to use your machine for other tasks during the build.

### Build Instructions

First, ensure the environment is set to use **Clang 18** as the compiler:

```bash
# Example: set CC and CXX to Clang 18 compilers (adjust path if installed elsewhere)
export CC=/opt/clang/clang-18/bin/clang
export CXX=/opt/clang/clang-18/bin/clang++
```

> **Environment parity with Docker (recommended):**
> ```bash
> # Deterministic vcpkg behavior & correct compiler tracking
> export VCPKG_FEATURE_FLAGS=manifests,registries,versions,compilertracking
> export VCPKG_DEFAULT_TRIPLET=x64-linux
> export VCPKG_OVERLAY_TRIPLETS=$PWD/vcpkg/triplets
> 
> # If you created the chainload file in 1b:
> export VCPKG_CHAINLOAD_TOOLCHAIN_FILE=/opt/clang/clang18-vcpkg-toolchain.cmake
> export VCPKG_KEEP_ENV_VARS="CC;CXX;AR;RANLIB;B2_TOOLSET;VCPKG_CHAINLOAD_TOOLCHAIN_FILE"
> ```

Now run CMake to configure the build and generate build files. We recommend using **Ninja** as the build generator for faster builds (we installed `ninja-build` earlier):

```bash
cmake -B build -S . -G Ninja \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/opt/llvm/llvm-11;/opt/clang/clang-18" \
  -DENABLE_CCACHE=ON
```

In the above command:
- `-B build -S .` tells CMake to create (or use) the directory `build/` for the build files.
- `-G Ninja` chooses the Ninja generator. If you prefer Makefiles, you can omit this (default is Unix Makefiles), but ensure you adjust build commands accordingly.
- `CMAKE_C_COMPILER` and `CMAKE_CXX_COMPILER` are pointed to Clang 18 (from Step 1b).
- `CMAKE_TOOLCHAIN_FILE` points to the vcpkg toolchain file, so CMake will integrate vcpkg dependencies automatically.
- `CMAKE_BUILD_TYPE=Release` produces an optimized build. (You can use `Debug` for development, etc.)
- **CMAKE_PREFIX_PATH** is set to the locations of LLVM 11 and Clang 18 installations. This is crucial so CMake can find LLVM 11's headers/libraries. Adjust the paths if you installed to a different prefix or if you installed Clang 18 via apt (e.g., apt installs might be under `/usr/lib/llvm-18`). Separate multiple paths with semicolons. In this example, we use `/opt/llvm/llvm-11` (from Step 1c) and `/opt/clang/clang-18` (from Step 1b).
- `ENABLE_CCACHE=ON` will use **ccache** to cache compilation results (if `ccache` is installed). This can significantly speed up rebuilds. You can omit this or set `OFF` if you do not have ccache or do not want to use it.

If CMake configuration is successful, it will generate the build files in the `build/` directory. Now proceed to compile:

```bash
cmake --build build -- -j$(nproc)
```

This will start the build process (using all available CPU cores by default). If you find your system running low on memory or becoming unresponsive, cancel the build (`Ctrl+C`) and re-run with a lower parallel job count, for example: 

```bash
cmake --build build -- -j4
``` 

(to limit to 4 threads). Ninja will respect the `-j` flag similarly, or you can set the environment variable `CMAKE_BUILD_PARALLEL_LEVEL` before running the `cmake --build` command.

Once the build completes successfully, the Wire Sysio binaries and libraries will be available in `build/bin/` (and other subdirectories under `build/`). 

You can now proceed to [test](README.md#testing) your build (optional), or create installation packages as described below.

## Step 3 - Install

After you have [built](#build) Wire Sysio (and optionally [tested](README.md#testing) it), you can install it on your system. There are two primary ways to install:

**A. Install via Debian Package:**  
We recommend installing using the Debian package that the build produces, as it ensures all files go to appropriate system locations. To build the package (if it hasn’t been built already), run:

```bash
cmake --build build --target package
```

This will create the Wire Sysio `.deb` package in the `build` directory. Then install it using `dpkg` or `apt`:

```bash
sudo apt install ./build/wire-sysio_*.deb
```

*(Replace `wire-sysio_*.deb` with the actual filename of the package. Using `apt install` on the local file will automatically handle any missing dependencies.)*

**B. Install via CMake target:**  
Alternatively, you can install the built files directly to your system using CMake. This is useful if you prefer not to create a package:

```bash
sudo cmake --install build
```

This will copy the Wire Sysio binaries, libraries, and headers to the default installation prefixes (e.g., under `/usr/local/` by default, or whatever `CMAKE_INSTALL_PREFIX` was set to during configuration). You may omit `sudo` if installing to a location your user has write access to (for example, a custom `CMAKE_INSTALL_PREFIX` within your home directory).

Choose **either** method A or B according to your preference. Method A (deb package) is cleaner for system installs, as it can be easily removed or upgraded using the system package manager.

# Docker Container

Wire Sysio also provides a Docker-based build environment for convenience. This is the easiest way to build and run Wire Sysio without manually installing all dependencies, as the Dockerfile encapsulates all requirements (including Clang 18, LLVM 11, etc.) and build steps.

The provided Docker setup supports building on any modern 64-bit Linux or Apple Silicon (arm64) host that can run Docker (x86_64, arm64/aarch64). The container will produce Wire Sysio binaries for the same architecture as the host.

To build Wire Sysio using Docker:

```bash
# From the root of the wire-sysio repository
./scripts/docker-build.sh
```

This script will build the Docker image (using the `./etc/docker/Dockerfile`) and compile Wire Sysio inside it. The final Wire Sysio build artifacts will be available within the Docker image (and can be copied out or used via Docker container).

By default, it builds the `app-build-repo` target (fetching the source from the repository). You can also build from your local source or choose other stages:

- To build using **local source context** (your cloned repository code), use the `app-build-local` target:
  ```bash
  ./scripts/docker-build.sh --target=app-build-local
  ```
- To specify a different Git branch or repository for the source, set the `SYSIO_BRANCH` or `REPO` build args via the script (see script help for usage).

- To tag the resulting Docker image with a specific name (default tag is `wire/sysio:latest`), use the `--tag` option:
  ```bash
  ./scripts/docker-build.sh --target=app-build-local --tag=wire/sysio:mybuild
  ```
- There is an additional flag that can be used to specify which Ubuntu version your image builds on (defaults Ubuntu 24.04)
  ```bash
  ./scripts/docker-build.sh --ubuntu-tag=24.04
  ```
  
After the script finishes, you will have a Docker image with Wire Sysio built inside. You can run this image or extract the built binaries. (Refer to the Dockerfile targets and the script for details on where the binaries reside in each stage.)

Using the Docker build is particularly useful if you are on a host that is not Ubuntu 24/25 or if you want to avoid installing compilers and dependencies on your system. The Docker environment ensures a consistent, reproducible build.

---
