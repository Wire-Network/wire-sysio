# GLOBAL DEPENDENCIES
# -------------------

# GLOBAL VARIABLES
set(BOOST_VERSION 1.83.0)
set(vcpkgPrefixDir "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-linux")
message(NOTICE "VCPKG PREFIX Directory: ${vcpkgPrefixDir}")
list(APPEND CMAKE_PREFIX_PATH "${vcpkgPrefixDir}/lib/cmake" "${vcpkgPrefixDir}")

# LOAD CMAKE TOOLS
find_package(PkgConfig REQUIRED)

# FIND PACKAGES WITH VCPKG
# BOOST
find_package(Boost ${BOOST_VERSION}
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

add_library(boost_numeric_ublas INTERFACE)
add_library(Boost::numeric_ublas ALIAS boost_numeric_ublas)

find_package(Boost ${BOOST_VERSION}
  EXACT
  CONFIG
  REQUIRED
  COMPONENTS
  accumulators
)

# OTHER DEPENDENCIES
find_package(CLI11 CONFIG REQUIRED)
find_package(libsodium CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)
find_package(softfloat CONFIG REQUIRED)
