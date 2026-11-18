# WASM Spec Tests

This directory provides a set of unit tests that can be used to check a WASM Backend's conformance to the
WebAssembly spec.

## Tests
### How tests are generated
1. The JSON file for a spec test suite is read.
2. For each spec test wasm defined in the JSON:
    - All the spec tests are created in a C++ file to match the function declaration as interpreted from the JSON.
    - Test are categorized into `assert_trap`/`assert_exhaustion` and `assert_return`.
    - Tests are split up into `sub_apply` functions based on the rules defined below.
    - An `apply` function is written that switches on the third parameter to decide which `sub_apply` function to run.
    - A map of the function name and the index in order is created to be used for merging.
    - `f32`/`f64` results are checked by exact bit pattern. `assert_return_canonical_nan` and
      `assert_return_arithmetic_nan` results are checked against the spec's NaN class masks, except the pinned cases
      under Known Issues.
3. Unit Tests are generated based on the rules below.
4. The generated C++ files are compiled and linked, without optimizations to prevent the empty functions from being optimized out.
5. The generated WASM is combined with the original test wasm.
    - The imports and apply functions (and any helper functions) from the generated wasm are combined with the test function definitions from the spec test wasm.
    - Any necessary shifting of type/import/function/call/exports numbers is done.
        - This is where the generated map from above is used.
6. The newly created merged wasms and unit test C++ files are copied into the appropriate directory in the Spring repo.


### How tests are split up
- Within a spec test suite, each `assert_trap` and `assert_exhaustion` test case is given a unique `sub_apply` function.
    - All tests in a suite are in the same WASM file, so the test that is run is based on the `test.name` passed in to `apply` (which calls the correct `sub_apply`).
- Within a test suite, `assert_return` tests are grouped into sets of 100.
    - This is due to the limit on 1024 locals and 1024 func defs built into nodeop. Some spec tests had too many functions to have a `sub_apply` per test, and some had too many variables to be put all into one `sub_apply`.
    - 100 was found to be the number that did not exceed this maximum for all the tests.
    - The tests also have some reliance on ordering (a store may need to be called before a load for example).
    - 100 also works out to make sure the right ordering is achieved.

- The unit tests are split into 2 groups. All of the `assert_trap` tests are grouped into one `BOOST_DATA_TEST_CASE` and all the `assert_return` tests are grouped into a second `BOOST_DATA_TEST_CASE`
- The unit test files are grouped by test suite (all `address` tests are together, all `call` tests together, etc.)


### How to generate tests
- Run the `setup_sysio_tests.py` script with no options to see the help text. It runs the generator from
  `build/generator/sysio_test_generator` under this directory (`cmake -S generator -B build/generator && cmake --build
  build/generator`) and needs the CDT tools on `PATH`.
- Inputs: the `<suite>.json` files and module wasms that `wast2json` from WABT 1.0.12 writes for that release's
  `third_party/testsuite`. They match `EOSIO/eos-vm-test-wasms` byte for byte. Later `wast2json` releases write NaN
  expectations as `assert_return` values (`nan:canonical`), which the generator does not read.
- Modules in `generator/altered-wasms/` are copied unchanged. When the generated code of one changes, rebuild it from
  the spec module with the edits described in its `notes.md`.


### Known Issues
- memory_grow.3 -- Will fail if not deleted from generated tests.
    - Unclear how to hand alter this to have memory properly zeroed where expected.

- start.7 -- Will fail if not deleted from generated tests.
    - Imports "print" from "spectest". Changing to any of the `sysio::print` functions results in "start function must be nullary" due to their requiring a parameter.

- globals.2 -- Delete from generated tests or it segfaults due to missing wasm.
    - `sysio-wasm2wast` error "mutable globals cannot be exported" when converting to wast.
        - `wasm2wat` provided by WABT handles this correctly, implying an error in CDT.
- globals.3 -- Delete from generated tests or it segfaults due to missing wasm.
    - `sysio-wasm2wast` error "mutable globals cannot be exported" when converting to wast.
        - `wasm2wat` provided by WABT handles this correctly, implying an error in CDT.
- globals.14 -- Delete from generated tests or it segfaults due to missing wasm.
    - Imports "global_i32" from "spectest".
        - Unclear what an appropriate substition would be.

- f32/f64 `min`, `max`, `floor`, `ceil`, `trunc`, `nearest` -- return a NaN operand unchanged, so a signaling NaN stays
  signaling where the spec requires an arithmetic NaN.
    - The generator pins that result for these `assert_return_arithmetic_nan` cases, so a change to it in
      `libraries/sys-vm/include/sysio/vm/softfloat.hpp` (interpreter, JIT) or
      `libraries/chain/webassembly/softfloat.cpp` (OC) fails the tests.
