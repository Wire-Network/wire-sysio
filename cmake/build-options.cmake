option(ENABLE_CCACHE "Enable ccache if available" ON)
option(ENABLE_DISTCC "Enable distcc if available" OFF)

option(ENABLE_TOOLS "Build tools" OFF)
option(ENABLE_TESTS "Build tests" OFF)
option(ENABLE_ADDRESS_SANITIZER "Use address sanitizer" OFF)
option(ENABLE_UNDEFINED_BEHAVIOR_SANITIZER "Use UB sanitizer" OFF)

option(ENABLE_PROFILE "Enable for profile builds" OFF)
option(ENABLE_WERROR "Enable `-Werror` compilation flag." Off)
option(ENABLE_WEXTRA "Enable `-Wextra` compilation flag." Off)

option(DISABLE_LLVM_LINKAGE_OVERRIDE "Disable LLVM linkage override" OFF)

option(ENABLE_OC "Enable sysvm-oc on supported platforms" ON)
option(SYSIO_ENABLE_DEVELOPER_OPTIONS "enable developer options for WIRE" OFF)

if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(SYSIO_DEFAULT_SYS_VM_JIT_IS_DEFAULT OFF)
else()
  set(SYSIO_DEFAULT_SYS_VM_JIT_IS_DEFAULT ON)
endif()
option(SYSIO_SYS_VM_JIT_IS_DEFAULT "Use sys-vm-jit as the default WASM runtime when it is compiled in" ${SYSIO_DEFAULT_SYS_VM_JIT_IS_DEFAULT})

# macOS arm64 builds are developer-only, but sys-vm must remain the default runtime.
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$" AND SYSIO_SYS_VM_JIT_IS_DEFAULT)
  message(FATAL_ERROR "SYSIO_SYS_VM_JIT_IS_DEFAULT must remain OFF for macOS arm64 builds")
endif()

option(ENABLE_MULTIVERSION_PROTOCOL_TEST "Enable nodeop multiversion protocol test" OFF)
option(ENABLE_COVERAGE_TESTING "Build WIRE for code coverage analysis" OFF)
option(DISABLE_WASM_SPEC_TESTS "disable building of wasm spec unit tests" OFF)

# allocators (mutually exclusive; enforced in cmake/compiler-config.cmake)
option(ENABLE_TCMALLOC "use tcmalloc (requires gperftools)" OFF)
if(APPLE)
  set(SYSIO_DEFAULT_ENABLE_JEMALLOC OFF)
else()
  set(SYSIO_DEFAULT_ENABLE_JEMALLOC ON)
endif()
option(ENABLE_JEMALLOC "link jemalloc statically into nodeop (via vcpkg)" ${SYSIO_DEFAULT_ENABLE_JEMALLOC})

# Build Artifact Flags
option(BUILD_DOXYGEN "Build doxygen documentation on every make" OFF)
option(BUILD_OPP_BUNDLES "Build OPP bundles for supported platforms" ON)
