# Bios Boot Tutorial

The `bios-boot-tutorial.py` script simulates the bios boot sequence.

## Prerequisites

1. Python 3.x
2. CMake
3. git
4. curl
5. libcurl4-gnutls-dev

## Steps

1. Install the latest [Spring binaries](https://github.com/AntelopeIO/spring/releases) by following the steps provided in the README.

2. Install the latest [CDT binaries](https://github.com/AntelopeIO/cdt/releases) by following the steps provided in the README.

3. Compile the latest [SYS System Contracts](https://github.com/eosnetworkfoundation/sys-system-contracts/releases). Replaces `release/*latest*` with the latest release branch.

```bash
$ cd ~
$ git clone https://github.com/eosnetworkfoundation/sys-system-contracts
$ cd ./sys-system-contracts/
$ git checkout release/*latest*
$ mkdir build
$ cd ./build
$ cmake -DCMAKE_BUILD_TYPE=Release ..
$ make -j $(nproc)
$ cd ./contracts/
$ pwd
```

4. Make note of the path where the contracts were compiled
The last command in the previous step printed the contracts directory. Make note of it; we will reference it from now on as the environment variable `CORE_CONTRACTS_DIRECTORY`.

5. Compile the latest [Vaulta Contracts](https://github.com/VaultaFoundation/vaulta-system-contract). Run off the latest from branch `main`

```
$ cd ~
$ git clone https://github.com/VaultaFoundation/vaulta-system-contract
$ cd ./vaulta-system-contract/
$ git checkout main
$ mkdir build
$ cd ./build
$ export SYSTEM_CONTRACTS_PATH=${CORE_CONTRACTS_DIRECTORY}
$ cmake -DCMAKE_BUILD_TYPE=Release ..
$ make -j $(nproc)
$ cd ./contracts/
$ pwd
```

6. Make note of the path where the vaulta contracts were compiled
The last command in the previous step printed the vaulta contracts directory. Make note of it; we will reference it from now on as the environment variable `VAULTA_CONTRACTS_DIRECTOR `.

7. Launch the `bios-boot-tutorial.py` script:

```bash
$ pip install numpy
$ cd ~
$ git clone -b release/*latest* https://github.com/AntelopeIO/wire_sysio
$ cd ./wire_sysio/tutorials/bios-boot-tutorial/
$ python3 bios-boot-tutorial.py --clio=clio --nodeop=nodeop --kiod=kiod --contracts-dir="${CONTRACTS_DIRECTORY}" -w -a
```
