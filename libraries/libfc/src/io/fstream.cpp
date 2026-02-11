#include <filesystem>
#include <fstream>
#include <sstream>

#include <gsl-lite/gsl-lite.hpp>

#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>


/**
 * @brief Cleans up a file resource.
 *
 * This function takes a pointer to a file pointer and closes the file resource
 * pointed to by it, ensuring that the pointer is set to nullptr after the cleanup.
 *
 * @param file variable name of file
 *
 * @return A function object that cleans up the file resource when invoked.
 */
#define FILE_RESOURCE_DISPOSER(file) gsl_lite::finally([&] {\
if (file) {\
std::fclose(file);\
file = nullptr;\
}\
})

namespace fc {

namespace fs = std::filesystem;

std::vector<unsigned char> read_file_contents(const std::filesystem::path& filename) {
   auto file_path = fs::absolute(filename);
   FC_ASSERT(fs::exists(file_path), "{} does not exist", file_path.string());
   auto                       file_size = fs::file_size(file_path);
   std::vector<unsigned char> file_contents(file_size);
   auto                       buf = file_contents.data();

   {
      auto file = std::fopen(file_path.c_str(), "rb");
      FC_ASSERT(file, "Failed to open file: {}", file_path.string());

      auto   cleanup    = FILE_RESOURCE_DISPOSER(file);
      size_t read_total = 0;

      while (read_total < file_size) {
         size_t read = std::fread(buf + read_total, 1, file_size - read_total, file);
         if (!read) {
            break;
         }
         read_total += read;
      }

      FC_ASSERT(read_total == file_size,
                "Failed to read entire file (file_path={},read={},expected={})",
                file_path.string(), read_total, file_size
         );

   }

   return file_contents;
}

void read_file_contents(const std::filesystem::path& filename, std::string& result) {
   std::ifstream f(filename.string(), std::ios::in | std::ios::binary);
   FC_ASSERT(f, "Failed to open {}", filename.string());
   // don't use fc::stringstream here as we need something with override for << rdbuf()
   std::stringstream ss;
   ss << f.rdbuf();
   FC_ASSERT(f, "Failed reading {}", filename.string());
   result = ss.str();
}

} // namespace fc