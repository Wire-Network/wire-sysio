# Contract suite selection

Each owning test source links into a separate `contract_<source>` Boost.Test
executable. `CMakeLists.txt` calls `add_contract_suite(...)` once for every
top-level suite, registering one `contract.<suite>` CTest test per call. The
helper checks each call against `contract_suites.json` at configure time.
Suites in the same source share a
binary; suites in different sources can run in parallel. `contract_impact.json`
is the reviewed path,
consumer, and platform-flow map used by the Wire validation planner. Both files
are versioned and are inputs to validation evidence.

The target name strips `.cpp` and replaces dots with underscores: for example,
`sysio.reserv_tests.cpp` builds `contract_sysio_reserv_tests`. Shared Boost.Test
startup is linked into each target. The finalizer key fixture used by both
emissions and finalizer tests lives in `finalizer_test_keys.cpp`.

The manifest has 34 suites in 30 owning test sources and currently lists 761
Boost cases. `emissions_tests.cpp` owns four suites and
`sysio.msgch_tests.cpp` owns two. A test-only edit selects every suite in its
owning source file. A contract edit selects suites that deploy or otherwise
exercise that account, plus explicit runtime consumer edges and mapped OPP
flows. `sysio.reserv` source currently selects ten suites and seven flows:
reserve, dispatch, four emissions suites, epoch, underwriting, and the two
message-channel suites. The four explicit runtime edges cite the caller source
files in `contract_impact.json`.

The Wire validation skill's case inventory inspects each built binary's
`--list_content` and exact CTest registration. CMake and the impact planner
already check source suite
declarations and account metadata. Inventory rejects missing or stale binaries,
empty selectors, mismatched labels, or an aggregate CTest entry. The skill
profile runs it after an ON/ON build and before contract CTest.

The contract build gate explicitly builds `contracts_project`,
`test_contracts_project`, and `contract_suite_binaries`. The second project supplies
`noop.wasm` for ROA. Tracked WASM/ABI synchronization and complete generated
artifact parity precede either selected or full contract behavior. On a broad
platform path, the parallel and NP/LR CTest groups exclude the `contract`
label; `contracts-all` runs the 34 suites once after parity with CTest `-j`.

Headers, protocol inputs, shared contract code, target or packaging CMake
changes, ambiguous paths, and unbounded ownership escalate to all contract
suites or the full platform path. Contract-local CMake remains a platform
change because the outer `contracts_project` always invokes its nested build;
an isolated target closure has not been proven. New suite declarations and new
contract includes must update the manifest before focused selection can pass.

The local validation record is in the implementation workspace's
`VALIDATION_REPORT.md`. The independent current-head CI artifact-copy proof and
ticket-scoped reviews are still delivery checks when this implementation is
adopted for a sysio ticket.
