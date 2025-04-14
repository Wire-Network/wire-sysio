---
content_title: Kiod
---

## Introduction

`kiod` is a key manager service daemon for storing private keys and signing digital messages. It provides a secure key storage medium for keys to be encrypted at rest in the associated wallet file. `kiod` also defines a secure enclave for signing transaction created by `clio` or a third part library.

## Installation

`kiod` is distributed as part of the [Antelope software suite](https://github.com/AntelopeIO/leap). To install `kiod` just visit the [Antelope Software Installation](../00_install/index.md) section.

## Operation

When a wallet is unlocked with the corresponding password, `clio` can request `kiod` to sign a transaction with the appropriate private keys. Also, `kiod` provides support for hardware-based wallets such as Secure Encalve and YubiHSM.

[[info | Audience]]
| `kiod` is intended to be used by Antelope developers only.
