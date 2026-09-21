# template_plugin

`template_plugin` is the scaffold a new `nodeop` plugin starts from: the smallest appbase plugin that still
has the right shape — a `plugin_target` CMake file, a public header declaring the four lifecycle methods, and
a source file with a private `impl` struct behind a `unique_ptr`. It implements no behavior, registers no
options, and is absent from `plugins/CMakeLists.txt`, so nothing builds it, it is never compiled into
`nodeop`, and there is nothing for an operator to enable. Copy it — by hand, with
[`plugins/sysio-make_new_plugin.sh`](../sysio-make_new_plugin.sh), or by generating a fresh copy with
`npx plop create-cxx-plugin` — when adding a plugin.

Because nothing builds it, the scaffold is not checked by any CI target. It compiles clean when checked
with a neighbouring plugin's flags (`clang++-18 -std=gnu++23 -fsyntax-only` against `build/<dir>`'s
`compile_commands.json` entry for `test_control_api_plugin`, with this directory's `include/` added); repeat
that check after editing it, since a mistake here is copied into every new plugin.

## What it demonstrates

Three files, mirroring what every other plugin in this tree looks like (plus this README, which documents the
scaffold and is not part of it):

```
template_plugin/
├── CMakeLists.txt                                  plugin_target(template_plugin LIBRARIES appbase)
├── include/sysio/template_plugin/template_plugin.hpp   the public header other plugins include
└── src/template_plugin.cpp                         the definitions
```

- **`plugin_target`** (from `cmake/plugin-tools.cmake`) is the only build call needed. It globs
  `src/*.cpp` into a static library, publishes `include/` as the target's public include directory, links the
  shared default dependency set (fc, the Boost components, boringssl, libsodium) whole-archive plus whatever
  is named after `LIBRARIES`, and — when `ENABLE_TESTS` is on and a `test/` directory exists — builds a
  `test_<name>` executable from `test/*.cpp` and registers it with CTest.
- **The header** derives from `appbase::plugin<template_plugin>`, declares `APPBASE_PLUGIN_REQUIRES()` with
  an empty dependency list, and declares `set_program_options`, `plugin_initialize`, `plugin_startup`, and
  `plugin_shutdown`. `APPBASE_PLUGIN_REQUIRES` is not optional even when empty: the macro defines the
  `plugin_requires` member appbase calls to walk and auto-register dependencies, so a plugin named there
  needs no explicit registration of its own.
- **The pimpl** — `struct impl;` in the header, `std::unique_ptr<impl> _impl` as the only member, the struct
  defined in the `.cpp`, and an out-of-line destructor — keeps the plugin's state out of its public header.
  Per the lifecycle rules in [`usage_pattern.md`](../usage_pattern.md), resources allocated during
  `plugin_initialize` are released by the destructor, not by `plugin_shutdown`.
- **`plugin_initialize`** wraps its body in `try { } FC_LOG_AND_RETHROW()`, which is how a configuration
  failure is logged and re-raised so appbase aborts startup. The macro comes from
  `<fc/exception/exception.hpp>`, which the source includes directly rather than relying on it arriving
  through `<sysio/chain/application.hpp>`.

## Generating a copy

