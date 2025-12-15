set(BOOST_VERSION 1.83.0)
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
  asio
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

if (NOT TARGET boost_numeric_ublas)
  add_library(boost_numeric_ublas INTERFACE)
  add_library(Boost::numeric_ublas ALIAS boost_numeric_ublas)
endif()

find_package(Boost ${BOOST_VERSION}
  EXACT
  CONFIG
  REQUIRED
  COMPONENTS
  accumulators
)
