## Goal

Delegate CPU bandwidth for an account or application.

## Before you begin

Make sure you meet the following requirements:

* Familiarize with the [`clio system delegatebw`](../03_command-reference/system/system-delegatebw.md) command and its parameters.
* Install the currently supported version of `clio`.

[[info | Note]]
| `clio` is bundled with the Antelope software. [Installing Antelope](../../00_install/index.md) will also install `clio`.

* Ensure the reference system contracts from [`reference-contracts`](https://github.com/AntelopeIO/reference-contracts) repository is deployed and used to manage system resources.
* Understand what an [account](/glossary.md#account) is and its role in the blockchain.
* Understand [CPU bandwidth](/glossary.md#cpu) in an Antelope blockchain.
* Understand [NET bandwidth](/glossary.md#net) in an Antelope blockchain.

## Steps

Perform the step below:

Delegate CPU bandwidth from a source account to a receiver account:

```sh
clio system delegatebw <from> <receiver> <stake_net_quantity> <stake_cpu_quantity>
```

Where `from` is the account to delegate bandwidth from, `receiver` is the account to receive the delegated bandwidth, and `stake_net_quantity` and/or `stake_cpu_quantity` is the amount of tokens to stake for network (NET) bandwidth and/or CPU bandwidth, respectively.

Some examples are provided below:

* Delegate 0.01 SYS CPU bandwidth from `bob` to `alice`:

**Example Output**

```sh
clio system delegatebw bob alice "0 SYS" "0.01 SYS"
```
```json
executed transaction: 5487afafd67bf459a20fcc2dbc5d0c2f0d1f10e33123eaaa07088046fd18e3ae  192 bytes  503 us
#         sysio <= sysio::delegatebw            {"from":"bob","receiver":"alice","stake_net_quantity":"0.0000 SYS","stake_cpu_quantity":"0.0100 SYS"...
#   sysio.token <= sysio.token::transfer        {"from":"bob","to":"sysio.stake","quantity":"0.0010 SYS","memo":"stake bandwidth"}
#  bob <= sysio.token::transfer        {"from":"bob","to":"sysio.stake","quantity":"0.0010 SYS","memo":"stake bandwidth"}
#   sysio.stake <= sysio.token::transfer        {"from":"bob","to":"sysio.stake","quantity":"0.0010 SYS","memo":"stake bandwidth"}
```
