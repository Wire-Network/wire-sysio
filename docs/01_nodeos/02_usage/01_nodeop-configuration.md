---
content_title: Nodeop Configuration
---

The plugin-specific options can be configured using either CLI options or a configuration file, `config.ini`. Nodeop-specific options can only be configured from the command line. All CLI options and `config.ini` options can be found by running `nodeop --help` as shown above.

Each `config.ini` option has a corresponding CLI option. However, not all CLI options are available in `config.ini`. For instance, most plugin-specific options that perform actions are not available in `config.ini`, such as `--delete-state-history` from `state_history_plugin`.

For example, the CLI option `--plugin sysio::chain_api_plugin` can also be set by adding `plugin = sysio::chain_api_plugin` in `config.ini`.

## `config.ini` location

The default `config.ini` can be found in the following folder on Linux:
`~/.local/share/sysio/nodeop/config`

A custom `config.ini` file can be set by passing the `nodeop` option `--config path/to/config.ini`.

## Nodeop Example

The example below shows a typical usage of `nodeop` when starting a block producing node:

```sh
nodeop \
  -e -p sysio \
  --data-dir /users/mydir/sysio/data     \
  --config-dir /users/mydir/sysio/config \
  --plugin sysio::producer_plugin      \
  --plugin sysio::chain_plugin         \
  --plugin sysio::http_plugin          \
  --plugin sysio::state_history_plugin \
  --contracts-console   \
  --access-control-allow-origin='*' \
  --http-validate-host=false        \
  --verbose-http-errors             \
  --state-history-dir /shpdata \
  --trace-history              \
  --chain-state-history        \
  >> nodeop.log 2>&1 &
```

The above `nodeop` command starts a producing node by:

* enabling block production (`-e`)
* identifying itself as block producer "sysio" (`-p`)
* setting the blockchain data directory (`--data-dir`)
* setting the `config.ini` directory (`--config-dir`)
* loading plugins `producer_plugin`, `chain_plugin`, `http_plugin`, `state_history_plugin` (`--plugin`)
* passing `chain_plugin` options (`--contracts-console`)
* passing `http-plugin` options (`--access-control-allow-origin`, `--http-validate-host`, `--verbose-http-errors`)
* passing `state_history` options (`--state-history-dir`, `--trace-history`, `--chain-state-history`)
* redirecting both `stdout` and `stderr` to the `nodeop.log` file
* returning to the shell by running in the background (&)
