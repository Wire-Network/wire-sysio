## Description

The `wallet_api_plugin` exposes functionality from the [`wallet_plugin`](../wallet_plugin/index.md) to the RPC API interface managed by the [`http_plugin`](../../../01_nodeop/03_plugins/http_plugin/index.md).

[[caution | Caution]]
| This plugin exposes wallets. Therefore, running this plugin on a publicly accessible node is not recommended. As of 1.2.0, the `wallet_api_plugin` is only available through `keosd`. It is no longer supported by `nodeop`.

## Usage

```sh
# config.ini
plugin = sysio::wallet_api_plugin

# command-line
keosd ... --plugin sysio::wallet_api_plugin
```

## Options

None

## Dependencies

* [`wallet_plugin`](../wallet_plugin/index.md)
* [`http_plugin`](../../../01_nodeop/03_plugins/http_plugin/index.md)

### Load Dependency Examples

The following plugins are loaded with default settings if not specified on the command line or `config.ini`:

```sh
# config.ini
plugin = sysio::wallet_plugin
[options]
plugin = sysio::http_plugin
[options]

# command-line
keosd ... --plugin sysio::wallet_plugin [options]  \
           --plugin sysio::http_plugin [options]
```
