---
content_title: Nodeos
---

## Introduction

`nodeop` is the core service daemon that runs on every Antelope node. It can be configured to process smart contracts, validate transactions, produce blocks containing valid transactions, and confirm blocks to record them on the blockchain.

## Installation

`nodeop` is distributed as part of the [Antelope software suite](https://github.com/AntelopeIO/leap). To install `nodeop`, visit the [Antelope Software Installation](../00_install/index.md) section.

## Explore

Navigate the sections below to configure and use `nodeop`.

* [Usage](02_usage/index.md) - Configuring and using `nodeop`, node setups/environments.
* [Plugins](03_plugins/index.md) - Using plugins, plugin options, mandatory vs. optional.
* [Replays](04_replays/index.md) - Replaying the chain from a snapshot or a blocks.log file.
* [RPC APIs](05_rpc_apis/index.md) - Remote Procedure Call API reference for plugin HTTP endpoints.
* [Logging](06_logging/index.md) - Logging config/usage, loggers, appenders, logging levels.
* [Concepts](07_concepts/index.md) - `nodeop` concepts, explainers, implementation aspects.
* [Troubleshooting](08_troubleshooting/index.md) - Common `nodeop` troubleshooting questions.

[[info | Access Node]]
| A local or remote Antelope access node running `nodeop` is required for a client application or smart contract to interact with the blockchain.
