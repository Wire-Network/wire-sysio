# LLVM 11 → LLVM 18 Migration — Final Changes

## Overview

All LLVM usage in wire-sysio lives in `libraries/chain/webassembly/runtimes/sys-vm-oc/`. The migration touched 8 files across the repository. LLVM 18.1.6 is now provided via vcpkg.

### Files Changed

| File | Change Summary |
|------|---------------|
| `vcpkg.json` | Added LLVM 18 dependency with `enable-rtti`, `target-aarch64`, `target-x86` |
| `vcpkg-configuration.json` | Updated wire-vcpkg-registry baseline |
| `cmake/dependencies.cmake` | Added `find_package(LLVM CONFIG REQUIRED)` with version >= 18 check |
| `CMakeLists.txt` (root) | Removed old LLVM 7-11 version check (now in `dependencies.cmake`) |
| `libraries/chain/CMakeLists.txt` | Added `orcshared orctargetprocess` to LLVM component list |
| `cmake/SysioTester.cmake.in` | Same component additions |
| `LLVMEmitIR.cpp` | Opaque pointer migration (~30 call sites), removed compat code |
| `LLVMJIT.cpp` | Replaced ORC JIT v1 with direct RuntimeDyld, removed compat code |

---

## 1. vcpkg / CMake Changes

### vcpkg.json
Added LLVM as a direct dependency with pinned version:
```json
{
  "name": "llvm",
  "default-features": true,
  "features": ["enable-rtti", "target-aarch64", "target-x86"]
}
```
Override: `"version": "18.1.6#5"`

Also added `zstd` as a dependency (required by LLVM 18).

### cmake/dependencies.cmake
Moved LLVM discovery here (previously in root `CMakeLists.txt`):
```cmake
find_package(LLVM CONFIG REQUIRED)
if(LLVM_VERSION_MAJOR VERSION_LESS 18)
    message(FATAL_ERROR "WIRE requires LLVM version 18 or later")
endif()
```
Also added `find_package(ZLIB REQUIRED)` and `find_package(zstd CONFIG REQUIRED)` before LLVM.

### CMakeLists.txt (root)
Removed the old `find_package(LLVM)` and version 7-11 check block (6 lines).

### LLVM Component Linkage
Both `libraries/chain/CMakeLists.txt` and `cmake/SysioTester.cmake.in`:
```cmake
# Old:
llvm_map_components_to_libnames(LLVM_LIBS support core passes mcjit native orcjit)
# New:
llvm_map_components_to_libnames(LLVM_LIBS support core passes mcjit native orcjit orcshared orctargetprocess)
```
Note: `orcshared` and `orctargetprocess` are pulled in transitively by `orcjit` but listed explicitly for clarity.

---

## 2. LLVMEmitIR.cpp — Opaque Pointer Migration

LLVM 15 made opaque pointers default; LLVM 17+ removed typed pointers entirely. This required pervasive but mechanical changes.

### 2a. New Global Pointer Types

```cpp
// Old:
llvmI8PtrType = llvmI8Type->getPointerTo();

// New:
llvmI8PtrType = llvm::PointerType::getUnqual(context);   // addr space 0
llvmPtrAS256Type = llvm::PointerType::get(context, 256);  // addr space 256
```

All uses of `type->getPointerTo()` and `type->getPointerTo(256)` replaced with these two globals. `CreatePointerCast` calls between same-address-space pointers removed (unnecessary with opaque pointers).

### 2b. `createCall` Helper Rewritten

```cpp
// Old — extracted FunctionType from typed pointer (impossible with opaque pointers):
llvm::CallInst* createCall(llvm::Value* Callee, llvm::ArrayRef<llvm::Value*> Args) {
    auto* PTy = llvm::cast<llvm::PointerType>(Callee->getType());
    auto* FTy = llvm::cast<llvm::FunctionType>(PTy->getElementType());
    return irBuilder.CreateCall(FTy, Callee, Args);
}

// New — two overloads:
llvm::CallInst* createCall(llvm::FunctionType* FTy, llvm::Value* Callee, llvm::ArrayRef<llvm::Value*> Args);
llvm::CallInst* createCall(llvm::Function* Callee, llvm::ArrayRef<llvm::Value*> Args);
```

All call sites updated: `call()`, `call_indirect()`, `emitRuntimeIntrinsic()`, intrinsic calls. `getLLVMIntrinsic` return type changed from `llvm::Value*` to `llvm::Function*`.

### 2c. `CreateLoad` — Explicit Type Required

Every `CreateLoad(pointer)` call updated to `CreateLoad(type, pointer)`:

| Context | Type Provided |
|---------|--------------|
| `EMIT_LOAD_OP` macro | `llvmMemoryType` (the WASM memory type being loaded) |
| `get_local` | `AllocaInst::getAllocatedType()` |
| `get_global` / `set_global` | `globalValueTypes[imm.variableIndex]` (new tracking vector) |
| `getNonConstantZero` | `zero->getType()` |
| Depth counter loads | `llvmI32Type` |
| Intrinsic pointer loads | `llvmI64Type` |
| Table entry loads | `llvmI8PtrType` / `llvmI64Type` |

### 2d. `CreateInBoundsGEP` — Explicit Type Required

All `CreateInBoundsGEP(base, index)` calls updated to `CreateInBoundsGEP(type, base, index)`:
- Memory access GEPs use `llvmI8Type` as the element type
- Table GEPs use the table's array element type (recovered via `getValueType()`)
- Mutable global GEPs use the global's value type

### 2e. Global Variable Type Tracking

