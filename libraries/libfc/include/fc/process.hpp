#pragma once

#include <filesystem>
#include <string>

namespace fc {

/// @return path of the running executable as the operating system reports it: /proc/self/exe on Linux,
///         _NSGetExecutablePath on macOS
/// @throws std::filesystem::filesystem_error or fc::exception when the operating system does not report it
std::filesystem::path program_path();

inline std::string program_name() {
   return program_path().stem().string();
}
} // namespace fc
