# Protocol Buffer Support

Wire supports [Protocol Buffers](https://protobuf.dev/) (protobuf) as an alternative serialization format for smart contract action data. Contracts use protobuf wire format instead of the default CDT datastream packing, providing:

- **ID-based field encoding** for stable on-chain data formats
- **Backwards compatibility** for schema evolution (add/remove fields without breaking existing data)
- **Language-neutral** message definitions with extensive library support
- **Compact binary encoding** with fast serialization/deserialization

For the full CDT-side reference (type mappings, generated code details, limitations), see the [wire-cdt protocol-buffers.md](https://github.com/Wire-Network/wire-cdt/blob/master/docs/protocol-buffers.md).

## Architecture Overview

Protobuf support spans two components:

| Component | Role |
|-----------|------|
| **wire-cdt** | Compiles `.proto` files into C++ structs using `zpp_bits` (header-only, no exceptions). Generates ABI with `protobuf_types` section. |
| **wire-sysio** | Reads `protobuf_types` from ABIs and uses Google's `libprotobuf` (`DynamicMessageFactory`) to serialize/deserialize protobuf action data at runtime. |

### Action Parameter Mapping

When an action has a single `pb<T>` parameter, CDT flattens the ABI — the action type points directly at the protobuf type with no wrapper struct. The action data JSON uses the protobuf fields directly:

```cpp
[[sysio::action]]
void greet(const sysio::pb<Greeting>& greeting) { ... }
```

```bash
# Flat JSON — protobuf fields at the top level
clio push action pbexample greet '{"from":"alice","message":"hello","importance":5}' -p pbexample@active
```

When an action has multiple parameters (protobuf or mixed), CDT generates a wrapper struct with one field per parameter. The C++ parameter names become the field names in the JSON:

```cpp
[[sysio::action]]
void settle(const sysio::pb<Header>& header, const sysio::pb<Body>& body) { ... }
```

The generated ABI creates a wrapper struct:
```json
{
  "structs": [{
    "name": "settle",
    "fields": [
      { "name": "header", "type": "protobuf::mypackage.Header" },
      { "name": "body", "type": "protobuf::mypackage.Body" }
    ]
  }],
  "actions": [{ "name": "settle", "type": "settle" }]
}
```

```bash
# Nested JSON — each parameter is a wrapper field
clio push action mycontract settle \
  '{"header":{"version":1,"timestamp":1234},"body":{"payload":"data","checksum":42}}' \
  -p myaccount@active
```

You can also mix protobuf and regular parameters:

```cpp
[[sysio::action]]
void process(const sysio::pb<Request>& request, sysio::name authorized_by) { ... }
```

```bash
clio push action mycontract process \
  '{"request":{"id":1,"action":"approve"},"authorized_by":"alice"}' \
  -p alice@active
```

### Wire Format Compatibility

CDT contracts use `zpp_bits` with the `size_varint{}` option, which adds a varint length prefix before the protobuf message bytes. The node's `abi_serializer` matches this format exactly:

```
[outer varint length] [inner varint length] [protobuf message bytes]
```

- **Outer varint**: total byte count of inner length + message bytes (standard CDT datastream length prefix)
- **Inner varint**: byte count of just the protobuf message bytes (`zpp_bits` `size_varint` format)
- **Message bytes**: standard protobuf wire format

## ABI Version 1.3

Contracts with protobuf actions produce ABIs with version `sysio::abi/1.3`. The ABI includes a `protobuf_types` field containing the [FileDescriptorSet](https://protobuf.dev/reference/protobuf/google.protobuf/#filedescriptorset) as a JSON object:

```json
{
  "version": "sysio::abi/1.3",
  "structs": [],
  "actions": [
    { "name": "greet", "type": "protobuf::pbexample.Greeting", "ricardian_contract": "" }
  ],
  "protobuf_types": {
    "file": [
      {
        "name": "pbexample.proto",
        "package": "pbexample",
        "messageType": [
          {
            "name": "Greeting",
            "field": [
              { "name": "from", "number": 1, "type": "TYPE_STRING", "label": "LABEL_OPTIONAL" },
              { "name": "message", "number": 2, "type": "TYPE_STRING", "label": "LABEL_OPTIONAL" },
              { "name": "importance", "number": 3, "type": "TYPE_INT32", "label": "LABEL_OPTIONAL" }
            ]
          }
        ]
      }
    ]
  }
}
```

Protobuf types use the naming convention `protobuf::package.MessageType`. For single-parameter actions, the action type is the protobuf type directly.

## How the Node Processes Protobuf Actions

When `abi_serializer::set_abi()` encounters a non-empty `protobuf_types` field:

1. Parses the FileDescriptorSet JSON into a `google::protobuf::FileDescriptorSet`
2. Builds a `DescriptorPool` containing all message descriptors
3. Creates a `DynamicMessageFactory` for runtime message construction

When serializing/deserializing action data:

- **JSON to binary** (`variant_to_binary`): Detects `protobuf::` type prefix, constructs a `DynamicMessage`, populates fields from the JSON variant, serializes with the double-varint length prefix
- **Binary to JSON** (`binary_to_variant`): Reads the varint length prefixes, parses the protobuf bytes into a `DynamicMessage`, converts fields back to a JSON variant

This is used by `clio`, `nodeop` (transaction logging, API responses), and any tool that links `libchain`.

## Using clio with Protobuf Actions

### Push actions

Protobuf actions are pushed the same way as regular actions. The node fetches the ABI from the chain, detects the protobuf type, and serializes automatically:

```bash
clio push action pbexample greet '{"from":"alice","message":"hello world","importance":5}' \
  -p pbexample@active
```

### Offline pack/unpack with --abi-file

For offline serialization without a running node:

```bash
# Pack JSON to hex
clio --abi-file pbexample:/path/to/pbexample.abi \
  convert pack_action_data pbexample greet '{"from":"alice","message":"hello world","importance":5}'

# Unpack hex back to JSON
clio --abi-file pbexample:/path/to/pbexample.abi \
  convert unpack_action_data pbexample greet <hex_output>
```

## Build Requirements

Protobuf support requires `libprotobuf` at build time. It is declared in `vcpkg.json` and linked in `libraries/chain/CMakeLists.txt`:

```cmake
find_package(protobuf CONFIG REQUIRED)
target_link_libraries(sysio_chain PUBLIC protobuf::libprotobuf)
```

No additional build flags are needed — protobuf support is always available when the dependency is present.

## End-to-End Walkthrough: Creating a Protobuf Contract

This walkthrough creates a contract called `pbexample` under `contracts/` that uses protobuf-serialized action data.

### Prerequisites

- wire-cdt built with protobuf support (`feature/protobuf-support` branch or later)
- wire-sysio configured with `BUILD_SYSTEM_CONTRACTS=ON` and CDT available via `find_package(cdt)`

### Step 1: Create the contract directory

```
contracts/
  pbexample/
    pbexample.proto
    pbexample.cpp
    CMakeLists.txt
```

### Step 2: Define proto messages

Create `contracts/pbexample/pbexample.proto`:

```protobuf
syntax = "proto3";
package pbexample;

message Greeting {
  string from = 1;
  string message = 2;
  int32  importance = 3;
}

message GreetResult {
  int32 status = 1;
}
```

### Step 3: Write the contract

Create `contracts/pbexample/pbexample.cpp`:

```cpp
#include <sysio/sysio.hpp>
#include <sysio/pb.hpp>
#include <pbexample/pbexample.pb.hpp>  // generated from pbexample.proto

namespace pbexample {

class [[sysio::contract]] pbexample : public sysio::contract {
public:
   using sysio::contract::contract;

   // Action with protobuf input and protobuf return value
   [[sysio::action]]
   sysio::pb<GreetResult> greet(const sysio::pb<Greeting>& greeting) {
      sysio::require_auth(get_self());

      auto importance = static_cast<int32_t>(greeting.importance);
      sysio::check(importance >= 0, "importance must be non-negative");

      sysio::print("Greeting from ", greeting.from, ": ", greeting.message,
                   " (importance=", importance, ")");

      GreetResult result;
      result.status = zpp::bits::vint32_t(1);  // success
      return result;
   }

   // Action with protobuf input, no return value
   [[sysio::action]]
   void notify(const sysio::pb<Greeting>& greeting) {
      sysio::require_auth(get_self());
      sysio::print("Notification: ", greeting.message);
   }
};

} // namespace pbexample
```

Key points:
- `sysio::pb<T>` wraps protobuf message types for action parameters and return values
- Protobuf integer fields use `zpp::bits` varint wrappers — use `static_cast<int32_t>(field)` to read, and assign via `zpp::bits::vint32_t(value)` to write
- `#include <pbexample/pbexample.pb.hpp>` references the header generated by `protoc-gen-zpp` (the output directory matches what you configure in CMake)
- Single `pb<T>` parameter actions produce flat JSON — no wrapper field needed

### Step 4: Write the CMakeLists.txt

Create `contracts/pbexample/CMakeLists.txt`:

```cmake
if(BUILD_SYSTEM_CONTRACTS)
   # Create an INTERFACE library for proto definitions
   add_library(pbexample_protos INTERFACE)
   target_add_protobuf(pbexample_protos
      OUTPUT_DIRECTORY pbexample
      FILES pbexample.proto
   )

   # Create the contract and link protobuf support
   add_contract(pbexample pbexample pbexample.cpp)
   contract_use_protobuf(pbexample pbexample_protos)
else()
   # When not building contracts, copy pre-built artifacts
   configure_file(${CMAKE_CURRENT_SOURCE_DIR}/pbexample.wasm ${CMAKE_CURRENT_BINARY_DIR}/pbexample.wasm COPYONLY)
   configure_file(${CMAKE_CURRENT_SOURCE_DIR}/pbexample.abi  ${CMAKE_CURRENT_BINARY_DIR}/pbexample.abi  COPYONLY)
endif()
```

- `target_add_protobuf()` runs `protoc` with `protoc-gen-zpp` to generate the `.pb.hpp` header
- `OUTPUT_DIRECTORY pbexample` means the generated header is at `pbexample/pbexample.pb.hpp`
- `contract_use_protobuf()` links proto definitions and tells `cdt-codegen` to embed the `protobuf_types` in the ABI

### Step 5: Register in top-level CMakeLists.txt

Add the subdirectory to `contracts/CMakeLists.txt`:

```cmake
add_subdirectory(pbexample)
```

### Step 6: Build

```bash
ninja -C cmake-build-debug pbexample
```

This produces:
- `cmake-build-debug/contracts/pbexample/pbexample.wasm` — the WASM contract
- `cmake-build-debug/contracts/pbexample/pbexample.abi` — ABI with `protobuf_types` (version `sysio::abi/1.3`)

### Step 7: Deploy and use

```bash
# Create account and deploy
clio create account sysio pbexample <OWNER_KEY> <ACTIVE_KEY>
clio set contract pbexample cmake-build-debug/contracts/pbexample

# Push the greet action (flat JSON — single pb<T> parameter)
clio push action pbexample greet \
  '{"from":"alice","message":"hello world","importance":5}' \
  -p pbexample@active

# Push the notify action
clio push action pbexample notify \
  '{"from":"bob","message":"heads up"}' \
  -p pbexample@active
```

### Step 8: Commit pre-built artifacts

Once the contract builds successfully, commit the `.wasm` and `.abi` files into the source directory so that builds without CDT can still use the contract (the `else()` branch in the CMakeLists copies these pre-built artifacts):

```bash
cp cmake-build-debug/contracts/pbexample/pbexample.wasm contracts/pbexample/
cp cmake-build-debug/contracts/pbexample/pbexample.abi  contracts/pbexample/
```