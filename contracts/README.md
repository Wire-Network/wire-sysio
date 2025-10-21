# Wire system contracts

Wire system contracts are a collection of contracts specifically designed for the Wire blockchain, which implements a lot of critical functionality that goes beyond what is provided by the base Antelope protocol, the protocol on which Wire blockchain is built on.

- **Accounts and Permissions**: Flexible permission system for transaction-specific actions via multi-signature authorizations.
- **Advanced Consensus**: Extends the basic consensus framework to include detailed processes for selecting Node Operators and aligning their incentives, using a system of rewards and penalties.
- **Sophisticated Resource Management(ROA)**: Extended systems for tracking and usage of CPU/NET and RAM, and facilitates the dynamically management of resource rights.
- **Token Functionality:** Support for creation and management of both fungible and non-fungible tokens.


The collection of system contracts consists of the following individual contracts:

* [bios contract](contracts/sysio.bios/include/sysio.bios/sysio.bios.hpp): A simple alternative to the core contract which is suitable for test chains or perhaps centralized blockchains. (Note: this contract must be deployed to the privileged `sysio` account.)
* [token contract](contracts/sysio.token/include/sysio.token/sysio.token.hpp): A contract enabling fungible tokens.
* [core contract](contracts/sysio.system/include/sysio.system/sysio.system.hpp): A monolithic contract that includes a variety of different functions. Note: This contract must be deployed to the privileged `sysio` account. Additionally, this contract requires that the token contract is deployed to the `sysio.token` account and has already been used to setup the core token.) The functions contained within this monolithic contract include (non-exhaustive):
   + Appointed Proof of Stake (APoS) consensus mechanism for selecting and paying (via core token inflation) a set of Node Operators that are chosen through delegation of the staked core tokens.
   + Allocation of CPU/NET resources based on core tokens in which the core tokens are either staked for an indefinite allocation of some fraction of available CPU/NET resources, or they are paid as a fee in exchange for a time-limited allocation of CPU/NET resources via ROA.
   + An automated market maker enabling a market for RAM resources which allows users to buy or sell available RAM allocations.
   + An auction for bidding for premium account names.
* [multisig contract](contracts/sysio.msig/include/sysio.msig/sysio.msig.hpp): A contract that enables proposing transactions on the blockchain, collecting authorization approvals for many accounts, and then executing the actions within the transaction after authorization requirements of the transaction have been reached. (Note: this contract must be deployed to a privileged account.)
* [wrap contract](contracts/sysio.wrap/include/sysio.wrap/sysio.wrap.hpp): A contract that wraps around any transaction and allows for executing its actions without needing to satisfy the authorization requirements of the transaction. If used, the permissions of the account hosting this contract should be configured to only allow highly trusted parties (e.g. the operators of the blockchain) to have the ability to execute its actions. (Note: this contract must be deployed to a privileged account.)

## Branches

The `master` branch contains the latest stable branch 


## Building

### Prerequisites ###

Before proceeding with the instructions below, please ensure the following prerequisites are met:

 - **CDT Dependency**: Ensure that the Wire Contract Development Toolkit (CDT) is installed on your system.
 - *Optional* - **Wire Sysio Built from Source**: If you wish to run build system contracts with tests, you should have already built Wire Sysio from source and installed it on your system.

##### Build or install CDT dependency

The CDT dependency is required. 

At the moment, we only support building from source for CDT. Please refer to the guide in the [CDT README](https://github.com/Wire-Network/wire-cdt#building-from-source) for instructions on how to do this. 

It is important to keep the path to the build directory in the shell environment variable `CDT_BUILD_PATH` for later use when building the system contracts.

Example cmake options for building the contracts with CDT:
```
-DCDT_ROOT=/home/kevin/ext/wire-cdt/build
-DBUILD_SYSTEM_CONTRACTS=ON
-DBUILD_TEST_CONTRACTS=ON
-DENABLE_TEST=ON
-DCMAKE_PREFIX_PATH=/opt/llvm/llvm-11;/home/kevin/ext/wire-cdt/build
-DCMAKE_TOOLCHAIN_FILE=/home/kevin/ext/wire-sysio/vcpkg/scripts/buildsystems/vcpkg.cmake
```

### Running tests
```shell
./contracts/tests/contracts_unit_test
```
---

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
</table