Added `globalValueTypes` vector to `EmitModuleContext` to track the LLVM type of each WASM global variable, since `getPointerElementType()` is no longer available:
```cpp
std::vector<llvm::Type*> globalValueTypes;
// Populated during emit():
globalValueTypes.push_back(asLLVMType(global.type.valueType));
```

### 2f. Local Variable Type Recovery

For local variables (allocas), type is recovered via:
```cpp
auto allocaType = llvm::cast<llvm::AllocaInst>(localPointers[imm.variableIndex])->getAllocatedType();
```

### 2g. `getBasicBlockList().push_back()` → `insertInto()`

```cpp
// Old:
llvmFunction->getBasicBlockList().push_back(block);
// New:
block->insertInto(llvmFunction);
```

### 2h. Removed Dead Compatibility Code

- **LLVM <9 comparison operators** (`#if LLVM_VERSION_MAJOR < 9` block) — removed, kept `EMIT_INT_BINARY_OP` versions
- **LLVM <10/==10 alignment** (`#if LLVM_VERSION_MAJOR < 10` block) — removed, kept `llvm::Align(1)`
- **`llvm/Analysis/Passes.h`** — removed (unused)

---

## 3. LLVMJIT.cpp — JIT Pipeline Rewrite

### 3a. Replaced ORC JIT v1 with Direct RuntimeDyld

The Legacy ORC v1 API (`LegacyRTDyldObjectLinkingLayer`, `LegacyIRCompileLayer`, `VModuleKey`, `NullResolver`) was removed in LLVM 12-13.

**Initial approach** (ORC v2 with `ExecutionSession` + `RTDyldObjectLinkingLayer`) caused `Failed to materialize symbols` errors at runtime because a bare `JITDylib` cannot resolve external symbols like `memcpy`/`memset` introduced by LLVM codegen.

**Final approach**: Bypass ORC entirely, use `RuntimeDyld` directly with `SimpleCompiler`:

```cpp
// Compile module to object code
llvm::orc::SimpleCompiler compiler(*targetMachine);
auto objBuffer = llvm::cantFail(compiler(*llvmModule));

// Parse and link using RuntimeDyld directly
auto objFile = llvm::cantFail(llvm::object::ObjectFile::createObjectFile(objBuffer->getMemBufferRef()));
llvm::RuntimeDyld dyld(*unitmemorymanager, *unitmemorymanager);
dyld.setProcessAllSections(true);
auto loadedObjInfo = dyld.loadObject(*objFile);
dyld.resolveRelocations();
unitmemorymanager->finalizeMemory();
```

**Key insight**: `UnitMemoryManager` inherits from `RTDyldMemoryManager`, which implements both `RuntimeDyld::MemoryManager` and `JITSymbolResolver`. The resolver's `getSymbolAddress` defaults to `getSymbolAddressInProcess`, which resolves external symbols from the host process — exactly matching the LLVM 11 behavior.

Symbol extraction (function offsets, table offset) moved from the ORC `NotifyLoaded` callback to a post-link loop over `computeSymbolSizes(*objFile)`.

### 3b. JITModule Simplified

```cpp
// Old: ORC v1 infrastructure
struct JITModule {
    llvm::orc::ExecutionSession ES;
    std::unique_ptr<llvm::orc::LegacyRTDyldObjectLinkingLayer> objectLayer;
    std::unique_ptr<CompileLayer> compileLayer;
    std::shared_ptr<UnitMemoryManager> unitmemorymanager;
    // ...
};

// New: No ORC, just a memory manager
struct JITModule {
    UnitMemoryManager* unitmemorymanager = nullptr;
    // ...
    JITModule() { unitmemorymanager = new UnitMemoryManager(); }
    ~JITModule() { delete unitmemorymanager; }
};
```

### 3c. Header Changes

```cpp
// Removed:
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/LambdaResolver.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/NullResolver.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/Analysis/Passes.h"

// Added:
#include "llvm/ExecutionEngine/RuntimeDyld.h"

// Kept:
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"  // for SimpleCompiler
```

### 3d. `reserveAllocationSpace` Signature

LLVM 18 changed alignment parameters from `U32` to `llvm::Align`:
```cpp
// Old:
void reserveAllocationSpace(uintptr_t numCodeBytes, U32 codeAlignment, ...);
// New:
void reserveAllocationSpace(uintptr_t numCodeBytes, llvm::Align codeAlignment, ...);
```

### 3e. Removed Optimization Passes

```cpp
// Removed (no longer exist in LLVM 18):
fpm->add(llvm::createJumpThreadingPass());
fpm->add(llvm::createConstantPropagationPass());
```
`createConstantPropagationPass()` was removed in LLVM 12 (subsumed by instcombine). `createJumpThreadingPass()` was removed in LLVM 18.

### 3f. Other Cleanups

- `llvm/Support/Host.h` → `llvm/TargetParser/Host.h`
- `OpenFlags::F_Text` → `OF_Text`
- Removed LLVM 7 compatibility aliases (`LegacyRTDyldObjectLinkingLayer`, `LegacyIRCompileLayer`)
- Removed LLVM <10 `de_offset_t` typedef — replaced with `uint64_t` directly
- Memory manager ownership changed from `shared_ptr` to raw `new`/`delete` (RuntimeDyld takes references, not ownership)

---

## Appendix
### Tests & Results

| Test Suite              | Result                   |
|-------------------------|--------------------------|
| `contracts_unit_test`   | 71 test cases, 0 errors  |
| `unit_test -- --sys-vm` | 871 test cases, 0 errors |

The two pre-existing failures:
1. `deep_mind_tests/deep_mind` — whitespace/encoding mismatch in log comparison
2. `sysvmoc_interrupt_tests/wasm_interrupt_test` — interrupt counter assertion (`post_count == pre_count + 1`)
