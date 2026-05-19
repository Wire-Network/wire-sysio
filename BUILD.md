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
- **CMake 3.16+** – Build system generator.
- **libcurl 7.40.0+** – HTTP client library.
- **GMP** – Arbitrary precision arithmetic library.
- **Python 3** – Python is needed for some build steps and tools.
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

  # Option A (recommended): precreate and chown once, then run as your user
  sudo mkdir -p "$BASE_DIR"; sudo chown "$USER":"$USER" "$BASE_DIR"
  ./scripts/clang-18/clang-18-ubuntu-build-source.sh

  # Option B: keep /opt/clang root-owned, but preserve the env for sudo
  sudo --preserve-env=BASE_DIR ./scripts/clang-18/clang-18-ubuntu-build-source.sh
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
### 1c. Bootstrap vcpkg

Wire Sysio uses the **vcpkg** package manager for managing third-party dependencies. After installing the required system packages above, you must bootstrap vcpkg from the root of the cloned `wire-sysio` repository:

```bash
# From the wire-sysio repository root directory
./vcpkg/bootstrap-vcpkg.sh
```

This will build the vcpkg executable and set up the local vcpkg infrastructure. (It may take a few minutes on first run.)

You are now ready to build Wire Sysio.

## Recommended: Build with the GitHub Packages vcpkg Cache

Make sure you are in the root of the `wire-sysio` repo, then perform the build with the shared script used by CI and local developer builds.

> ⚠️ **Memory/Parallelism Warning** ⚠️  
> Building Wire Sysio from source can be resource-intensive. Some source files require **up to 4 GB of RAM** each to compile. If you use all CPU cores for parallel compilation (e.g. `make -j$(nproc)` or Ninja default parallelism), you may exhaust memory and encounter compiler crashes. Consider using a lower parallel job count (`-j`) if you run into memory issues or if you need to use your machine for other tasks during the build.

The simplest way to build with the same vcpkg NuGet binary cache used by CI is
to run:

```bash
export CC=/opt/clang/clang-18/bin/clang
export CXX=/opt/clang/clang-18/bin/clang++
export CMAKE_PREFIX_PATH="/opt/clang/clang-18"

scripts/build-with-github-vcpkg-cache.sh
```

The script bootstraps vcpkg, configures the GitHub Packages NuGet binary cache,
runs CMake with the repository vcpkg toolchain, and builds the project.
It does not run tests unless `--run-tests` is passed.

When `ccache` is installed, the CMake build uses it through `ENABLE_CCACHE=ON`
and stores cache files in `.ccache` by default.

Useful options:

```bash
scripts/build-with-github-vcpkg-cache.sh --build-dir build/release
scripts/build-with-github-vcpkg-cache.sh --jobs 8
scripts/build-with-github-vcpkg-cache.sh --clean
scripts/build-with-github-vcpkg-cache.sh --run-tests
```

The script has three build modes:

- `developer`: local developer builds; reads packages from the GitHub Packages
  NuGet cache and never publishes packages
- `trusted-ci`: trusted GitHub Actions runs; reads and writes the GitHub
  Packages NuGet cache
- `forked-pr-ci`: fork pull-request runs; uses vcpkg's default local cache so
  the workflow does not need package credentials

The default mode is `developer`. Developer mode keeps the local vcpkg caches
between runs unless `--clean` is passed. GitHub Actions uses the same script
with trusted pull requests running in `trusted-ci` mode and fork pull requests
running in `forked-pr-ci` mode. CI runs tests in separate jobs after archiving
the build directory.

## Optional: Use the GitHub Packages vcpkg Binary Cache Manually

The project can restore vcpkg-built dependencies from the same NuGet-backed
binary cache used by CI. This is optional, but it avoids rebuilding large vcpkg
dependencies locally.

To use the cache, you need:

- a GitHub token that can read Wire-Network GitHub Packages
- `read:packages` scope on that token
- Mono, because vcpkg runs `nuget.exe` on Linux

Install Mono:

```bash
sudo apt-get install -y mono-complete
```

If you use the GitHub CLI, refresh the local token with package-read scope:

```bash
gh auth refresh -h github.com -s read:packages
gh auth status
```

`gh auth status` should list `read:packages` in the token scopes.

Configure the NuGet source:

