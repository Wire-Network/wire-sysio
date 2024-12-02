
## Description

The `test_control_plugin` is designed to cause a graceful shutdown when reaching a particular block in a sequence of blocks produced by a specific block producer. It can be invoked to either shutdown on the **head block** or the **last irreversible block**.

This is intended for testing, to determine exactly when a `nodeop` instance will shutdown.

## Usage

```console
# config.ini
plugin = sysio::test_control_plugin
```
```sh
# command-line
nodeop ... --plugin sysio::test_control_plugin
```

## Options

None

## Dependencies

* [`chain_plugin`](../chain_plugin/index.md)

### Load Dependency Examples

```console
# config.ini
plugin = sysio::chain_plugin
[options]
```
```sh
# command-line
nodeop ... --plugin sysio::chain_plugin [operations] [options]
```
