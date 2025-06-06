# Performance Harness

The Performance Harness is a module which provides the framework and utilities to run performance load tests on node infrastructure.

`PerformanceHarnessScenarioRunner.py` is currently the main entry point and provides the utility to configure and run such tests.  It also serves as an example of how one can import the `PerformanceHarness` module to configure and run performance tests.  The `PerformanceHarnessScenarioRunner` currently provides two options for running performance tests.  The first `findMax` uses the `PerformanceTest` class to run a suite of `PerformanceTestBasic` test run to zero in on a max performance (see section on `PerformanceTest` following.).  The second is `singleTest` which allows a user to run a single `PerformanceTestBasic` and see the results of a single configuration (see `PerformanceTestBasic` section following).

The `PerformanceTest`'s main goal is to measure current peak performance metrics through iteratively tuning and running basic performance tests. The current `PerformanceTest` scenario works to determine the maximum throughput of Token Transfers (or other transaction types if configured) the system can sustain.  It does this by conducting a binary search of possible Token Transfers Per Second (TPS) configurations, testing each configuration in a short duration test and scoring its result. The search algorithm iteratively configures and runs `PerformanceTestBasic` test scenarios and analyzes the output to determine a success metric used to continue the search.  When the search completes, a max TPS throughput value is reported (along with other performance metrics from that run).  The `PerformanceTest` then proceeds to conduct an additional search with longer duration test runs within a narrowed TPS configuration range to determine the sustainable max TPS. Finally it produces a report on the entire performance run, summarizing each individual test scenario, results, and full report details on the tests when maximum TPS was achieved ([Performance Test Report](#performance-test-report))

