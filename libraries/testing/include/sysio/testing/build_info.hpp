#pragma once

#include <boost/filesystem.hpp>

namespace sysio::testing {
namespace bfs = boost::filesystem;
constexpr auto wire_build_root_file = ".wire-build-root";
bfs::path      get_build_root_path();
bfs::path      get_source_root_path();
bfs::path      get_test_fixtures_path();
}