## Goal

Connect to a specific `nodeop` or `kiod` host to send COMMAND

`clio` and `kiod` can connect to a specific node by using the `--url` or `--wallet-url` optional arguments, respectively, followed by the http address and port number these services are listening to.

[[info | Default address:port]]
| If no optional arguments are used (i.e. `--url` or `--wallet-url`), `clio` attempts to connect to a local `nodeop` or `kiod` running at localhost `127.0.0.1` and default port `8888`.

## Before you begin

* Install the currently supported version of `clio`

## Steps
### Connecting to Nodeop

```sh
clio -url http://nodeop-host:8888 COMMAND
```

### Connecting to Kiod

```sh
clio --wallet-url http://kiod-host:8888 COMMAND
```
