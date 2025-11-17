# Wire Sysio

Wire Sysio is a fork of Leap, a C++ implementation of the [Antelope](https://github.com/AntelopeIO) protocol. It
contains blockchain node software and supporting tools for developers and node operators.

## Branches

The `master` branch is the latest stable branch.

## Supported Operating Systems

We currently support the following operating systems.

| **Operating Systems** |
|-----------------------|
| Ubuntu 24.04 Jammy    |
| Ubuntu 25.04 Plucky   |

<!-- TODO: needs to add and test build on unsupported environments -->

## Installation

In the future, we plan to support downloading Debian packages directly from
our [release page](https://github.com/Wire-Network/wire-sysio/releases), providing a more streamlined and convenient
setup process. However, for the time being, installation requires *building the software from source*.

Finally, verify Wire Sysio was installed correctly:

```bash
nodeop --full-version
```

You should see a [semantic version](https://semver.org) string followed by a `git` commit hash with no errors. For
example:

```
v3.1.2-0b64f879e3ebe2e4df09d2e62f1fc164cc1125d1
```

## Building from source

Follow the instructions [BUILD.md](./BUILD.md) to build Wire Sysio
from source.

## Testing

Wire Sysio supports the following test suites:

 Test Suite                                    |       Test Type       | [Test Size](https://testing.googleblog.com/2010/12/test-sizes.html) | Notes                                                                
-----------------------------------------------|:---------------------:|:-------------------------------------------------------------------:|----------------------------------------------------------------------
 [Parallelizable tests](#parallelizable-tests) |      Unit tests       |                                Small                                
 [WASM spec tests](#wasm-spec-tests)           |      Unit tests       |                                Small                                | Unit tests for our WASM runtime, each short but *very* CPU-intensive 
 [Serial tests](#serial-tests)                 | Component/Integration |                               Medium                                
 [Long-running tests](#long-running-tests)     |      Integration      |                           Medium-to-Large                           | Tests which take an extraordinarily long amount of time to run       

When building from source, we recommended running at least the [parallelizable tests](#parallelizable-tests).

#### Parallelizable Tests

This test suite consists of any test that does not require shared resources, such as file descriptors, specific folders,
or ports, and can therefore be run concurrently in different threads without side effects (hence, easily parallelized).
These are mostly unit tests and [small tests](https://testing.googleblog.com/2010/12/test-sizes.html) which complete in
a short amount of time.

You can invoke them by running `ctest` from a terminal in your build directory and specifying the following arguments:

```bash
ctest -j "$(nproc)" -LE _tests
```

Since Wire resource handling changes caused considerable changes for unit test setup, some tests have been turned off
till they can be fixed, so that the core set of tests can be successfully validated. To include these tests in running
the above ctest command, the following flag must be passed to cmake before compiling and running: "
-DDONT_SKIP_TESTS=TRUE"

#### WASM Spec Tests

The WASM spec tests verify that our WASM execution engine is compliant with the web assembly standard. These are
very [small](https://testing.googleblog.com/2010/12/test-sizes.html), very fast unit tests. However, there are over a
thousand of them so the suite can take a little time to run. These tests are extremely CPU-intensive.

You can invoke them by running `ctest` from a terminal in your Wire Sysio build directory and specifying the following
arguments:

```bash
ctest -j "$(nproc)" -L wasm_spec_tests
```

We have observed severe performance issues when multiple virtual machines are running this test suite on the same
physical host at the same time, for example in a CICD system. This can be resolved by disabling hyperthreading on the
host.

#### Serial Tests

The serial test suite consists of [medium](https://testing.googleblog.com/2010/12/test-sizes.html) component or
integration tests that use specific paths, ports, rely on process names, or similar, and cannot be run concurrently with
other tests. Serial tests can be sensitive to other software running on the same host and they may `SIGKILL` other
`nodeop` processes. These tests take a moderate amount of time to complete, but we recommend running them.

You can invoke them by running `ctest` from a terminal in your build directory and specifying the following arguments:

```bash
ctest -L "nonparallelizable_tests"
```

#### Long-Running Tests

The long-running tests are [medium-to-large](https://testing.googleblog.com/2010/12/test-sizes.html) integration tests
that rely on shared resources and take a very long time to run.
Follow the instructions [BUILD.md](./BUILD.md) to build Wire Sysio
from source.
## Testing

You can invoke them by running `ctest` from a terminal in your `build` directory and specifying the following arguments:

```bash
ctest -L "long_running_tests"
```

---

<!-- markdownlint-disable MD033 -->
<table>
  <tr>
    <td><img src="https://bucket.gitgo.app/frontend-assets/icons/favicon.png" alt="Wire Network" width="50"/></td>
    <td>
      <strong>Wire Network</strong><br>
      <a href="https://www.wire.network/">Website</a> |
      <a href="https://x.com/wire_blockchain">Twitter</a> |
      <a href="https://www.linkedin.com/company/wire-network-blockchain/">LinkedIn</a><br>
      © 2024 Wire Network. All rights reserved.
    </td>
  </tr>
</table>