```bash
export GITHUB_TOKEN="$(gh auth token)"
export GITHUB_USER="$(gh api user --jq .login)"
export VCPKG_NUGET_FEED="https://nuget.pkg.github.com/Wire-Network/index.json"

NUGET_EXE="$(./vcpkg/vcpkg fetch nuget | tail -n 1)"

mono "$NUGET_EXE" sources remove -Name "github" >/dev/null 2>&1 || true
mono "$NUGET_EXE" sources add \
  -Name "github" \
  -Source "$VCPKG_NUGET_FEED" \
  -UserName "$GITHUB_USER" \
  -Password "$GITHUB_TOKEN" \
  -StorePasswordInClearText

mono "$NUGET_EXE" setapikey "$GITHUB_TOKEN" -Source "$VCPKG_NUGET_FEED"
```

Enable read-only binary cache restores for the current shell:

```bash
export VCPKG_FEATURE_FLAGS="manifests,binarycaching"
export VCPKG_BINARY_SOURCES="clear;nuget,$VCPKG_NUGET_FEED,read"
```

A successful restore looks like:

```text
Restored 9 package(s) from NuGet
```

If vcpkg prints `Restored 0 package(s) from NuGet`, check:

- `gh auth status` includes `read:packages`
- the compiler and platform match the CI platform image
- the vcpkg manifest, registry configuration, and dependency features match CI

## Configure

From the repository root:

```bash
export CC=/opt/clang/clang-18/bin/clang
export CXX=/opt/clang/clang-18/bin/clang++
export CMAKE_MAKE_PROGRAM=/usr/bin/ninja
export CMAKE_PREFIX_PATH="/opt/clang/clang-18"

cmake -B build -S . -G Ninja \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_MAKE_PROGRAM="$CMAKE_MAKE_PROGRAM" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH" \
  -DENABLE_CCACHE=ON \
  -DENABLE_TESTS=ON
```

## Build

```bash
cmake --build build -- -j "$(nproc)"
```

If you find your system running low on memory or becoming unresponsive, cancel
the build (`Ctrl+C`) and re-run with a lower parallel job count, for example:

```bash
cmake --build build -- -j 4
```

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

Wire Sysio also provides a Docker-based build environment for convenience. This is the easiest way to build and run Wire Sysio without manually installing all dependencies, as the Dockerfile encapsulates all requirements (including Clang 18, etc.) and build steps.

The provided Docker setup is intended for x86_64 hosts that can run Docker. The Dockerfile configures vcpkg with the `x64-linux` triplet.

To build Wire Sysio using Docker:

```bash
# From the root of the wire-sysio repository
./scripts/docker-build.sh
```

This script will build the Docker image using `etc/docker/Dockerfile` and compile Wire Sysio inside it. The final Wire Sysio build artifacts are available inside the image under `/wire/wire-sysio/build/Release`.

By default, it builds the `app-build-local` target using your current checkout as the source context. You can also choose other stages:

- To build using **local source context** explicitly, use the `app-build-local` target:
  ```bash
  ./scripts/docker-build.sh --target=app-build-local
  ```
- To build from the remote repository source, use the `app-build-repo` target:
  ```bash
  ./scripts/docker-build.sh --target=app-build-repo --sysio-branch=master
  ```
- To build the repository source with Wire CDT as well, use the `cdt-build-repo` target:
  ```bash
  ./scripts/docker-build.sh --target=cdt-build-repo --sysio-branch=master --cdt-branch=master
  ```

- To tag the resulting Docker image with a specific name (default tag is `wire/sysio`), use the `--tag` option:
  ```bash
  ./scripts/docker-build.sh --target=app-build-local --tag=wire/sysio:mybuild
  ```
- To specify which Ubuntu base image tag to build on, use `--ubuntu-tag` (defaults to `24.04`):
  ```bash
  ./scripts/docker-build.sh --ubuntu-tag=24.04
  ```

The script requires at least 32 GiB of available memory and passes `--memory 32G` to `docker build`.
  
After the script finishes, you will have a Docker image with Wire Sysio built inside. You can run this image or extract the built binaries. (Refer to the Dockerfile targets and the script for details on where the binaries reside in each stage.)

Using the Docker build is particularly useful if you are on a host that is not Ubuntu 24/25 or if you want to avoid installing compilers and dependencies on your system. The Docker environment ensures a consistent, reproducible build.

---