The repository ships a [plop](https://plopjs.com) generator, `create-cxx-plugin`, registered in
`plopfile.js`. It is the path the header comment points at, and it emits the same three files plus a test
target:

```bash
pnpm install                     # plop is a devDependency; the repo pins pnpm as its package manager
npx plop create-cxx-plugin
```

It asks two questions:

| Prompt | Rule | Default |
|---|---|---|
| `C++ Plugin Name (must end with _plugin)` | lowercase letters, digits, and underscores only; must end in `_plugin` | none |
| `C++ Namespace` | lowercase segments separated by `::`, no leading or trailing `::` | `sysio` |

and writes five files under `plugins/<name>/`:

```
CMakeLists.txt
include/sysio/<name>/<name>.hpp
src/<name>.cpp
test/main.cpp                 Boost.Test module entry point
test/test_<name>.cpp          one case asserting the plugin can be constructed
```

Two differences from the checked-in `template_plugin` are worth knowing. The generator emits the `test/`
pair, which this directory does not carry; and its `plugin_startup` / `plugin_shutdown` bodies each log one
line (`<name>: startup`, `<name>: shutdown`) where the checked-in versions are empty. Note also that the
header path segment is always `sysio` regardless of the namespace answered — a plugin in a different
namespace still installs its header under `include/sysio/<name>/`.

The generated pair compiles clean under the same syntax check as the scaffold: the header carries the
`using namespace appbase;` that the unqualified `options_description` and `variables_map` parameter types
need, and the source includes `<fc/exception/exception.hpp>` for `FC_LOG_AND_RETHROW` and `ilog`.

## Copying it by hand

[`plugins/sysio-make_new_plugin.sh <name>`](../sysio-make_new_plugin.sh) does the mechanical part: it copies
the directory, drops this README, renames the header directory and the files, and rewrites every
`template_plugin` occurrence inside them. Doing it by hand is the same work. Either way, wire the result up:

1. Add `add_subdirectory(<name>)` to [`plugins/CMakeLists.txt`](../CMakeLists.txt).
2. Name the dependencies after `LIBRARIES` in the new `CMakeLists.txt`, and declare the same plugins in
   `APPBASE_PLUGIN_REQUIRES` so appbase initializes them first.
3. To make it loadable by `nodeop`, link it into the shared plugin set in `chain_target`
   (`cmake/chain-tools.cmake`) and add `application_base::register_plugin<<name>>();` to
   `programs/nodeop/main.cpp`. A plugin reached only through another plugin's `APPBASE_PLUGIN_REQUIRES` is
   registered by that macro and needs no line in `main.cpp`.
4. Add a `test/` directory with a Boost.Test `main.cpp` to get a `test_<name>` target for free.

Linking is not registration. A plugin whose archive is linked into `nodeop` but for which
`register_plugin<...>()` is never called is not in the registry, so no `plugin = <namespace>::<name>` line can
resolve it. The call happens either in `programs/nodeop/main.cpp` (step 3) or from a static initializer in the
plugin's own source — `static auto _x = application::register_plugin<x_plugin>();`, the form
`plugins/http_plugin/test/unit_tests.cpp` uses. Neither this scaffold nor the plop template emits that
initializer, so a copy that skips step 3 stays unreachable.

A plugin that lives outside this tree does not need steps 1 and 3's CMake edit:
`programs/nodeop/CMakeLists.txt` accepts a `SYSIO_ADDITIONAL_PLUGINS` list of external source directories,
adds each as a subdirectory, and links every target the directory reports through the
`sysio_additional_plugin` macro. It still needs its own `register_plugin<...>()` call.

## Enabling / configuration

Nothing to enable. `template_plugin` is not listed in `plugins/CMakeLists.txt`, so no build produces it and
no `plugin = sysio::template_plugin` line will resolve.

## Options

`set_program_options` registers no options — the body is empty, and it is there to be filled in. Register
options that belong in `config.ini` on the `cfg` description; appbase adds those to the command-line
description as well, so a `cfg` option works in both places. Reserve `cli` for options that only make sense
on the command line.

## HTTP API

None. Registering endpoints means depending on `http_plugin` and calling `add_api` from `plugin_startup` —
[`test_control_api_plugin`](../test_control_api_plugin/README.md) is the smallest worked example in the tree.

## Diagnostics

The checked-in scaffold logs nothing; `plugin_startup` and `plugin_shutdown` are empty. The plop template
emits `ilog("<name>: startup")` and `ilog("<name>: shutdown")` in their place. `ilog`, `wlog`, `dlog`, and
`elog` write to fc's `default` logger; a plugin that wants its own category declares a named `fc::logger` and
uses the `fc_ilog(logger(), ...)` family instead.

## Tests

This directory has no `test/` subdirectory, so no test target is generated. Add `test/main.cpp` with

```cpp
#define BOOST_TEST_MODULE <name>
#include <boost/test/included/unit_test.hpp>
```

and one or more `test/test_*.cpp` files, and `plugin_target` picks them up automatically:

```bash
ninja -C build/debug test_<name>
./build/debug/plugins/<name>/test_<name>
```

If the `test/` directory carries its own `CMakeLists.txt`, `plugin_target` defers to it instead of globbing,
and the binary lands under `build/debug/plugins/<name>/test/` — that is how the larger suites in this tree
(`chain_plugin`, `net_plugin`, `producer_plugin`, `status_monitor_plugin`) are laid out.

## Related

- [`plugins/usage_pattern.md`](../usage_pattern.md) — the appbase lifecycle contract: what belongs in
  `plugin_initialize`, `plugin_startup`, and `plugin_shutdown`, and what appbase guarantees around each.
- [`plugins/chain_interface`](../chain_interface/README.md) — the shared channel and method declarations a
  new plugin uses to receive block and transaction events.
- `cmake/plugin-tools.cmake` — the `plugin_target` macro this scaffold's `CMakeLists.txt` calls.
- `tools/plop/generators/create-cxx-plugin.js` and `tools/plop/templates/create-cxx-plugin/` — the generator
  and its templates.
- [`plugins/sysio-make_new_plugin.sh`](../sysio-make_new_plugin.sh) — the shell copy-and-rename alternative to
  the plop generator.
