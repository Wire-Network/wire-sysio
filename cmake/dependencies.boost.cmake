# Use vcpkg’s BoostConfig.cmake (CONFIG mode), not FindBoost.
unset(Boost_DIR CACHE)
unset(BOOST_ROOT CACHE)
# Make sure we don't force module-mode anywhere:
# unset(Boost_NO_BOOST_CMAKE CACHE)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

# Ask for the version you installed via vcpkg (or omit the version to accept any)
find_package(Boost 1.89.0 EXACT CONFIG REQUIRED
  COMPONENTS
    system
    filesystem
    iostreams
    date_time
    thread
    chrono
    context
    coroutine
    program_options
    interprocess
    process
    unit_test_framework
    dll
    beast
    bimap
    multi_index
    signals2
    multiprecision 
    hana
    property_tree
    lockfree
    assign
    accumulators
    rational
    format
)

# Keep your uBLAS shim only if your code links Boost::numeric_ublas
if (NOT TARGET boost_numeric_ublas)
  add_library(boost_numeric_ublas INTERFACE)
  add_library(Boost::numeric_ublas ALIAS boost_numeric_ublas)
endif()