The `PerformanceTestBasic` test performs a single basic performance test that targets a configurable TPS target and, if successful, reports statistics on performance metrics measured during the test.  It configures and launches a blockchain test environment, creates wallets and accounts for testing, and configures and launches transaction generators for creating specific transaction load in the ecosystem.  Finally it analyzes the performance of the system under the configuration through log analysis and chain queries and produces a [Performance Test Basic Report](#performance-test-basic-report).

The `TransactionGeneratorsLauncher` provides a means to easily calculate and spawn the number of transaction generator instances to generate a given target TPS, distributing generation load between the instances in a fair manner such that the aggregate load meets the requested test load.

The `log_reader.py` support script is used primarily to analyze `nodeop` log files to glean information about generated blocks and transactions within those blocks after a test has concluded.  This information is used to produce the performance test report.

# Getting Started
## Prerequisites

Please refer to [Leap: Build and Install from Source](https://github.com/AntelopeIO/leap/#build-and-install-from-source) for a full list of prerequisites.

## Steps

1. Build Leap. For complete instructions on building from source please refer to [Leap: Build and Install from Source](https://github.com/AntelopeIO/leap/#build-and-install-from-source) For older compatible nodeop versions, such as 2.X, the following binaries need to be replaced with the older version: `build/programs/nodeop/nodeop`, `build/programs/clio/clio`, `bin/nodeop`, and `bin/clio`.
2. Run Performance Tests
    1. Full Performance Harness Test Run (Standard):
        ``` bash
        ./build/tests/PerformanceHarnessScenarioRunner.py findMax testBpOpMode
        ```
    2. Single Performance Test Basic Run (Manually run one-off test):
        ```bash
        ./build/tests/PerformanceHarnessScenarioRunner.py singleTest
        ```
3. Collect Results - By default the Performance Harness will capture and save logs.  To delete logs, use `--del-perf-logs`.  Additionally, final reports will be collected by default.  To omit final reports, use `--del-report` and/or `--del-test-report`.
    1. Navigate to performance test logs directory
        ```bash
        cd ./build/PHSRLogs/
        ```
    2. Log Directory Structure is hierarchical with each run of the `PerformanceHarnessScenarioRunner` reporting into a timestamped directory where it includes the full performance report as well as a directory containing output from each test type run (here, `PerformanceTestBasic`) and each individual test run outputs into a timestamped directory within `testRunLogs` that may contain block data logs and transaction generator logs as well as the test's basic report.  An example directory structure follows:
        <details>
            <summary>Expand Example Directory Structure</summary>

        ``` bash
        PHSRLogs/
        └── 2023-04-05_14-35-59
            ├── pluginThreadOptRunLogs
            │   ├── chainThreadResults.txt
            │   ├── netThreadResults.txt
            │   ├── PHSRLogs
            │   │   ├── 2023-04-05_14-35-59-50000
            │   │   │   ├── blockDataLogs
            │   │   │   │   ├── blockData.txt
            │   │   │   │   ├── blockTrxData.txt
            │   │   │   │   └── transaction_metrics.csv
            │   │   │   ├── etc
            │   │   │   │   └── sysio
            │   │   │   │       ├── node_00
            │   │   │   │       │   ├── config.ini
            │   │   │   │       │   ├── genesis.json
            │   │   │   │       │   ├── logging.json
            │   │   │   │       │   └── protocol_features
            │   │   │   │       │       ├── BUILTIN-ACTION_RETURN_VALUE.json
            │   │   │   │       │       ├── BUILTIN-BLOCKCHAIN_PARAMETERS.json
            │   │   │   │       │       ├── BUILTIN-CONFIGURABLE_WASM_LIMITS2.json
            │   │   │   │       │       ├── BUILTIN-CRYPTO_PRIMITIVES.json
            │   │   │   │       │       ├── BUILTIN-DISALLOW_EMPTY_PRODUCER_SCHEDULE.json
            │   │   │   │       │       ├── BUILTIN-FIX_LINKAUTH_RESTRICTION.json
            │   │   │   │       │       ├── BUILTIN-FORWARD_SETCODE.json
            │   │   │   │       │       ├── BUILTIN-GET_BLOCK_NUM.json
            │   │   │   │       │       ├── BUILTIN-GET_CODE_HASH.json
            │   │   │   │       │       ├── BUILTIN-GET_SENDER.json
            │   │   │   │       │       ├── BUILTIN-NO_DUPLICATE_DEFERRED_ID.json
            │   │   │   │       │       ├── BUILTIN-ONLY_BILL_FIRST_AUTHORIZER.json
            │   │   │   │       │       ├── BUILTIN-ONLY_LINK_TO_EXISTING_PERMISSION.json
            │   │   │   │       │       ├── BUILTIN-PREACTIVATE_FEATURE.json
            │   │   │   │       │       ├── BUILTIN-RAM_RESTRICTIONS.json
            │   │   │   │       │       ├── BUILTIN-REPLACE_DEFERRED.json
            │   │   │   │       │       ├── BUILTIN-RESTRICT_ACTION_TO_SELF.json
            │   │   │   │       │       ├── BUILTIN-WEBAUTHN_KEY.json
            │   │   │   │       │       └── BUILTIN-WTMSIG_BLOCK_SIGNATURES.json
            │   │   │   │       ├── node_01
            │   │   │   │       │   ├── config.ini
            │   │   │   │       │   ├── genesis.json
            │   │   │   │       │   ├── logging.json
            │   │   │   │       │   └── protocol_features
            │   │   │   │       │       ├── BUILTIN-ACTION_RETURN_VALUE.json
            │   │   │   │       |       .
            │   │   │   │       |       .
            │   │   │   │       |       .
            │   │   │   │       │       └── BUILTIN-WTMSIG_BLOCK_SIGNATURES.json
            │   │   │   │       └── node_bios
            │   │   │   │           ├── config.ini
            │   │   │   │           ├── genesis.json
            │   │   │   │           ├── logging.json
            │   │   │   │           └── protocol_features
            │   │   │   │               ├── BUILTIN-ACTION_RETURN_VALUE.json
            │   │   │   │               .
            │   │   │   │               .
            │   │   │   │               .
            │   │   │   │               └── BUILTIN-WTMSIG_BLOCK_SIGNATURES.json
            │   │   │   ├── trxGenLogs
            │   │   │   │   ├── first_trx_9486.txt
            │   │   │   │   .
            │   │   │   │   .
            │   │   │   │   .
            │   │   │   │   ├── first_trx_9498.txt
            │   │   │   │   ├── trx_data_output_9486.txt
            │   │   │   │   .
            │   │   │   │   .
            │   │   │   │   .
            │   │   │   │   └── trx_data_output_9498.txt
            │   │   │   └── var
            │   │   │       └── PerformanceHarnessScenarioRunner8480
            │   │   │           ├── node_00
            │   │   │           │   ├── blocks
            │   │   │           │   │   ├── blocks.index
            │   │   │           │   │   ├── blocks.log
            │   │   │           │   │   └── reversible
            │   │   │           │   ├── nodeop.pid
            │   │   │           │   ├── snapshots
            │   │   │           │   ├── state
            │   │   │           │   │   └── shared_memory.bin
            │   │   │           │   ├── stderr.2023_04_05_09_35_59.txt
            │   │   │           │   ├── stderr.txt -> stderr.2023_04_05_09_35_59.txt
            │   │   │           │   └── stdout.txt
            │   │   │           ├── node_01
            │   │   │           │   ├── blocks
            │   │   │           │   │   ├── blocks.index
            │   │   │           │   │   ├── blocks.log
            │   │   │           │   │   └── reversible
            │   │   │           │   ├── nodeop.pid
            │   │   │           │   ├── snapshots
            │   │   │           │   ├── state
            │   │   │           │   │   └── shared_memory.bin
            │   │   │           │   ├── stderr.2023_04_05_09_35_59.txt
            │   │   │           │   ├── stderr.txt -> stderr.2023_04_05_09_35_59.txt
            │   │   │           │   ├── stdout.txt
            │   │   │           │   └── traces
            │   │   │           │       ├── trace_0000000000-0000010000.log
            │   │   │           │       ├── trace_index_0000000000-0000010000.log
            │   │   │           │       └── trace_trx_id_0000000000-0000010000.log
            │   │   │           └── node_bios
            │   │   │               ├── blocks
            │   │   │               │   ├── blocks.index
            │   │   │               │   ├── blocks.log
            │   │   │               │   └── reversible
            │   │   │               │       └── fork_db.dat
            │   │   │               ├── nodeop.pid
            │   │   │               ├── snapshots
            │   │   │               ├── state
            │   │   │               │   └── shared_memory.bin
            │   │   │               ├── stderr.2023_04_05_09_35_59.txt
            │   │   │               ├── stderr.txt -> stderr.2023_04_05_09_35_59.txt
            │   │   │               ├── stdout.txt
            │   │   │               └── traces
            │   │   │                   ├── trace_0000000000-0000010000.log
            │   │   │                   ├── trace_index_0000000000-0000010000.log
            │   │   │                   └── trace_trx_id_0000000000-0000010000.log
            │   │   ├── 2023-04-05_14-37-31-25001
            │   │   ├── 2023-04-05_14-38-59-12501
            │   │   .
            │   │   .
            │   │   .
            │   │   └── 2023-04-05_16-13-11-13001
            │   └── producerThreadResults.txt
            ├── report.json
            └── testRunLogs
                └── PHSRLogs
                    ├── 2023-04-05_16-14-31-50000
                    │   ├── blockDataLogs
                    │   │   ├── blockData.txt
                    │   │   ├── blockTrxData.txt
                    │   │   └── transaction_metrics.csv
                    │   ├── data.json
                    │   ├── etc
                    │   │   └── sysio
                    │   │       ├── node_00
                    │   │       │   ├── config.ini
                    │   │       │   ├── genesis.json
                    │   │       │   ├── logging.json
                    │   │       │   └── protocol_features
                    │   │       │       ├── BUILTIN-ACTION_RETURN_VALUE.json
                    │   │       │       ├── BUILTIN-BLOCKCHAIN_PARAMETERS.json
                    │   │       │       ├── BUILTIN-CONFIGURABLE_WASM_LIMITS2.json
                    │   │       │       ├── BUILTIN-CRYPTO_PRIMITIVES.json
                    │   │       │       ├── BUILTIN-DISALLOW_EMPTY_PRODUCER_SCHEDULE.json
                    │   │       │       ├── BUILTIN-FIX_LINKAUTH_RESTRICTION.json
                    │   │       │       ├── BUILTIN-FORWARD_SETCODE.json
                    │   │       │       ├── BUILTIN-GET_BLOCK_NUM.json
                    │   │       │       ├── BUILTIN-GET_CODE_HASH.json
                    │   │       │       ├── BUILTIN-GET_SENDER.json
                    │   │       │       ├── BUILTIN-NO_DUPLICATE_DEFERRED_ID.json
                    │   │       │       ├── BUILTIN-ONLY_BILL_FIRST_AUTHORIZER.json
                    │   │       │       ├── BUILTIN-ONLY_LINK_TO_EXISTING_PERMISSION.json
                    │   │       │       ├── BUILTIN-PREACTIVATE_FEATURE.json
                    │   │       │       ├── BUILTIN-RAM_RESTRICTIONS.json
                    │   │       │       ├── BUILTIN-REPLACE_DEFERRED.json
                    │   │       │       ├── BUILTIN-RESTRICT_ACTION_TO_SELF.json
                    │   │       │       ├── BUILTIN-WEBAUTHN_KEY.json
                    │   │       │       └── BUILTIN-WTMSIG_BLOCK_SIGNATURES.json
                    │   │       ├── node_01
                    │   │       │   ├── config.ini
                    │   │       │   ├── genesis.json
                    │   │       │   ├── logging.json
                    │   │       │   └── protocol_features
                    │   │       │       ├── BUILTIN-ACTION_RETURN_VALUE.json
                    │   │       |       .
                    │   │       |       .
                    │   │       |       .
                    │   │       │       └── BUILTIN-WTMSIG_BLOCK_SIGNATURES.json
                    │   │       └── node_bios
                    │   │           ├── config.ini
                    │   │           ├── genesis.json
                    │   │           ├── logging.json
                    │   │           └── protocol_features
                    │   │               ├── BUILTIN-ACTION_RETURN_VALUE.json
                    │   │               .
                    │   │               .
                    │   │               .
                    │   │               └── BUILTIN-WTMSIG_BLOCK_SIGNATURES.json
                    │   ├── trxGenLogs
                    │   │   ├── first_trx_20199.txt
                    │   │   .
                    │   │   .
                    │   │   .
                    │   │   ├── first_trx_20211.txt
                    │   │   ├── trx_data_output_20199.txt
                    │   │   .
                    │   │   .
                    │   │   .
                    │   │   └── trx_data_output_20211.txt
                    │   └── var
                    │       └── PerformanceHarnessScenarioRunner8480
                    │           ├── node_00
                    │           │   ├── blocks
                    │           │   │   ├── blocks.index
                    │           │   │   ├── blocks.log
                    │           │   │   └── reversible
                    │           │   ├── nodeop.pid
                    │           │   ├── snapshots
                    │           │   ├── state
                    │           │   │   └── shared_memory.bin
                    │           │   ├── stderr.2023_04_05_11_14_31.txt
                    │           │   ├── stderr.txt -> stderr.2023_04_05_11_14_31.txt
                    │           │   └── stdout.txt
                    │           ├── node_01
                    │           │   ├── blocks
                    │           │   │   ├── blocks.index
                    │           │   │   ├── blocks.log
                    │           │   │   └── reversible
                    │           │   ├── nodeop.pid
                    │           │   ├── snapshots
                    │           │   ├── state
                    │           │   │   └── shared_memory.bin
                    │           │   ├── stderr.2023_04_05_11_14_31.txt
                    │           │   ├── stderr.txt -> stderr.2023_04_05_11_14_31.txt
                    │           │   ├── stdout.txt
                    │           │   └── traces
                    │           │       ├── trace_0000000000-0000010000.log
                    │           │       ├── trace_index_0000000000-0000010000.log
                    │           │       └── trace_trx_id_0000000000-0000010000.log
                    │           └── node_bios
                    │               ├── blocks
                    │               │   ├── blocks.index
                    │               │   ├── blocks.log
                    │               │   └── reversible
                    │               │       └── fork_db.dat
                    │               ├── nodeop.pid
                    │               ├── snapshots
                    │               ├── state
                    │               │   └── shared_memory.bin
                    │               ├── stderr.2023_04_05_11_14_31.txt
                    │               ├── stderr.txt -> stderr.2023_04_05_11_14_31.txt
                    │               ├── stdout.txt
                    │               └── traces
                    │                   ├── trace_0000000000-0000010000.log
                    │                   ├── trace_index_0000000000-0000010000.log
                    │                   └── trace_trx_id_0000000000-0000010000.log
                    ├── 2023-04-05_16-16-03-25001
                    ├── 2023-04-05_16-17-34-12501
                    .
                    .
                    .
                    └── 2023-04-05_16-25-39-13501
        ```
        </details>

# Configuring Performance Harness Tests

## Performance Harness Scenario Runner

The Performance Harness Scenario Runner is the main script that configures and runs `PerformanceTest` runs or single `PerformanceTestBasic` runs.

<details open>
    <summary>Usage</summary>

```
usage: PerformanceHarnessScenarioRunner.py [-h] {singleTest,findMax} ...
```

</details>

<details open>
    <summary>Expand Scenario Type Sub-Command List</summary>

```
optional arguments:
  -h, --help            show this help message and exit

Scenario Types:
  Each Scenario Type sets up either a Performance Test Basic or Performance Test testing scenario and allows further configuration of the scenario.

  {singleTest,findMax}  Currently supported scenario type sub-commands.
    singleTest          Run a single Performance Test Basic test scenario.
    findMax             Runs a Performance Test scenario.
```

</details>

## Performance Test

The Performance Harness main test class `PerformanceTest` (residing in `performance_test.py`) can be configured through the `findMax` sub-command to `PerformanceHarnessScenarioRunner` using the following command line arguments:

<details open>
    <summary>Usage</summary>

```
usage: PerformanceHarnessScenarioRunner.py findMax [-h] {testBpOpMode,testApiOpMode} ...
```

</details>

<details open>
    <summary>Expand Operational Mode Sub-Command List</summary>

```
optional arguments:
-h, --help      show this help message and exit

Operational Modes:
  Each Operational Mode sets up a known node operator configuration and
  performs load testing and analysis catered to the expectations of that
  specific operational mode. For additional configuration options for each
  operational mode use, pass --help to the sub-command. Eg:
  performance_test.py testBpOpMode --help

  {testBpOpMode,testApiOpMode}
                        Currently supported operational mode sub-commands.
    testBpOpMode        Test the Block Producer Operational Mode.
    testApiOpMode       Test the API Node Operational Mode.
```

</details>

### Operational Modes

- Block Producer Mode
  - Transactions are sent to the p2p endpoint of the block producer node
  - Topology:
    - Default Producer Node Count=1
    - Default Validation Node Count=1
- Api Node Mode
  - Transactions are sent to the http api endpoint of the api node
  - Topology:
    - Default Producer Node Count=1
    - Default Validation Node Count=1
    - Default API Node Count=1

### Operations Mode Usage/Configuration
<br/>
<details>
    <summary>Usage</summary>

```
usage: PerformanceHarnessScenarioRunner.py findMax testBpOpMode [--skip-tps-test]
                                                                [--calc-producer-threads {none,lmax,full}]
                                                                [--calc-chain-threads {none,lmax,full}]
                                                                [--calc-net-threads {none,lmax,full}]
                                                                [--del-test-report]
                                                                [--max-tps-to-test MAX_TPS_TO_TEST]
                                                                [--min-tps-to-test MIN_TPS_TO_TEST]
                                                                [--test-iteration-duration-sec TEST_ITERATION_DURATION_SEC]
                                                                [--test-iteration-min-step TEST_ITERATION_MIN_STEP]
                                                                [--final-iterations-duration-sec FINAL_ITERATIONS_DURATION_SEC]
                                                                [-h]
                                                                {overrideBasicTestConfig} ...
```

</details>

<details>
    <summary>Expand Block Producer Mode Argument List</summary>

```
optional arguments:
  -h, --help            show this help message and exit

Performance Harness:
  Performance Harness testing configuration items.

  --skip-tps-test       Determines whether to skip the max TPS measurement
                        tests
  --calc-producer-threads {none,lmax,full}
                        Determines whether to calculate number of worker
                        threads to use in producer thread pool ("none",
                        "lmax", or "full"). In "none" mode, the default, no
                        calculation will be attempted and the configured
                        --producer-threads value will be used. In "lmax" mode,
                        producer threads will incrementally be tested,
                        starting at plugin default, until the performance rate
                        ceases to increase with the addition of additional
                        threads. In "full" mode producer threads will
                        incrementally be tested from plugin default..num
                        logical processors, recording each performance and
                        choosing the local max performance (same value as
                        would be discovered in "lmax" mode). Useful for
                        graphing the full performance impact of each available
                        thread.
  --calc-chain-threads {none,lmax,full}
                        Determines whether to calculate number of worker
                        threads to use in chain thread pool ("none", "lmax",
                        or "full"). In "none" mode, the default, no
                        calculation will be attempted and the configured
                        --chain-threads value will be used. In "lmax" mode,
                        producer threads will incrementally be tested,
                        starting at plugin default, until the performance rate
                        ceases to increase with the addition of additional
                        threads. In "full" mode producer threads will
                        incrementally be tested from plugin default..num
                        logical processors, recording each performance and
                        choosing the local max performance (same value as
                        would be discovered in "lmax" mode). Useful for
                        graphing the full performance impact of each available
                        thread.
  --calc-net-threads {none,lmax,full}
                        Determines whether to calculate number of worker
                        threads to use in net thread pool ("none", "lmax", or
                        "full"). In "none" mode, the default, no calculation
                        will be attempted and the configured --net-threads
                        value will be used. In "lmax" mode, producer threads
                        will incrementally be tested, starting at plugin
                        default, until the performance rate ceases to increase
                        with the addition of additional threads. In "full"
                        mode producer threads will incrementally be tested
                        from plugin default..num logical processors, recording
                        each performance and choosing the local max
                        performance (same value as would be discovered in
                        "lmax" mode). Useful for graphing the full performance
                        impact of each available thread.
  --del-test-report     Whether to save json reports from each test scenario.

Performance Harness - TPS Test Config:
  TPS Performance Test configuration items.

  --max-tps-to-test MAX_TPS_TO_TEST
                        The max target transfers realistic as ceiling of test
                        range
  --min-tps-to-test MIN_TPS_TO_TEST
                        The min target transfers to use as floor of test range
  --test-iteration-duration-sec TEST_ITERATION_DURATION_SEC
                        The duration of transfer trx generation for each
                        iteration of the test during the initial search
                        (seconds)
  --test-iteration-min-step TEST_ITERATION_MIN_STEP
                        The step size determining granularity of tps result
                        during initial search
  --final-iterations-duration-sec FINAL_ITERATIONS_DURATION_SEC
                        The duration of transfer trx generation for each final
                        longer run iteration of the test during the final
                        search (seconds)

Advanced Configuration Options:
  Block Producer Operational Mode Advanced Configuration Options allow low
  level adjustments to the basic test configuration as well as the node
  topology being tested. For additional information on available advanced
  configuration options, pass --help to the sub-command. Eg:
  performance_test.py testBpOpMode overrideBasicTestConfig --help

  {overrideBasicTestConfig}
                        sub-command to allow overriding advanced configuration
                        options
    overrideBasicTestConfig
                        Use this sub-command to override low level controls
                        for basic test, logging, node topology, etc.
```

</details>
<br/>

#### Advanced Configuration: OverrideBasicTestConfig sub-command
<br/>
<details>
    <summary>Usage</summary>

```
usage: PerformanceHarnessScenarioRunner.py findMax testBpOpMode overrideBasicTestConfig
       [-h] [-d D] [--dump-error-details] [-v] [--leave-running] [--unshared]
       [--endpoint-mode {p2p,http}]
       [--producer-nodes PRODUCER_NODES] [--validation-nodes VALIDATION_NODES] [--api-nodes API_NODES]
       [--api-nodes-read-only-threads API_NODES_READ_ONLY_THREADS]
       [--tps-limit-per-generator TPS_LIMIT_PER_GENERATOR]
       [--genesis GENESIS] [--num-blocks-to-prune NUM_BLOCKS_TO_PRUNE]
       [--signature-cpu-billable-pct SIGNATURE_CPU_BILLABLE_PCT]
       [--chain-threads CHAIN_THREADS]
       [--database-map-mode {mapped,heap,locked}]
       [--cluster-log-lvl {all,debug,info,warn,error,off}]
       [--net-threads NET_THREADS]
       [--disable-subjective-billing DISABLE_SUBJECTIVE_BILLING]
       [--produce-block-offset-ms PRODUCE_BLOCK_OFFSET_MS]
       [--producer-threads PRODUCER_THREADS]
       [--read-only-write-window-time-us READ_ONLY_WRITE_WINDOW_TIME_US]
       [--read-only-read-window-time-us READ_ONLY_READ_WINDOW_TIME_US]
       [--http-max-in-flight-requests HTTP_MAX_IN_FLIGHT_REQUESTS]
       [--http-max-response-time-ms HTTP_MAX_RESPONSE_TIME_MS]
       [--http-max-bytes-in-flight-mb HTTP_MAX_BYTES_IN_FLIGHT_MB]
       [--del-perf-logs] [--del-report] [--save-state] [--quiet] [--prods-enable-trace-api]
       [--print-missing-transactions] [--account-name ACCOUNT_NAME]
       [--contract-dir CONTRACT_DIR] [--wasm-file WASM_FILE]
       [--abi-file ABI_FILE] [--user-trx-data-file USER_TRX_DATA_FILE]
       [--wasm-runtime {sys-vm-jit,sys-vm}] [--contracts-console]
       [--sys-vm-oc-cache-size-mb SYS_VM_OC_CACHE_SIZE_MB]
       [--sys-vm-oc-compile-threads SYS_VM_OC_COMPILE_THREADS]
       [--non-prods-sys-vm-oc-enable]
       [--block-log-retain-blocks BLOCK_LOG_RETAIN_BLOCKS]
       [--http-threads HTTP_THREADS]
       [--chain-state-db-size-mb CHAIN_STATE_DB_SIZE_MB]
```

</details>

<details>
    <summary>Expand Override Basic Test Config Argument List</summary>

```
optional arguments:
  -h, --help            show this help message and exit

Test Helper Arguments:
  Test Helper configuration items used to configure and spin up the regression test framework and blockchain environment.

  -d D                  delay between nodes startup
  --dump-error-details  Upon error print etc/sysio/node_*/config.ini and <test_name><pid>/node_*/stderr.log to stdout
  -v                    verbose logging
  --leave-running       Leave cluster running after test finishes
  --unshared            Run test in isolated network namespace

Performance Test Basic Base:
  Performance Test Basic base configuration items.

  --endpoint-mode {p2p,http}
                        Endpoint Mode ("p2p", "http"). In "p2p" mode transactions will be directed to the p2p endpoint on a producer node. In "http" mode transactions will be directed to the http endpoint on an api node.
  --producer-nodes PRODUCER_NODES
                        Producing nodes count
  --validation-nodes VALIDATION_NODES
                        Validation nodes count
  --api-nodes API_NODES
                        API nodes count
  --api-nodes-read-only-threads API_NODES_READ_ONLY_THREADS
                        API nodes read only threads count for use with read-only transactions
  --tps-limit-per-generator TPS_LIMIT_PER_GENERATOR
                        Maximum amount of transactions per second a single generator can have.
  --genesis GENESIS     Path to genesis.json
  --num-blocks-to-prune NUM_BLOCKS_TO_PRUNE
                        The number of potentially non-empty blocks, in addition to leading and trailing size 0 blocks,
                        to prune from the beginning and end of the range of blocks of interest for evaluation.
  --signature-cpu-billable-pct SIGNATURE_CPU_BILLABLE_PCT
                        Percentage of actual signature recovery cpu to bill. Whole number percentages, e.g. 50 for 50%
  --chain-threads CHAIN_THREADS
                        Number of worker threads in controller thread pool
  --database-map-mode {mapped,heap,locked}
                        Database map mode ("mapped", "heap", or "locked").
                        In "mapped" mode database is memory mapped as a file.
                        In "heap" mode database is preloaded in to swappable memory and will use huge pages if available.
                        In "locked" mode database is preloaded, locked in to memory, and will use huge pages if available.
  --cluster-log-lvl {all,debug,info,warn,error,off}
                        Cluster log level ("all", "debug", "info", "warn", "error", or "off").
                        Performance Harness Test Basic relies on some logging at "info" level,
                        so it is recommended lowest logging level to use.
                        However, there are instances where more verbose logging can be useful.
  --net-threads NET_THREADS
                        Number of worker threads in net_plugin thread pool
  --disable-subjective-billing DISABLE_SUBJECTIVE_BILLING
                        Disable subjective CPU billing for API/P2P transactions
  --produce-block-offset-ms PRODUCE_BLOCK_OFFSET_MS
                        The number of milliseconds early the last block of a production round should
                        be produced.
  --producer-threads PRODUCER_THREADS
                        Number of worker threads in producer thread pool
  --read-only-write-window-time-us READ_ONLY_WRITE_WINDOW_TIME_US
                        Time in microseconds the write window lasts.
  --read-only-read-window-time-us READ_ONLY_READ_WINDOW_TIME_US
                        Time in microseconds the read window lasts.
  --http-max-in-flight-requests HTTP_MAX_IN_FLIGHT_REQUESTS
                        Maximum number of requests http_plugin should use for processing http requests. 429 error response when exceeded. -1 for unlimited
  --http-max-response-time-ms HTTP_MAX_RESPONSE_TIME_MS
                        Maximum time for processing a request, -1 for unlimited
  --http-max-bytes-in-flight-mb HTTP_MAX_BYTES_IN_FLIGHT_MB
                        Maximum size in megabytes http_plugin should use for processing http requests. -1 for unlimited. 429 error response when exceeded.
  --del-perf-logs       Whether to delete performance test specific logs.
  --del-report          Whether to delete overarching performance run report.
  --save-state          Whether to save node state. (Warning: large disk usage)
  --quiet               Whether to quiet printing intermediate results and reports to stdout
  --prods-enable-trace-api
                        Determines whether producer nodes should have sysio::trace_api_plugin enabled
  --print-missing-transactions
                        Toggles if missing transactions are be printed upon test completion.
  --account-name ACCOUNT_NAME
                        Name of the account to create and assign a contract to
  --contract-dir CONTRACT_DIR
                        Path to contract dir
  --wasm-file WASM_FILE
                        WASM file name for contract
  --abi-file ABI_FILE   ABI file name for contract
  --user-trx-data-file USER_TRX_DATA_FILE
                        Path to transaction data JSON file
  --wasm-runtime {sys-vm-jit,sys-vm}
                        Override default WASM runtime ("sys-vm-jit", "sys-vm")
                        "sys-vm-jit" : A WebAssembly runtime that compiles WebAssembly code to native x86 code prior to execution.
                        "sys-vm" : A WebAssembly interpreter.
  --contracts-console   print contract's output to console
  --sys-vm-oc-cache-size-mb SYS_VM_OC_CACHE_SIZE_MB
                        Maximum size (in MiB) of the SYS VM OC code cache
  --sys-vm-oc-compile-threads SYS_VM_OC_COMPILE_THREADS
                        Number of threads to use for SYS VM OC tier-up
  --non-prods-sys-vm-oc-enable
                        Enable SYS VM OC tier-up runtime on non producer nodes
  --block-log-retain-blocks BLOCK_LOG_RETAIN_BLOCKS
                        If set to greater than 0, periodically prune the block log to store only configured number of most recent blocks.
                        If set to 0, no blocks are be written to the block log; block log file is removed after startup.
  --http-threads HTTP_THREADS
                        Number of worker threads in http thread pool
  --chain-state-db-size-mb CHAIN_STATE_DB_SIZE_MB
                        Maximum size (in MiB) of the chain state database
```

</details>
<br/>

# Support Classes and Scripts

The following classes and scripts are typically used by the Performance Harness main test class `PerformanceTest` to perform specific tasks as delegated and configured by the main scenario.  However, there may be applications in certain use cases where running a single one-off test or transaction generator is desired.  In those situations, the following argument details might be useful to understanding how to run these utilities in stand-alone mode.  The argument breakdown may also be useful in understanding how the Performance Harness arguments are being passed through to configure lower-level entities.

## Performance Test Basic

`PerformanceTestBasic` (file: `performance_test_basic.py`) can be configured using the following command line arguments:

<details>
    <summary>Usage</summary>

  ```
  usage: PerformanceHarnessScenarioRunner.py singleTest
                                  [-h] [-d D]
                                  [--dump-error-details] [-v] [--leave-running]
                                  [--unshared]
                                  [--endpoint-mode {p2p,http}]
                                  [--producer-nodes PRODUCER_NODES]
                                  [--validation-nodes VALIDATION_NODES]
                                  [--api-nodes API_NODES]
                                  [--api-nodes-read-only-threads API_NODES_READ_ONLY_THREADS]
                                  [--tps-limit-per-generator TPS_LIMIT_PER_GENERATOR]
                                  [--genesis GENESIS]
                                  [--num-blocks-to-prune NUM_BLOCKS_TO_PRUNE]
                                  [--signature-cpu-billable-pct SIGNATURE_CPU_BILLABLE_PCT]
                                  [--chain-threads CHAIN_THREADS]
                                  [--database-map-mode {mapped,heap,locked}]
                                  [--cluster-log-lvl {all,debug,info,warn,error,off}]
                                  [--net-threads NET_THREADS]
                                  [--disable-subjective-billing DISABLE_SUBJECTIVE_BILLING]
                                  [--produce-block-offset-ms PRODUCE_BLOCK_OFFSET_MS]
                                  [--producer-threads PRODUCER_THREADS]
                                  [--http-max-in-flight-requests HTTP_MAX_IN_FLIGHT_REQUESTS]
                                  [--http-max-response-time-ms HTTP_MAX_RESPONSE_TIME_MS]
                                  [--http-max-bytes-in-flight-mb HTTP_MAX_BYTES_IN_FLIGHT_MB]
                                  [--del-perf-logs] [--del-report] [--save-state] [--quiet]
                                  [--prods-enable-trace-api]
                                  [--print-missing-transactions]
                                  [--account-name ACCOUNT_NAME]
                                  [--contract-dir CONTRACT_DIR]
                                  [--wasm-file WASM_FILE] [--abi-file ABI_FILE]
                                  [--user-trx-data-file USER_TRX_DATA_FILE]
                                  [--wasm-runtime {sys-vm-jit,sys-vm}]
                                  [--contracts-console]
                                  [--sys-vm-oc-cache-size-mb SYS_VM_OC_CACHE_SIZE_MB]
                                  [--sys-vm-oc-compile-threads SYS_VM_OC_COMPILE_THREADS]
                                  [--non-prods-sys-vm-oc-enable]
                                  [--block-log-retain-blocks BLOCK_LOG_RETAIN_BLOCKS]
                                  [--http-threads HTTP_THREADS]
                                  [--chain-state-db-size-mb CHAIN_STATE_DB_SIZE_MB]
                                  [--target-tps TARGET_TPS]
                                  [--test-duration-sec TEST_DURATION_SEC]
  ```

</details>

<details>
    <summary>Expand Argument List</summary>

```
optional arguments:
  -h, --help            show this help message and exit

Test Helper Arguments:
  Test Helper configuration items used to configure and spin up the regression test framework and blockchain environment.

  -d D                  delay between nodes startup (default: 1)
  --dump-error-details  Upon error print etc/sysio/node_*/config.ini and <test_name><pid>/node_*/stderr.log to stdout (default: False)
  -v                    verbose logging (default: False)
  --leave-running       Leave cluster running after test finishes (default: False)
  --unshared            Run test in isolated network namespace (default: False)

Performance Test Basic Base:
  Performance Test Basic base configuration items.

  --endpoint-mode {p2p,http}
                        Endpoint Mode ("p2p", "http"). In "p2p" mode transactions will be directed to the p2p endpoint on a producer node. In "http" mode transactions will be directed to the http endpoint on an api node.
                        (default: p2p)
  --producer-nodes PRODUCER_NODES
                        Producing nodes count (default: 1)
  --validation-nodes VALIDATION_NODES
                        Validation nodes count (default: 1)
  --api-nodes API_NODES
                        API nodes count (default: 0)
  --api-nodes-read-only-threads API_NODES_READ_ONLY_THREADS
                        API nodes read only threads count for use with read-only transactions (default: 0)
  --tps-limit-per-generator TPS_LIMIT_PER_GENERATOR
                        Maximum amount of transactions per second a single generator can have. (default: 4000)
  --genesis GENESIS     Path to genesis.json (default: tests/PerformanceHarness/genesis.json)
  --num-blocks-to-prune NUM_BLOCKS_TO_PRUNE
                        The number of potentially non-empty blocks, in addition to leading and trailing size 0 blocks,
                        to prune from the beginning and end of the range of blocks of interest for evaluation. (default: 2)
  --signature-cpu-billable-pct SIGNATURE_CPU_BILLABLE_PCT
                        Percentage of actual signature recovery cpu to bill. Whole number percentages, e.g. 50 for 50% (default: 0)
  --chain-threads CHAIN_THREADS
                        Number of worker threads in controller thread pool (default: 2)
  --database-map-mode {mapped,heap,locked}
                        Database map mode ("mapped", "heap", or "locked").
                        In "mapped" mode database is memory mapped as a file.
                        In "heap" mode database is preloaded in to swappable memory and will use huge pages if available.
                        In "locked" mode database is preloaded, locked in to memory, and will use huge pages if available. (default: mapped)
  --cluster-log-lvl {all,debug,info,warn,error,off}
                        Cluster log level ("all", "debug", "info", "warn", "error", or "off").
                        Performance Harness Test Basic relies on some logging at "info" level, so it is recommended lowest logging level to use.
                        However, there are instances where more verbose logging can be useful. (default: info)
  --net-threads NET_THREADS
                        Number of worker threads in net_plugin thread pool (default: 4)
  --disable-subjective-billing DISABLE_SUBJECTIVE_BILLING
                        Disable subjective CPU billing for API/P2P transactions (default: True)
  --produce-block-offset-ms PRODUCE_BLOCK_OFFSET_MS
                        The number of milliseconds early the last block of a production round should
                        be produced.
  --producer-threads PRODUCER_THREADS
                        Number of worker threads in producer thread pool (default: 2)
  --http-max-in-flight-requests HTTP_MAX_IN_FLIGHT_REQUESTS
                        Maximum number of requests http_plugin should use for processing http requests. 429 error response when exceeded. -1 for unlimited (default: -1)
  --http-max-response-time-ms HTTP_MAX_RESPONSE_TIME_MS
                        Maximum time for processing a request, -1 for unlimited (default: -1)
  --http-max-bytes-in-flight-mb HTTP_MAX_BYTES_IN_FLIGHT_MB
                        Maximum size in megabytes http_plugin should use for processing http requests. -1 for unlimited. 429 error response when exceeded. (default: -1)
  --del-perf-logs       Whether to delete performance test specific logs. (default: False)
  --del-report          Whether to delete overarching performance run report. (default: False)
  --save-state          Whether to save node state. (Warning: large disk usage)
  --quiet               Whether to quiet printing intermediate results and reports to stdout (default: False)
  --prods-enable-trace-api
                        Determines whether producer nodes should have sysio::trace_api_plugin enabled (default: False)
  --print-missing-transactions
                        Toggles if missing transactions are be printed upon test completion. (default: False)
  --account-name ACCOUNT_NAME
                        Name of the account to create and assign a contract to (default: sysio)
  --contract-dir CONTRACT_DIR
                        Path to contract dir (default: unittests/contracts/sysio.system)
  --wasm-file WASM_FILE
                        WASM file name for contract (default: sysio.system.wasm)
  --abi-file ABI_FILE   ABI file name for contract (default: sysio.system.abi)
  --user-trx-data-file USER_TRX_DATA_FILE
                        Path to transaction data JSON file (default: None)
  --wasm-runtime {sys-vm-jit,sys-vm}
                        Override default WASM runtime ("sys-vm-jit", "sys-vm")
                        "sys-vm-jit" : A WebAssembly runtime that compiles WebAssembly code to native x86 code prior to execution.
                        "sys-vm" : A WebAssembly interpreter. (default: sys-vm-jit)
  --contracts-console   print contract's output to console (default: False)
  --sys-vm-oc-cache-size-mb SYS_VM_OC_CACHE_SIZE_MB
                        Maximum size (in MiB) of the SYS VM OC code cache (default: 1024)
  --sys-vm-oc-compile-threads SYS_VM_OC_COMPILE_THREADS
                        Number of threads to use for SYS VM OC tier-up (default: 1)
  --non-prods-sys-vm-oc-enable
                        Enable SYS VM OC tier-up runtime on non producer nodes (default: False)
  --block-log-retain-blocks BLOCK_LOG_RETAIN_BLOCKS
                        If set to greater than 0, periodically prune the block log to store only configured number of most recent blocks.
                        If set to 0, no blocks are be written to the block log; block log file is removed after startup. (default: None)
  --http-threads HTTP_THREADS
                        Number of worker threads in http thread pool (default: 2)
  --chain-state-db-size-mb CHAIN_STATE_DB_SIZE_MB
                        Maximum size (in MiB) of the chain state database (default: 25600)

Performance Test Basic Single Test:
  Performance Test Basic single test configuration items. Useful for running a single test directly.
  These items may not be directly configurable from higher level scripts as the scripts themselves may configure these internally.

  --target-tps TARGET_TPS
                        The target transfers per second to send during test (default: 8000)
  --test-duration-sec TEST_DURATION_SEC
                        The duration of transfer trx generation for the test in seconds (default: 90)
```

</details>

## Transaction Generator
`./build/tests/trx_generator/trx_generator` can be configured using the following command line arguments:

<details>
    <summary>Expand Argument List</summary>

```
Transaction Generator command line options.:
  --generator-id arg (=0)               Id for the transaction generator.
                                        Allowed range (0-960). Defaults to 0.
  --chain-id arg                        set the chain id
  --contract-owner-account arg          Account name of the contract account
                                        for the transaction actions
  --accounts arg                        comma-separated list of accounts that
                                        will be used for transfers. Minimum
                                        required accounts: 2.
  --priv-keys arg                       comma-separated list of private keys in
                                        same order of accounts list that will
                                        be used to sign transactions. Minimum
                                        required: 2.
  --trx-expiration arg (=3600)          transaction expiration time in seconds.
                                        Defaults to 3,600. Maximum allowed:
                                        3,600
  --trx-gen-duration arg (=60)          Transaction generation duration
                                        (seconds). Defaults to 60 seconds.
  --target-tps arg (=1)                 Target transactions per second to
                                        generate/send. Defaults to 1
                                        transaction per second.
  --last-irreversible-block-id arg      Current last-irreversible-block-id (LIB
                                        ID) to use for transactions.
  --monitor-spinup-time-us arg (=1000000)
                                        Number of microseconds to wait before
                                        monitoring TPS. Defaults to 1000000
                                        (1s).
  --monitor-max-lag-percent arg (=5)    Max percentage off from expected
                                        transactions sent before being in
                                        violation. Defaults to 5.
  --monitor-max-lag-duration-us arg (=1000000)
                                        Max microseconds that transaction
                                        generation can be in violation before
                                        quitting. Defaults to 1000000 (1s).
  --log-dir arg                         set the logs directory
  --stop-on-trx-failed arg (=1)         stop transaction generation if sending
                                        fails.
  --abi-file arg                        The path to the contract abi file to
                                        use for the supplied transaction action
                                        data
  --actions-data arg                    The json actions data file or json
                                        actions data description string to use
  --actions-auths arg                   The json actions auth file or json
                                        actions auths description string to
                                        use, containting authAcctName to
                                        activePrivateKey pairs.
  --api-endpoint arg                    The api endpoint to direct transactions to.
                                        Defaults to: '/v1/chain/send_transaction2'
  --peer-endpoint-type arg (=p2p)       Identify the peer endpoint api type to
                                        determine how to send transactions.
                                        Allowable 'p2p' and 'http'. Default:
                                        'p2p'
  --peer-endpoint arg (=127.0.0.1)      set the peer endpoint to send
                                        transactions to
  --port arg (=9876)                    set the peer endpoint port to send
                                        transactions to
  -h [ --help ]                         print this list
```

</details>

# Result Reports

## Performance Test Report

The Performance Harness Scenario Runner, through the `PerformanceTest` and `PerformanceTestBasic` classes in the `PerformanceHarness` module, generates a report to summarize results of test scenarios as well as overarching results of the performance harness run.  By default the report described below will be written to the top level timestamped directory for the performance run with the file name `report.json`. To omit final report, use `--del-report`.

Command used to run test and generate report:

``` bash
./tests/PerformanceHarnessScenarioRunner.py findMax testBpOpMode --test-iteration-duration-sec 10 --final-iterations-duration-sec 30 --calc-producer-threads lmax --calc-chain-threads lmax --calc-net-threads lmax
```

### Report Breakdown
The report begins by delivering the max TPS results of the performance run.

* `InitialMaxTpsAchieved` - the max TPS throughput achieved during initial, short duration test scenarios to narrow search window
* `LongRunningMaxTpsAchieved` - the max TPS throughput achieved during final, longer duration test scenarios to zero in on sustainable max TPS

Next, a high level summary of the search scenario target and results is included.  Each line item shows a target tps search scenario and whether that scenario passed or failed.
<details>
    <summary>Expand Search Scenario Results Summary Example</summary>

``` json
  "InitialSearchScenariosSummary": {
    "50000": "FAIL",
    "25001": "FAIL",
    "12501": "PASS",
    "19001": "FAIL",
    "16001": "FAIL",
    "14501": "FAIL",
    "13501": "PASS",
    "14001": "PASS"
  },
  "LongRunningSearchScenariosSummary": {
    "14001": "PASS"
  },
```
</details>

Next, a summary of the search scenario conducted and respective results is included.  Each summary includes information on the current state of the overarching search as well as basic results of the individual test that are used to determine whether the basic test was considered successful. The list of summary results are included in `InitialSearchResults` and `LongRunningSearchResults`. The number of entries in each list will vary depending on the TPS range tested (`--min-tps-to-test` & `--max-tps-to-test`) and the configured `--test-iteration-min-step`.
<details>
    <summary>Expand Search Scenario Summary Example</summary>

``` json
    "0": {
      "success": true,
      "searchTarget": 14001,
      "searchFloor": 1,
      "searchCeiling": 14001,
      "basicTestResult": {
        "testStart": "2023-08-18T17:49:42.016053",
        "testEnd": "2023-08-18T17:50:56.550087",
        "testDuration": "0:01:14.534034",
        "testPassed": true,
        "testRunSuccessful": true,
        "testRunCompleted": true,
        "tpsExpectMet": true,
        "trxExpectMet": true,
        "targetTPS": 14001,
        "resultAvgTps": 14060.375,
        "expectedTxns": 140010,
        "resultTxns": 140010,
        "testAnalysisBlockCnt": 17,
        "logsDir": "PHSRLogs/2023-08-18_16-16-57/testRunLogs/PHSRunLogs/2023-08-18_17-49-42-14001"
      }
    }
```
</details>

Finally, the full detail test report for each of the determined max TPS throughput (`InitialMaxTpsAchieved` and `LongRunningMaxTpsAchieved`) runs is included after each scenario summary list in the full report.  **Note:** In the example full report below, these have been truncated as they are single performance test basic run reports as detailed in the following section [Performance Test Basic Report](#performance-test-basic).  Herein these truncated reports appear as:

<details>
    <summary>Expand Truncated Report Example</summary>

``` json
"InitialMaxTpsReport": {
    <truncated>
    "Result": {
      <truncated>
    },
    "Analysis": {
      <truncated>
    },
    "args": {
      <truncated>
    },
    "env": {
      <truncated>
    },
    <truncated>
}
```
</details>

<details>
    <summary>Expand for full sample Performance Test Report</summary>

``` json
{
  "perfTestsBegin": "2023-08-18T16:16:57.515935",
  "perfTestsFinish": "2023-08-18T17:50:56.573105",
  "perfTestsDuration": "1:33:59.057170",
  "operationalMode": "Block Producer Operational Mode",
  "InitialMaxTpsAchieved": 14001,
  "LongRunningMaxTpsAchieved": 14001,
  "tpsTestStart": "2023-08-18T17:39:08.002919",
  "tpsTestFinish": "2023-08-18T17:50:56.573095",
  "tpsTestDuration": "0:11:48.570176",
  "InitialSearchScenariosSummary": {
    "50000": "FAIL",
    "25001": "FAIL",
    "12501": "PASS",
    "19001": "FAIL",
    "16001": "FAIL",
    "14501": "FAIL",
    "13501": "PASS",
    "14001": "PASS"
  },
  "LongRunningSearchScenariosSummary": {
    "14001": "PASS"
  },
  "InitialSearchResults": {
    "0": {
      "success": false,
      "searchTarget": 50000,
      "searchFloor": 1,
      "searchCeiling": 50000,
      "basicTestResult": {
        "testStart": "2023-08-18T17:39:08.002959",
        "testEnd": "2023-08-18T17:40:45.539974",
        "testDuration": "0:01:37.537015",
        "testPassed": false,
        "testRunSuccessful": false,
        "testRunCompleted": true,
        "tpsExpectMet": false,
        "trxExpectMet": false,
        "targetTPS": 50000,
        "resultAvgTps": 13264.95,
        "expectedTxns": 500000,
        "resultTxns": 295339,
        "testAnalysisBlockCnt": 41,
        "logsDir": "PHSRLogs/2023-08-18_16-16-57/testRunLogs/PHSRunLogs/2023-08-18_17-39-08-50000"
      }
    },
    "1": {
      "success": false,
      "searchTarget": 25001,
      "searchFloor": 1,
      "searchCeiling": 49500,
      "basicTestResult": {
        "testStart": "2023-08-18T17:40:45.541240",
        "testEnd": "2023-08-18T17:42:10.566883",
        "testDuration": "0:01:25.025643",
        "testPassed": false,
        "testRunSuccessful": false,
        "testRunCompleted": true,
        "tpsExpectMet": false,
        "trxExpectMet": false,
        "targetTPS": 25001,
        "resultAvgTps": 13415.515151515152,
        "expectedTxns": 250010,
        "resultTxns": 249933,
        "testAnalysisBlockCnt": 34,
        "logsDir": "PHSRLogs/2023-08-18_16-16-57/testRunLogs/PHSRunLogs/2023-08-18_17-40-45-25001"
      }
    },
    "2": {
      "success": true,
      "searchTarget": 12501,
      "searchFloor": 1,
      "searchCeiling": 24501,
      "basicTestResult": {
        "testStart": "2023-08-18T17:42:10.568046",
        "testEnd": "2023-08-18T17:43:23.733271",
        "testDuration": "0:01:13.165225",
        "testPassed": true,
        "testRunSuccessful": true,
        "testRunCompleted": true,
        "tpsExpectMet": true,
        "trxExpectMet": true,
        "targetTPS": 12501,
        "resultAvgTps": 12509.9375,
        "expectedTxns": 125010,
        "resultTxns": 125010,
        "testAnalysisBlockCnt": 17,
        "logsDir": "PHSRLogs/2023-08-18_16-16-57/testRunLogs/PHSRunLogs/2023-08-18_17-42-10-12501"
      }
    },
    "3": {
      "success": false,
      "searchTarget": 19001,
      "searchFloor": 13001,
      "searchCeiling": 24501,
      "basicTestResult": {
        "testStart": "2023-08-18T17:43:23.734927",
        "testEnd": "2023-08-18T17:44:44.562268",
        "testDuration": "0:01:20.827341",
        "testPassed": false,
        "testRunSuccessful": true,
        "testRunCompleted": true,
        "tpsExpectMet": false,
        "trxExpectMet": true,
        "targetTPS": 19001,
        "resultAvgTps": 14669.863636363636,
        "expectedTxns": 190010,
        "resultTxns": 190010,
        "testAnalysisBlockCnt": 23,
        "logsDir": "PHSRLogs/2023-08-18_16-16-57/testRunLogs/PHSRunLogs/2023-08-18_17-43-23-19001"
      }
    },
    "4": {
      "success": false,
      "searchTarget": 16001,
      "searchFloor": 13001,
      "searchCeiling": 18501,
      "basicTestResult": {
        "testStart": "2023-08-18T17:44:44.563387",
        "testEnd": "2023-08-18T17:46:01.838736",
        "testDuration": "0:01:17.275349",
        "testPassed": false,
        "testRunSuccessful": true,
        "testRunCompleted": true,
        "tpsExpectMet": false,
        "trxExpectMet": true,
        "targetTPS": 16001,
        "resultAvgTps": 14538.444444444445,
        "expectedTxns": 160010,
        "resultTxns": 160010,
        "testAnalysisBlockCnt": 19,
        "logsDir": "PHSRLogs/2023-08-18_16-16-57/testRunLogs/PHSRunLogs/2023-08-18_17-44-44-16001"
      }
    },
    "5": {
      "success": false,
      "searchTarget": 14501,
      "searchFloor": 13001,
      "searchCeiling": 15501,
      "basicTestResult": {
        "testStart": "2023-08-18T17:46:01.839865",
        "testEnd": "2023-08-18T17:47:15.595123",
        "testDuration": "0:01:13.755258",
        "testPassed": false,
        "testRunSuccessful": false,
        "testRunCompleted": true,
        "tpsExpectMet": true,
        "trxExpectMet": false,
        "targetTPS": 14501,
        "resultAvgTps": 14433.25,
        "expectedTxns": 145010,
        "resultTxns": 144898,
        "testAnalysisBlockCnt": 17,
        "logsDir": "PHSRLogs/2023-08-18_16-16-57/testRunLogs/PHSRunLogs/2023-08-18_17-46-01-14501"
      }
    },
    "6": {
      "success": true,
      "searchTarget": 13501,
      "searchFloor": 13001,
      "searchCeiling": 14001,
      "basicTestResult": {
        "testStart": "2023-08-18T17:47:15.596266",
        "testEnd": "2023-08-18T17:48:29.481603",
        "testDuration": "0:01:13.885337",
        "testPassed": true,
        "testRunSuccessful": true,
        "testRunCompleted": true,
        "tpsExpectMet": true,
        "trxExpectMet": true,
        "targetTPS": 13501,
        "resultAvgTps": 13542.625,
        "expectedTxns": 135010,
        "resultTxns": 135010,
        "testAnalysisBlockCnt": 17,
        "logsDir": "PHSRLogs/2023-08-18_16-16-57/testRunLogs/PHSRunLogs/2023-08-18_17-47-15-13501"
      }
    },
    "7": {
      "success": true,
      "searchTarget": 14001,
      "searchFloor": 14001,
      "searchCeiling": 14001,
      "basicTestResult": {
        "testStart": "2023-08-18T17:48:29.482846",
        "testEnd": "2023-08-18T17:49:41.993743",
        "testDuration": "0:01:12.510897",
        "testPassed": true,
        "testRunSuccessful": true,
        "testRunCompleted": true,
        "tpsExpectMet": true,
        "trxExpectMet": true,
        "targetTPS": 14001,
        "resultAvgTps": 14035.8125,
        "expectedTxns": 140010,
        "resultTxns": 140010,
        "testAnalysisBlockCnt": 17,
        "logsDir": "PHSRLogs/2023-08-18_16-16-57/testRunLogs/PHSRunLogs/2023-08-18_17-48-29-14001"
      }
    }
  },
  "InitialMaxTpsReport": {
    <truncated>
    "Result": {
      <truncated>
    },
    "Analysis": {
      <truncated>
    },
    "args": {
      <truncated>
    },
    "env": {
      <truncated>
    },
    <truncated>
  },
  "LongRunningSearchResults": {
    "0": {
      "success": true,
      "searchTarget": 14001,
      "searchFloor": 1,
      "searchCeiling": 14001,
      "basicTestResult": {
        "testStart": "2023-08-18T17:49:42.016053",
        "testEnd": "2023-08-18T17:50:56.550087",
        "testDuration": "0:01:14.534034",
        "testPassed": true,
        "testRunSuccessful": true,
        "testRunCompleted": true,
        "tpsExpectMet": true,
        "trxExpectMet": true,
        "targetTPS": 14001,
        "resultAvgTps": 14060.375,
        "expectedTxns": 140010,
        "resultTxns": 140010,
        "testAnalysisBlockCnt": 17,
        "logsDir": "PHSRLogs/2023-08-18_16-16-57/testRunLogs/PHSRunLogs/2023-08-18_17-49-42-14001"
      }
    }
  },
  "LongRunningMaxTpsReport": {
    <truncated>
    "Result": {
      <truncated>
    },
    "Analysis": {
      <truncated>
    },
    "args": {
      <truncated>
    },
    "env": {
      <truncated>
    },
    <truncated>
  },
  "ProducerThreadAnalysis": {
    "recommendedThreadCount": 3,
    "threadToMaxTpsDict": {
      "2": 12001,
      "3": 16001,
      "4": 14001
    },
    "analysisStart": "2023-08-18T16:16:57.535103",
    "analysisFinish": "2023-08-18T16:46:36.202669"
  },
  "ChainThreadAnalysis": {
    "recommendedThreadCount": 2,
    "threadToMaxTpsDict": {
      "2": 14001,
      "3": 13001
    },
    "analysisStart": "2023-08-18T16:46:36.203279",
    "analysisFinish": "2023-08-18T17:07:30.813917"
  },
  "NetThreadAnalysis": {
    "recommendedThreadCount": 5,
    "threadToMaxTpsDict": {
      "4": 12501,
      "5": 13001,
      "6": 11001
    },
    "analysisStart": "2023-08-18T17:07:30.814441",
    "analysisFinish": "2023-08-18T17:39:08.002767"
  },
  "args": {
    "rawCmdLine ": "tests/PerformanceHarnessScenarioRunner.py findMax testBpOpMode --test-iteration-duration-sec 10 --final-iterations-duration-sec 30 --calc-producer-threads lmax --calc-chain-threads lmax --calc-net-threads lmax",
    "dumpErrorDetails": false,
    "delay": 1,
    "nodesFile": null,
    "verbose": false,
    "unshared": false,
    "producerNodeCount": 1,
    "validationNodeCount": 1,
    "apiNodeCount": 0,
    "dontKill": false,
    "extraNodeopArgs": {
      "chainPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "chain_plugin",
        "blocksDir": null,
        "_blocksDirNodeopDefault": "\"blocks\"",
        "_blocksDirNodeopArg": "--blocks-dir",
        "blocksLogStride": null,
        "_blocksLogStrideNodeopDefault": null,
        "_blocksLogStrideNodeopArg": "--blocks-log-stride",
        "maxRetainedBlockFiles": null,
        "_maxRetainedBlockFilesNodeopDefault": null,
        "_maxRetainedBlockFilesNodeopArg": "--max-retained-block-files",
        "blocksRetainedDir": null,
        "_blocksRetainedDirNodeopDefault": null,
        "_blocksRetainedDirNodeopArg": "--blocks-retained-dir",
        "blocksArchiveDir": null,
        "_blocksArchiveDirNodeopDefault": null,
        "_blocksArchiveDirNodeopArg": "--blocks-archive-dir",
        "stateDir": null,
        "_stateDirNodeopDefault": "\"state\"",
        "_stateDirNodeopArg": "--state-dir",
        "protocolFeaturesDir": null,
        "_protocolFeaturesDirNodeopDefault": "\"protocol_features\"",
        "_protocolFeaturesDirNodeopArg": "--protocol-features-dir",
        "checkpoint": null,
        "_checkpointNodeopDefault": null,
        "_checkpointNodeopArg": "--checkpoint",
        "wasmRuntime": "sys-vm-jit",
        "_wasmRuntimeNodeopDefault": "sys-vm-jit",
        "_wasmRuntimeNodeopArg": "--wasm-runtime",
        "profileAccount": null,
        "_profileAccountNodeopDefault": null,
        "_profileAccountNodeopArg": "--profile-account",
        "abiSerializerMaxTimeMs": 990000,
        "_abiSerializerMaxTimeMsNodeopDefault": 15,
        "_abiSerializerMaxTimeMsNodeopArg": "--abi-serializer-max-time-ms",
        "chainStateDbSizeMb": 25600,
        "_chainStateDbSizeMbNodeopDefault": 1024,
        "_chainStateDbSizeMbNodeopArg": "--chain-state-db-size-mb",
        "chainStateDbGuardSizeMb": null,
        "_chainStateDbGuardSizeMbNodeopDefault": 128,
        "_chainStateDbGuardSizeMbNodeopArg": "--chain-state-db-guard-size-mb",
        "signatureCpuBillablePct": 0,
        "_signatureCpuBillablePctNodeopDefault": 50,
        "_signatureCpuBillablePctNodeopArg": "--signature-cpu-billable-pct",
        "chainThreads": 2,
        "_chainThreadsNodeopDefault": 2,
        "_chainThreadsNodeopArg": "--chain-threads",
        "contractsConsole": false,
        "_contractsConsoleNodeopDefault": false,
        "_contractsConsoleNodeopArg": "--contracts-console",
        "deepMind": null,
        "_deepMindNodeopDefault": false,
        "_deepMindNodeopArg": "--deep-mind",
        "actorWhitelist": null,
        "_actorWhitelistNodeopDefault": null,
        "_actorWhitelistNodeopArg": "--actor-whitelist",
        "actorBlacklist": null,
        "_actorBlacklistNodeopDefault": null,
        "_actorBlacklistNodeopArg": "--actor-blacklist",
        "contractWhitelist": null,
        "_contractWhitelistNodeopDefault": null,
        "_contractWhitelistNodeopArg": "--contract-whitelist",
        "contractBlacklist": null,
        "_contractBlacklistNodeopDefault": null,
        "_contractBlacklistNodeopArg": "--contract-blacklist",
        "actionBlacklist": null,
        "_actionBlacklistNodeopDefault": null,
        "_actionBlacklistNodeopArg": "--action-blacklist",
        "keyBlacklist": null,
        "_keyBlacklistNodeopDefault": null,
        "_keyBlacklistNodeopArg": "--key-blacklist",
        "senderBypassWhiteblacklist": null,
        "_senderBypassWhiteblacklistNodeopDefault": null,
        "_senderBypassWhiteblacklistNodeopArg": "--sender-bypass-whiteblacklist",
        "readMode": null,
        "_readModeNodeopDefault": "head",
        "_readModeNodeopArg": "--read-mode",
        "apiAcceptTransactions": null,
        "_apiAcceptTransactionsNodeopDefault": 1,
        "_apiAcceptTransactionsNodeopArg": "--api-accept-transactions",
        "validationMode": null,
        "_validationModeNodeopDefault": "full",
        "_validationModeNodeopArg": "--validation-mode",
        "disableRamBillingNotifyChecks": null,
        "_disableRamBillingNotifyChecksNodeopDefault": false,
        "_disableRamBillingNotifyChecksNodeopArg": "--disable-ram-billing-notify-checks",
        "maximumVariableSignatureLength": null,
        "_maximumVariableSignatureLengthNodeopDefault": 16384,
        "_maximumVariableSignatureLengthNodeopArg": "--maximum-variable-signature-length",
        "trustedProducer": null,
        "_trustedProducerNodeopDefault": null,
        "_trustedProducerNodeopArg": "--trusted-producer",
        "databaseMapMode": "mapped",
        "_databaseMapModeNodeopDefault": "mapped",
        "_databaseMapModeNodeopArg": "--database-map-mode",
        "sysVmOcCacheSizeMb": 1024,
        "_sysVmOcCacheSizeMbNodeopDefault": 1024,
        "_sysVmOcCacheSizeMbNodeopArg": "--sys-vm-oc-cache-size-mb",
        "sysVmOcCompileThreads": 1,
        "_sysVmOcCompileThreadsNodeopDefault": 1,
        "_sysVmOcCompileThreadsNodeopArg": "--sys-vm-oc-compile-threads",
        "sysVmOcEnable": null,
        "_sysVmOcEnableNodeopDefault": "auto",
        "_sysVmOcEnableNodeopArg": "--sys-vm-oc-enable",
        "enableAccountQueries": null,
        "_enableAccountQueriesNodeopDefault": 0,
        "_enableAccountQueriesNodeopArg": "--enable-account-queries",
        "maxNonprivilegedInlineActionSize": null,
        "transactionRetryMaxStorageSizeGb": null,
        "_transactionRetryMaxStorageSizeGbNodeopDefault": null,
        "_transactionRetryMaxStorageSizeGbNodeopArg": "--transaction-retry-max-storage-size-gb",
        "transactionRetryIntervalSec": null,
        "_transactionRetryIntervalSecNodeopDefault": 20,
        "_transactionRetryIntervalSecNodeopArg": "--transaction-retry-interval-sec",
        "transactionRetryMaxExpirationSec": null,
        "_transactionRetryMaxExpirationSecNodeopDefault": 120,
        "_transactionRetryMaxExpirationSecNodeopArg": "--transaction-retry-max-expiration-sec",
        "transactionFinalityStatusMaxStorageSizeGb": null,
        "_transactionFinalityStatusMaxStorageSizeGbNodeopDefault": null,
        "_transactionFinalityStatusMaxStorageSizeGbNodeopArg": "--transaction-finality-status-max-storage-size-gb",
        "transactionFinalityStatusSuccessDurationSec": null,
        "_transactionFinalityStatusSuccessDurationSecNodeopDefault": 180,
        "_transactionFinalityStatusSuccessDurationSecNodeopArg": "--transaction-finality-status-success-duration-sec",
        "transactionFinalityStatusFailureDurationSec": null,
        "_transactionFinalityStatusFailureDurationSecNodeopDefault": 180,
        "_transactionFinalityStatusFailureDurationSecNodeopArg": "--transaction-finality-status-failure-duration-sec",
        "disableReplayOpts": null,
        "_disableReplayOptsNodeopDefault": false,
        "_disableReplayOptsNodeopArg": "--disable-replay-opts",
        "integrityHashOnStart": null,
        "_integrityHashOnStartNodeopDefault": false,
        "_integrityHashOnStartNodeopArg": "--integrity-hash-on-start",
        "integrityHashOnStop": null,
        "_integrityHashOnStopNodeopDefault": false,
        "_integrityHashOnStopNodeopArg": "--integrity-hash-on-stop",
        "blockLogRetainBlocks": null,
        "_blockLogRetainBlocksNodeopDefault": null,
        "_blockLogRetainBlocksNodeopArg": "--block-log-retain-blocks",
        "genesisJson": null,
        "_genesisJsonNodeopDefault": null,
        "_genesisJsonNodeopArg": "--genesis-json",
        "genesisTimestamp": null,
        "_genesisTimestampNodeopDefault": null,
        "_genesisTimestampNodeopArg": "--genesis-timestamp",
        "printGenesisJson": null,
        "_printGenesisJsonNodeopDefault": false,
        "_printGenesisJsonNodeopArg": "--print-genesis-json",
        "extractGenesisJson": null,
        "_extractGenesisJsonNodeopDefault": null,
        "_extractGenesisJsonNodeopArg": "--extract-genesis-json",
        "printBuildInfo": null,
        "_printBuildInfoNodeopDefault": false,
        "_printBuildInfoNodeopArg": "--print-build-info",
        "extractBuildInfo": null,
        "_extractBuildInfoNodeopDefault": null,
        "_extractBuildInfoNodeopArg": "--extract-build-info",
        "forceAllChecks": null,
        "_forceAllChecksNodeopDefault": false,
        "_forceAllChecksNodeopArg": "--force-all-checks",
        "replayBlockchain": null,
        "_replayBlockchainNodeopDefault": false,
        "_replayBlockchainNodeopArg": "--replay-blockchain",
        "hardReplayBlockchain": null,
        "_hardReplayBlockchainNodeopDefault": false,
        "_hardReplayBlockchainNodeopArg": "--hard-replay-blockchain",
        "deleteAllBlocks": null,
        "_deleteAllBlocksNodeopDefault": false,
        "_deleteAllBlocksNodeopArg": "--delete-all-blocks",
        "truncateAtBlock": null,
        "_truncateAtBlockNodeopDefault": 0,
        "_truncateAtBlockNodeopArg": "--truncate-at-block",
        "terminateAtBlock": null,
        "_terminateAtBlockNodeopDefault": 0,
        "_terminateAtBlockNodeopArg": "--terminate-at-block",
        "snapshot": null,
        "_snapshotNodeopDefault": null,
        "_snapshotNodeopArg": "--snapshot"
      },
      "httpPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "http_plugin",
        "unixSocketPath": null,
        "_unixSocketPathNodeopDefault": null,
        "_unixSocketPathNodeopArg": "--unix-socket-path",
        "httpServerAddress": null,
        "_httpServerAddressNodeopDefault": "127.0.0.1:8888",
        "_httpServerAddressNodeopArg": "--http-server-address",
        "httpCategoryAddress": null,
        "_httpCategoryAddressNodeopDefault": null,
        "_httpCategoryAddressNodeopArg": "--http-category-address",
        "accessControlAllowOrigin": null,
        "_accessControlAllowOriginNodeopDefault": null,
        "_accessControlAllowOriginNodeopArg": "--access-control-allow-origin",
        "accessControlAllowHeaders": null,
        "_accessControlAllowHeadersNodeopDefault": null,
        "_accessControlAllowHeadersNodeopArg": "--access-control-allow-headers",
        "accessControlMaxAge": null,
        "_accessControlMaxAgeNodeopDefault": null,
        "_accessControlMaxAgeNodeopArg": "--access-control-max-age",
        "accessControlAllowCredentials": null,
        "_accessControlAllowCredentialsNodeopDefault": false,
        "_accessControlAllowCredentialsNodeopArg": "--access-control-allow-credentials",
        "maxBodySize": null,
        "_maxBodySizeNodeopDefault": 2097152,
        "_maxBodySizeNodeopArg": "--max-body-size",
        "httpMaxBytesInFlightMb": -1,
        "_httpMaxBytesInFlightMbNodeopDefault": 500,
        "_httpMaxBytesInFlightMbNodeopArg": "--http-max-bytes-in-flight-mb",
        "httpMaxInFlightRequests": -1,
        "_httpMaxInFlightRequestsNodeopDefault": -1,
        "_httpMaxInFlightRequestsNodeopArg": "--http-max-in-flight-requests",
        "httpMaxResponseTimeMs": -1,
        "_httpMaxResponseTimeMsNodeopDefault": 15,
        "_httpMaxResponseTimeMsNodeopArg": "--http-max-response-time-ms",
        "verboseHttpErrors": null,
        "_verboseHttpErrorsNodeopDefault": false,
        "_verboseHttpErrorsNodeopArg": "--verbose-http-errors",
        "httpValidateHost": null,
        "_httpValidateHostNodeopDefault": 1,
        "_httpValidateHostNodeopArg": "--http-validate-host",
        "httpAlias": null,
        "_httpAliasNodeopDefault": null,
        "_httpAliasNodeopArg": "--http-alias",
        "httpThreads": 2,
        "_httpThreadsNodeopDefault": 2,
        "_httpThreadsNodeopArg": "--http-threads",
        "httpKeepAlive": null,
        "_httpKeepAliveNodeopDefault": 1,
        "_httpKeepAliveNodeopArg": "--http-keep-alive"
      },
      "netPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "net_plugin",
        "p2pListenEndpoint": null,
        "_p2pListenEndpointNodeopDefault": "0.0.0.0:9876",
        "_p2pListenEndpointNodeopArg": "--p2p-listen-endpoint",
        "p2pServerAddress": null,
        "_p2pServerAddressNodeopDefault": null,
        "_p2pServerAddressNodeopArg": "--p2p-server-address",
        "p2pPeerAddress": null,
        "_p2pPeerAddressNodeopDefault": null,
        "_p2pPeerAddressNodeopArg": "--p2p-peer-address",
        "p2pMaxNodesPerHost": null,
        "_p2pMaxNodesPerHostNodeopDefault": 1,
        "_p2pMaxNodesPerHostNodeopArg": "--p2p-max-nodes-per-host",
        "p2pAcceptTransactions": null,
        "_p2pAcceptTransactionsNodeopDefault": 1,
        "_p2pAcceptTransactionsNodeopArg": "--p2p-accept-transactions",
        "p2pAutoBpPeer": null,
        "_p2pAutoBpPeerNodeopDefault": null,
        "_p2pAutoBpPeerNodeopArg": "--p2p-auto-bp-peer",
        "agentName": null,
        "_agentNameNodeopDefault": "SYS Test Agent",
        "_agentNameNodeopArg": "--agent-name",
        "allowedConnection": null,
        "_allowedConnectionNodeopDefault": "any",
        "_allowedConnectionNodeopArg": "--allowed-connection",
        "peerKey": null,
        "_peerKeyNodeopDefault": null,
        "_peerKeyNodeopArg": "--peer-key",
        "peerPrivateKey": null,
        "_peerPrivateKeyNodeopDefault": null,
        "_peerPrivateKeyNodeopArg": "--peer-private-key",
        "maxClients": 0,
        "_maxClientsNodeopDefault": 25,
        "_maxClientsNodeopArg": "--max-clients",
        "connectionCleanupPeriod": null,
        "_connectionCleanupPeriodNodeopDefault": 30,
        "_connectionCleanupPeriodNodeopArg": "--connection-cleanup-period",
        "maxCleanupTimeMsec": null,
        "_maxCleanupTimeMsecNodeopDefault": 10,
        "_maxCleanupTimeMsecNodeopArg": "--max-cleanup-time-msec",
        "p2pDedupCacheExpireTimeSec": null,
        "_p2pDedupCacheExpireTimeSecNodeopDefault": 10,
        "_p2pDedupCacheExpireTimeSecNodeopArg": "--p2p-dedup-cache-expire-time-sec",
        "netThreads": 4,
        "_netThreadsNodeopDefault": 4,
        "_netThreadsNodeopArg": "--net-threads",
        "syncFetchSpan": null,
        "_syncFetchSpanNodeopDefault": 1000,
        "_syncFetchSpanNodeopArg": "--sync-fetch-span",
        "syncPeerLimit": null,
        "_syncPeerLimitNodeopDefault": 3,
        "_syncPeerLimitNodeopArg": "--sync-peer-limit",
        "useSocketReadWatermark": null,
        "_useSocketReadWatermarkNodeopDefault": 0,
        "_useSocketReadWatermarkNodeopArg": "--use-socket-read-watermark",
        "peerLogFormat": null,
        "_peerLogFormatNodeopDefault": "[\"${_name}\" - ${_cid} ${_ip}:${_port}] ",
        "_peerLogFormatNodeopArg": "--peer-log-format",
        "p2pKeepaliveIntervalMs": null,
        "_p2pKeepaliveIntervalMsNodeopDefault": 10000,
        "_p2pKeepaliveIntervalMsNodeopArg": "--p2p-keepalive-interval-ms"
      },
      "producerPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "producer_plugin",
        "enableStaleProduction": null,
        "_enableStaleProductionNodeopDefault": false,
        "_enableStaleProductionNodeopArg": "--enable-stale-production",
        "pauseOnStartup": null,
        "_pauseOnStartupNodeopDefault": false,
        "_pauseOnStartupNodeopArg": "--pause-on-startup",
        "maxTransactionTime": -1,
        "_maxTransactionTimeNodeopDefault": 30,
        "_maxTransactionTimeNodeopArg": "--max-transaction-time",
        "maxIrreversibleBlockAge": null,
        "_maxIrreversibleBlockAgeNodeopDefault": -1,
        "_maxIrreversibleBlockAgeNodeopArg": "--max-irreversible-block-age",
        "producerName": null,
        "_producerNameNodeopDefault": null,
        "_producerNameNodeopArg": "--producer-name",
        "signatureProvider": null,
        "_signatureProviderNodeopDefault": "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV=KEY:5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3",
        "_signatureProviderNodeopArg": "--signature-provider",
        "greylistAccount": null,
        "_greylistAccountNodeopDefault": null,
        "_greylistAccountNodeopArg": "--greylist-account",
        "greylistLimit": null,
        "_greylistLimitNodeopDefault": 1000,
        "_greylistLimitNodeopArg": "--greylist-limit",
        "produceBlockOffsetMs": 0,
        "_produceBlockOffsetMsDefault": 450,
        "_produceBlockOffsetMsArg": "--produce-block-offset-ms",
        "maxBlockCpuUsageThresholdUs": null,
        "_maxBlockCpuUsageThresholdUsNodeopDefault": 5000,
        "_maxBlockCpuUsageThresholdUsNodeopArg": "--max-block-cpu-usage-threshold-us",
        "maxBlockNetUsageThresholdBytes": null,
        "_maxBlockNetUsageThresholdBytesNodeopDefault": 1024,
        "_maxBlockNetUsageThresholdBytesNodeopArg": "--max-block-net-usage-threshold-bytes",
        "subjectiveCpuLeewayUs": null,
        "_subjectiveCpuLeewayUsNodeopDefault": 31000,
        "_subjectiveCpuLeewayUsNodeopArg": "--subjective-cpu-leeway-us",
        "subjectiveAccountMaxFailures": null,
        "_subjectiveAccountMaxFailuresNodeopDefault": 3,
        "_subjectiveAccountMaxFailuresNodeopArg": "--subjective-account-max-failures",
        "subjectiveAccountMaxFailuresWindowSize": null,
        "_subjectiveAccountMaxFailuresWindowSizeNodeopDefault": 1,
        "_subjectiveAccountMaxFailuresWindowSizeNodeopArg": "--subjective-account-max-failures-window-size",
        "subjectiveAccountDecayTimeMinutes": null,
        "_subjectiveAccountDecayTimeMinutesNodeopDefault": 1440,
        "_subjectiveAccountDecayTimeMinutesNodeopArg": "--subjective-account-decay-time-minutes",
        "incomingTransactionQueueSizeMb": null,
        "_incomingTransactionQueueSizeMbNodeopDefault": 1024,
        "_incomingTransactionQueueSizeMbNodeopArg": "--incoming-transaction-queue-size-mb",
        "disableSubjectiveAccountBilling": null,
        "_disableSubjectiveAccountBillingNodeopDefault": false,
        "_disableSubjectiveAccountBillingNodeopArg": "--disable-subjective-account-billing",
        "disableSubjectiveP2pBilling": true,
        "_disableSubjectiveP2pBillingNodeopDefault": 1,
        "_disableSubjectiveP2pBillingNodeopArg": "--disable-subjective-p2p-billing",
        "disableSubjectiveApiBilling": true,
        "_disableSubjectiveApiBillingNodeopDefault": 1,
        "_disableSubjectiveApiBillingNodeopArg": "--disable-subjective-api-billing",
        "producerThreads": 2,
        "_producerThreadsNodeopDefault": 2,
        "_producerThreadsNodeopArg": "--producer-threads",
        "snapshotsDir": null,
        "_snapshotsDirNodeopDefault": "\"snapshots\"",
        "_snapshotsDirNodeopArg": "--snapshots-dir",
        "readOnlyThreads": null,
        "_readOnlyThreadsNodeopDefault": null,
        "_readOnlyThreadsNodeopArg": "--read-only-threads",
        "readOnlyWriteWindowTimeUs": null,
        "_readOnlyWriteWindowTimeUsNodeopDefault": 200000,
        "_readOnlyWriteWindowTimeUsNodeopArg": "--read-only-write-window-time-us",
        "readOnlyReadWindowTimeUs": null,
        "_readOnlyReadWindowTimeUsNodeopDefault": 60000,
        "_readOnlyReadWindowTimeUsNodeopArg": "--read-only-read-window-time-us"
      },
      "resourceMonitorPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "resource_monitor_plugin",
        "resourceMonitorIntervalSeconds": null,
        "_resourceMonitorIntervalSecondsNodeopDefault": 2,
        "_resourceMonitorIntervalSecondsNodeopArg": "--resource-monitor-interval-seconds",
        "resourceMonitorSpaceThreshold": null,
        "_resourceMonitorSpaceThresholdNodeopDefault": 90,
        "_resourceMonitorSpaceThresholdNodeopArg": "--resource-monitor-space-threshold",
        "resourceMonitorSpaceAbsoluteGb": null,
        "_resourceMonitorSpaceAbsoluteGbNodeopDefault": null,
        "_resourceMonitorSpaceAbsoluteGbNodeopArg": "--resource-monitor-space-absolute-gb",
        "resourceMonitorNotShutdownOnThresholdExceeded": true,
        "_resourceMonitorNotShutdownOnThresholdExceededNodeopDefault": false,
        "_resourceMonitorNotShutdownOnThresholdExceededNodeopArg": "--resource-monitor-not-shutdown-on-threshold-exceeded",
        "resourceMonitorWarningInterval": null,
        "_resourceMonitorWarningIntervalNodeopDefault": 30,
        "_resourceMonitorWarningIntervalNodeopArg": "--resource-monitor-warning-interval"
      },
      "signatureProviderPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "signature_provider_plugin",
        "kiodProviderTimeout": null,
        "_kiodProviderTimeoutNodeopDefault": 5,
        "_kiodProviderTimeoutNodeopArg": "--kiod-provider-timeout"
      },
      "stateHistoryPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "state_history_plugin",
        "stateHistoryDir": null,
        "_stateHistoryDirNodeopDefault": "\"state-history\"",
        "_stateHistoryDirNodeopArg": "--state-history-dir",
        "stateHistoryRetainedDir": null,
        "_stateHistoryRetainedDirNodeopDefault": null,
        "_stateHistoryRetainedDirNodeopArg": "--state-history-retained-dir",
        "stateHistoryArchiveDir": null,
        "_stateHistoryArchiveDirNodeopDefault": null,
        "_stateHistoryArchiveDirNodeopArg": "--state-history-archive-dir",
        "stateHistoryStride": null,
        "_stateHistoryStrideNodeopDefault": null,
        "_stateHistoryStrideNodeopArg": "--state-history-stride",
        "maxRetainedHistoryFiles": null,
        "_maxRetainedHistoryFilesNodeopDefault": null,
        "_maxRetainedHistoryFilesNodeopArg": "--max-retained-history-files",
        "traceHistory": null,
        "_traceHistoryNodeopDefault": false,
        "_traceHistoryNodeopArg": "--trace-history",
        "chainStateHistory": null,
        "_chainStateHistoryNodeopDefault": false,
        "_chainStateHistoryNodeopArg": "--chain-state-history",
        "stateHistoryEndpoint": null,
        "_stateHistoryEndpointNodeopDefault": "127.0.0.1:8080",
        "_stateHistoryEndpointNodeopArg": "--state-history-endpoint",
        "stateHistoryUnixSocketPath": null,
        "_stateHistoryUnixSocketPathNodeopDefault": null,
        "_stateHistoryUnixSocketPathNodeopArg": "--state-history-unix-socket-path",
        "traceHistoryDebugMode": null,
        "_traceHistoryDebugModeNodeopDefault": false,
        "_traceHistoryDebugModeNodeopArg": "--trace-history-debug-mode",
        "stateHistoryLogRetainBlocks": null,
        "_stateHistoryLogRetainBlocksNodeopDefault": null,
        "_stateHistoryLogRetainBlocksNodeopArg": "--state-history-log-retain-blocks",
        "deleteStateHistory": null,
        "_deleteStateHistoryNodeopDefault": false,
        "_deleteStateHistoryNodeopArg": "--delete-state-history"
      },
      "traceApiPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "trace_api_plugin",
        "traceDir": null,
        "_traceDirNodeopDefault": "\"traces\"",
        "_traceDirNodeopArg": "--trace-dir",
        "traceSliceStride": null,
        "_traceSliceStrideNodeopDefault": 10000,
        "_traceSliceStrideNodeopArg": "--trace-slice-stride",
        "traceMinimumIrreversibleHistoryBlocks": null,
        "_traceMinimumIrreversibleHistoryBlocksNodeopDefault": -1,
        "_traceMinimumIrreversibleHistoryBlocksNodeopArg": "--trace-minimum-irreversible-history-blocks",
        "traceMinimumUncompressedIrreversibleHistoryBlocks": null,
        "_traceMinimumUncompressedIrreversibleHistoryBlocksNodeopDefault": -1,
        "_traceMinimumUncompressedIrreversibleHistoryBlocksNodeopArg": "--trace-minimum-uncompressed-irreversible-history-blocks",
        "traceRpcAbi": null,
        "_traceRpcAbiNodeopDefault": null,
        "_traceRpcAbiNodeopArg": "--trace-rpc-abi",
        "traceNoAbis": null,
        "_traceNoAbisNodeopDefault": false,
        "_traceNoAbisNodeopArg": "--trace-no-abis"
      }
    },
    "specifiedContract": {
      "contractDir": "unittests/contracts/sysio.system",
      "wasmFile": "sysio.system.wasm",
      "abiFile": "sysio.system.abi",
      "account": "Name: sysio"
    },
    "genesisPath": "tests/PerformanceHarness/genesis.json",
    "maximumP2pPerHost": 5000,
    "maximumClients": 0,
    "keepLogs": true,
    "loggingLevel": "info",
    "loggingDict": {
      "bios": "off"
    },
    "prodsEnableTraceApi": false,
    "nodeopVers": "v4.1.0-dev",
    "specificExtraNodeopArgs": {
      "1": "--plugin sysio::trace_api_plugin "
    },
    "_totalNodes": 2,
    "_pNodes": 1,
    "_producerNodeIds": [
      0
    ],
    "_validationNodeIds": [
      1
    ],
    "_apiNodeIds": [
      2
    ],
    "nonProdsSysVmOcEnable": false,
    "apiNodesReadOnlyThreadCount": 0,
    "testDurationSec": 10,
    "finalDurationSec": 30,
    "delPerfLogs": false,
    "maxTpsToTest": 50000,
    "minTpsToTest": 1,
    "testIterationMinStep": 500,
    "tpsLimitPerGenerator": 4000,
    "delReport": false,
    "delTestReport": false,
    "numAddlBlocksToPrune": 2,
    "quiet": false,
    "logDirRoot": ".",
    "skipTpsTests": false,
    "calcProducerThreads": "lmax",
    "calcChainThreads": "lmax",
    "calcNetThreads": "lmax",
    "userTrxDataFile": null,
    "endpointMode": "p2p",
    "opModeCmd": "testBpOpMode",
    "logDirBase": "PHSRLogs",
    "logDirTimestamp": "2023-08-18_16-16-57",
    "logDirPath": "PHSRLogs/2023-08-18_16-16-57",
    "ptbLogsDirPath": "PHSRLogs/2023-08-18_16-16-57/testRunLogs",
    "pluginThreadOptLogsDirPath": "PHSRLogs/2023-08-18_16-16-57/pluginThreadOptRunLogs"
  },
  "env": {
    "system": "Linux",
    "os": "posix",
    "release": "5.15.90.1-microsoft-standard-WSL2",
    "logical_cpu_count": 16
  },
  "nodeopVersion": "v4.1.0-dev"
}
```
</details>


## Performance Test Basic Report

The Performance Test Basic generates, by default, a report that details results of the test, statistics around metrics of interest, as well as diagnostic information about the test run.  If `PerformanceHarnessScenarioRunner.py findMax` is run with `--del-test-report`, or `PerformanceHarnessScenarioRunner.py singleTest` is run with `--del-report`, the report described below will not be written.  Otherwise the report will be written to the timestamped directory within the `PHSRLogs` log directory for the test run with the file name `data.json`.

<details>
    <summary>Expand for full sample report</summary>

``` json
{
  "targetApiEndpointType": "p2p",
  "targetApiEndpoint": "NA for P2P",
  "Result": {
    "testStart": "2023-08-18T17:49:42.016053",
    "testEnd": "2023-08-18T17:50:56.550087",
    "testDuration": "0:01:14.534034",
    "testPassed": true,
    "testRunSuccessful": true,
    "testRunCompleted": true,
    "tpsExpectMet": true,
    "trxExpectMet": true,
    "targetTPS": 14001,
    "resultAvgTps": 14060.375,
    "expectedTxns": 140010,
    "resultTxns": 140010,
    "testAnalysisBlockCnt": 17,
    "logsDir": "PHSRLogs/2023-08-18_16-16-57/testRunLogs/PHSRunLogs/2023-08-18_17-49-42-14001"
  },
  "Analysis": {
    "BlockSize": {
      "min": 164525,
      "max": 181650,
      "avg": 175497.23529411765,
      "sigma": 4638.469493136106,
      "emptyBlocks": 0,
      "numBlocks": 17
    },
    "BlocksGuide": {
      "firstBlockNum": 99,
      "lastBlockNum": 130,
      "totalBlocks": 32,
      "testStartBlockNum": 99,
      "testEndBlockNum": 130,
      "setupBlocksCnt": 0,
      "tearDownBlocksCnt": 0,
      "leadingEmptyBlocksCnt": 1,
      "trailingEmptyBlocksCnt": 10,
      "configAddlDropCnt": 2,
      "testAnalysisBlockCnt": 17
    },
    "TPS": {
      "min": 13598,
      "max": 14344,
      "avg": 14060.375,
      "sigma": 202.9254404331798,
      "emptyBlocks": 0,
      "numBlocks": 17,
      "configTps": 14001,
      "configTestDuration": 10,
      "tpsPerGenerator": [
        3500,
        3500,
        3500,
        3501
      ],
      "generatorCount": 4
    },
    "TrxCPU": {
      "min": 8.0,
      "max": 767.0,
      "avg": 24.468759374330403,
      "sigma": 11.149625462006687,
      "samples": 140010
    },
    "TrxLatency": {
      "min": 0.0009999275207519531,
      "max": 0.5320000648498535,
      "avg": 0.25838474393291105,
      "sigma": 0.14487074243481057,
      "samples": 140010,
      "units": "seconds"
    },
    "TrxNet": {
      "min": 24.0,
      "max": 25.0,
      "avg": 24.85718162988358,
      "sigma": 0.3498875294629824,
      "samples": 140010
    },
    "TrxAckResponseTime": {
      "min": -1.0,
      "max": -1.0,
      "avg": -1.0,
      "sigma": 0.0,
      "samples": 140010,
      "measurementApplicable": "NOT APPLICABLE",
      "units": "microseconds"
    },
    "ExpectedTransactions": 140010,
    "DroppedTransactions": 0,
    "ProductionWindowsTotal": 2,
    "ProductionWindowsAverageSize": 12.0,
    "ProductionWindowsMissed": 0,
    "ForkedBlocks": {
      "00": [],
      "01": []
    },
    "ForksCount": {
      "00": 0,
      "01": 0
    },
    "DroppedBlocks": {
      "00": {},
      "01": {}
    },
    "DroppedBlocksCount": {
      "00": 0,
      "01": 0
    }
  },
  "args": {
    "rawCmdLine ": "tests/PerformanceHarnessScenarioRunner.py findMax testBpOpMode --test-iteration-duration-sec 10 --final-iterations-duration-sec 30 --calc-producer-threads lmax --calc-chain-threads lmax --calc-net-threads lmax",
    "dumpErrorDetails": false,
    "delay": 1,
    "nodesFile": null,
    "verbose": false,
    "unshared": false,
    "producerNodeCount": 1,
    "validationNodeCount": 1,
    "apiNodeCount": 0,
    "dontKill": false,
    "extraNodeopArgs": {
      "chainPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "chain_plugin",
        "blocksDir": null,
        "_blocksDirNodeopDefault": "\"blocks\"",
        "_blocksDirNodeopArg": "--blocks-dir",
        "blocksLogStride": null,
        "_blocksLogStrideNodeopDefault": null,
        "_blocksLogStrideNodeopArg": "--blocks-log-stride",
        "maxRetainedBlockFiles": null,
        "_maxRetainedBlockFilesNodeopDefault": null,
        "_maxRetainedBlockFilesNodeopArg": "--max-retained-block-files",
        "blocksRetainedDir": null,
        "_blocksRetainedDirNodeopDefault": null,
        "_blocksRetainedDirNodeopArg": "--blocks-retained-dir",
        "blocksArchiveDir": null,
        "_blocksArchiveDirNodeopDefault": null,
        "_blocksArchiveDirNodeopArg": "--blocks-archive-dir",
        "stateDir": null,
        "_stateDirNodeopDefault": "\"state\"",
        "_stateDirNodeopArg": "--state-dir",
        "protocolFeaturesDir": null,
        "_protocolFeaturesDirNodeopDefault": "\"protocol_features\"",
        "_protocolFeaturesDirNodeopArg": "--protocol-features-dir",
        "checkpoint": null,
        "_checkpointNodeopDefault": null,
        "_checkpointNodeopArg": "--checkpoint",
        "wasmRuntime": "sys-vm-jit",
        "_wasmRuntimeNodeopDefault": "sys-vm-jit",
        "_wasmRuntimeNodeopArg": "--wasm-runtime",
        "profileAccount": null,
        "_profileAccountNodeopDefault": null,
        "_profileAccountNodeopArg": "--profile-account",
        "abiSerializerMaxTimeMs": 990000,
        "_abiSerializerMaxTimeMsNodeopDefault": 15,
        "_abiSerializerMaxTimeMsNodeopArg": "--abi-serializer-max-time-ms",
        "chainStateDbSizeMb": 25600,
        "_chainStateDbSizeMbNodeopDefault": 1024,
        "_chainStateDbSizeMbNodeopArg": "--chain-state-db-size-mb",
        "chainStateDbGuardSizeMb": null,
        "_chainStateDbGuardSizeMbNodeopDefault": 128,
        "_chainStateDbGuardSizeMbNodeopArg": "--chain-state-db-guard-size-mb",
        "signatureCpuBillablePct": 0,
        "_signatureCpuBillablePctNodeopDefault": 50,
        "_signatureCpuBillablePctNodeopArg": "--signature-cpu-billable-pct",
        "chainThreads": 2,
        "_chainThreadsNodeopDefault": 2,
        "_chainThreadsNodeopArg": "--chain-threads",
        "contractsConsole": false,
        "_contractsConsoleNodeopDefault": false,
        "_contractsConsoleNodeopArg": "--contracts-console",
        "deepMind": null,
        "_deepMindNodeopDefault": false,
        "_deepMindNodeopArg": "--deep-mind",
        "actorWhitelist": null,
        "_actorWhitelistNodeopDefault": null,
        "_actorWhitelistNodeopArg": "--actor-whitelist",
        "actorBlacklist": null,
        "_actorBlacklistNodeopDefault": null,
        "_actorBlacklistNodeopArg": "--actor-blacklist",
        "contractWhitelist": null,
        "_contractWhitelistNodeopDefault": null,
        "_contractWhitelistNodeopArg": "--contract-whitelist",
        "contractBlacklist": null,
        "_contractBlacklistNodeopDefault": null,
        "_contractBlacklistNodeopArg": "--contract-blacklist",
        "actionBlacklist": null,
        "_actionBlacklistNodeopDefault": null,
        "_actionBlacklistNodeopArg": "--action-blacklist",
        "keyBlacklist": null,
        "_keyBlacklistNodeopDefault": null,
        "_keyBlacklistNodeopArg": "--key-blacklist",
        "senderBypassWhiteblacklist": null,
        "_senderBypassWhiteblacklistNodeopDefault": null,
        "_senderBypassWhiteblacklistNodeopArg": "--sender-bypass-whiteblacklist",
        "readMode": null,
        "_readModeNodeopDefault": "head",
        "_readModeNodeopArg": "--read-mode",
        "apiAcceptTransactions": null,
        "_apiAcceptTransactionsNodeopDefault": 1,
        "_apiAcceptTransactionsNodeopArg": "--api-accept-transactions",
        "validationMode": null,
        "_validationModeNodeopDefault": "full",
        "_validationModeNodeopArg": "--validation-mode",
        "disableRamBillingNotifyChecks": null,
        "_disableRamBillingNotifyChecksNodeopDefault": false,
        "_disableRamBillingNotifyChecksNodeopArg": "--disable-ram-billing-notify-checks",
        "maximumVariableSignatureLength": null,
        "_maximumVariableSignatureLengthNodeopDefault": 16384,
        "_maximumVariableSignatureLengthNodeopArg": "--maximum-variable-signature-length",
        "trustedProducer": null,
        "_trustedProducerNodeopDefault": null,
        "_trustedProducerNodeopArg": "--trusted-producer",
        "databaseMapMode": "mapped",
        "_databaseMapModeNodeopDefault": "mapped",
        "_databaseMapModeNodeopArg": "--database-map-mode",
        "sysVmOcCacheSizeMb": 1024,
        "_sysVmOcCacheSizeMbNodeopDefault": 1024,
        "_sysVmOcCacheSizeMbNodeopArg": "--sys-vm-oc-cache-size-mb",
        "sysVmOcCompileThreads": 1,
        "_sysVmOcCompileThreadsNodeopDefault": 1,
        "_sysVmOcCompileThreadsNodeopArg": "--sys-vm-oc-compile-threads",
        "sysVmOcEnable": null,
        "_sysVmOcEnableNodeopDefault": "auto",
        "_sysVmOcEnableNodeopArg": "--sys-vm-oc-enable",
        "enableAccountQueries": null,
        "_enableAccountQueriesNodeopDefault": 0,
        "_enableAccountQueriesNodeopArg": "--enable-account-queries",
        "maxNonprivilegedInlineActionSize": null,
        "transactionRetryMaxStorageSizeGb": null,
        "_transactionRetryMaxStorageSizeGbNodeopDefault": null,
        "_transactionRetryMaxStorageSizeGbNodeopArg": "--transaction-retry-max-storage-size-gb",
        "transactionRetryIntervalSec": null,
        "_transactionRetryIntervalSecNodeopDefault": 20,
        "_transactionRetryIntervalSecNodeopArg": "--transaction-retry-interval-sec",
        "transactionRetryMaxExpirationSec": null,
        "_transactionRetryMaxExpirationSecNodeopDefault": 120,
        "_transactionRetryMaxExpirationSecNodeopArg": "--transaction-retry-max-expiration-sec",
        "transactionFinalityStatusMaxStorageSizeGb": null,
        "_transactionFinalityStatusMaxStorageSizeGbNodeopDefault": null,
        "_transactionFinalityStatusMaxStorageSizeGbNodeopArg": "--transaction-finality-status-max-storage-size-gb",
        "transactionFinalityStatusSuccessDurationSec": null,
        "_transactionFinalityStatusSuccessDurationSecNodeopDefault": 180,
        "_transactionFinalityStatusSuccessDurationSecNodeopArg": "--transaction-finality-status-success-duration-sec",
        "transactionFinalityStatusFailureDurationSec": null,
        "_transactionFinalityStatusFailureDurationSecNodeopDefault": 180,
        "_transactionFinalityStatusFailureDurationSecNodeopArg": "--transaction-finality-status-failure-duration-sec",
        "disableReplayOpts": null,
        "_disableReplayOptsNodeopDefault": false,
        "_disableReplayOptsNodeopArg": "--disable-replay-opts",
        "integrityHashOnStart": null,
        "_integrityHashOnStartNodeopDefault": false,
        "_integrityHashOnStartNodeopArg": "--integrity-hash-on-start",
        "integrityHashOnStop": null,
        "_integrityHashOnStopNodeopDefault": false,
        "_integrityHashOnStopNodeopArg": "--integrity-hash-on-stop",
        "blockLogRetainBlocks": null,
        "_blockLogRetainBlocksNodeopDefault": null,
        "_blockLogRetainBlocksNodeopArg": "--block-log-retain-blocks",
        "genesisJson": null,
        "_genesisJsonNodeopDefault": null,
        "_genesisJsonNodeopArg": "--genesis-json",
        "genesisTimestamp": null,
        "_genesisTimestampNodeopDefault": null,
        "_genesisTimestampNodeopArg": "--genesis-timestamp",
        "printGenesisJson": null,
        "_printGenesisJsonNodeopDefault": false,
        "_printGenesisJsonNodeopArg": "--print-genesis-json",
        "extractGenesisJson": null,
        "_extractGenesisJsonNodeopDefault": null,
        "_extractGenesisJsonNodeopArg": "--extract-genesis-json",
        "printBuildInfo": null,
        "_printBuildInfoNodeopDefault": false,
        "_printBuildInfoNodeopArg": "--print-build-info",
        "extractBuildInfo": null,
        "_extractBuildInfoNodeopDefault": null,
        "_extractBuildInfoNodeopArg": "--extract-build-info",
        "forceAllChecks": null,
        "_forceAllChecksNodeopDefault": false,
        "_forceAllChecksNodeopArg": "--force-all-checks",
        "replayBlockchain": null,
        "_replayBlockchainNodeopDefault": false,
        "_replayBlockchainNodeopArg": "--replay-blockchain",
        "hardReplayBlockchain": null,
        "_hardReplayBlockchainNodeopDefault": false,
        "_hardReplayBlockchainNodeopArg": "--hard-replay-blockchain",
        "deleteAllBlocks": null,
        "_deleteAllBlocksNodeopDefault": false,
        "_deleteAllBlocksNodeopArg": "--delete-all-blocks",
        "truncateAtBlock": null,
        "_truncateAtBlockNodeopDefault": 0,
        "_truncateAtBlockNodeopArg": "--truncate-at-block",
        "terminateAtBlock": null,
        "_terminateAtBlockNodeopDefault": 0,
        "_terminateAtBlockNodeopArg": "--terminate-at-block",
        "snapshot": null,
        "_snapshotNodeopDefault": null,
        "_snapshotNodeopArg": "--snapshot"
      },
      "httpPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "http_plugin",
        "unixSocketPath": null,
        "_unixSocketPathNodeopDefault": null,
        "_unixSocketPathNodeopArg": "--unix-socket-path",
        "httpServerAddress": null,
        "_httpServerAddressNodeopDefault": "127.0.0.1:8888",
        "_httpServerAddressNodeopArg": "--http-server-address",
        "httpCategoryAddress": null,
        "_httpCategoryAddressNodeopDefault": null,
        "_httpCategoryAddressNodeopArg": "--http-category-address",
        "accessControlAllowOrigin": null,
        "_accessControlAllowOriginNodeopDefault": null,
        "_accessControlAllowOriginNodeopArg": "--access-control-allow-origin",
        "accessControlAllowHeaders": null,
        "_accessControlAllowHeadersNodeopDefault": null,
        "_accessControlAllowHeadersNodeopArg": "--access-control-allow-headers",
        "accessControlMaxAge": null,
        "_accessControlMaxAgeNodeopDefault": null,
        "_accessControlMaxAgeNodeopArg": "--access-control-max-age",
        "accessControlAllowCredentials": null,
        "_accessControlAllowCredentialsNodeopDefault": false,
        "_accessControlAllowCredentialsNodeopArg": "--access-control-allow-credentials",
        "maxBodySize": null,
        "_maxBodySizeNodeopDefault": 2097152,
        "_maxBodySizeNodeopArg": "--max-body-size",
        "httpMaxBytesInFlightMb": -1,
        "_httpMaxBytesInFlightMbNodeopDefault": 500,
        "_httpMaxBytesInFlightMbNodeopArg": "--http-max-bytes-in-flight-mb",
        "httpMaxInFlightRequests": -1,
        "_httpMaxInFlightRequestsNodeopDefault": -1,
        "_httpMaxInFlightRequestsNodeopArg": "--http-max-in-flight-requests",
        "httpMaxResponseTimeMs": -1,
        "_httpMaxResponseTimeMsNodeopDefault": 15,
        "_httpMaxResponseTimeMsNodeopArg": "--http-max-response-time-ms",
        "verboseHttpErrors": null,
        "_verboseHttpErrorsNodeopDefault": false,
        "_verboseHttpErrorsNodeopArg": "--verbose-http-errors",
        "httpValidateHost": null,
        "_httpValidateHostNodeopDefault": 1,
        "_httpValidateHostNodeopArg": "--http-validate-host",
        "httpAlias": null,
        "_httpAliasNodeopDefault": null,
        "_httpAliasNodeopArg": "--http-alias",
        "httpThreads": 2,
        "_httpThreadsNodeopDefault": 2,
        "_httpThreadsNodeopArg": "--http-threads",
        "httpKeepAlive": null,
        "_httpKeepAliveNodeopDefault": 1,
        "_httpKeepAliveNodeopArg": "--http-keep-alive"
      },
      "netPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "net_plugin",
        "p2pListenEndpoint": null,
        "_p2pListenEndpointNodeopDefault": "0.0.0.0:9876",
        "_p2pListenEndpointNodeopArg": "--p2p-listen-endpoint",
        "p2pServerAddress": null,
        "_p2pServerAddressNodeopDefault": null,
        "_p2pServerAddressNodeopArg": "--p2p-server-address",
        "p2pPeerAddress": null,
        "_p2pPeerAddressNodeopDefault": null,
        "_p2pPeerAddressNodeopArg": "--p2p-peer-address",
        "p2pMaxNodesPerHost": null,
        "_p2pMaxNodesPerHostNodeopDefault": 1,
        "_p2pMaxNodesPerHostNodeopArg": "--p2p-max-nodes-per-host",
        "p2pAcceptTransactions": null,
        "_p2pAcceptTransactionsNodeopDefault": 1,
        "_p2pAcceptTransactionsNodeopArg": "--p2p-accept-transactions",
        "p2pAutoBpPeer": null,
        "_p2pAutoBpPeerNodeopDefault": null,
        "_p2pAutoBpPeerNodeopArg": "--p2p-auto-bp-peer",
        "agentName": null,
        "_agentNameNodeopDefault": "SYS Test Agent",
        "_agentNameNodeopArg": "--agent-name",
        "allowedConnection": null,
        "_allowedConnectionNodeopDefault": "any",
        "_allowedConnectionNodeopArg": "--allowed-connection",
        "peerKey": null,
        "_peerKeyNodeopDefault": null,
        "_peerKeyNodeopArg": "--peer-key",
        "peerPrivateKey": null,
        "_peerPrivateKeyNodeopDefault": null,
        "_peerPrivateKeyNodeopArg": "--peer-private-key",
        "maxClients": 0,
        "_maxClientsNodeopDefault": 25,
        "_maxClientsNodeopArg": "--max-clients",
        "connectionCleanupPeriod": null,
        "_connectionCleanupPeriodNodeopDefault": 30,
        "_connectionCleanupPeriodNodeopArg": "--connection-cleanup-period",
        "maxCleanupTimeMsec": null,
        "_maxCleanupTimeMsecNodeopDefault": 10,
        "_maxCleanupTimeMsecNodeopArg": "--max-cleanup-time-msec",
        "p2pDedupCacheExpireTimeSec": null,
        "_p2pDedupCacheExpireTimeSecNodeopDefault": 10,
        "_p2pDedupCacheExpireTimeSecNodeopArg": "--p2p-dedup-cache-expire-time-sec",
        "netThreads": 4,
        "_netThreadsNodeopDefault": 4,
        "_netThreadsNodeopArg": "--net-threads",
        "syncFetchSpan": null,
        "_syncFetchSpanNodeopDefault": 1000,
        "_syncFetchSpanNodeopArg": "--sync-fetch-span",
        "syncPeerLimit": null,
        "_syncPeerLimitNodeopDefault": 3,
        "_syncPeerLimitNodeopArg": "--sync-peer-limit",
        "useSocketReadWatermark": null,
        "_useSocketReadWatermarkNodeopDefault": 0,
        "_useSocketReadWatermarkNodeopArg": "--use-socket-read-watermark",
        "peerLogFormat": null,
        "_peerLogFormatNodeopDefault": "[\"${_name}\" - ${_cid} ${_ip}:${_port}] ",
        "_peerLogFormatNodeopArg": "--peer-log-format",
        "p2pKeepaliveIntervalMs": null,
        "_p2pKeepaliveIntervalMsNodeopDefault": 10000,
        "_p2pKeepaliveIntervalMsNodeopArg": "--p2p-keepalive-interval-ms"
      },
      "producerPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "producer_plugin",
        "enableStaleProduction": null,
        "_enableStaleProductionNodeopDefault": false,
        "_enableStaleProductionNodeopArg": "--enable-stale-production",
        "pauseOnStartup": null,
        "_pauseOnStartupNodeopDefault": false,
        "_pauseOnStartupNodeopArg": "--pause-on-startup",
        "maxTransactionTime": -1,
        "_maxTransactionTimeNodeopDefault": 30,
        "_maxTransactionTimeNodeopArg": "--max-transaction-time",
        "maxIrreversibleBlockAge": null,
        "_maxIrreversibleBlockAgeNodeopDefault": -1,
        "_maxIrreversibleBlockAgeNodeopArg": "--max-irreversible-block-age",
        "producerName": null,
        "_producerNameNodeopDefault": null,
        "_producerNameNodeopArg": "--producer-name",
        "signatureProvider": null,
        "_signatureProviderNodeopDefault": "SYS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV=KEY:5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3",
        "_signatureProviderNodeopArg": "--signature-provider",
        "greylistAccount": null,
        "_greylistAccountNodeopDefault": null,
        "_greylistAccountNodeopArg": "--greylist-account",
        "greylistLimit": null,
        "_greylistLimitNodeopDefault": 1000,
        "_greylistLimitNodeopArg": "--greylist-limit",
        "produceBlockOffsetMs": 0,
        "_produceBlockOffsetMsDefault": 450,
        "_produceBlockOffsetMsArg": "--produce-block-offset-ms",
        "maxBlockCpuUsageThresholdUs": null,
        "_maxBlockCpuUsageThresholdUsNodeopDefault": 5000,
        "_maxBlockCpuUsageThresholdUsNodeopArg": "--max-block-cpu-usage-threshold-us",
        "maxBlockNetUsageThresholdBytes": null,
        "_maxBlockNetUsageThresholdBytesNodeopDefault": 1024,
        "_maxBlockNetUsageThresholdBytesNodeopArg": "--max-block-net-usage-threshold-bytes",
        "subjectiveCpuLeewayUs": null,
        "_subjectiveCpuLeewayUsNodeopDefault": 31000,
        "_subjectiveCpuLeewayUsNodeopArg": "--subjective-cpu-leeway-us",
        "subjectiveAccountMaxFailures": null,
        "_subjectiveAccountMaxFailuresNodeopDefault": 3,
        "_subjectiveAccountMaxFailuresNodeopArg": "--subjective-account-max-failures",
        "subjectiveAccountMaxFailuresWindowSize": null,
        "_subjectiveAccountMaxFailuresWindowSizeNodeopDefault": 1,
        "_subjectiveAccountMaxFailuresWindowSizeNodeopArg": "--subjective-account-max-failures-window-size",
        "subjectiveAccountDecayTimeMinutes": null,
        "_subjectiveAccountDecayTimeMinutesNodeopDefault": 1440,
        "_subjectiveAccountDecayTimeMinutesNodeopArg": "--subjective-account-decay-time-minutes",
        "incomingTransactionQueueSizeMb": null,
        "_incomingTransactionQueueSizeMbNodeopDefault": 1024,
        "_incomingTransactionQueueSizeMbNodeopArg": "--incoming-transaction-queue-size-mb",
        "disableSubjectiveAccountBilling": null,
        "_disableSubjectiveAccountBillingNodeopDefault": false,
        "_disableSubjectiveAccountBillingNodeopArg": "--disable-subjective-account-billing",
        "disableSubjectiveP2pBilling": true,
        "_disableSubjectiveP2pBillingNodeopDefault": 1,
        "_disableSubjectiveP2pBillingNodeopArg": "--disable-subjective-p2p-billing",
        "disableSubjectiveApiBilling": true,
        "_disableSubjectiveApiBillingNodeopDefault": 1,
        "_disableSubjectiveApiBillingNodeopArg": "--disable-subjective-api-billing",
        "producerThreads": 2,
        "_producerThreadsNodeopDefault": 2,
        "_producerThreadsNodeopArg": "--producer-threads",
        "snapshotsDir": null,
        "_snapshotsDirNodeopDefault": "\"snapshots\"",
        "_snapshotsDirNodeopArg": "--snapshots-dir",
        "readOnlyThreads": null,
        "_readOnlyThreadsNodeopDefault": null,
        "_readOnlyThreadsNodeopArg": "--read-only-threads",
        "readOnlyWriteWindowTimeUs": null,
        "_readOnlyWriteWindowTimeUsNodeopDefault": 200000,
        "_readOnlyWriteWindowTimeUsNodeopArg": "--read-only-write-window-time-us",
        "readOnlyReadWindowTimeUs": null,
        "_readOnlyReadWindowTimeUsNodeopDefault": 60000,
        "_readOnlyReadWindowTimeUsNodeopArg": "--read-only-read-window-time-us"
      },
      "resourceMonitorPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "resource_monitor_plugin",
        "resourceMonitorIntervalSeconds": null,
        "_resourceMonitorIntervalSecondsNodeopDefault": 2,
        "_resourceMonitorIntervalSecondsNodeopArg": "--resource-monitor-interval-seconds",
        "resourceMonitorSpaceThreshold": null,
        "_resourceMonitorSpaceThresholdNodeopDefault": 90,
        "_resourceMonitorSpaceThresholdNodeopArg": "--resource-monitor-space-threshold",
        "resourceMonitorSpaceAbsoluteGb": null,
        "_resourceMonitorSpaceAbsoluteGbNodeopDefault": null,
        "_resourceMonitorSpaceAbsoluteGbNodeopArg": "--resource-monitor-space-absolute-gb",
        "resourceMonitorNotShutdownOnThresholdExceeded": true,
        "_resourceMonitorNotShutdownOnThresholdExceededNodeopDefault": false,
        "_resourceMonitorNotShutdownOnThresholdExceededNodeopArg": "--resource-monitor-not-shutdown-on-threshold-exceeded",
        "resourceMonitorWarningInterval": null,
        "_resourceMonitorWarningIntervalNodeopDefault": 30,
        "_resourceMonitorWarningIntervalNodeopArg": "--resource-monitor-warning-interval"
      },
      "signatureProviderPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "signature_provider_plugin",
        "kiodProviderTimeout": null,
        "_kiodProviderTimeoutNodeopDefault": 5,
        "_kiodProviderTimeoutNodeopArg": "--kiod-provider-timeout"
      },
      "stateHistoryPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "state_history_plugin",
        "stateHistoryDir": null,
        "_stateHistoryDirNodeopDefault": "\"state-history\"",
        "_stateHistoryDirNodeopArg": "--state-history-dir",
        "stateHistoryRetainedDir": null,
        "_stateHistoryRetainedDirNodeopDefault": null,
        "_stateHistoryRetainedDirNodeopArg": "--state-history-retained-dir",
        "stateHistoryArchiveDir": null,
        "_stateHistoryArchiveDirNodeopDefault": null,
        "_stateHistoryArchiveDirNodeopArg": "--state-history-archive-dir",
        "stateHistoryStride": null,
        "_stateHistoryStrideNodeopDefault": null,
        "_stateHistoryStrideNodeopArg": "--state-history-stride",
        "maxRetainedHistoryFiles": null,
        "_maxRetainedHistoryFilesNodeopDefault": null,
        "_maxRetainedHistoryFilesNodeopArg": "--max-retained-history-files",
        "traceHistory": null,
        "_traceHistoryNodeopDefault": false,
        "_traceHistoryNodeopArg": "--trace-history",
        "chainStateHistory": null,
        "_chainStateHistoryNodeopDefault": false,
        "_chainStateHistoryNodeopArg": "--chain-state-history",
        "stateHistoryEndpoint": null,
        "_stateHistoryEndpointNodeopDefault": "127.0.0.1:8080",
        "_stateHistoryEndpointNodeopArg": "--state-history-endpoint",
        "stateHistoryUnixSocketPath": null,
        "_stateHistoryUnixSocketPathNodeopDefault": null,
        "_stateHistoryUnixSocketPathNodeopArg": "--state-history-unix-socket-path",
        "traceHistoryDebugMode": null,
        "_traceHistoryDebugModeNodeopDefault": false,
        "_traceHistoryDebugModeNodeopArg": "--trace-history-debug-mode",
        "stateHistoryLogRetainBlocks": null,
        "_stateHistoryLogRetainBlocksNodeopDefault": null,
        "_stateHistoryLogRetainBlocksNodeopArg": "--state-history-log-retain-blocks",
        "deleteStateHistory": null,
        "_deleteStateHistoryNodeopDefault": false,
        "_deleteStateHistoryNodeopArg": "--delete-state-history"
      },
      "traceApiPluginArgs": {
        "_pluginNamespace": "sysio",
        "_pluginName": "trace_api_plugin",
        "traceDir": null,
        "_traceDirNodeopDefault": "\"traces\"",
        "_traceDirNodeopArg": "--trace-dir",
        "traceSliceStride": null,
        "_traceSliceStrideNodeopDefault": 10000,
        "_traceSliceStrideNodeopArg": "--trace-slice-stride",
        "traceMinimumIrreversibleHistoryBlocks": null,
        "_traceMinimumIrreversibleHistoryBlocksNodeopDefault": -1,
        "_traceMinimumIrreversibleHistoryBlocksNodeopArg": "--trace-minimum-irreversible-history-blocks",
        "traceMinimumUncompressedIrreversibleHistoryBlocks": null,
        "_traceMinimumUncompressedIrreversibleHistoryBlocksNodeopDefault": -1,
        "_traceMinimumUncompressedIrreversibleHistoryBlocksNodeopArg": "--trace-minimum-uncompressed-irreversible-history-blocks",
        "traceRpcAbi": null,
        "_traceRpcAbiNodeopDefault": null,
        "_traceRpcAbiNodeopArg": "--trace-rpc-abi",
        "traceNoAbis": null,
        "_traceNoAbisNodeopDefault": false,
        "_traceNoAbisNodeopArg": "--trace-no-abis"
      }
    },
    "specifiedContract": {
      "contractDir": "unittests/contracts/sysio.system",
      "wasmFile": "sysio.system.wasm",
      "abiFile": "sysio.system.abi",
      "account": "Name: sysio"
    },
    "genesisPath": "tests/PerformanceHarness/genesis.json",
    "maximumP2pPerHost": 5000,
    "maximumClients": 0,
    "keepLogs": true,
    "loggingLevel": "info",
    "loggingDict": {
      "bios": "off"
    },
    "prodsEnableTraceApi": false,
    "nodeopVers": "v4.1.0-dev",
    "specificExtraNodeopArgs": {
      "1": "--plugin sysio::trace_api_plugin "
    },
    "_totalNodes": 2,
    "_pNodes": 1,
    "_producerNodeIds": [
      0
    ],
    "_validationNodeIds": [
      1
    ],
    "_apiNodeIds": [
      2
    ],
    "nonProdsSysVmOcEnable": false,
    "apiNodesReadOnlyThreadCount": 0,
    "targetTps": 14001,
    "testTrxGenDurationSec": 10,
    "tpsLimitPerGenerator": 4000,
    "numAddlBlocksToPrune": 2,
    "logDirRoot": "PHSRLogs/2023-08-18_16-16-57/testRunLogs",
    "delReport": false,
    "quiet": false,
    "delPerfLogs": false,
    "expectedTransactionsSent": 140010,
    "printMissingTransactions": false,
    "userTrxDataFile": null,
    "endpointMode": "p2p",
    "apiEndpoint": null,
    "logDirBase": "PHSRLogs/2023-08-18_16-16-57/testRunLogs/PHSRunLogs",
    "logDirTimestamp": "2023-08-18_17-49-42",
    "logDirTimestampedOptSuffix": "-14001",
    "logDirPath": "PHSRLogs/2023-08-18_16-16-57/testRunLogs/PHSRunLogs/2023-08-18_17-49-42-14001",
    "userTrxData": "NOT CONFIGURED"
  },
  "env": {
    "system": "Linux",
    "os": "posix",
    "release": "5.15.90.1-microsoft-standard-WSL2",
    "logical_cpu_count": 16
  },
  "nodeopVersion": "v4.1.0-dev"
}
```
</details>
