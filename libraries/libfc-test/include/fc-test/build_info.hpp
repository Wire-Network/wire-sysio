#pragma once

#include <filesystem>

namespace fc::test {
constexpr auto wire_build_root_file = ".wire-build-root";
std::filesystem::path get_exe_path();
std::filesystem::path get_build_root_path();
std::filesystem::path get_source_root_path();
std::filesystem::path get_test_fixtures_path();
} // namespace fc::test