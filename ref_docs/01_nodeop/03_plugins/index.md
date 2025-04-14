---
content_title: Nodeop Plugins
---

## Overview

Plugins extend the core functionality implemented in `nodeop`. Some plugins are mandatory, such as `chain_plugin`, `net_plugin`, and `producer_plugin`, which reflect the modular design of `nodeop`. The other plugins are optional as they provide nice to have features, but non-essential for the nodes operation.

For information on specific plugins, just select from the list below:

* [`chain_api_plugin`](chain_api_plugin/index.md)
* [`chain_plugin`](chain_plugin/index.md)
* [`db_size_api_plugin`](db_size_api_plugin/index.md)
* [`http_plugin`](http_plugin/index.md)
* [`net_api_plugin`](net_api_plugin/index.md)
* [`net_plugin`](net_plugin/index.md)
* [`producer_plugin`](producer_plugin/index.md)
* [`state_history_plugin`](state_history_plugin/index.md)
* [`test_control_api_plugin`](test_control_api_plugin/index.md)
* [`test_control_plugin`](test_control_plugin/index.md)
* [`trace_api_plugin`](trace_api_plugin/index.md)

[[info | Nodeop is modular]]
| Plugins add incremental functionality to `nodeop`. Unlike runtime plugins, `nodeop` plugins are built at compile-time.
