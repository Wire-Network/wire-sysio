#include <fc/process.hpp>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <cstdint>
#include <cstring>
#include <system_error>

namespace fc {

std::filesystem::path program_path() {
#if defined(__linux__)
   return std::filesystem::read_symlink("/proc/self/exe");
#elif defined(__APPLE__)
   uint32_t size = 0;
   _NSGetExecutablePath(nullptr, &size); // with no room, reports the size needed, terminator included
   std::string path(size, '\0');
   if (_NSGetExecutablePath(path.data(), &size) != 0) {
      throw std::filesystem::filesystem_error("unable to read the executable path",
                                              std::make_error_code(std::errc::no_buffer_space));
   }
   path.resize(std::strlen(path.c_str()));
   return path;
#else
#error "fc::program_path() supports Linux and macOS"
#endif
}

} // namespace fc
