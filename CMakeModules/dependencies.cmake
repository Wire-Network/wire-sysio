# Find packages with vcpkg
set(vcpkgPrefixDir "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-linux")
message(NOTICE "VCPKG PREFIX Directory: ${vcpkgPrefixDir}")
list(APPEND CMAKE_PREFIX_PATH "${vcpkgPrefixDir}/lib/cmake" "${vcpkgPrefixDir}")

find_package(PkgConfig REQUIRED)

# Create softfloat target alias for compatibility
if(TARGET softfloat::softfloat AND NOT TARGET softfloat)
  add_library(softfloat ALIAS softfloat::softfloat)
endif()

# Create libsodium target alias for compatibility
if(TARGET libsodium::libsodium AND NOT TARGET libsodium)
  add_library(libsodium ALIAS libsodium::libsodium)
endif()

# Create CLI11 target alias for compatibility
if(TARGET CLI11::CLI11 AND NOT TARGET cli11)
  add_library(cli11 ALIAS CLI11::CLI11)
endif()

## Boost::accumulators depends on Boost::numeric_ublas, which is still missing cmake support (see
## https://github.com/boostorg/cmake/issues/39). Until this is fixed, manually add Boost::numeric_ublas
## as an interface library
## ----------------------------------------------------------------------------------------------------

find_package(Boost 1.83.0
  EXACT
  CONFIG
  REQUIRED
  COMPONENTS
  assign
  system
  multiprecision
  signals2
  beast
  property_tree
  headers
  hana
  bimap
  multi_index
  filesystem
  dll
  lockfree
  process
  program_options
  iostreams
  date_time
  regex
  thread
  chrono
  context
  coroutine
  atomic
  interprocess
  unit_test_framework
)

#find_library(boost_numeric_ublas 1.83.0 EXACT)
#add_library(boost_numeric_ublas ALIAS Boost::headers)
#add_library(Boost::numeric_ublas ALIAS Boost::headers)
add_library(boost_numeric_ublas INTERFACE)
add_library(Boost::numeric_ublas ALIAS boost_numeric_ublas)


find_package(Boost 1.83.0
  EXACT
  CONFIG
  REQUIRED
  COMPONENTS
  accumulators
)

find_package(CLI11 CONFIG REQUIRED)
find_package(libsodium CONFIG REQUIRED)
# find_package(libsodium CONFIG REQUIRED)
find_package(softfloat CONFIG REQUIRED)
