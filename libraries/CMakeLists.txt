set(CHAINBASE_INSTALL_COMPONENT "dev")
set(FC_INSTALL_COMPONENT "dev")
set(APPBASE_INSTALL_COMPONENT "dev")
set(SOFTFLOAT_INSTALL_COMPONENT "dev")
set(SYSVM_INSTALL_COMPONENT "dev")
set(BN256_INSTALL_COMPONENT "dev")

set( Boost_USE_MULTITHREADED      ON )
set( Boost_USE_STATIC_LIBS ON CACHE STRING "ON or OFF" )
# don't include boost mysql library as it does a find_package(OpenSSL) thus finding the system openssl which could conflict with the bundled boringssl
set( BOOST_EXCLUDE_LIBRARIES "mysql" )
add_subdirectory( boost EXCLUDE_FROM_ALL )

# Add ExternalProject module for building non-CMake projects
include(ExternalProject)

# Build libsodium with ExternalProject using the bundled configure script
ExternalProject_Add( libsodium
  PREFIX          ${CMAKE_BINARY_DIR}/libraries/libsodium
  SOURCE_DIR      ${CMAKE_CURRENT_SOURCE_DIR}/libsodium
  INSTALL_DIR     ${CMAKE_BINARY_DIR}/libraries/libsodium-install    # <<< add this
  CONFIGURE_COMMAND
    ${CMAKE_CURRENT_SOURCE_DIR}/libsodium/configure
      --prefix=<INSTALL_DIR>                                         # <<< use the INSTALL_DIR internally
  BUILD_BYPRODUCTS ${CMAKE_BINARY_DIR}/libraries/libsodium-install/libsodium.a
  BUILD_COMMAND   make -C <SOURCE_DIR>
  INSTALL_COMMAND make -C <SOURCE_DIR> install
  BUILD_IN_SOURCE 1
)

# Grab the install directory from that ExternalProject
ExternalProject_Get_Property(libsodium INSTALL_DIR install_dir)

# Now point to the real include/lib under that install prefix
set(LIBSODIUM_INCLUDE_DIR
    ${install_dir}/include
    CACHE PATH "Path to libsodium headers")
set(LIBSODIUM_LIBRARIES
    ${install_dir}/lib/libsodium.a
    CACHE FILEPATH "Path to libsodium static library")

add_subdirectory( libfc )
add_subdirectory( builtins )
add_subdirectory( softfloat )
add_subdirectory( wasm-jit )
add_subdirectory( chainbase )
set(APPBASE_ENABLE_AUTO_VERSION OFF CACHE BOOL "enable automatic discovery of version via 'git describe'")
add_subdirectory( appbase )
add_subdirectory( custom_appbase )
add_subdirectory( chain )
add_subdirectory( testing )
add_subdirectory( version )
add_subdirectory( state_history )
add_subdirectory( cli11 )

set(USE_EXISTING_SOFTFLOAT ON CACHE BOOL "use pre-exisiting softfloat lib")
set(ENABLE_TOOLS OFF CACHE BOOL "Build tools")
set(ENABLE_TESTS OFF CACHE BOOL "Build tests")
set(ENABLE_ADDRESS_SANITIZER OFF CACHE BOOL "Use address sanitizer")
set(ENABLE_UNDEFINED_BEHAVIOR_SANITIZER OFF CACHE BOOL "Use UB sanitizer")
set(ENABLE_PROFILE OFF CACHE BOOL "Enable for profile builds")
if(sys-vm IN_LIST SYSIO_WASM_RUNTIMES OR sys-vm-jit IN_LIST SYSIO_WASM_RUNTIMES)
add_subdirectory( sys-vm )
endif()
add_subdirectory( prometheus )
