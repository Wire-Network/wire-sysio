#pragma once

#include <boost/dll/runtime_symbol_info.hpp>
#include <filesystem>
#include <string>

namespace fc {

inline std::filesystem::path program_path() {
   return boost::dll::program_location().string(); // e.g. "my_app.exe" or "my_app"
}

inline std::string program_name() {
   return program_path().stem().string();
}
} // namespace fc